//! `<archive>.RESTORE.txt`: a plain-text, bilingual (Türkçe / English) guide written next to every backup
//! Health → Backups makes. It says how to restore that exact archive by hand, with Centurion not running
//! (a live USB, a rescue shell): which files, in which order, with which flags. The commands appear once;
//! the prose around them is Turkish, then English. Nothing here is secret (no key, no passphrase).

use crate::backup::{ETC_RESTORE, MOUNT_POINTS, VARLIB_RESTORE};

/// How an image is encrypted (its chain has one kind throughout).
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Crypt { None, Key, Passphrase }

pub struct Image<'a> {
    /// Folder the image was written to.
    pub dir: &'a str,
    /// File names in restore order: the full image first, this image last.
    pub steps: &'a [String],
    pub crypt: Crypt,
    pub include_home: bool,
    pub created: &'a str,
    pub host: &'a str,
}

pub struct Config<'a> {
    pub archive: &'a str,
    pub created: &'a str,
    pub host: &'a str,
    pub profile_only: bool,
    /// Files per part, from the manifest.
    pub user_config: u64,
    pub user_state: u64,
    pub system: u64,
    pub system_profile: u64,
}

/// `<archive>.RESTORE.txt`.
pub fn guide_path(archive: &std::path::Path) -> String { format!("{}.RESTORE.txt", archive.display()) }

fn sq(s: &str) -> String { format!("'{}'", s.replace('\'', r"'\''")) }

/// The command that turns an image file into a tar stream: `[age -d …] | <decompressor>`.
fn unpack_stage(file: &str, crypt: Crypt) -> String {
    let plain = file.strip_suffix(".age").unwrap_or(file);
    let dec = if plain.ends_with(".tar.zst") { "zstd -dc" } else if plain.ends_with(".tar.xz") { "xz -dc" } else { "gzip -dc" };
    let f = sq(file);
    match crypt {
        Crypt::None => format!("{dec} {f}"),
        Crypt::Key => format!("age -d -i \"$AGEKEY\" {f} | {dec}"),
        Crypt::Passphrase => format!("age -d {f} | {dec}"),
    }
}

const TAR_X: &str = "tar --acls --xattrs --xattrs-include='*' --numeric-owner -xpf - -C \"$TARGET\"";

fn line(s: &mut String, t: &str) { s.push_str(t); s.push('\n'); }
fn bi(s: &mut String, tr: &str, en: &str) {
    for l in tr.lines() { s.push_str("  TR: "); s.push_str(l); s.push('\n'); }
    for l in en.lines() { s.push_str("  EN: "); s.push_str(l); s.push('\n'); }
}
fn cmd(s: &mut String, c: &str) { s.push('\n'); for l in c.lines() { s.push_str("      "); s.push_str(l); s.push('\n'); } s.push('\n'); }
fn head(s: &mut String, t: &str) { s.push('\n'); line(s, &"=".repeat(78)); line(s, t); line(s, &"=".repeat(78)); }

