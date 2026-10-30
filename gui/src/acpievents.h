#pragma once
// ACPI events from the kernel's generic-netlink family "acpi_event" (multicast group "acpi_mc_group"),
// the stream acpid reads, without acpid. On Legion the WMI notifications arrive here too (16AFR10H:
// Fn+Space → d0 + e8, charger → e5/e7/e9, Fn+Q → e3/e7; data is always 0, so the state must be re-read).
// AC and platform-profile changes already have event paths of their own (power_supply uevents,
// sysfs_notify); this is for what has none: the Spectrum brightness Fn+Space cycles in the EC.
#include <QObject>
#include <cstdint>
#include <string>
#include <vector>

namespace acpievents {
struct Event { std::string deviceClass, busId; uint32_t type = 0, data = 0; };
/// The events in one recv() buffer, from messages of genetlink family `family` (others skipped).
std::vector<Event> parse(const void *buf, size_t len, uint16_t family);
} // namespace acpievents

class AcpiEvents : public QObject {
    Q_OBJECT
public:
    /// Subscribes at once; without the family (no ACPI, old kernel) it stays inactive and never emits.
    explicit AcpiEvents(QObject *parent = nullptr);
    bool active() const { return fd_ >= 0; }

Q_SIGNALS:
    void event(const QString &deviceClass, const QString &busId, uint type, uint data);

private:
    void drain();
    int fd_ = -1;
    uint16_t family_ = 0;
};
