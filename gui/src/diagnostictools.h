#pragma once
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QMap>

// User-triggered, read-only system inventories. A hardware baseline is stored
// only when the user explicitly clicks Save baseline; it never changes hardware.
namespace diagnostictools {
// Pure fixture-friendly parsers; also used by the real system tools.
QString inspectEdidBytes(const QByteArray &data);
QMap<QString,QString> parseNvidiaOptionsText(const QString &content);
QString snapshotDifferenceText(const QJsonObject &before, const QJsonObject &after);
QString hardwarePreview();
QString saveHardwareBaseline();
QString compareHardwareBaseline();
QString edidReport();
QString moduleParameterReport();
}
