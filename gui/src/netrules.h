#pragma once
// Health → Network: the pure part of the firewall and lookup tabs (what a typed port list or address
// list means). netguard-helper validates everything again; this only spares a password prompt for a typo.

#include <QList>
#include <QString>

/// "22, 80, 8000-8010" → the ports in it, in order and without repeats. Empty when the text is invalid
/// or names more than `max` ports.
QList<int> expandPorts(const QString &text, int max);
/// A port list for a firewall rule (any size of range); empty text is valid and means every port.
bool validPortList(const QString &text);
/// A firewall rule's remote side: empty, "lan", or addresses, CIDR blocks and "a - b" ranges separated by commas.
bool validRemote(const QString &text);
/// A network interface name, optionally ending in * ("wg*"); empty is valid.
bool validInterface(const QString &text);
/// What the user typed reduced to a host: a URL's host name, punycode for an international domain.
QString lookupHost(const QString &text);
/// A host that is safe to hand to ping and traceroute as an argument.
bool validHost(const QString &host);
/// The DNS name that holds an address's PTR record, or empty when `ip` is not an address.
QString reverseDnsName(const QString &ip);
