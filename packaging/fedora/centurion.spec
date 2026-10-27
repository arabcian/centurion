# Fedora / COPR. Source0 is the self-contained tarball from ./make-dist.sh (crates vendored:
# COPR builds offline). File layout: packaging/install-files.sh (same list as install.sh).
#
#   ./make-dist.sh && cp dist/centurion-%{version}.tar.xz ~/rpmbuild/SOURCES/
#   rpmbuild -ba packaging/fedora/centurion.spec      (or: copr-cli build … the SRPM)
#
# SELinux: the helpers in %{_libexecdir}/centurion get the default bin_t context, the same as
# other pkexec targets there; no policy module is needed.

# Cargo.toml strips the release binaries: nothing for a debuginfo package.
%global debug_package %{nil}
%global _vpath_srcdir gui
%global _vpath_builddir gui/build
%global units centurion-boot-guard.service nvcurve-autoload.service centurion-tune.service centurion-netguard.service centurion-intel-uv.service centurion-intel-uv-daemon.service centurion-calibrate-boot.service

Name:           centurion
Version:        2.0.0
Release:        1%{?dist}
Summary:        Power profile, firmware attribute and CPU/GPU curve tuning for Lenovo Legion laptops
License:        GPL-3.0-or-later AND (Apache-2.0 OR MIT) AND Unicode-3.0
URL:            https://github.com/arabcian/centurion
Source0:        %{name}-%{version}.tar.xz
ExclusiveArch:  x86_64

BuildRequires:  cargo >= 1.75
BuildRequires:  rust >= 1.75
BuildRequires:  cmake >= 3.19
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  qt6-linguist
BuildRequires:  libxkbcommon-devel
BuildRequires:  systemd-rpm-macros
BuildRequires:  desktop-file-utils

Requires:       polkit
Requires:       hicolor-icon-theme
Recommends:     nftables
Recommends:     pigz
Recommends:     zstd
Suggests:       mangohud

%description
Centurion is a Qt6 tray application with Rust root helpers for Lenovo Legion
(and related) laptops: power profiles, firmware limits, fans, NVIDIA / Ryzen /
Intel curves, keyboard lighting, scenes, system optimizations and health checks.
Not an official Lenovo product.

%prep
%autosetup -p1

%build
# Vendored crates through .cargo/config.toml from make-dist.sh.
cargo build --release --frozen --offline
# Portable build: no -march=native, no PGO; Fedora's own flags (LTO, hardening) apply.
# CENTURION_HELPER_DIR must match the polkit exec.path annotations (LIBEXEC in %%install).
%cmake -DCENTURION_HELPER_DIR=%{_libexecdir}/centurion -DCENTURION_NATIVE=OFF -DCENTURION_PGO= -DCENTURION_LTO=OFF
%cmake_build

%check
cargo test --release --frozen --offline
desktop-file-validate %{buildroot}%{_datadir}/applications/centurion.desktop

%install
# Root helpers are password-free for a wheel/sudo admin at the machine;
# firmware writes, backups and netguard rules always ask.
DESTDIR=%{buildroot} PREFIX=%{_prefix} LIBEXEC=%{_libexecdir}/centurion UNITDIR=%{_unitdir} \
    UDEVDIR=%{_udevrulesdir} SYSTEMD_SLEEP=%{_prefix}/lib/systemd/system-sleep \
    POLKIT_RULESDIR=%{_datadir}/polkit-1/rules.d \
    bash packaging/install-files.sh
# OpenRC scripts are of no use on Fedora.
rm -r %{buildroot}%{_sysconfdir}/init.d

# Nothing is enabled: every boot service is opt-in (no preset ships with the package).
%post
%systemd_post %{units}
%udev_rules_update

%preun
%systemd_preun %{units}
# Removal (not upgrade): put every tuned value back while the helper is still there.
if [ $1 -eq 0 ]; then
    printf '%s' '{"op":"restore"}' | %{_libexecdir}/centurion/tune-helper >/dev/null 2>&1 || :
fi

%postun
%systemd_postun %{units}
%udev_rules_update

%files
%license LICENSE NOTICE
%doc README.md DISCLAIMER.md docs/HANDBOOK.md docs/TECHNICAL.md docs/AUTOTUNE.md
%{_bindir}/centurion
%{_bindir}/nvcurve
%{_bindir}/centurion-gamemode
%{_bindir}/centurion-intel-uv
%{_bindir}/centurion-autotune
%{_bindir}/centurion-calibrate
%{_bindir}/centurion-netguard
%{_bindir}/centurion-display
%{_bindir}/centurion-cooltest
%dir %{_libexecdir}/centurion
%{_libexecdir}/centurion/legion-profile-helper
%{_libexecdir}/centurion/fwattr-helper
%{_libexecdir}/centurion/ryzen-co-helper
%{_libexecdir}/centurion/tune-helper
%{_libexecdir}/centurion/tune-profile-helper
%{_libexecdir}/centurion/intel-uv-helper
%{_libexecdir}/centurion/legion-gpu-helper
%{_libexecdir}/centurion/legion-firmware-helper
%{_libexecdir}/centurion/lighting-helper
%{_libexecdir}/centurion/amdgpu-helper
%{_libexecdir}/centurion/netguard-helper
%{_libexecdir}/centurion/backup-helper
%{_libexecdir}/centurion/nvcurve-sensors
%{_libexecdir}/centurion/legion-ec-sensors
%{_libexecdir}/centurion/health-reader
%{_libexecdir}/centurion/centurion-boot-guard
%attr(0700,root,root) %{_libexecdir}/centurion/nvcurve-root-helper
%{_datadir}/applications/centurion.desktop
%{_datadir}/icons/hicolor/scalable/apps/centurion.svg
%{_sysconfdir}/xdg/autostart/centurion.desktop
%{_datadir}/polkit-1/actions/com.centurion.policy
%{_datadir}/polkit-1/rules.d/49-centurion.rules
%{_udevrulesdir}/70-centurion-lighting.rules
%{_udevrulesdir}/90-centurion-dgpu.rules
%{_unitdir}/*.service
%{_prefix}/lib/systemd/system-sleep/centurion-sleep-log
%dir %{_sysconfdir}/nvcurve
%dir %{_sysconfdir}/nvcurve/profiles

%changelog
* Sat Oct 24 2026 Cihan <arabcian@users.noreply.github.com> - 2.0.0-1
- First package
