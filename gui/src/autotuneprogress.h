#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <utility>

namespace autotuneprogress {
// One process/run owns one parser. A solver-returned event is not a successful
// result: the process exit code and stdout result are independently checked.
class Run {
public:
    explicit Run(QString id) : id_(std::move(id)) {}
    std::optional<int> accept(const QByteArray &line) {
        if (line.size() > 8192) return std::nullopt;
        QByteArray phase;
        int sequence = -1;
        const bool structured = line.startsWith("CENTURION_EVENT ");
        if (structured) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line.mid(sizeof("CENTURION_EVENT ") - 1), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
            const auto event = document.object();
            if (event.value("version").toInteger(-1) != 1 || id_.isEmpty() || event.value("run_id").toString() != id_) return std::nullopt;
            const auto seq = event.value("sequence").toInteger(-1);
            if (seq < 1 || seq > 5) return std::nullopt;
            sequence = int(seq);
            phase = event.value("phase").toString().toLatin1();
        } else if (!structuredSeen_ && line.startsWith("CENTURION_STAGE ")) {
            phase = line.mid(sizeof("CENTURION_STAGE ") - 1);
        } else return std::nullopt;
        static constexpr const char *names[] = {"inputs", "model", "decision", "validation", "finished"};
        int index = -1;
        for (int i = 0; i < 5; ++i) if (phase == names[i]) index = i;
        if (index < 0 || index <= last_ || (structured && sequence != index + 1)) return std::nullopt;
        structuredSeen_ |= structured;
        last_ = index;
        return index;
    }
private:
    QString id_;
    int last_ = -1;
    bool structuredSeen_ = false;
};
}
