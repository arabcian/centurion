#pragma once
#include "incrementalrows.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVariant>
#include <utility>
#include <vector>

// Identity here is a setting path within one capture scope, not physical device lifetime.
class KernelParamsModel final : public QAbstractTableModel {
public:
    enum { SearchRole = Qt::UserRole + 1, PathRole };
    explicit KernelParamsModel(QObject *parent=nullptr) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex &parent={}) const override { return parent.isValid()?0:int(rows_.size()); }
    int columnCount(const QModelIndex &parent={}) const override { return parent.isValid()?0:3; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation!=Qt::Horizontal || role!=Qt::DisplayRole) return {};
        switch(section) {
        case 0:return QCoreApplication::translate("KernelParamsPage","Setting");
        case 1:return QCoreApplication::translate("KernelParamsPage","Value");
        case 2:return QCoreApplication::translate("KernelParamsPage","Writable");
        default:return {};
        }
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row()<0 || index.row()>=rowCount() || index.column()<0 || index.column()>=3) return {};
        const auto &r=rows_[std::size_t(index.row())];
        if (role==PathRole) return r.path;
        if (role==SearchRole) return r.path+QLatin1Char('\n')+r.value;
        if (role==Qt::ToolTipRole && index.column()==1) return r.value;
        if (role!=Qt::DisplayRole) return {};
        if (index.column()==0) return r.path;
        if (index.column()==1) {
            const auto first=r.value.section(QLatin1Char('\n'),0,0);
            return r.value.contains(QLatin1Char('\n'))?first+QStringLiteral(" …"):first;
        }
        return r.writable?QCoreApplication::translate("KernelParamsPage","yes"):QString();
    }
    QModelIndex byPath(const QString &path) const {
        const auto it=std::lower_bound(rows_.begin(),rows_.end(),path,[](const Row &r,const QString &p){return r.path<p;});
        return it!=rows_.end() && it->path==path?index(int(it-rows_.begin()),0):QModelIndex{};
    }
    void clear() { if(rows_.empty()) return; beginResetModel();rows_.clear();endResetModel(); }
    // Reject malformed or ambiguous captures before altering the previous view.
    bool replace(const QJsonArray &entries) {
        if(entries.size()>65000) return false;
        std::vector<Row> next; next.reserve(std::size_t(entries.size()));
        for(const auto &v:entries) {
            if(!v.isObject()) return false;
            const auto o=v.toObject(); const auto p=o.value(QStringLiteral("p")), val=o.value(QStringLiteral("v")), w=o.value(QStringLiteral("w"));
            if(!p.isString() || !val.isString() || !w.isBool() || p.toString().isEmpty()) return false;
            next.push_back({p.toString(),val.toString(),w.toBool()});
        }
        std::sort(next.begin(),next.end(),[](const Row&a,const Row&b){return a.path<b.path;});
        std::vector<QString> oldKeys,newKeys;
        oldKeys.reserve(rows_.size());newKeys.reserve(next.size());
        for(const auto &r:rows_) oldKeys.push_back(r.path);
        for(const auto &r:next) newKeys.push_back(r.path);
        const auto edits=incrementalrows::plan(oldKeys,newKeys);
        if(!edits.valid) return false;
        // Avoid pathological quadratic shifts for highly fragmented replacements.
        // Ordinary value-only and small structural updates never reset the model.
        if(edits.remove.size()+edits.insert.size()>128) {
            beginResetModel();rows_=std::move(next);endResetModel();return true;
        }
        for(auto it=edits.remove.rbegin();it!=edits.remove.rend();++it) {
            beginRemoveRows({},int(it->first),int(it->first+it->count-1));
            rows_.erase(rows_.begin()+it->first,rows_.begin()+it->first+it->count);endRemoveRows();
        }
        for(const auto &r:edits.insert) {
            beginInsertRows({},int(r.first),int(r.first+r.count-1));
            rows_.insert(rows_.begin()+r.first,next.begin()+r.first,next.begin()+r.first+r.count);endInsertRows();
        }
        for(std::size_t i=0;i<next.size();++i) {
            if(rows_[i].value==next[i].value && rows_[i].writable==next[i].writable) continue;
            const bool value=rows_[i].value!=next[i].value, writable=rows_[i].writable!=next[i].writable;
            rows_[i]=std::move(next[i]);
            Q_EMIT dataChanged(index(int(i),value?0:2),index(int(i),writable?2:1),{Qt::DisplayRole,Qt::ToolTipRole,SearchRole});
        }
        return true;
    }
private:
    struct Row { QString path,value;bool writable; };
    std::vector<Row> rows_;
};
