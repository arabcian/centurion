//! Reads the headers of a tar stream while it passes through to the real tar, for the restore of an
//! incremental image chain (backup.rs). It reports every member name and GNU tar's *dumpdirs*: the
//! listing `--listed-incremental` stores with each directory (`Y`/`N`/`D` names, `R`→`T` renames).
//!
//! It never changes the bytes and never decides on its own: any header it does not understand (a bad
//! checksum, a multi-volume or sparse member, an oversized dumpdir) marks the stream `broken`, and the
//! caller then applies no rename and no deletion from it. Formats: GNU (`L` long names), ustar
//! (`prefix`), pax (`x` records, `path`).

const BLOCK: usize = 512;
/// A dumpdir larger than this (a directory with ~millions of names) is not kept: the stream is marked broken.
const MAX_DUMPDIR: u64 = 64 << 20;
const MAX_META: u64 = 1 << 20;

/// One directory's listing in an incremental archive. `dir` and every name are normalized (see `norm`).
#[derive(Debug, Default, Clone, PartialEq)]
pub struct Dumpdir {
    pub dir: Vec<u8>,
    /// Names present in the directory when the archive was made (dumped or not).
    pub names: Vec<Vec<u8>>,
    /// Directory renames recorded in this listing: (old path, new path), both from the archive root.
    pub renames: Vec<(Vec<u8>, Vec<u8>)>,
    /// false when the listing held a code this reader does not know: nothing may be deleted from it.
    pub ok: bool,
}

#[derive(Debug, PartialEq)]
pub enum Event {
    Member(Vec<u8>),
    Dumpdir(Dumpdir),
}

enum Data { Skip, Keep(Vec<u8>) }

pub struct Scanner {
    hdr: Vec<u8>,
    /// Bytes of member data still to pass (including the padding to a full block).
    left: u64,
    /// Data bytes of the current member that are content (the rest of `left` is padding).
    content: u64,
    data: Data,
    kind: u8,
    name: Vec<u8>,
    long_name: Option<Vec<u8>>,
    pax_path: Option<Vec<u8>>,
    /// POSIX format (what GNU tar writes with --xattrs): the dumpdir travels as the pax record GNU.dumpdir.
    pax_dumpdir: Option<Vec<u8>>,
    sparse_ext: bool,
    zeros: u8,
    pub broken: bool,
    pub ended: bool,
}

impl Default for Scanner { fn default() -> Self { Scanner::new() } }

impl Scanner {
    pub fn new() -> Scanner {
        Scanner { hdr: Vec::with_capacity(BLOCK), left: 0, content: 0, data: Data::Skip, kind: 0, name: Vec::new(),
                  long_name: None, pax_path: None, pax_dumpdir: None, sparse_ext: false, zeros: 0, broken: false, ended: false }
    }

    /// Feeds the next bytes of the (decompressed) stream; events are reported as soon as they are complete.
    pub fn feed(&mut self, mut buf: &[u8], on: &mut dyn FnMut(Event)) {
        while !buf.is_empty() && !self.broken && !self.ended {
            if self.left > 0 {
                let n = (self.left.min(buf.len() as u64)) as usize;
                if let Data::Keep(v) = &mut self.data {
                    let used = (v.len() as u64).min(self.content);
                    let want = (self.content - used).min(n as u64) as usize;
                    v.extend_from_slice(&buf[..want]);
                }
                self.left -= n as u64;
                buf = &buf[n..];
                if self.left == 0 { self.member_done(on); }
                continue;
            }
            let n = (BLOCK - self.hdr.len()).min(buf.len());
            self.hdr.extend_from_slice(&buf[..n]);
            buf = &buf[n..];
            if self.hdr.len() == BLOCK {
                let h = std::mem::take(&mut self.hdr);
                self.header(&h, on);
                self.hdr = h;
                self.hdr.clear();
            }
        }
    }

