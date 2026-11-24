#pragma once
#include "diagreport.h"
#include <QAbstractItemModel>
#include <QHash>
#include <utility>

// Frozen report model. Only index vectors are built eagerly; text is retrieved
// by the view on demand. No hardware callbacks and no mutable report reference.
class HierarchyViewModel final : public QAbstractItemModel {
public:
    enum { KeyRole = Qt::UserRole + 1 };
    explicit HierarchyViewModel(diag::Table table, QObject *parent = nullptr)
        : QAbstractItemModel(parent), table_(std::move(table)), root_(int(table_.rows.size())), children_(root_ + 1), parents_(root_), positions_(root_) {
        for (int i = 0; i < root_; ++i) {
            const int p = table_.parents.value(i, -1);
            parents_[i] = p >= 0 && p < i ? p : root_;
            auto &siblings = children_[parents_[i]];
            positions_[i] = int(siblings.size()); siblings << i;
            const QString key = table_.rowKeys.value(i);
            if (!key.isEmpty()) keys_.insert(key, keys_.contains(key) ? -1 : i);
        }
    }
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override {
        if (row < 0 || column < 0 || column >= columnCount() || (parent.isValid() && parent.column() != 0)) return {};
        const int p = parent.isValid() ? node(parent) : root_;
        if (p < 0 || row >= children_.at(p).size()) return {};
        return createIndex(row, column, quintptr(children_.at(p).at(row)) + 1);
    }
    QModelIndex parent(const QModelIndex &child) const override {
        const int i = node(child);
        if (i < 0 || parents_.at(i) == root_) return {};
        return atNode(parents_.at(i));
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        if (parent.isValid() && parent.column() != 0) return 0;
        const int p = parent.isValid() ? node(parent) : root_;
        return p < 0 ? 0 : int(children_.at(p).size());
    }
    int columnCount(const QModelIndex & = {}) const override { return qMax(1, int(table_.headers.size())); }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        const int i = node(index);
        if (i < 0) return {};
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return table_.rows.at(i).value(index.column());
        if (role == KeyRole) {
            const auto key = table_.rowKeys.value(i);
            return keys_.value(key, -1) == i ? QVariant(key) : QVariant();
        }
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(table_.headers.value(section)) : QVariant();
    }
    QModelIndex byKey(const QString &key) const { return atNode(keys_.value(key, -1)); }
    QModelIndex atNode(int i) const { return i >= 0 && i < root_ ? createIndex(positions_.at(i), 0, quintptr(i) + 1) : QModelIndex(); }
    int nodeCount() const { return root_; }
private:
    int node(const QModelIndex &i) const {
        return i.isValid() && i.model() == this && i.internalId() > 0 && i.internalId() <= quintptr(root_) ? int(i.internalId() - 1) : -1;
    }
    diag::Table table_;
    int root_;
    QList<QList<int>> children_;
    QList<int> parents_, positions_;
    QHash<QString, int> keys_;
};
