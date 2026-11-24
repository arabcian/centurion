#pragma once
#include <QComboBox>
#include <QHash>
#include <QStringList>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>
#include <functional>
#include <optional>
#include <algorithm>

namespace profilediff {
inline QString tr(const char *s) { return QCoreApplication::translate("profilediff", s); }
inline std::optional<QJsonObject> read(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024) return std::nullopt;
    const auto d = QJsonDocument::fromJson(f.read(256 * 1024 + 1));
    return d.isObject() ? std::optional<QJsonObject>(d.object()) : std::nullopt;
}
inline QString fieldLabel(QString key) {
    static const QHash<QString, QString> labels{{"coall", tr("All-core CO")}, {"coper", tr("Curve offset")}, {"cores", tr("Physical cores")},
        {"firmware", tr("Firmware limits")}, {"ppt_pl1_spl", tr("CPU PL1 (W)")}, {"ppt_pl2_sppt", tr("CPU PL2 (W)")}, {"ppt_pl3_fppt", tr("CPU PL3 (W)")},
        {"ppt_cpu_cl", tr("CPU cross-load limit (W)")}, {"cpu_temp", tr("CPU temperature (°C)")}, {"gpu_temp", tr("GPU temperature (°C)")},
        {"gpu_nv_ctgp", tr("GPU cTGP (W)")}, {"gpu_nv_ppab", tr("GPU Dynamic Boost ceiling (W)")}, {"gpu_nv_cpu_boost", tr("GPU Dynamic Boost floor (W)")},
        {"fan_table", tr("Fan curve")}, {"fan_fullspeed", tr("Full-speed fans")}, {"cx", tr("Context ID")}, {"sess", tr("Session ID")},
        {"refs", tr("Reference runs")}, {"runs", tr("Runs")}, {"weak", tr("Weak measurements")}, {"dropped", tr("Dropped measurements")},
        {"refused", tr("Refused measurements")}, {"noise", tr("Noise (recorded objective order)")}, {"ended_early", tr("Ended early")}, {"power_waits", tr("Power waits")}};
    return labels.value(key, key);
}
inline QString display(const QJsonValue &v, int depth = 0) {
    if (v.isUndefined()) return tr("Not specified");
    if (v.isNull()) return tr("Unknown");
    if (v.isString()) return v.toString().size()>65536 ? v.toString().left(65536)+QStringLiteral(" …") : v.toString();
    if (v.isBool()) return v.toBool() ? tr("Yes") : tr("No");
    if (v.isDouble()) { const auto b = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact); return QString::fromUtf8(b.mid(1,b.size()-2)); }
    if (depth >= 12) return tr("More nested data in exported snapshot");
    QStringList lines;
    qsizetype chars=0;
    if (v.isObject()) {
        const auto o = v.toObject();
        for (auto it=o.constBegin();it!=o.constEnd() && lines.size()<2000 && chars<65536;++it) {
            lines << fieldLabel(it.key()) + QStringLiteral(": ") + display(it.value(),depth+1).replace(QLatin1Char('\n'),QStringLiteral("\n    ")); chars+=lines.last().size();
        }
        if (o.size()>lines.size()) lines << tr("Additional entries omitted from view");
    } else if (v.isArray()) {
        const auto a=v.toArray();
        for(qsizetype i=0;i<a.size() && i<2000 && chars<65536;++i) { lines << QStringLiteral("%1. %2").arg(i+1).arg(display(a.at(i),depth+1).replace(QLatin1Char('\n'),QStringLiteral("\n    "))); chars+=lines.last().size(); }
        if(a.size()>lines.size()) lines << tr("Additional entries omitted from view");
    }
    return lines.isEmpty() ? tr("Empty") : lines.join(QLatin1Char('\n'));
}
inline void show(QWidget *parent, const QStringList &names, const std::function<std::optional<QJsonObject>(const QString &)> &load, const QString &extraHint = {}) {
    QDialog dlg(parent); dlg.setWindowTitle(tr("Profile Comparison")); dlg.resize(900,550);
    auto *layout = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(tr("Saved configuration comparison only. Missing values are not interpreted as zero or reset. No hardware writes.") + (extraHint.isEmpty()?QString():QStringLiteral("\n") + extraHint));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *a = new QComboBox; auto *b = new QComboBox; a->addItems(names); b->addItems(names);
    if (b->count() > 1) b->setCurrentIndex(1);
    auto *row = new QHBoxLayout; row->addWidget(a,1); row->addWidget(b,1); layout->addLayout(row);
    auto *table = new QTableWidget(0,3); table->setHorizontalHeaderLabels({tr("Field"),tr("Left"),tr("Right")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->verticalHeader()->hide(); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); layout->addWidget(table,1);
    auto *status = new QLabel; status->setWordWrap(true); layout->addWidget(status);
    auto compare = [a,b,table,status,load] {
        table->setRowCount(0);
        if (a->currentIndex()<0 || b->currentIndex()<0) { status->setText(tr("Two readable snapshots are needed.")); return; }
        const auto left=load(a->currentText()), right=load(b->currentText());
        if (!left || !right) { status->setText(tr("A snapshot could not be read.")); return; }
        // JSON objects are expanded by key; arrays remain atomic (indices need not be stable identities).
        int changes=0, omitted=0;
        std::function<void(const QString&, const QJsonValue&, const QJsonValue&,int)> walk;
        walk = [&](const QString &path,const QJsonValue &x,const QJsonValue &y,int depth) {
            if (x==y) return;
            if (depth<16 && (x.isObject() || y.isObject()) && (x.isObject() || x.isUndefined()) && (y.isObject() || y.isUndefined())) {
                const auto xo=x.toObject(), yo=y.toObject(); QSet<QString> keys;
                for(auto it=xo.begin();it!=xo.end();++it) keys.insert(it.key());
                for(auto it=yo.begin();it!=yo.end();++it) keys.insert(it.key());
                auto sorted=keys.values(); std::sort(sorted.begin(),sorted.end());
                for(const auto &k:sorted) walk(path.isEmpty()?k:path+QStringLiteral(" / ")+k,xo.value(k),yo.value(k),depth+1);
                if (!keys.isEmpty()) return;
            }
            ++changes;
            if(table->rowCount()>=5000) { ++omitted; return; }
            int r=table->rowCount(); table->insertRow(r);
            for (const auto &cell : {qMakePair(0,[&]{ QStringList labels; for(const auto &key:path.split(QStringLiteral(" / "))) labels << fieldLabel(key); return labels.join(QStringLiteral(" › ")); }()),qMakePair(1,display(x)),qMakePair(2,display(y))}) {
                auto *item=new QTableWidgetItem(cell.second); item->setToolTip(cell.first==0 ? path : cell.second); if(cell.second.size()>2048) item->setText(cell.second.left(2048)+QStringLiteral(" …")); table->setItem(r,cell.first,item);
            }
        };
        walk(QString(),*left,*right,0);
        table->resizeRowsToContents();
        status->setText(tr("%1 differing fields; %2 omitted by the display limit.").arg(changes).arg(omitted));
    };
    QObject::connect(a,&QComboBox::currentIndexChanged,&dlg,compare); QObject::connect(b,&QComboBox::currentIndexChanged,&dlg,compare); compare();
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); layout->addWidget(buttons); dlg.exec();
}
}