    fn header(&mut self, h: &[u8], on: &mut dyn FnMut(Event)) {
        if self.sparse_ext {
            // Old GNU sparse extension block: the last byte of its map says whether another follows.
            self.sparse_ext = h[504] != 0;
            if !self.sparse_ext { self.start_data(on); }
            return;
        }
        if h.iter().all(|&b| b == 0) {
            self.zeros += 1;
            if self.zeros >= 2 { self.ended = true; }
            return;
        }
        self.zeros = 0;
        if !checksum_ok(h) { self.broken = true; return; }
        let Some(size) = number(&h[124..136]) else { self.broken = true; return };
        self.kind = h[156];
        self.content = size;
        self.data = Data::Skip;
        let keep = |max: u64| if size <= max { Some(Data::Keep(Vec::with_capacity(size as usize))) } else { None };
        match self.kind {
            b'L' | b'x' => match keep(MAX_META) { Some(d) => self.data = d, None => { self.broken = true; return; } },
            b'K' | b'g' | b'V' => {}
            b'M' => { self.broken = true; return; }
            b'D' => {
                self.name = self.member_name(h);
                on(Event::Member(norm(&self.name)));
                match keep(MAX_DUMPDIR) { Some(d) => self.data = d, None => { self.broken = true; return; } }
            }
            b'S' => {
                self.name = self.member_name(h);
                on(Event::Member(norm(&self.name)));
                // Old GNU sparse: the stored size is in `realsize`'s sibling field; extension blocks may follow.
                if h[482] != 0 { self.sparse_ext = true; return; }
            }
            _ => {
                let dd = self.pax_dumpdir.take();
                self.name = self.member_name(h);
                let n = norm(&self.name);
                on(Event::Member(n.clone()));
                if let Some(v) = dd.filter(|_| self.kind == b'5') { on(Event::Dumpdir(dumpdir(&n, &v))); }
            }
        }
        self.start_data(on);
    }

    fn start_data(&mut self, on: &mut dyn FnMut(Event)) {
        self.left = self.content.div_ceil(BLOCK as u64) * BLOCK as u64;
        if self.left == 0 { self.member_done(on); }
    }

    /// The name of a real member: a pending GNU long name or pax path wins over the header's own.
    fn member_name(&mut self, h: &[u8]) -> Vec<u8> {
        let pax = self.pax_path.take();
        let long = self.long_name.take();
        if let Some(p) = pax { return p; }
        if let Some(l) = long { return l; }
        let name = cstr(&h[0..100]);
        // POSIX ustar ("ustar\0"): a prefix field. GNU ("ustar  \0") keeps other data there.
        if &h[257..263] == b"ustar\0" {
            let prefix = cstr(&h[345..500]);
            if !prefix.is_empty() { return [prefix, b"/", name].concat(); }
        }
        name.to_vec()
    }

    fn member_done(&mut self, on: &mut dyn FnMut(Event)) {
        let Data::Keep(v) = std::mem::replace(&mut self.data, Data::Skip) else { return };
        match self.kind {
            b'L' => self.long_name = Some(cstr(&v).to_vec()),
            b'x' => {
                let (p, d) = pax_records(&v);
                self.pax_path = p;
                self.pax_dumpdir = d;
            }
            b'D' => on(Event::Dumpdir(dumpdir(&norm(&self.name), &v))),
            _ => {}
        }
    }
}

fn cstr(b: &[u8]) -> &[u8] { &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())] }

/// Octal (space/NUL terminated) or GNU base-256 (high bit of the first byte set, positive only).
fn number(f: &[u8]) -> Option<u64> {
    if f[0] & 0x80 != 0 {
        if f[0] & 0x40 != 0 { return None; }
        let mut v: u64 = (f[0] & 0x3f) as u64;
        for &b in &f[1..] { v = v.checked_mul(256)?.checked_add(b as u64)?; }
        return Some(v);
    }
    let s = f.iter().skip_while(|&&b| b == b' ').take_while(|&&b| (b'0'..=b'7').contains(&b));
    let mut v: u64 = 0;
    for &b in s { v = v.checked_mul(8)?.checked_add((b - b'0') as u64)?; }
    Some(v)
}

