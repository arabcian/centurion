#pragma once
// Diagnostics → This Boot (H02, H03, H04, H05, H09): reports over one health-reader "diagnostics" scan of
// this boot's kernel log, plus the sysfs state that explains it. Pure functions over their inputs (tests use
// fixtures); the page gathers the inputs.
#include "diagreport.h"
#include "diagsources.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

namespace evidencereports {

/// Where a logical CPU sits for the Curve Optimizer (Ryzen tab: CCD and core slot), and its CCD's L3.
struct CoreRef { int ccd = -1, slot = -1, l3Kib = 0; };

diag::Report mceAtlas(const QJsonObject &scan, const QHash<int, CoreRef> &cores);
/// `previous`: the last saved sample ({"boot_id", "taken_at", "counters"}); compared only within one boot.
diag::Report aerDelta(const QJsonObject &scan, const QJsonObject &previous);
/// The sample to save for the next comparison.
QJsonObject aerSample(const QJsonObject &scan);
diag::Report xidCases(const QJsonObject &scan);
/// One mounted disk file system: every mount point of the device, its kernel device name ("sda1", "dm-0"), the
/// error state the file system keeps in sysfs / procfs (f2fs flags, ext4 error count, btrfs device stats, ZFS
/// pool state) and its size in bytes (-1 = unknown).
struct FsVolume { QString source, fstype, options, dev; QStringList mounts; QHash<QString, QString> sys; qint64 total = -1, avail = -1; };
QList<FsVolume> fsVolumes(const QJsonObject &scan);
diag::Report fsGuard(const QJsonObject &scan, const QList<FsVolume> &volumes, const QList<diagsrc::LogLine> &history);
diag::Report radioLedger(const QJsonObject &scan, const QList<diagsrc::LogLine> &history);

}  // namespace evidencereports
