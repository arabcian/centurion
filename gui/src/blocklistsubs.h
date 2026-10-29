#pragma once
// Health → Network → Rules → Subscriptions: published address lists (netsets) that are
// banned as one named group. This file is the pure part (parsing, due time, storage);
// the window and the download live in netpage.cpp. The daemon never goes online itself:
// the list is fetched by the GUI, validated by netguard-helper, and applied through pkexec.

#include <QList>
#include <QString>
#include <QStringList>

constexpr int BLOCKLIST_MAX_ENTRIES = 100000;
constexpr qint64 BLOCKLIST_MAX_BYTES = 8LL << 20;

struct BlocklistSub {
    QString name;          // also the group label in the blacklist (at most 64 characters)
    QString url;           // https:// only
    int hours = 168;       // fetch again after this long
    qint64 updated = 0;    // unix seconds of the last applied update; 0 = never
    int entries = 0;       // entries the last applied update banned
    QString error;         // why the last attempt failed; empty after a success
};

/// https:// URL with a host, at most 512 characters.
bool blocklistUrlOk(const QString &url);

/// Never updated, or the interval has passed since the last applied update.
bool blocklistDue(const BlocklistSub &s, qint64 nowSecs);

struct ParsedBlocklist {
    QStringList entries;   // tokens as sent to the helper, which validates each one
    QString error;         // empty on success
};

/// One address, CIDR block or range per token; commas and spaces separate tokens, "#" starts a
/// comment, "first - last" stays one range. Fails on an empty list or more than BLOCKLIST_MAX_ENTRIES.
ParsedBlocklist parseBlocklist(const QByteArray &text);

/// The gui.ini of the Centurion settings (the same file the other pages use).
QString blocklistSettingsPath();
QList<BlocklistSub> loadBlocklistSubs(const QString &settingsPath);
void saveBlocklistSubs(const QString &settingsPath, const QList<BlocklistSub> &subs);