fn checksum_ok(h: &[u8]) -> bool {
    let Some(want) = number(&h[148..156]) else { return false };
    let field = |i: usize, b: u8| if (148..156).contains(&i) { b' ' } else { b };
    let unsigned: u64 = h.iter().enumerate().map(|(i, &b)| field(i, b) as u64).sum();
    // Some old tars summed signed bytes; GNU tar accepts both.
    let signed: i64 = h.iter().enumerate().map(|(i, &b)| field(i, b) as i8 as i64).sum();
    unsigned == want || signed == want as i64
}

/// `path` and `GNU.dumpdir` of pax records: "<len> <key>=<value>\n"… (values may hold NULs).
/// A malformed record list yields neither.
fn pax_records(v: &[u8]) -> (Option<Vec<u8>>, Option<Vec<u8>>) {
    let mut rest = v;
    let (mut path, mut dd) = (None, None);
    while !rest.is_empty() {
        let Some(sp) = rest.iter().position(|&b| b == b' ') else { return (None, None) };
        let Some(len) = std::str::from_utf8(&rest[..sp]).ok().and_then(|s| s.parse::<usize>().ok()) else { return (None, None) };
        if len <= sp + 1 || len > rest.len() || rest[len - 1] != b'\n' { return (None, None); }
        let rec = &rest[sp + 1..len - 1];
        if let Some(val) = rec.strip_prefix(b"path=") { path = Some(val.to_vec()); }
        if let Some(val) = rec.strip_prefix(b"GNU.dumpdir=") { dd = Some(val.to_vec()); }
        rest = &rest[len..];
    }
    (path, dd)
}

/// The dumpdir of `dir`: "<code><name>\0"… ending with an empty entry.
fn dumpdir(dir: &[u8], v: &[u8]) -> Dumpdir {
    let mut d = Dumpdir { dir: dir.to_vec(), ok: true, ..Default::default() };
    let mut pending: Option<Vec<u8>> = None;
    for e in v.split(|&b| b == 0) {
        let Some((&code, name)) = e.split_first() else { break };
        match code {
            b'Y' | b'N' | b'D' => d.names.push(name.to_vec()),
            b'R' => pending = Some(norm(name)),
            b'T' => match pending.take() {
                Some(old) => d.renames.push((old, norm(name))),
                None => d.ok = false,
            },
            b'X' => {}
            _ => d.ok = false,
        }
    }
    d
}

/// Archive names relative to the archive root: no leading "/" or "./", no trailing "/"; the root is "".
pub fn norm(p: &[u8]) -> Vec<u8> {
    let mut s = p;
    loop {
        if let Some(r) = s.strip_prefix(b"./") { s = r; }
        else if let Some(r) = s.strip_prefix(b"/") { s = r; }
        else { break; }
    }
    while let Some(r) = s.strip_suffix(b"/") { s = r; }
    if s == b"." { s = b""; }
    s.to_vec()
}

