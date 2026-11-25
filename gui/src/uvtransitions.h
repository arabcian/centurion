#pragma once
// Intel Undervolt → Context transitions: the daemon's journal (/run/centurion/intel-uv-transitions.json) of
// full applies at boot, power-source switches, resume and scene holds, each field read back after writing.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace uvtransitions {

inline const char *FILE_PATH = "/run/centurion/intel-uv-transitions.json";

struct Row {
    qint64 t = 0;
    QString event, source;
    bool ok = false;
    QStringList failed;   // "what: message"
    int locked = 0;       // failures the firmware refused (lock bit / blocked MSR writes)
    int fields = 0;
};

/// Newest first. Unknown versions or malformed text give no rows (never a guessed "ok").
inline QList<Row> parse(const QByteArray &text) {
    QList<Row> out;
    const auto doc = QJsonDocument::fromJson(text).object();
    if (doc.value("version").toInt() != 1) return out;
    for (const auto &v : doc.value("entries").toArray()) {
        const auto e = v.toObject();
        Row r{qint64(e.value("t").toDouble()), e.value("event").toString(), e.value("source").toString(), e.value("ok").toBool(), {}, 0, 0};
        if (e.contains("error")) r.failed << e.value("error").toString();
        for (const auto &f : e.value("fields").toArray()) {
            const auto o = f.toObject();
            ++r.fields;
            if (o.value("ok").toBool()) continue;
            r.failed << o.value("what").toString() + QStringLiteral(": ") + o.value("message").toString();
            r.locked += o.value("locked").toBool();
        }
        if (!r.failed.isEmpty()) r.ok = false;
        out.prepend(r);
    }
    return out;
}

} // namespace uvtransitions
