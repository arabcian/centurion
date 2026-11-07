#pragma once
// Health → Boot history: the flight recorder's samples of a boot as text
// (crates/centurion-helpers/src/flight.rs). Pure, so it is tested without the page.
// Strings keep the "BootPage" translation context.
#include <QJsonArray>
#include <QString>

namespace flightrec {

inline constexpr int ROWS = 24;   // 2 minutes at the default 5 s interval

/// Peaks of the whole stretch, then the last ROWS rows; a missing reading is "—". `samples` must not be empty.
QString text(const QJsonArray &samples);

} // namespace flightrec
