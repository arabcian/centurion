# Copyright 2026 Cihan
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit cmake linux-info systemd udev xdg

DESCRIPTION="Power profile, firmware attribute and CPU/GPU curve tuning for Lenovo Legion laptops"
HOMEPAGE="https://localhost/centurion"
# Self-contained tarball from ./make-dist.sh (crates vendored, offline build).
SRC_URI="${P}.tar.xz"

LICENSE="GPL-3+"
# Vendored crates
LICENSE+=" Apache-2.0 MIT Unicode-3.0"
SLOT="0"
KEYWORDS="~amd64"
RESTRICT="fetch"

DEPEND="
	dev-qt/qtbase:6[gui,network,widgets]
	dev-qt/qtsvg:6
	x11-libs/libxkbcommon
"
RDEPEND="
	${DEPEND}
	sys-auth/polkit
	net-firewall/nftables
"
# Health → Network (centurion-netguard); warnings only.
CONFIG_CHECK="~NETFILTER_NETLINK_QUEUE ~NF_TABLES ~NF_TABLES_INET ~NFT_QUEUE ~NF_CONNTRACK ~NFT_CT
	~INET_DIAG ~INET_TCP_DIAG ~INET_UDP_DIAG ~INET_DIAG_DESTROY"
BDEPEND=">=virtual/rust-1.75"

pkg_nofetch() {
	einfo "Build the tarball with ./make-dist.sh in the source tree and copy"
	einfo "dist/${P}.tar.xz into your DISTDIR (usually /var/cache/distfiles)."
}

src_configure() {
	CMAKE_USE_DIR="${S}/gui"
	local mycmakeargs=(
		-DCENTURION_HELPER_DIR="${EPREFIX}/usr/libexec/${PN}"
	)
	cmake_src_configure
}

src_compile() {
	# Vendored crates via .cargo/config.toml; portage handles stripping.
	export CARGO_HOME="${T}/cargo" CARGO_PROFILE_RELEASE_STRIP=false
	cargo build --release --frozen --offline || die "cargo build failed"
	cmake_src_compile
}

src_test() {
	cargo test --release --frozen --offline || die "cargo test failed"
}

src_install() {
	local r="${S}/target/release"
	exeinto /usr/libexec/${PN}
	doexe "${r}"/{legion-profile-helper,fwattr-helper,ryzen-co-helper,tune-helper,tune-profile-helper,intel-uv-helper,legion-gpu-helper,legion-firmware-helper,lighting-helper,amdgpu-helper,nvcurve-sensors,legion-ec-sensors,health-reader,centurion-boot-guard,netguard-helper,backup-helper}
	exeopts -m0700
	doexe "${r}"/nvcurve-root-helper
	dobin "${r}"/{nvcurve,centurion-gamemode,centurion-intel-uv,centurion-autotune,centurion-calibrate,centurion-netguard}

	cmake_src_install

	insinto /usr/share/polkit-1/actions
	sed -i "s|@LIBEXEC@|${EPREFIX}/usr/libexec/centurion|g" packaging/polkit/* || die
	doins packaging/polkit/com.centurion.policy
	# Security level 1 (silent for wheel at the machine); for level 2 or 3 edit SECURITY_LEVEL in the installed rules file.
	sed -i "s|@SECLEVEL@|1|g" packaging/polkit/49-centurion.rules || die
	insinto /etc/polkit-1/rules.d
	doins packaging/polkit/49-centurion.rules
	local s
	for s in nvcurve-autoload centurion-tune centurion-intel-uv centurion-intel-uv-daemon centurion-boot-guard centurion-netguard centurion-calibrate-boot; do
		sed -e "s|@BINDIR@|${EPREFIX}/usr/bin|g" \
			-e "s|@LIBEXEC@|${EPREFIX}/usr/libexec/centurion|g" \
			packaging/openrc/${s} > "${T}"/${s}.initd || die
		newinitd "${T}"/${s}.initd ${s}
		sed -e "s|@BINDIR@|${EPREFIX}/usr/bin|g" \
			-e "s|@LIBEXEC@|${EPREFIX}/usr/libexec/centurion|g" \
			packaging/systemd/${s}.service > "${T}"/${s}.service || die
		systemd_dounit "${T}"/${s}.service
	done
	sed -e "s|@LIBEXEC@|${EPREFIX}/usr/libexec/centurion|g" \
		packaging/sleep/centurion-intel-uv > "${T}"/50-centurion-intel-uv || die
	exeinto /$(get_libdir)/elogind/system-sleep
	exeopts -m0755
	doexe "${T}"/50-centurion-intel-uv
	udev_dorules packaging/udev/70-centurion-lighting.rules
	sed -e "s|@LIBEXEC@|${EPREFIX}/usr/libexec/centurion|g" \
		packaging/udev/90-centurion-dgpu.rules > "${T}"/90-centurion-dgpu.rules || die
	udev_dorules "${T}"/90-centurion-dgpu.rules
	keepdir /etc/nvcurve/profiles

	dodoc README.md NOTICE DISCLAIMER.md docs/TECHNICAL.md
}

pkg_postinst() {
	xdg_pkg_postinst
	udev_reload
	elog "Keyboard lighting (Spectrum / 4-zone RGB): re-login or re-plug once so the"
	elog "  uaccess udev rule applies to the keyboard's hidraw node; until then the"
	elog "  Lighting tab goes through pkexec."
	elog "Boot-time GPU profile (set with ★ Default in the NVIDIA tab):"
	elog "  OpenRC:  rc-update add nvcurve-autoload default"
	elog "  systemd: systemctl enable nvcurve-autoload.service"
	elog "Optimizations boot preset (set with ⏻ Apply at boot):"
	elog "  OpenRC:  rc-update add centurion-tune boot"
	elog "  systemd: systemctl enable centurion-tune.service"
	elog "Intel undervolt boot/resume profile (⏻ in the Intel Undervolt tab):"
	elog "  OpenRC:  rc-update add centurion-intel-uv boot   (resume: elogind hook installed)"
	elog "  systemd: systemctl enable centurion-intel-uv.service"
	elog "  or the daemon (AC/battery switch, periodic re-apply, hwphint):"
	elog "  OpenRC:  rc-update add centurion-intel-uv-daemon default / systemd: centurion-intel-uv-daemon.service"
	elog "GPU power limits (Firmware tab: cTGP / Dynamic Boost, NVIDIA laptops), the Custom-mode fan"
	elog "  curve and the EC sensors need sys-power/acpi_call; the GPU limits also need the Custom profile."
	elog "Network guard (Health \u2192 Network: IP blacklist, Wine/.exe guard):"
	elog "  OpenRC:  rc-update add centurion-netguard default / systemd: centurion-netguard.service"
	elog "Calibration scheduled for the next boot (Autotune: Calibrate at next boot; idle otherwise):"
	elog "  OpenRC:  rc-update add centurion-calibrate-boot default / systemd: centurion-calibrate-boot.service"
	elog "Lutris hooks: /usr/bin/centurion-gamemode PRE / POST / RUN (see the Game launch sub-tab)."
	elog "The Ryzen tab needs a root-owned ryzenadj in /usr/bin, /usr/sbin,"
	elog "/usr/local/{bin,sbin} or /opt/ryzenadj."
}

pkg_postrm() {
	xdg_pkg_postrm
	udev_reload
}
