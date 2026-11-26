#pragma once
// Health → Boot history: the flight recorder's samples of a boot as text
// (crates/centurion-helpers/src/flight.rs). Pure, so it is tested without the page.
// Strings keep the "BootPage" translation context.
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace flightrec {

inline constexpr int ROWS = 24;   // 2 minutes at the default 5 s interval

inline constexpr qsizetype SLOT_BYTES = 192;
inline constexpr qsizetype SAMPLE_SLOTS = 720;
inline constexpr qsizetype RING_BYTES = (SAMPLE_SLOTS + 1) * SLOT_BYTES;

/// An empty object indicates an invalid header. Samples are ordered by sequence.
QJsonObject parseRing(const QByteArray &bytes);

/// Peaks of the whole stretch, then the last ROWS rows; a missing reading is "—". `samples` must not be empty.
QString text(const QJsonArray &samples);

} // namespace flightrec
