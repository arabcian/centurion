// Native Qt fixture: isolated config; no MainWindow, helper or hardware access.
#include "../src/workspacestate.h"
#include <QApplication>
#include <QTemporaryDir>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"Failed: %s\n",#x); return 1; } } while(false)
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
    return 0;
}
