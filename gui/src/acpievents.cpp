#include "acpievents.h"
#include <QSocketNotifier>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr char FAMILY[] = "acpi_event", GROUP[] = "acpi_mc_group";
constexpr int ATTR_EVENT = 1;  // ACPI_GENL_ATTR_EVENT
// struct acpi_genl_event (drivers/acpi/event.c), natural alignment: 44 bytes.
struct GenlEvent { char deviceClass[20]; char busId[15]; uint32_t type; uint32_t data; };

// Calls f(type, payload, length) for each attribute; stops at the first malformed one.
template <class F> void forAttrs(const char *p, size_t len, F f) {
    while (len >= NLA_HDRLEN) {
        nlattr a;
        memcpy(&a, p, sizeof a);
        if (a.nla_len < NLA_HDRLEN || a.nla_len > len) return;
        f(a.nla_type & NLA_TYPE_MASK, p + NLA_HDRLEN, size_t(a.nla_len - NLA_HDRLEN));
        const size_t step = NLA_ALIGN(a.nla_len);
        if (step >= len) return;
        p += step;
        len -= step;
    }
}

// Calls f(header, message) for each netlink message; stops at the first malformed one.
template <class F> void forMessages(const char *p, size_t len, F f) {
    while (len >= NLMSG_HDRLEN) {
        nlmsghdr h;
        memcpy(&h, p, sizeof h);
        if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > len) return;
        f(h, p);
        const size_t step = NLMSG_ALIGN(h.nlmsg_len);
        if (step >= len) return;
        p += step;
        len -= step;
    }
}

std::string cstr(const char *s, size_t max) { return std::string(s, strnlen(s, max)); }

// CTRL_CMD_GETFAMILY "acpi_event": the family id and the id of its multicast group.
bool resolve(int fd, uint16_t *family, uint32_t *group) {
    struct { nlmsghdr n; genlmsghdr g; char attrs[32]; } req{};
    const nlattr name{uint16_t(NLA_HDRLEN + sizeof FAMILY), CTRL_ATTR_FAMILY_NAME};
    memcpy(req.attrs, &name, sizeof name);
    memcpy(req.attrs + NLA_HDRLEN, FAMILY, sizeof FAMILY);
    req.n.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN + NLA_ALIGN(name.nla_len);
    req.n.nlmsg_type = GENL_ID_CTRL;
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.n.nlmsg_seq = 1;
    req.g.cmd = CTRL_CMD_GETFAMILY;
    req.g.version = 1;
    if (::send(fd, &req, req.n.nlmsg_len, 0) < 0) return false;
    alignas(4) char buf[4096];
    const ssize_t len = ::recv(fd, buf, sizeof buf, 0);
    if (len <= 0) return false;
    *family = 0;
    *group = 0;
    forMessages(buf, size_t(len), [&](const nlmsghdr &h, const char *m) {
        if (h.nlmsg_type != GENL_ID_CTRL || h.nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) return;  // NLMSG_ERROR: unknown family
        forAttrs(m + NLMSG_HDRLEN + GENL_HDRLEN, h.nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, [&](int t, const char *d, size_t n) {
            if (t == CTRL_ATTR_FAMILY_ID && n >= 2) memcpy(family, d, 2);
            if (t != CTRL_ATTR_MCAST_GROUPS) return;
            forAttrs(d, n, [&](int, const char *gd, size_t gn) {
                std::string gname;
                uint32_t gid = 0;
                forAttrs(gd, gn, [&](int gt, const char *v, size_t vn) {
                    if (gt == CTRL_ATTR_MCAST_GRP_NAME) gname = cstr(v, vn);
                    if (gt == CTRL_ATTR_MCAST_GRP_ID && vn >= 4) memcpy(&gid, v, 4);
                });
                if (gname == GROUP) *group = gid;
            });
        });
    });
    return *family != 0 && *group != 0;
}
} // namespace

namespace acpievents {
std::vector<Event> parse(const void *buf, size_t len, uint16_t family) {
    std::vector<Event> out;
    forMessages(static_cast<const char *>(buf), len, [&](const nlmsghdr &h, const char *m) {
        if (h.nlmsg_type != family || h.nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) return;
        forAttrs(m + NLMSG_HDRLEN + GENL_HDRLEN, h.nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, [&](int t, const char *d, size_t n) {
            if (t != ATTR_EVENT || n < sizeof(GenlEvent)) return;
            GenlEvent e;
            memcpy(&e, d, sizeof e);
            out.push_back({cstr(e.deviceClass, sizeof e.deviceClass), cstr(e.busId, sizeof e.busId), e.type, e.data});
        });
    });
    return out;
}
} // namespace acpievents

AcpiEvents::AcpiEvents(QObject *parent) : QObject(parent) {
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) return;
    const timeval wait{0, 500000};  // the family lookup must never hang the GUI
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
    uint32_t group = 0;
    if (!resolve(fd, &family_, &group) || ::setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof group) != 0
        || ::fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        ::close(fd);
        return;
    }
    fd_ = fd;
    auto *n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(n, &QSocketNotifier::activated, this, &AcpiEvents::drain);
    connect(this, &QObject::destroyed, [fd] { ::close(fd); });
}

void AcpiEvents::drain() {
    alignas(4) char buf[8192];
    for (;;) {
        const ssize_t len = ::recv(fd_, buf, sizeof buf, 0);
        // ENOBUFS: the socket overflowed and events were dropped; what is queued after it is still good.
        if (len < 0 && (errno == EINTR || errno == ENOBUFS)) continue;
        if (len <= 0) return;  // EAGAIN: drained
        for (const acpievents::Event &e : acpievents::parse(buf, size_t(len), family_))
            Q_EMIT event(QString::fromStdString(e.deviceClass), QString::fromStdString(e.busId), e.type, e.data);
    }
}
