// Native Qt fixture: isolated config; no MainWindow, helper or hardware access.
#include "../src/workspacestate.h"
#include <QApplication>
#include <QTemporaryDir>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"Failed: %s\n",#x); return 1; } } while(false)
class TestSplitter final : public QSplitter {
public:
    TestSplitter() {
        addWidget(new QWidget);
        addWidget(new QWidget);
        resize(800, 200);
    }
    using QSplitter::moveSplitter;
};

int main(int argc,char **argv) {
    QTemporaryDir temp; CHECK(temp.isValid());
    qputenv("XDG_CONFIG_HOME",temp.path().toUtf8());
    QApplication app(argc,argv);
    CHECK(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)==temp.path());
    const auto classic=workspace::key(false,QStringLiteral("test")), experimental=workspace::key(true,QStringLiteral("test"));
    CHECK(classic!=experimental);
    workspace::write(classic,QJsonObject{{"widths",QJsonArray{100,200,300}},{"order",QJsonArray{2,0,1}},{"sort",1},{"descending",true}});
    CHECK(workspace::read(experimental).isEmpty());
    QTreeWidget tree; tree.setColumnCount(3); tree.header()->setStretchLastSection(false);
    workspace::restoreHeader(&tree,workspace::read(classic),true);
    CHECK(tree.header()->logicalIndex(0)==2); CHECK(tree.columnWidth(0)==100);
    CHECK(tree.isSortingEnabled() && tree.sortColumn()==1 && tree.header()->sortIndicatorOrder()==Qt::DescendingOrder);
    const auto before=workspace::captureHeader(&tree);
    workspace::restoreHeader(&tree,QJsonObject{{"widths",QJsonArray{100,-1,300}},{"order",QJsonArray{2,2,1}},{"sort",8},{"descending",false}},true);
    CHECK(workspace::captureHeader(&tree)==before);
    CHECK(workspace::integers(QJsonArray{1,2.5}).empty());
    workspace::settings().setValue(experimental,QByteArray("{invalid")); CHECK(workspace::read(experimental).isEmpty());
    workspace::settings().setValue(experimental,QByteArray(32769,'a')); CHECK(workspace::read(experimental).isEmpty());
    const auto immediate=workspace::key(false,QStringLiteral("immediate"));
    {
        QTreeWidget closing; closing.setColumnCount(2); closing.header()->setStretchLastSection(false);
        workspace::rememberHeader(&closing,immediate,true);
        closing.setColumnWidth(0,222); closing.header()->moveSection(0,1);
    }
    const auto persisted=workspace::read(immediate);
    CHECK(persisted.value("widths").toArray().at(0).toInt()==222);
    CHECK(persisted.value("order").toArray().at(0).toInt()==1);
    const auto classicSplit = workspace::key(false, QStringLiteral("health/incidents/splitter"));
    const auto experimentalSplit = workspace::key(true, QStringLiteral("health/incidents/splitter"));
    QList<int> moved;
    {
        TestSplitter first;
        workspace::rememberSplitter(&first, classicSplit);
        first.show();
        first.moveSplitter(220, 1);
        moved = first.sizes();
        CHECK(moved.at(0) == 220 && moved.at(1) > moved.at(0));
    }
    CHECK(workspace::read(experimentalSplit).isEmpty());
    TestSplitter reopened;
    workspace::rememberSplitter(&reopened, classicSplit);
    reopened.show();
    CHECK(reopened.sizes() == moved);

    workspace::write(experimentalSplit, QJsonObject{{"sizes", QJsonArray{0, 800}}});
    TestSplitter invalid;
    invalid.setSizes({400, 400});
    workspace::rememberSplitter(&invalid, experimentalSplit);
    invalid.show();
    CHECK(invalid.sizes().at(0) == invalid.sizes().at(1));
    constexpr int recordIdentityRole = Qt::UserRole + 1;
    QTreeWidget history;
    auto *group = new QTreeWidgetItem(&history, QStringList{QStringLiteral("boot")});
    auto *savedItem = new QTreeWidgetItem(group, QStringList{QStringLiteral("record")});
    savedItem->setData(0, recordIdentityRole, QStringLiteral("event/abc"));
    auto *newItem = new QTreeWidgetItem(QStringList{QStringLiteral("newer")});
    group->insertChild(0, newItem);
    newItem->setData(0, recordIdentityRole, QStringLiteral("event/def"));
    workspace::restoreSelection(&history, QStringLiteral("event/abc"), recordIdentityRole);
    CHECK(history.currentItem() == savedItem);
    history.setCurrentItem(nullptr);
    group->setHidden(true);
    workspace::restoreSelection(&history, QStringLiteral("event/abc"), recordIdentityRole);
    CHECK(!history.currentItem());
    group->setHidden(false);
    workspace::restoreSelection(&history, QStringLiteral("event/gone"), recordIdentityRole);
    CHECK(!history.currentItem());
    newItem->setData(0, recordIdentityRole, QStringLiteral("event/abc"));
    workspace::restoreSelection(&history, QStringLiteral("event/abc"), recordIdentityRole);
    CHECK(!history.currentItem());
    return 0;
}
