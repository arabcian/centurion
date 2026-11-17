#include "distroprofile.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>

namespace distroprofile {
namespace {

QString tr(const char *s) { return QCoreApplication::translate("distroprofile", s); }

QString osRelease() {
    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly)) f.setFileName(QStringLiteral("/usr/lib/os-release"));
    if (!f.isOpen() && !f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.read(64 << 10));
}

QString field(const QString &text, const QString &key) {
    for (const QString &line : text.split(QLatin1Char('\n')))
        if (line.startsWith(key + QLatin1Char('=')))
            return line.mid(key.size() + 1).trimmed().remove(QLatin1Char('"')).remove(QLatin1Char('\''));
    return {};
}

// Same lists as distro.rs: COMMON_EXCLUDES and family_excludes().
const char *const COMMON[] = {"/proc/*", "/sys/*", "/dev/*", "/run/*", "/tmp/*", "/var/tmp/*", "/mnt/*", "/media/*", "/lost+found",
                              "/var/cache/*", "/var/lib/systemd/coredump/*", "/.snapshots/*", "/timeshift/*"};
const char *const GENTOO[] = {"/usr/portage/distfiles/*", "/usr/portage/packages/*", "/var/db/repos/gentoo/*"};
const char *const ARCH[] = {"/var/lib/pacman/sync/*"};
const char *const DEBIAN[] = {"/var/lib/apt/lists/*", "/var/crash/*", "/var/lib/snapd/cache/*", "/snap/*/[0-9]*/*"};

}  // namespace

QString familyOf(const QString &text) {
    QStringList ids = field(text, QStringLiteral("ID")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    ids += field(text, QStringLiteral("ID_LIKE")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &id : std::as_const(ids)) {
        if (id == QLatin1String("gentoo")) return id;
        if (id == QLatin1String("arch") || id == QLatin1String("archarm")) return QStringLiteral("arch");
        if (id == QLatin1String("debian") || id == QLatin1String("ubuntu")) return QStringLiteral("debian");
        if (id == QLatin1String("fedora") || id == QLatin1String("rhel") || id == QLatin1String("centos")) return QStringLiteral("fedora");
        if (id == QLatin1String("suse") || id == QLatin1String("sles") || id.startsWith(QLatin1String("opensuse"))) return QStringLiteral("opensuse");
        if (id == QLatin1String("void") || id == QLatin1String("alpine")) return id;
    }
    return {};
}

QString family() {
    static const QString f = familyOf(osRelease());
    return f;
}

QString prettyName() {
    static const QString n = [] {
        const QString p = field(osRelease(), QStringLiteral("PRETTY_NAME"));
        return p.isEmpty() ? familyLabel(family()) : p;
    }();
    return n;
}

QString familyLabel(const QString &f) {
    if (f == QLatin1String("gentoo")) return QStringLiteral("Gentoo");
    if (f == QLatin1String("arch")) return tr("Arch Linux & derivatives (Manjaro, EndeavourOS, CachyOS)");
    if (f == QLatin1String("debian")) return tr("Debian / Ubuntu & derivatives (Mint, Pop!_OS)");
    if (f == QLatin1String("fedora")) return tr("Fedora / RHEL family (Rocky, Alma)");
    if (f == QLatin1String("opensuse")) return QStringLiteral("openSUSE / SLES");
    if (f == QLatin1String("void")) return QStringLiteral("Void Linux");
    if (f == QLatin1String("alpine")) return QStringLiteral("Alpine Linux");
    return tr("Generic Linux");
}

QStringList families() {
    return {QStringLiteral("gentoo"), QStringLiteral("arch"), QStringLiteral("debian"), QStringLiteral("fedora"),
            QStringLiteral("opensuse"), QStringLiteral("void"), QStringLiteral("alpine"), QString()};
}

QStringList imageExcludes(const QString &f) {
    QStringList out;
    for (const char *e : COMMON) out << QLatin1String(e);
    auto add = [&out](const auto &list) { for (const char *e : list) out << QLatin1String(e); };
    if (f == QLatin1String("gentoo")) add(GENTOO);
    else if (f == QLatin1String("arch")) add(ARCH);
    else if (f == QLatin1String("debian")) add(DEBIAN);
    return out;
}

QString excludeSummary(const QString &f) {
    if (f == QLatin1String("gentoo"))
        return tr("distfiles and binary packages (/var/cache, old /usr/portage layout) and the ebuild repository "
                  "(/var/db/repos/gentoo: emerge --sync fetches it again)");
    if (f == QLatin1String("arch")) return tr("the package cache (/var/cache/pacman) and the sync databases (pacman -Sy fetches them again)");
    if (f == QLatin1String("debian"))
        return tr("the package cache and lists (/var/cache/apt, /var/lib/apt/lists: apt update), crash reports and "
                  "mounted snap revisions (their images in /var/lib/snapd/snaps are kept)");
    if (f == QLatin1String("fedora")) return tr("the dnf cache (/var/cache/dnf, /var/cache/libdnf5)");
    if (f == QLatin1String("opensuse")) return tr("the zypper cache (/var/cache/zypp) and snapper snapshots (/.snapshots)");
    if (f == QLatin1String("void")) return tr("the xbps package cache (/var/cache/xbps)");
    if (f == QLatin1String("alpine")) return tr("the apk cache (/var/cache/apk)");
    return tr("caches, temporary files and virtual file systems only");
}

QString legacyDefaults() {
    return QStringLiteral("/proc/*\n/sys/*\n/dev/*\n/run/*\n/tmp/*\n/var/tmp/*\n/mnt/*\n/media/*\n/lost+found\n/var/cache/*\n/usr/portage/distfiles/*");
}

QStringList swapFiles() {
    QStringList out;
    QFile f(QStringLiteral("/proc/swaps"));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList cols = lines.at(i).simplified().split(QLatin1Char(' '));
        if (cols.size() >= 2 && cols.at(1) == QLatin1String("file") && cols.at(0).startsWith(QLatin1Char('/')))
            out << QString(cols.at(0)).replace(QStringLiteral("\\040"), QStringLiteral(" "));
    }
    return out;
}

