#pragma once
// Read-only analysis for Health incident correlation, capability diagnostics,
// Autotune evidence and cooling trade-offs. No helper calls or hardware writes.
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
class QWidget;
namespace insights {
QStringList confidence(const QJsonObject &report);
QStringList pareto(const QJsonArray &runs);
QStringList capabilities();
QStringList incident(const QJsonObject &event, const QString &bootId);
void show(QWidget *parent, const QString &title, const QStringList &lines);
QWidget *capabilityPage(QWidget *parent = nullptr);
}
