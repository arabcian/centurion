#pragma once
// UI preferences only. No helper invocation, tuning writes or opaque Qt state blobs.
#include "workspacefields.h"
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QSplitter>
#include <QTimer>
#include <QVariant>
#include <cmath>
namespace workspace {
inline QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}
inline QString key(bool experimental, const QString &view) {
    return QStringLiteral("workspace/v1/") + (experimental ? QStringLiteral("experimental/") : QStringLiteral("classic/")) + view;
}
inline QJsonObject read(const QString &path) {
    const QByteArray bytes = settings().value(path).toByteArray();
    if (bytes.size() > 32768) return {};
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}
inline void write(const QString &path, const QJsonObject &obj) {
    const auto bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (bytes.size() <= 32768) settings().setValue(path, bytes);
}
inline std::vector<int> integers(const QJsonValue &value) {
    std::vector<int> result;
    const auto array = value.toArray();
    if (array.size() > 128) return {};
    for (const auto &v : array) {
        const double d = v.toDouble(-1);
        if (!v.isDouble() || !std::isfinite(d) || d < 0 || d > 100000 || std::floor(d) != d) return {};
        result.push_back(static_cast<int>(d));
    }
    return result;
}
inline QJsonObject captureHeader(const QTreeWidget *tree) {
    QJsonArray widths, order;
    for (int c = 0; c < tree->columnCount(); ++c) {
        widths.append(tree->columnWidth(c)); order.append(tree->header()->logicalIndex(c));
    }
    QJsonObject out{{"widths", widths}, {"order", order}};
    if (tree->isSortingEnabled()) {
        out.insert("sort", tree->header()->sortIndicatorSection());
        out.insert("descending", tree->header()->sortIndicatorOrder() == Qt::DescendingOrder);
    }
    return out;
}
inline void restoreHeader(QTreeWidget *tree, const QJsonObject &obj, bool allowSort) {
    auto *header = tree->header(); const int count = tree->columnCount();
    const auto order = integers(obj.value("order")), widths = integers(obj.value("widths"));
    if (workspacefields::permutation(order, count))
        for (int i = 0; i < count; ++i) header->moveSection(header->visualIndex(order[i]), i);
    if (workspacefields::widths(widths, count, header->minimumSectionSize()))
        for (int i = 0; i < count; ++i) tree->setColumnWidth(i, widths[i]);
    const auto column = obj.value("sort"); const int c = column.toInt(-1);
    if (allowSort && c >= 0 && c < count && obj.value("descending").isBool()) {
        tree->setSortingEnabled(true);
        tree->sortByColumn(c, obj.value("descending").toBool() ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
}
inline void rememberHeader(QTreeWidget *tree, const QString &path, bool allowSort, const QVariantList &legacy = {}) {
    auto obj = read(path);
    if (obj.isEmpty() && legacy.size() == tree->columnCount()) {
        QJsonArray widths; for (const auto &v : legacy) { bool ok = false; const int n = v.toInt(&ok); widths.append(ok ? n : -1); }
        obj.insert("widths", widths);
    }
    restoreHeader(tree, obj, allowSort);
    tree->header()->setSectionsMovable(true);
    auto *timer = new QTimer(tree); timer->setSingleShot(true); timer->setInterval(350);
    QObject::connect(tree->header(), &QHeaderView::sectionResized, timer, [timer]{timer->start();});
    QObject::connect(tree->header(), &QHeaderView::sectionMoved, timer, [timer]{timer->start();});
    if (allowSort) QObject::connect(tree->header(), &QHeaderView::sortIndicatorChanged, timer, [timer]{timer->start();});
    QObject::connect(timer, &QTimer::timeout, tree, [tree,path,allowSort]{
        auto state = captureHeader(tree); if (!allowSort) {state.remove("sort"); state.remove("descending");}
        write(path,state);
    });
}
inline void rememberSplitter(QSplitter *splitter, const QString &path) {
    const auto saved = integers(read(path).value("sizes"));
    splitter->setChildrenCollapsible(false);
    if (workspacefields::split(saved, splitter->count())) {
        QList<int> sizes; for (int v : saved) sizes.append(v); splitter->setSizes(sizes);
    }
    QObject::connect(splitter,&QSplitter::splitterMoved,splitter,[splitter,path]{
        QJsonArray sizes; for (int n : splitter->sizes()) sizes.append(n);
        if (workspacefields::split(integers(sizes),splitter->count())) write(path,QJsonObject{{"sizes",sizes}});
    });
}
}
