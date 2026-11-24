#pragma once
// Runs one read-only op of tune-profile-helper as the user: no pkexec, no password. The Health pages
// that need the Rust analysis (the Overview cards, the dGPU usage view, the diagnostic report) go
// through it. The callback runs on the GUI thread with the helper's JSON reply, or with
// {"ok": false, "error": …} when the helper is missing, silent or killed.
#include <QJsonObject>
#include <QStringList>
#include <functional>

class QObject;

namespace userhelper {

using Callback = std::function<void(const QJsonObject &reply)>;

/// `context` bounds the callback's lifetime (not invoked once it is destroyed). A helper that does not
/// answer within `timeoutMs` is killed.
void run(const QJsonObject &request, QObject *context, Callback cb, int timeoutMs = 15000);

/// Explicit, unprivileged read-only executable. No shell or privilege escalation.
/// Normal exit + JSON success are required; output is bounded and timeout is final.
void runProgram(const QString &exe, const QStringList &args, const QJsonObject &request,
                QObject *context, Callback cb, int timeoutMs = 15000);

} // namespace userhelper
