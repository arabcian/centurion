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
    return 0;
}