/// A normalized path that can be resolved under a target directory: no empty, "." or ".." component.
pub fn plain_path(p: &[u8]) -> bool {
    !p.is_empty() && p.split(|&b| b == b'/').all(|c| !c.is_empty() && c != b"." && c != b"..")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    fn scan(bytes: &[u8], chunk: usize) -> (Vec<Event>, bool) {
        let mut s = Scanner::new();
        let mut out = Vec::new();
        for c in bytes.chunks(chunk) { s.feed(c, &mut |e| out.push(e)); }
        (out, s.broken)
    }

    #[test]
    fn names_are_normalized() {
        assert_eq!(norm(b"./etc/"), b"etc");
        assert_eq!(norm(b"/./a/b//"), b"a/b");
        assert_eq!(norm(b"./"), b"");
        assert!(plain_path(b"a/b"));
        assert!(!plain_path(b"a/../b"));
        assert!(!plain_path(b""));
    }

    #[test]
    fn dumpdir_codes() {
        let d = dumpdir(b"x", b"Ya\0Nb\0Dsub\0R./old\0T./new\0\0");
        assert_eq!(d.names, vec![b"a".to_vec(), b"b".to_vec(), b"sub".to_vec()]);
        assert_eq!(d.renames, vec![(b"old".to_vec(), b"new".to_vec())]);
        assert!(d.ok);
        assert!(!dumpdir(b"x", b"Qa\0\0").ok);
    }

    #[test]
    fn pax_record_lists() {
        assert_eq!(pax_records(b"20 path=a/long/name\n").0.as_deref(), Some(&b"a/long/name"[..]));
        assert_eq!(pax_records(b"9 x=yyyy\n"), (None, None));
        assert_eq!(pax_records(b"99 path=x\n"), (None, None));
        assert_eq!(pax_records(b"20 GNU.dumpdir=Yf\0\0\n").1.as_deref(), Some(&b"Yf\0\0"[..]));
    }

    #[test]
    fn garbage_is_broken_not_trusted() {
        let (ev, broken) = scan(&[0x55u8; 2048], 100);
        assert!(ev.is_empty());
        assert!(broken);
    }

    /// Real archives from GNU tar, fed in odd-sized chunks: every member and the dumpdirs come out.
    #[test]
    fn reads_gnu_incremental_archives() {
        if Command::new("tar").arg("--version").output().map_or(true, |o| !o.status.success()) { return; }
        let dir = std::env::temp_dir().join(format!("centurion-tarscan-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let src = dir.join("src");
        let long = "l".repeat(150);
        std::fs::create_dir_all(src.join("d1/sub").join(&long)).unwrap();
        std::fs::write(src.join("d1/sub/f"), "1").unwrap();
        std::fs::write(src.join("d1/we ird\nname"), "x").unwrap();
        std::fs::write(src.join("d1/sub").join(&long).join("deep"), vec![7u8; 3000]).unwrap();
        // posix is what system_backup writes in practice: GNU tar switches to it for --xattrs.
        for (fmt, file) in [("gnu", "g.tar"), ("posix", "p.tar")] {
            let ok = Command::new("tar").current_dir(&src).arg(format!("--format={fmt}"))
                .arg(format!("--listed-incremental={}", dir.join(format!("{fmt}.snar")).display()))
                .arg("-cf").arg(dir.join(file)).arg(".").status().unwrap().success();
            assert!(ok);
            let bytes = std::fs::read(dir.join(file)).unwrap();
            for chunk in [1, 7, 512, 4096, 1 << 20] {
                let (ev, broken) = scan(&bytes, chunk);
                assert!(!broken, "{fmt} chunk {chunk}");
                let members: Vec<&Vec<u8>> = ev.iter().filter_map(|e| if let Event::Member(m) = e { Some(m) } else { None }).collect();
                let deep = format!("d1/sub/{long}/deep").into_bytes();
                assert!(members.contains(&&deep), "{fmt}: {:?}", members.iter().map(|m| String::from_utf8_lossy(m)).collect::<Vec<_>>());
                assert!(members.contains(&&b"d1/we ird\nname".to_vec()));
                let dd: Vec<&Dumpdir> = ev.iter().filter_map(|e| if let Event::Dumpdir(d) = e { Some(d) } else { None }).collect();
                let d1 = dd.iter().find(|d| d.dir == b"d1").unwrap_or_else(|| panic!("{fmt} chunk {chunk}: {dd:?}"));
                assert!(d1.ok);
                assert!(d1.names.contains(&b"we ird\nname".to_vec()) && d1.names.contains(&b"sub".to_vec()));
            }
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
