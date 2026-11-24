#pragma once
// ACPI events from the kernel's generic-netlink family "acpi_event" (multicast group "acpi_mc_group"),
// the stream acpid reads, without acpid. On Legion the WMI notifications arrive here too (16AFR10H:
// Fn+Space → d0 + e8, charger → e5/e7/e9, Fn+Q → e3/e7; data is always 0, so the state must be re-read).
// AC and platform-profile changes already have event paths of their own (power_supply uevents,
// sysfs_notify); this is for what has none: the Spectrum brightness Fn+Space cycles in the EC.
#include <QObject>
#include "acpinetlink.h"

class AcpiEvents : public QObject {
    Q_OBJECT
public:
    /// Subscribes at once; without the family (no ACPI, old kernel) it stays inactive and never emits.
    explicit AcpiEvents(QObject *parent = nullptr);
    bool active() const { return fd_ >= 0; }

Q_SIGNALS:
    void event(const QString &deviceClass, const QString &busId, uint type, uint data);
    // Reports an observed overflow or rejected/truncated datagram, not an event count.
    void captureGap();
    void notification(const QString &deviceClass, const QString &busId, uint type, uint data, uint sequence);

private:
    void drain();
    int fd_ = -1;
    uint16_t family_ = 0;
};
