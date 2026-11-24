#include "../src/structuredtools.h"
#include <QCoreApplication>
#include <cstdio>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); return 1; } } while(false)
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    auto m = structuredtools::modules(QStringLiteral("$ uname -a\nLinux\n$ lsmod\nModule Size Used by\nnvidia 12345678900 2 nvidia_uvm,nvidia_drm\nfuse 32768 0\n"));
    CHECK(m && m->rows.size() == 2 && m->rows.first().at(1) == QStringLiteral("12345678900"));
    CHECK(!structuredtools::modules(QStringLiteral("$ lsmod\nModule Size Used by\nfuse broken 0\n")));
    CHECK(!structuredtools::modules(QStringLiteral("$ lsmod\nModul Größe\nfuse 100 0\n")));
    CHECK(!structuredtools::modules(QStringLiteral("$ lsmod\nModule Size Used by\nfuse 10 0\n(command failed or output was truncated)\n")));
    const auto p = structuredtools::powerSupply(QStringLiteral("$ cat /sys/class/power_supply/BAT0/uevent\nPOWER_SUPPLY_STATUS=Discharging\nPOWER_SUPPLY_SERIAL_NUMBER=secret\nPOWER_SUPPLY_CURRENT_NOW=-1200\n$ cat /sys/class/power_supply/AC/uevent\nPOWER_SUPPLY_ONLINE=0\n"));
    CHECK(p && p->rows.size() == 3 && p->rows.at(1).at(2) == QStringLiteral("-1200"));
    CHECK(!structuredtools::powerSupply(QStringLiteral("$ cat /sys/class/power_supply/BAT0/uevent\nPOWER_SUPPLY_CAPACITY=12\nPOWER_SUPPLY_CAPACITY=13\n")));
    CHECK(!structuredtools::powerSupply(QStringLiteral("$ cat /sys/class/power_supply/BAT0/uevent\n(cannot read)\n")));
    const auto usb = structuredtools::usb(QStringLiteral("$ lsusb\nBus 001 Device 003: ID 1234:abcd a device\n$ lsusb -t\nanything\n"));
    CHECK(usb && usb->rows.size() == 1 && usb->rows.first().at(3) == QStringLiteral("abcd"));
    CHECK(!structuredtools::usb(QStringLiteral("$ lsusb\nBus 001 Device 003: ID 1234:abcd\n(truncated)\n")));
    CHECK(!structuredtools::usb(QStringLiteral("$ lsusb\nBus 001 Device 003: ID 1234:abcd\nBus 001 Device 003: ID 1234:abcd\n")));
    const auto cpu = structuredtools::cpuTopology(QStringLiteral("$ lscpu\nCPU info\n$ lscpu -e\nCPU NODE SOCKET CORE ONLINE MAXMHZ\n0 0 0 0 yes 4000\n2 - 0 1 no -\n$ cpupower frequency-info\n"));
    CHECK(cpu && cpu->rows.size() == 2 && cpu->rows.at(1).at(1) == QStringLiteral("-"));
    CHECK(!structuredtools::cpuTopology(QStringLiteral("$ lscpu -e\nCPU CORE ONLINE\n0 1 maybe\n")));
    CHECK(!structuredtools::cpuTopology(QStringLiteral("$ lscpu -e\nCPU CORE ONLINE\n0 1 yes\n0 1 no\n")));
    CHECK(!structuredtools::cpuTopology(QStringLiteral("$ lscpu -e\nCPU CORE ONLINE\n0 1 yes\n(command failed or output was truncated)\n")));
    return 0;
}
