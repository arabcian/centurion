//! The backup restore path trusts what Scanner reports: it must never panic and must give the
//! same answer however the stream is split into reads.
#![no_main]
use centurion_helpers::tarscan::Scanner;
use libfuzzer_sys::fuzz_target;

fn scan(data: &[u8], chunk: usize) -> (bool, Vec<centurion_helpers::tarscan::Event>) {
    let mut s = Scanner::new();
    let mut ev = Vec::new();
    for part in data.chunks(chunk) { s.feed(part, &mut |e| ev.push(e)); }
    (s.complete(), ev)
}

fuzz_target!(|data: &[u8]| {
    let Some((&c, rest)) = data.split_first() else { return };
    assert_eq!(scan(rest, rest.len().max(1)), scan(rest, usize::from(c).max(1)));
});
