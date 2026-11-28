#pragma once
// Which raw /dev/kmsg records ("pri,seq,ts,flags;message") start a scan of the kernel log.
// Shared by Health (classified events) and Health → Kernel log (saved log), so both wake for the same lines.
// Must cover every kind health.rs::classify knows: a line that is not listed here never starts a scan
// (health.rs' test `gui_wakes_for_every_classified_line` reads this list and checks it).
// ";xe " / ";BUG:" ... are anchored at the start of the message (the record is "pri,seq,ts,flags;message").
#include <cstring>

namespace kmsgwake {

inline bool wakes(const char *record) {
    static const char *const KEYS[] = {"NVRM", "Hardware Error", "achine check", "achine Check", "AER",
                                       "PCIe Bus Error", "lockup", "LOCKUP", "stall", "D3cold", "amdgpu",
                                       ";nvme nvme", "EXT4-fs error", "BTRFS error", "csum failed", ";XFS",
                                       "Remounting filesystem read-only", "Aborting journal", "JBD2:", "EXT2-fs error", "EXT3-fs error",
                                       "BTRFS critical", "BTRFS: error", "FAT-fs", "UDF-fs", "NILFS error", "UBIFS error", "SQUASHFS error",
                                       "ntfs3", "bcachefs", "GPU HANG", ";xe ",
                                       "Microcode SW error", "mt79", "mt792", "firmware crashed", "firmware timeout", "Bluetooth:", "btusb", "btmtk", "F2FS-fs", "f2fs:", "critical temperature",
                                       "Package temperature above", "Out of memory", ";Oops:", ";BUG:", ";WARNING: CPU:"};
    for (const char *k : KEYS) if (std::strstr(record, k)) return true;
    return false;
}

}  // namespace kmsgwake