pub fn image_guide(g: &Image) -> String {
    let this = g.steps.last().map(String::as_str).unwrap_or("");
    let level = g.steps.len().saturating_sub(1);
    let mut s = String::new();
    head(&mut s, "CENTURION — SİSTEM İMAJI GERİ YÜKLEME KILAVUZU / SYSTEM IMAGE RESTORE GUIDE");
    bi(&mut s,
       "Bu dosya, aşağıdaki yedek alındığı anda Centurion tarafından kendiliğinden yazıldı. Centurion çalışmasa da (canlı USB, kurtarma kabuğu)\nyedeği elle geri yüklemek için gereken her şey burada. Yedek dosyasıyla birlikte saklayın.",
       "Centurion wrote this file by itself when the backup below was made. It holds everything needed to restore the backup by hand\nwith Centurion not running (live USB, rescue shell). Keep it together with the backup file.");
    s.push('\n');
    line(&mut s, &format!("  Yedek / Backup         : {this}"));
    line(&mut s, &format!("  Klasör / Folder        : {}", g.dir));
    line(&mut s, &format!("  Tür / Kind             : {}", if level == 0 { "tam imaj / full image".to_string() }
                                                        else { format!("artımlı, seviye {level} / incremental, level {level}") }));
    line(&mut s, &format!("  Şifreleme / Encryption : {}", match g.crypt {
        Crypt::None => "yok / none",
        Crypt::Key => "açık anahtarla (age) / to a public key (age) — gizli anahtar dosyası gerekir / the secret key file is needed",
        Crypt::Passphrase => "parola ile (age) / passphrase (age) — parola gerekir / the passphrase is needed" }));
    line(&mut s, &format!("  /home dahil / /home in : {}", if g.include_home { "evet / yes" } else { "hayır / no (/home/* yedeğe girmedi / not in the image)" }));
    line(&mut s, &format!("  Alındığı an / Created  : {}   Makine / Host: {}", g.created, g.host));

    head(&mut s, "0. NEYE İHTİYACINIZ VAR / WHAT YOU NEED");
    if g.steps.len() > 1 {
        bi(&mut s,
           &format!("Bu artımlı bir yedektir: tek başına işe yaramaz. Aşağıdaki {} dosyanın HEPSİ, bu sırayla, aynı klasörde olmalı\n(önce tam imaj, sonra her artım). Biri eksikse o noktadan sonrası geri yüklenemez.", g.steps.len()),
           &format!("This is an incremental backup: it is useless alone. ALL {} files below must be present, and are restored in this order\n(the full image first, then each increment). If one is missing, nothing after it can be restored.", g.steps.len()));
        for (i, f) in g.steps.iter().enumerate() {
            line(&mut s, &format!("      {}. {}{}", i + 1, f, if i == 0 { "   (tam imaj / full image)".to_string() } else { format!("   (artım / increment {i})") }));
        }
        s.push('\n');
        bi(&mut s,
           "Yalnızca bu noktaya kadarki durumu geri yükler; bundan SONRA alınan artımları uygulamayın (kendi RESTORE.txt dosyaları vardır).",
           "This restores the state up to this point only; do NOT apply increments made AFTER it (they have their own RESTORE.txt).");
    } else {
        bi(&mut s, "Bu tam bir imajdır; tek dosya yeter:", "This is a full image; one file is enough:");
        line(&mut s, &format!("      1. {this}"));
    }
    s.push('\n');
    // One compressor per file, from its name: an increment may use another one than the image it continues.
    let mut tools: Vec<&str> = Vec::new();
    for f in g.steps {
        let t = unpack_stage(f, Crypt::None).split(' ').next().unwrap_or("gzip").to_owned();
        let t = if t == "zstd" { "zstd" } else if t == "xz" { "xz" } else { "gzip" };
        if !tools.contains(&t) { tools.push(t); }
    }
    let list = tools.join(", ");
    bi(&mut s,
       &format!("Araçlar: tar (GNU tar) ve {list}{}", if tools.len() > 1 { " — bu zincirde dosyalar farklı sıkıştırıcılarla yazılmış, HEPSİ kurulu olmalı" } else { "" }),
       &format!("Tools: tar (GNU tar) and {list}{}", if tools.len() > 1 { " — the files of this chain were made with different compressors: ALL must be installed" } else { "" }));
    if g.crypt != Crypt::None {
        bi(&mut s, "Ayrıca: age (https://age-encryption.org). Canlı sistemde yoksa kurun veya static ikilisini indirin.",
           "Also: age (https://age-encryption.org). Install it in the live system, or download its static binary.");
    }
    match g.crypt {
        Crypt::Key => bi(&mut s, "Gizli anahtar dosyası (içinde AGE-SECRET-KEY-1… satırı olan dosya). Bu olmadan imaj AÇILAMAZ.",
                         "The secret key file (a file with an AGE-SECRET-KEY-1… line). Without it the image CANNOT be opened."),
        Crypt::Passphrase => bi(&mut s, "Yedek alınırken kullandığınız parola. Bu olmadan imaj AÇILAMAZ; Centurion'da kurtarma yolu yoktur.",
                                "The passphrase used when the backup was made. Without it the image CANNOT be opened; Centurion has no recovery."),
        Crypt::None => {}
    }

    head(&mut s, "1. HAZIRLIK / PREPARATION");
    bi(&mut s,
       "Geri yüklenecek sistemden DEĞİL, bir canlı USB'den (veya başka bir kurulu sistemden) açın. Çalışan bir sistemin üzerine '/' hedefiyle\nGERİ YÜKLEMEYİN: dosyalar kullanımdayken ezilir ve sistem açılmaz hale gelir.\nHedef bölüm yeni biçimlendirilmiş (boş) olmalı. Bölümü biçimlendirmek eski verinin tamamını siler.",
       "Boot a live USB (or another installed system), NOT the system being restored. Do NOT restore onto '/' of a running system:\nfiles in use get overwritten and the machine will not boot.\nThe target partition should be freshly formatted (empty). Formatting erases all of its old data.");
    bi(&mut s, "Hedef kök bölümünü bağlayın (disk adlarını kendi makinenize göre değiştirin; LUKS kullanıyorsanız önce cryptsetup open yapın):",
       "Mount the target root partition (change the device names to your machine; with LUKS, run cryptsetup open first):");
    cmd(&mut s, "# lsblk -f                      # bölümleri görün / see the partitions\n# mkfs.ext4 /dev/XXX             # yalnızca yeni/boş hedef için / only for a new, empty target\nmkdir -p /mnt/target\nmount /dev/XXX /mnt/target       # XXX: kök bölümünüz / your root partition\nmkdir -p /mnt/target/boot        # ayrı /boot veya EFI bölümü varsa, onu da bağlayın / mount a separate /boot or EFI partition too");
    bi(&mut s, "Yedek diskini bağlayın ve yedek klasörüne girin (klasör taşındıysa yolu değiştirin):",
       "Mount the backup disk and go to the backup folder (change the path if the folder was moved):");
    let mut prep = String::from("set -o pipefail                  # bir aşama başarısız olursa komut da başarısız sayılsın / fail if any stage fails\n");
    prep.push_str(&format!("cd {}\nTARGET=/mnt/target\n", sq(g.dir)));
    match g.crypt {
        Crypt::Key => prep.push_str("AGEKEY=/yol/gizli-anahtar.txt     # gizli anahtar dosyanız / your secret key file\n"),
        Crypt::Passphrase => prep.push_str("# age parolayı terminalden sorar; kendiniz yazın / age asks for the passphrase on the terminal; type it yourself\n"),
        Crypt::None => {}
    }
    cmd(&mut s, prep.trim_end());
    bi(&mut s, "İsteğe bağlı sağlama (hedefe hiçbir şey yazmaz; dosyanın bozuk olup olmadığını gösterir):",
       "Optional check (writes nothing to the target; shows whether the file is damaged):");
    cmd(&mut s, &format!("{} | tar -tf - > /dev/null && echo TAMAM-OK", unpack_stage(this, g.crypt)));

    head(&mut s, "2. GERİ YÜKLEME / RESTORE");
    bi(&mut s, "Bayraklar: -p izinleri, --acls ve --xattrs ACL ile genişletilmiş öznitelikleri (capabilities, SELinux), --numeric-owner sahipleri\nad yerine sayıyla geri yükler (canlı sistemin kullanıcı listesi farklı olsa da sahipler doğru kalır). -C hedef dizindir.",
       "Flags: -p keeps permissions, --acls and --xattrs keep ACLs and extended attributes (capabilities, SELinux), --numeric-owner restores owners\nby number, not name (so owners stay right even though the live system's user list differs). -C is the target directory.");
    if g.steps.len() > 1 {
        bi(&mut s, "Her dosyayı SIRAYLA, birbiri ardına çalıştırın. Bir adım hata verirse durun; sonrakini çalıştırmayın.",
           "Run each file IN ORDER, one after another. If a step fails, stop; do not run the next one.");
    }
    let mut run = String::new();
    for (i, f) in g.steps.iter().enumerate() {
        if g.steps.len() > 1 { run.push_str(&format!("# adım / step {} / {}: {}\n", i + 1, g.steps.len(), f)); }
        run.push_str(&format!("{} | {TAR_X}\n", unpack_stage(f, g.crypt)));
    }
    cmd(&mut s, run.trim_end());
    bi(&mut s, "tar çıkış kodları: 0 = temiz; 1 = bazı dosyalar okunurken değişmişti (yedek alınırken çalışan sistemde normaldir); 2 veya üstü = hata,\nçıktıyı okuyun. Bozuk/eksik arşiv ('Unexpected EOF', 'incorrect passphrase', 'no identity matched') için aşağıdaki sorun giderme bölümüne bakın.",
       "tar exit codes: 0 = clean; 1 = some files changed while being read (normal for a backup of a running system); 2 or more = an error,\nread the output. For a damaged/missing archive ('Unexpected EOF', 'incorrect passphrase', 'no identity matched') see Troubleshooting below.");
    if g.steps.len() > 1 {
        bi(&mut s, "ÖNEMLİ — artımlı geri yüklemede tar'ın kendi --listed-incremental / -G seçeneğini KULLANMAYIN. O seçenek, arşivde olmayan her şeyi\nsilmeye çalışır: yeni kullanıcılar (/home), bağlı diskler (/mnt, yedek diskin kendisi dahil). Yukarıdaki düz '-xpf' komutları güvenlidir.\nBedeli: yedekler arasında silinen veya yeniden adlandırılan dosyalar hedefte kalır (eski ad + yeni ad birlikte olabilir). Zararsız bir\nfazlalıktır; Centurion'ın kendi geri yüklemesi (Health → Backups) bunları yalnızca zincirin geri yüklediği dosyalar arasında uygular.",
           "IMPORTANT — for an incremental restore do NOT use tar's own --listed-incremental / -G. It tries to delete everything that is not in\nthe archive: new users under /home, mounted disks under /mnt (the backup disk itself included). The plain '-xpf' commands above are safe.\nThe price: files deleted or renamed between the backups stay on the target (an old name next to the new one). That is harmless\nclutter; Centurion's own restore (Health → Backups) applies them, but only among the files the chain restored.");
    }

    head(&mut s, "3. EKSİK KALAN KLASÖRLER / MISSING MOUNT POINTS");
    bi(&mut s, "İmaj /proc, /sys, /dev, /run, /tmp içeriğini taşımaz. Bu klasörler yoksa oluşturun (açılışta üzerlerine bağlanır):",
       "The image holds no contents of /proc, /sys, /dev, /run, /tmp. Create these directories if missing (they get mounted over at boot):");
    let mut mk = String::new();
    for (d, mode) in MOUNT_POINTS { mk.push_str(&format!("[ -e \"$TARGET/{d}\" ] || {{ mkdir -p \"$TARGET/{d}\" && chmod {mode:o} \"$TARGET/{d}\"; }}\n")); }
    cmd(&mut s, mk.trim_end());

    head(&mut s, "4. AÇILABİLİR HALE GETİRME / MAKING IT BOOTABLE");
    bi(&mut s, "Dosyalar geri geldi; ancak yeni bölümün UUID'si eskisinden farklıysa sistem açılmaz. Sırasıyla:\n  a) $TARGET/etc/fstab içindeki UUID/etiketleri 'blkid' çıktısıyla karşılaştırın ve düzeltin (kök, /boot, EFI, swap).\n  b) chroot'a girip önyükleyiciyi ve initramfs'i yeniden üretin (aşağıdakiler ÖRNEKTİR; dağıtımınıza uygun olanı kullanın).",
       "The files are back, but if the new partition's UUID differs from the old one the system will not boot. In order:\n  a) compare the UUIDs/labels in $TARGET/etc/fstab with the output of 'blkid' and fix them (root, /boot, EFI, swap).\n  b) enter the chroot and regenerate the bootloader and initramfs (the lines below are EXAMPLES; use the ones for your distribution).");
    cmd(&mut s, "for d in dev proc sys run; do mount --rbind /$d \"$TARGET/$d\"; done\nchroot \"$TARGET\" /bin/bash\n  # --- chroot içinde / inside the chroot ---\n  # initramfs:   dracut --force --regenerate-all   |  mkinitcpio -P  |  update-initramfs -u -k all\n  # GRUB:        grub-install /dev/XXX && grub-mkconfig -o /boot/grub/grub.cfg   (UEFI: --efi-directory=/boot/efi)\n  # systemd-boot: bootctl install\n  exit\numount -R \"$TARGET\"\nreboot");
    bi(&mut s, "LUKS kök kullanıyorsanız /etc/crypttab ve çekirdek komut satırındaki (cryptdevice= / rd.luks.uuid=) UUID'leri de güncelleyin.",
       "With a LUKS root, also update the UUIDs in /etc/crypttab and on the kernel command line (cryptdevice= / rd.luks.uuid=).");

    head(&mut s, "5. TEK DOSYA / KLASÖR GERİ YÜKLEMEK / RESTORING ONLY SOME FILES");
    bi(&mut s, "Üye adları başında '/' olmadan saklanır (örn. etc/fstab; bazen ./etc/fstab). Önce arayın, sonra yalnızca onu çıkarın:",
       "Members are stored without a leading '/' (e.g. etc/fstab; sometimes ./etc/fstab). Look first, then extract only that:");
    let mut part = format!("mkdir -p /tmp/geri-yukle\n{} | tar -tf - | grep fstab\n", unpack_stage(this, g.crypt));
    part.push_str(&format!("{} | tar --acls --xattrs --xattrs-include='*' --numeric-owner -xpf - -C /tmp/geri-yukle etc/fstab", unpack_stage(this, g.crypt)));
    cmd(&mut s, &part);
    if g.steps.len() > 1 {
        bi(&mut s, "Artımlı zincirde bir dosya yalnızca değiştiği artımda bulunur: onu bulamazsanız zincirdeki önceki dosyalarda (sondan başa) arayın.",
           "In an incremental chain a file is only in the increment where it changed: if you cannot find it, look in the earlier files of the chain (last to first).");
    }

    head(&mut s, "6. SORUN GİDERME / TROUBLESHOOTING");
    if g.crypt != Crypt::None {
        bi(&mut s, "'no identity matched' = yanlış gizli anahtar; 'incorrect passphrase' = yanlış parola; 'passphrase-encrypted' = bu imaj anahtara değil parolaya göre şifrelenmiş\n(-i yerine parolayı yazın). age, parolayı yalnızca terminalden sorar.",
           "'no identity matched' = wrong secret key; 'incorrect passphrase' = wrong passphrase; 'passphrase-encrypted' = this image was encrypted with a passphrase,\nnot to a key (drop -i and type the passphrase). age asks for a passphrase only on a terminal.");
    }
    bi(&mut s, "'Unexpected EOF' / 'not in gzip format' / 'Truncated' = dosya yarım kopyalanmış veya bozuk. Dosya boyutunu yedek diskteki özgün ile karşılaştırın,\nyeniden kopyalayın. Aynı klasördeki '.info.json' dosyası yedeğin bayt sayısını ve dosya sayısını kaydeder.",
       "'Unexpected EOF' / 'not in gzip format' / 'Truncated' = the file was copied halfway or is damaged. Compare its size with the original on the backup disk and\ncopy it again. The '.info.json' next to it records the backup's byte count and file count.");
    bi(&mut s, "Yedekle birlikte '.snar' / '.info.json' dosyaları da yazılır (şifreli imajlarda .snar, /var/lib/centurion/backup-snar altındadır); geri yüklemek için gerekmezler\n(yalnızca bir sonraki artımlı yedek için). Silinmişlerse sonraki yedek otomatik olarak yeni bir tam imaj olur.",
       "The '.snar' / '.info.json' files written with the backup (for encrypted images the .snar is under /var/lib/centurion/backup-snar) are not needed to restore\n(only to make the next increment). If they are gone, the next backup simply\nbecomes a new full image.");
    bi(&mut s, "Centurion çalışıyorsa aynı işlem: Health → Backups → bu yedeği seçip Restore (artımlı zinciri ve silinen/yeniden adlandırılanları kendisi uygular).",
       "With Centurion running it is the same job: Health → Backups → select this backup → Restore (it applies the chain, deletions and renames itself).");
    line(&mut s, "");
    line(&mut s, "(Eksik veya yanlış bir şey görürseniz bu kılavuzu düzeltin / If you find something missing or wrong, please amend this guide.)");
    s
}

