#pragma once
#include <QComboBox>
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
inline QString display(const QJsonValue &v) {
    if (v.isUndefined()) return tr("Not specified");
    if (v.isString()) return v.toString();
    const auto b = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(b.mid(1, b.size()-2));
}
inline void show(QWidget *parent, const QStringList &names, const std::function<std::optional<QJsonObject>(const QString &)> &load) {
    QDialog dlg(parent); dlg.setWindowTitle(tr("Profile Comparison")); dlg.resize(900,550);
    auto *layout = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(tr("Saved configuration comparison only. Missing values are not interpreted as zero or reset. No hardware writes."));
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
            if (depth<16 && x.isObject() && y.isObject()) {
                const auto xo=x.toObject(), yo=y.toObject(); QSet<QString> keys;
                for(auto it=xo.begin();it!=xo.end();++it) keys.insert(it.key());
                for(auto it=yo.begin();it!=yo.end();++it) keys.insert(it.key());
                auto sorted=keys.values(); std::sort(sorted.begin(),sorted.end());
                for(const auto &k:sorted) walk(path+QStringLiteral("/")+k,xo.value(k),yo.value(k),depth+1);
                return;
            }
            ++changes;
            if(table->rowCount()>=5000) { ++omitted; return; }
            int r=table->rowCount(); table->insertRow(r);
            for (const auto &cell : {qMakePair(0,path),qMakePair(1,display(x)),qMakePair(2,display(y))}) {
                auto *item=new QTableWidgetItem(cell.second); item->setToolTip(cell.second); table->setItem(r,cell.first,item);
            }
        };
        walk(QString(),*left,*right,0);
        status->setText(tr("%1 differing fields; %2 omitted by the display limit.").arg(changes).arg(omitted));
    };
    QObject::connect(a,&QComboBox::currentIndexChanged,&dlg,compare); QObject::connect(b,&QComboBox::currentIndexChanged,&dlg,compare); compare();
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); layout->addWidget(buttons); dlg.exec();
}
}