QList<Optional> optionalExcludes() {
    return {
        {QStringLiteral("/home/*/.cache/*"), tr("Users' caches"), tr("Browser, thumbnail and build caches under ~/.cache (only matters with /home included)")},
        {QStringLiteral("/root/.cache/*"), tr("root's cache"), tr("~/.cache of root")},
        {QStringLiteral("/home/*/.local/share/Trash/*"), tr("Users' trash"), tr("Files already moved to the trash")},
        {QStringLiteral("/home/*/.local/share/Steam/steamapps/*"), tr("Steam games"), tr("Installed games (Steam downloads them again); saves in the cloud or ~/.local/share stay")},
        {QStringLiteral("/home/*/.var/app/*/cache/*"), tr("Flatpak app caches"), tr("Per-app caches of Flatpak applications")},
        {QStringLiteral("/var/lib/flatpak/repo/*"), tr("Flatpak repository"), tr("System Flatpak objects: flatpak reinstalls the apps (their list is in /var/lib/flatpak/app)")},
        {QStringLiteral("/var/lib/docker/*"), tr("Docker"), tr("Images, containers and volumes of Docker — back up volumes separately if they hold data")},
        {QStringLiteral("/var/lib/containers/*"), tr("Podman (root)"), tr("Images and containers of rootful Podman")},
        {QStringLiteral("/var/lib/libvirt/images/*"), tr("Virtual machine disks"), tr("libvirt / virt-manager disk images: large, better copied while the VMs are off")},
        {QStringLiteral("/var/log/journal/*"), tr("systemd journal"), tr("Persistent journal files (often hundreds of MiB)")},
        {QStringLiteral("/var/lib/ollama/*"), tr("Ollama models"), tr("Downloaded language models (ollama pull fetches them again)")},
        {QStringLiteral("/usr/src/linux-*/*.o"), tr("Kernel build objects"), tr("Object files of built kernel sources; the sources and .config stay")},
    };
}

QString restoreHint(const QString &f) {
    if (f == QLatin1String("gentoo")) return tr("/mnt/gentoo — or / to restore over the running system");
    return tr("/mnt — the new root mounted from a live USB, or / to restore over the running system");
}

QString profileAbout(const QString &f) {
    QString pm;
    if (f == QLatin1String("gentoo")) pm = tr("/etc/portage, the world set, the kernel .config, conf.d, runlevels, env.d");
    else if (f == QLatin1String("arch")) pm = tr("pacman.conf and pacman.d (mirror list), makepkg.conf, mkinitcpio");
    else if (f == QLatin1String("debian")) pm = tr("APT sources and preferences, /etc/default, initramfs-tools, netplan");
    else if (f == QLatin1String("fedora")) pm = tr("dnf configuration, yum.repos.d, /etc/sysconfig, the SELinux mode");
    else if (f == QLatin1String("opensuse")) pm = tr("zypper repositories and zypp.conf, /etc/sysconfig, snapper configurations");
    else if (f == QLatin1String("void")) pm = tr("xbps.d, rc.conf, enabled runit services");
    else if (f == QLatin1String("alpine")) pm = tr("the apk world and repositories, mkinitfs, OpenRC runlevels");
    else pm = tr("no package manager configuration (distribution not recognised)");
    return tr("Also saves how this installation (%1) is put together, as reference material:\n"
              "%2;\n"
              "fstab, crypttab, GRUB/dracut/kernel settings, modprobe.d, modules-load.d, sysctl, udev rules,\n"
              "systemd units and logind/sleep settings, plus the kernel command line, DMI, loaded modules,\n"
              "the installed package list and the packages installed by name.\n"
              "It is stored under system-profile/ and is never restored automatically.")
        .arg(prettyName(), pm);
}

QString imageCaveat() {
    const QString text = osRelease();
    if (field(text, QStringLiteral("ID")) == QLatin1String("nixos"))
        return tr("NixOS: the system is rebuilt from configuration.nix; back up /etc/nixos and your data rather than relying on an image of /nix/store.");
    if (QFileInfo::exists(QStringLiteral("/run/ostree-booted")))
        return tr("Image-based (ostree) system: / is a read-only deployment. Back up /etc, /var and /home; the OS itself comes back with rpm-ostree / bootc.");
    return {};
}

}  // namespace distroprofile