pub fn config_guide(g: &Config) -> String {
    let mut s = String::new();
    head(&mut s, "CENTURION — YAPILANDIRMA YEDEĞİ GERİ YÜKLEME KILAVUZU / CONFIGURATION BACKUP RESTORE GUIDE");
    bi(&mut s,
       "Bu dosya, aşağıdaki yedek alındığı anda Centurion tarafından kendiliğinden yazıldı. Centurion çalışmasa da yedeği elle geri yüklemek için\ngereken her şey burada. Bu bir sistem imajı DEĞİLDİR: yalnızca Centurion ayarlarını (ve isteğe bağlı bir 'sistem profili' referansını) içerir.\nTam sistem için 'backup-….tar.*' imajlarının kendi RESTORE.txt dosyasına bakın.",
       "Centurion wrote this file by itself when the backup below was made. It holds everything needed to restore the backup by hand even with Centurion not running.\nThis is NOT a system image: it holds only Centurion's settings (and an optional 'system profile' for reference).\nFor a whole system see the RESTORE.txt of the 'backup-….tar.*' images.");
    s.push('\n');
    line(&mut s, &format!("  Yedek / Backup        : {}", g.archive));
    line(&mut s, &format!("  Alındığı an / Created : {}   Makine / Host: {}", g.created, g.host));
    line(&mut s, &format!("  İçerik / Contents     : kullanıcı ayarları / user settings: {}, kullanıcı durumu / user state: {}, kök (sistem) dosyaları / root files: {}, sistem profili / system profile: {}",
                          g.user_config, g.user_state, g.system, g.system_profile));
    if g.profile_only {
        bi(&mut s, "Bu yedek yalnızca 'sistem profili'dir (referans): geri yüklenecek Centurion ayarı yoktur. Bölüm 4'e bakın.",
           "This backup is the 'system profile' only (reference): there are no Centurion settings to restore. See section 4.");
    }

    head(&mut s, "1. İÇİNE BAKMAK / LOOKING INSIDE");
    bi(&mut s, "Arşiv sıradan bir .tar.gz'dir. Önce neyin içinde olduğunu görün ve kendi künyesini (manifest) okuyun; hiçbir şey değiştirmez:",
       "The archive is an ordinary .tar.gz. First see what is in it and read its manifest; this changes nothing:");
    cmd(&mut s, &format!("tar -tzf {a} | less\ntar -xzOf {a} Centurion-BACKUP.json", a = sq(g.archive)));
    bi(&mut s, "Güvenlik için arşivi doğrudan '/' üzerine AÇMAYIN. Önce geçici bir klasöre açın, oradan seçerek kopyalayın:",
       "For safety do NOT extract the archive straight onto '/'. Extract it into a scratch folder first, and copy from there selectively:");
    cmd(&mut s, &format!("W=$(mktemp -d)\ntar -xzf {} -C \"$W\" --no-same-owner --no-same-permissions\nls \"$W\"       # user/  system/  system-profile/  Centurion-BACKUP.json", sq(g.archive)));
    bi(&mut s, "Arşivdeki bağlantılar (symlink) ve özel dosyalar güvenilmezdir; Centurion bunları reddeder. Elle yaparken de kopyalamadan önce şunun boş çıktığını doğrulayın:",
       "Links (symlinks) and special files in an archive are not trusted; Centurion refuses them. Doing it by hand, check that this prints nothing before copying:");
    cmd(&mut s, "find \"$W\" -mindepth 1 ! -type f ! -type d");

    head(&mut s, "2. KULLANICI AYARLARI (normal kullanıcı olarak) / USER SETTINGS (as your normal user)");
    if g.user_config == 0 { bi(&mut s, "Bu yedekte kullanıcı ayarı yok; bölümü atlayın.", "This backup holds no user settings; skip this section."); }
    bi(&mut s, "Yedektekiler aynı adlı dosyaların üzerine yazılır; yedekte olmayan dosyalar yerinde kalır. root olarak (sudo) ÇALIŞTIRMAYIN: $HOME değişir.",
       "Files in the backup overwrite files of the same name; files not in the backup stay. Do NOT run this as root (sudo): $HOME changes.");
    cmd(&mut s, "CFG=\"${XDG_CONFIG_HOME:-$HOME/.config}\"\nmkdir -p \"$CFG/centurion\" \"$CFG/ryzen-curve-optimizer\"\n[ -d \"$W/user/config/centurion\" ] && cp -r \"$W/user/config/centurion/.\" \"$CFG/centurion/\"\n[ -d \"$W/user/config/ryzen-curve-optimizer\" ] && cp -r \"$W/user/config/ryzen-curve-optimizer/.\" \"$CFG/ryzen-curve-optimizer/\"");
    bi(&mut s, "Sahne (scene) dosyalarındaki \"command\" alanı bir programı çalıştırır. Güvenmediğiniz arşivden geldiyse $CFG/centurion/scenes/*.json içinde\n\"command\" satırlarını kontrol edin/silin (Centurion geri yüklerken bunları kendisi siler).",
       "The \"command\" field in scene files runs a program. If the archive is not fully yours, check/delete the \"command\" lines in $CFG/centurion/scenes/*.json\n(Centurion removes them itself when restoring).");
    if g.user_state > 0 {
        bi(&mut s, "Günlükler ('user/state') isteğe bağlıdır, geri yüklemek şart değil:", "Logs ('user/state') are optional; restoring them is not required:");
        cmd(&mut s, "mkdir -p \"${XDG_STATE_HOME:-$HOME/.local/state}/centurion\"\ncp -r \"$W/user/state/centurion/.\" \"${XDG_STATE_HOME:-$HOME/.local/state}/centurion/\"");
    }

    head(&mut s, "3. KÖK (SİSTEM) AYARLARI (root olarak) / ROOT (SYSTEM) SETTINGS (as root)");
    if g.system == 0 {
        bi(&mut s, "Bu yedekte kök dosyası yok; bölümü atlayın.", "This backup holds no root files; skip this section.");
    } else {
        bi(&mut s, "Yalnızca aşağıdaki izinli dosyalar geri yüklenir; hepsi root:root 0644 olur (arşivdeki sahip/izin KULLANILMAZ). Başka hiçbir dosyayı sistem\ndizinlerine kopyalamayın. Aşağıda 'inst' bağlantı olmayan düzenli dosyaları kopyalar:",
           "Only the allowed files below are restored; all become root:root 0644 (the owner/mode in the archive are NOT used). Do not copy anything else into system\ndirectories. 'inst' below copies regular files only, never links:");
        let etc = ETC_RESTORE.join(" ");
        let var = VARLIB_RESTORE.join(" ");
        let mut c = String::from("# root kabuğunda W tanımsızsa, yukarıdaki geçici klasörün yolunu verin / if W is unset in the root shell, set it to the scratch folder above:\n# W=/tmp/tmp.XXXXXXXXXX\nS=\"$W/system\"\ninst() { [ -f \"$1\" ] && [ ! -L \"$1\" ] && install -m 644 -o root -g root \"$1\" \"$2\"; }\n");
        c.push_str("install -d -m 755 -o root -g root /etc/centurion /etc/centurion/presets /etc/nvcurve /etc/nvcurve/profiles /var/lib/centurion\n");
        c.push_str(&format!("for f in {etc}; do inst \"$S/etc/centurion/$f\" /etc/centurion/; done\n"));
        c.push_str("for f in \"$S\"/etc/centurion/presets/*.json; do inst \"$f\" /etc/centurion/presets/; done\n");
        c.push_str("inst \"$S/etc/nvcurve/config.json\" /etc/nvcurve/\n");
        c.push_str("for f in \"$S\"/etc/nvcurve/profiles/*; do inst \"$f\" /etc/nvcurve/profiles/; done\n");
        c.push_str(&format!("for f in {var}; do inst \"$S/var/lib/centurion/$f\" /var/lib/centurion/; done"));
        cmd(&mut s, &c);
        bi(&mut s, "NVIDIA modül ayarı (etc/modprobe.d/zz-centurion-nvidia.conf) KOPYALANMAZ: modprobe.d yapılandırması root olarak çalıştırılabilen komutlar\n('install' satırları) içerebilir. Dosyayı okuyun; yalnızca tanıdığınız 'options nvidia …' satırlarını /etc/modprobe.d/zz-centurion-nvidia.conf\ndosyasına kendiniz yazın ve initramfs'i yeniden üretin.",
           "The NVIDIA module options (etc/modprobe.d/zz-centurion-nvidia.conf) are NOT copied: modprobe.d configuration can run commands as root\n('install' lines). Read the file; write only the 'options nvidia …' lines you recognise into /etc/modprobe.d/zz-centurion-nvidia.conf\nyourself, then regenerate the initramfs.");
        bi(&mut s, "Bitince Centurion'ın arka plan hizmetlerini yeniden başlatın veya bilgisayarı yeniden açın (ağ koruması netguard.json'u yeniden okumalı).",
           "When done, restart Centurion's background services or reboot (the network guard must re-read netguard.json).");
    }

    head(&mut s, "4. SİSTEM PROFİLİ (yalnızca referans) / SYSTEM PROFILE (reference only)");
    if g.system_profile == 0 {
        bi(&mut s, "Bu yedekte sistem profili yok.", "This backup holds no system profile.");
    } else {
        bi(&mut s, "'system-profile' bu kurulumun nasıl yapıldığını anlatan REFERANS malzemedir (paket yöneticisi ayarları, çekirdek yapılandırması, fstab, önyükleme\nayarları, paket listeleri). Hiçbir zaman '/' üzerine geri yüklenmez; yenisiyle karşılaştırmak ve gerekeni elle almak içindir. 'root/' altı gerçek yolları yansıtır.",
           "'system-profile' is REFERENCE material about how this installation was put together (package manager settings, kernel config, fstab, boot settings,\npackage lists). It is never restored onto '/'; use it to compare with the new system and copy over by hand what you need. 'root/' mirrors the real paths.");
        cmd(&mut s, "ls \"$W/system-profile\"                       # packages.txt, packages-explicit.txt, uname.txt, dmi.txt, cmdline.txt …\ndiff -u \"$W/system-profile/root/etc/fstab\" /etc/fstab\nless \"$W/system-profile/packages-explicit.txt\"      # elle kurduğunuz paketler / the packages you asked for");
        if !g.profile_only { bi(&mut s, "Dikkat: profildeki dosyaları (ör. /etc/portage, /etc/pacman.conf) körlemesine kopyalamayın; yeni sistemin sürümleri farklı olabilir.",
                                "Careful: do not blindly copy files from the profile (e.g. /etc/portage, /etc/pacman.conf); the new system's versions may differ."); }
    }

    head(&mut s, "5. TEMİZLİK / CLEANUP");
    cmd(&mut s, "rm -rf \"$W\"");
    bi(&mut s, "Centurion çalışıyorsa aynı işlem: Health → Backups → bu yedeği seçip Restore (önce bir 'pre-restore' yedeği alır).",
       "With Centurion running it is the same job: Health → Backups → select this backup → Restore (it makes a 'pre-restore' backup first).");
    line(&mut s, "");
    line(&mut s, "(Eksik veya yanlış bir şey görürseniz bu kılavuzu düzeltin / If you find something missing or wrong, please amend this guide.)");
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(v: &[&str]) -> Vec<String> { v.iter().map(|s| s.to_string()).collect() }

    #[test]
    fn plain_full_image() {
        let steps = names(&["backup-2026-11-26.tar.zst"]);
        let t = image_guide(&Image { dir: "/mnt/bk disk", steps: &steps, crypt: Crypt::None, include_home: false, created: "x", host: "h" });
        assert!(t.contains("cd '/mnt/bk disk'"));
        assert!(t.contains("zstd -dc 'backup-2026-11-26.tar.zst' | tar --acls --xattrs --xattrs-include='*' --numeric-owner -xpf - -C \"$TARGET\""));
        assert!(!t.contains("age -d") && !t.contains("--listed-incremental=") && !t.contains("AGEKEY"));
        assert!(t.contains("mkdir -p \"$TARGET/proc\""));
    }

    #[test]
    fn encrypted_incremental_chain_in_order_and_mixed_compression() {
        let steps = names(&["backup-2026-11-20.tar.gz.age", "backup-2026-11-21_010203-i1.tar.xz.age", "backup-2026-11-22_010203-i2.tar.zst.age"]);
        let t = image_guide(&Image { dir: "/b", steps: &steps, crypt: Crypt::Key, include_home: true, created: "x", host: "h" });
        let r = &t[t.find("2. GERİ YÜKLEME").unwrap()..];
        let a = r.find("age -d -i \"$AGEKEY\" 'backup-2026-11-20.tar.gz.age' | gzip -dc | tar").unwrap();
        let b = r.find("age -d -i \"$AGEKEY\" 'backup-2026-11-21_010203-i1.tar.xz.age' | xz -dc | tar").unwrap();
        let c = r.find("age -d -i \"$AGEKEY\" 'backup-2026-11-22_010203-i2.tar.zst.age' | zstd -dc | tar").unwrap();
        assert!(a < b && b < c);
        assert!(t.contains("AGEKEY=") && t.contains("NOT use tar's own --listed-incremental"));
        assert!(t.contains("Tools: tar (GNU tar) and gzip, xz, zstd") && t.contains("ALL must be installed"));
    }

    #[test]
    fn passphrase_image_asks_on_the_terminal() {
        let steps = names(&["backup-2026-11-26.tar.xz.age"]);
        let t = image_guide(&Image { dir: "/b", steps: &steps, crypt: Crypt::Passphrase, include_home: false, created: "x", host: "h" });
        assert!(t.contains("age -d 'backup-2026-11-26.tar.xz.age' | xz -dc | tar") && !t.contains("AGEKEY"));
    }

    #[test]
    fn quotes_in_names_are_escaped() {
        assert_eq!(sq("a'b"), r"'a'\''b'");
    }

    #[test]
    fn config_guide_lists_the_whitelist() {
        let t = config_guide(&Config { archive: "/x/centurion-config-1.tar.gz", created: "x", host: "h", profile_only: false,
                                       user_config: 3, user_state: 0, system: 2, system_profile: 0 });
        for f in ETC_RESTORE.iter().chain(VARLIB_RESTORE.iter()) { assert!(t.contains(f), "{f}"); }
        assert!(t.contains("tar -xzf '/x/centurion-config-1.tar.gz'") && t.contains("NOT copied"));
    }
}
