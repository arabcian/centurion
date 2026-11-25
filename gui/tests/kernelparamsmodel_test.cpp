#include "../src/kernelparamsmodel.h"
#include <QCoreApplication>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <cstdlib>
#include <iostream>
#define CHECK(x) do {if(!(x)){std::cerr<<"line "<<__LINE__<<" failed\n";return EXIT_FAILURE;}}while(false)
static QJsonObject row(int n,QString value=QStringLiteral("old")) {
    return {{"p",QStringLiteral("/sys/test/%1").arg(n,5,10,QLatin1Char('0'))},{"v",value},{"w",false}};
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);KernelParamsModel m;QSortFilterProxyModel proxy;proxy.setSourceModel(&m);proxy.setFilterRole(KernelParamsModel::SearchRole);proxy.setFilterKeyColumn(0);
    QJsonArray rows;for(int i=0;i<10000;++i) rows.append(row(i));
    CHECK(m.replace(rows));CHECK(m.rowCount()==10000);
    int resets=0,changed=0,inserted=0,removed=0;
    QObject::connect(&m,&QAbstractItemModel::modelReset,&m,[&]{++resets;});
    QObject::connect(&m,&QAbstractItemModel::dataChanged,&m,[&](const QModelIndex&a,const QModelIndex&b){changed+=b.row()-a.row()+1;});
    QObject::connect(&m,&QAbstractItemModel::rowsInserted,&m,[&](const QModelIndex&,int a,int b){inserted+=b-a+1;});
    QObject::connect(&m,&QAbstractItemModel::rowsRemoved,&m,[&](const QModelIndex&,int a,int b){removed+=b-a+1;});
    QPersistentModelIndex keep=m.byPath("/sys/test/00023");
    CHECK(m.replace(rows));CHECK(!changed&&!resets&&!inserted&&!removed);
    rows.replace(23,row(23,QStringLiteral("first\nsecond")));
    CHECK(m.replace(rows));CHECK(changed==1&&!resets&&!inserted&&!removed&&keep.isValid());
    CHECK(QModelIndex(keep).siblingAtColumn(1).data(Qt::ToolTipRole).toString()==QStringLiteral("first\nsecond"));
    proxy.setFilterFixedString(QStringLiteral("second"));CHECK(proxy.rowCount()==1);
    rows.removeAt(0);rows.append(row(10000));CHECK(m.replace(rows));CHECK(keep.isValid()&&inserted==1&&removed==1&&!resets);
    rows.append(row(23));CHECK(!m.replace(rows));CHECK(m.rowCount()==10000&&keep.isValid());
    // The knob column: exact files, per-policy wildcards, every feature bit of sched/features, nothing for unknown files.
    CHECK(knobsForPath(QStringLiteral("/sys/kernel/debug/sched/base_slice_ns"))==QStringList{QStringLiteral("sched.base_slice_ns")});
    CHECK(knobsForPath(QStringLiteral("/sys/devices/system/cpu/cpufreq/policy12/scaling_governor")).contains(QStringLiteral("cpu.governor")));
    CHECK(knobsForPath(QStringLiteral("/sys/devices/system/cpu/cpufreq/policy12/x/scaling_governor")).isEmpty());
    CHECK(knobsForPath(QStringLiteral("/sys/kernel/debug/sched/features")).size()>20);
    CHECK(knobsForPath(QStringLiteral("/sys/kernel/debug/sched/verbose")).isEmpty());
    return EXIT_SUCCESS;
}
