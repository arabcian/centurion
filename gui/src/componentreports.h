#pragma once
// Diagnostics → This Boot, the components that have no event kind of their own (H19–H26): kernel, storage,
// AMD / Intel GPUs, power and thermal, memory, USB, audio, firmware and drivers. Each report reads the
// health-reader "diagnostics" scan's "components" (this boot's kernel log, grouped by component) and the
// sysfs / procfs state that explains it. Pure functions over their inputs (tests use fixtures); the *State()
// functions read the running system.
#include "diagreport.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace componentreports {

// H19 Kernel: lockups, oops / BUG / warnings, hung tasks, and the kernel's taint flags.
struct KernelState { qint64 taint = -1; };
KernelState kernelState();
diag::Report kernel(const QJsonObject &scan, const KernelState &state);

// H20 Storage: NVMe, SATA, SCSI, block-layer I/O errors, software RAID, device mapper, SD / eMMC.
struct Disk { QString name, model; qint64 bytes = -1, ioErrors = -1; bool readOnly = false; };
struct StorageState { QList<Disk> disks; QString mdstat; };
StorageState storageState();
diag::Report storage(const QJsonObject &scan, const StorageState &state);

// H21 GPU (AMD / Intel / nouveau): hangs, resets, page faults, DRM driver errors.
struct DrmCard { QString card, driver, pci, name; };
QList<DrmCard> drmCards();
diag::Report gpu(const QJsonObject &scan, const QList<DrmCard> &cards);

// H22 Thermal & Power: critical temperatures, throttling, ACPI firmware errors, devices that did not wake,
// failed suspend steps.
struct ThermalZone { QString name, type; double tempC = -1000, criticalC = -1000; };
struct PowerState { QList<ThermalZone> zones; qint64 coreThrottle = -1, packageThrottle = -1; };
PowerState powerState();
diag::Report power(const QJsonObject &scan, const PowerState &state);

// H23 Memory: out-of-memory kills, allocation failures, ECC (EDAC) errors, pages taken offline.
struct EdacController { QString name; qint64 ce = -1, ue = -1; };
struct MemoryState { QList<EdacController> edac; qint64 hardwareCorruptedKib = -1, oomKills = -1; };
MemoryState memoryState();
diag::Report memory(const QJsonObject &scan, const MemoryState &state);

// H24 USB: devices that fail to enumerate, over-current, host controllers that die, USB-C (UCSI) errors.
diag::Report usb(const QJsonObject &scan);

// H25 Audio: codec timeouts, DSP firmware failures. `cards`: /proc/asound/cards.
diag::Report audio(const QJsonObject &scan, const QString &cards);

// H26 Firmware & Drivers: missing firmware files, drivers that fail to probe, modules built for another kernel.
diag::Report firmware(const QJsonObject &scan);

}  // namespace componentreports
