# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

# README: Rust >= 1.75 (cargo.eclass raises this to the oldest slot in tree).
RUST_MIN_VER="1.75.0"

inherit cargo cmake git-r3 linux-info optfeature systemd toolchain-funcs udev xdg

DESCRIPTION="Power profile, firmware and CPU/GPU curve tuning for Lenovo Legion laptops"
HOMEPAGE="https://github.com/arabcian/centurion"
EGIT_REPO_URI="https://github.com/arabcian/centurion.git"

LICENSE="GPL-3+"
# Dependent crate licenses
LICENSE+=" Apache-2.0 MIT Unicode-3.0"
SLOT="0"
IUSE="lto +netguard pgo test +xkb"
RESTRICT="!test? ( test )"

DEPEND="
	dev-qt/qtbase:6[dbus,gui,network,widgets]
	dev-qt/qtsvg:6
	xkb? ( x11-libs/libxkbcommon )
"
RDEPEND="
	${DEPEND}
	sys-auth/polkit
	netguard? ( net-firewall/nftables )
"
BDEPEND="
	dev-qt/qttools:6[linguist]
	xkb? ( virtual/pkgconfig )
	test? ( sys-apps/dbus )
"

# The GUI lives in gui/; the Rust helpers are a cargo workspace in ${S}.
CMAKE_USE_DIR="${S}/gui"

pkg_setup() {
	# Health -> Network (centurion-netguard); warnings only.
	if use netguard; then
		CONFIG_CHECK="~NETFILTER_NETLINK_QUEUE ~NF_TABLES ~NF_TABLES_INET ~NFT_QUEUE
			~NF_CONNTRACK ~NFT_CT ~INET_DIAG ~INET_TCP_DIAG ~INET_UDP_DIAG ~INET_DIAG_DESTROY"
		linux-info_pkg_setup
	fi
	# linux-info's exported pkg_setup would otherwise shadow rust's.
	rust_pkg_setup
}

src_unpack() {
	git-r3_src_unpack
	cargo_live_src_unpack
}

src_prepare() {
	cmake_src_prepare
}

src_configure() {
	# The Rust helpers always use fat LTO (Cargo.toml).
	cargo_src_configure --frozen

	# Portable build: no -march=native (that comes from the user's *FLAGS).
	# CENTURION_HELPER_DIR must match the polkit exec.path.
	local mycmakeargs=(
		-DCENTURION_HELPER_DIR="${EPREFIX}/usr/libexec/${PN}"
		-DCENTURION_NATIVE=OFF
		-DCENTURION_LTO=$(usex lto)
		-DCENTURION_PGO=$(usev pgo use)
		-DCENTURION_PGO_DIR="${WORKDIR}/pgo-profile"
		-DCENTURION_XKB=$(usex xkb)
		-DCENTURION_BUILD_TESTS=$(usex test)
	)
	cmake_src_configure

	if use pgo; then
		# Instrumented GUI for the training run, in its own build dir.
		mycmakeargs+=(
			-DCENTURION_PGO=generate
			-DCENTURION_BUILD_TESTS=OFF
		)
		BUILD_DIR="${WORKDIR}/${P}_pgo" cmake_src_configure
	fi
}

src_compile() {
	# Cargo.toml strips release binaries; leave that to portage.
	export CARGO_PROFILE_RELEASE_STRIP=false
	cargo_src_compile

	if use pgo; then
		BUILD_DIR="${WORKDIR}/${P}_pgo" cmake_src_compile

		# Same training run as install.sh: offscreen, every tab, private
		# HOME and runtime dir; the GUI keeps every root helper off while
		# CENTURION_PGO_TRAIN is set and quits by itself.
		local rt="${T}/pgo-train"
		mkdir -p "${rt}"/{home,run} || die
		chmod 700 "${rt}"/run || die
		# Thread names (pthread_setname_np) of the GUI and the tools it
		# runs, e.g. nvidia-smi.
		addpredict /proc/self/task
		einfo "PGO training run (offscreen, every tab) ..."
		env QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="${rt}/run" HOME="${rt}/home" \
			XDG_CONFIG_HOME="${rt}/home/.config" XDG_CACHE_HOME="${rt}/home/.cache" \
			XDG_DATA_HOME="${rt}/home/.local/share" CENTURION_PGO_TRAIN=5 \
			timeout 180 "${WORKDIR}/${P}_pgo"/centurion >/dev/null 2>&1 \
			|| ewarn "PGO training run exited with status $?"

		local prof="${WORKDIR}/pgo-profile"
		if tc-is-clang; then
			type -P llvm-profdata >/dev/null || die "llvm-profdata not found"
			llvm-profdata merge -o "${prof}"/default.profdata "${prof}"/*.profraw \
				|| die "llvm-profdata merge failed"
		fi
		[[ -n $(find "${prof}" -type f 2>/dev/null) ]] \
			|| die "PGO training produced no profile; rebuild with USE=-pgo"
	fi

	cmake_src_compile
}

src_test() {
	cargo_src_test
	# The GUI tests create a QApplication.
	local -x QT_QPA_PLATFORM=offscreen
	cmake_src_test
}

src_install() {
	local libexec="${EPREFIX}/usr/libexec/${PN}"
	local r="$(cargo_target_dir)"

	_centurion_subst() {
		sed -e "s|@BINDIR@|${EPREFIX}/usr/bin|g" \
			-e "s|@LIBEXEC@|${libexec}|g" "${1}" > "${2}" || die
	}

	exeinto /usr/libexec/${PN}
	doexe "${r}"/{legion-profile-helper,fwattr-helper,ryzen-co-helper,tune-helper,tune-profile-helper,intel-uv-helper,legion-gpu-helper,legion-firmware-helper,lighting-helper,amdgpu-helper,nvcurve-sensors,legion-ec-sensors,health-reader,centurion-boot-guard,netguard-helper,backup-helper}
	exeopts -m0700
	doexe "${r}"/nvcurve-root-helper
	exeopts -m0755
	dobin "${r}"/{nvcurve,centurion-gamemode,centurion-intel-uv,centurion-autotune,centurion-calibrate,centurion-netguard,centurion-display,centurion-cooltest,centurion-gpustress}

	cmake_src_install

	# polkit: action policy + allowlist rules. The rules go under /usr/share
	# (as on Arch/Fedora): a copy in /etc/polkit-1/rules.d overrides them.
	_centurion_subst packaging/polkit/com.centurion.policy "${T}"/com.centurion.policy
	_centurion_subst packaging/polkit/49-centurion.rules "${T}"/49-centurion.rules
	insinto /usr/share/polkit-1/actions
	doins "${T}"/com.centurion.policy
	insinto /usr/share/polkit-1/rules.d
	doins "${T}"/49-centurion.rules

	local s
	for s in nvcurve-autoload centurion-tune centurion-intel-uv centurion-intel-uv-daemon \
		centurion-boot-guard centurion-netguard centurion-calibrate-boot; do
		_centurion_subst packaging/openrc/${s} "${T}"/${s}.initd
		newinitd "${T}"/${s}.initd ${s}
		_centurion_subst packaging/systemd/${s}.service "${T}"/${s}.service
		systemd_dounit "${T}"/${s}.service
	done

	# Sleep hooks: Intel undervolt resume (elogind; systemd uses the unit's
	# sleep targets) and the per-sleep log for Health -> Sleep.
	_centurion_subst packaging/sleep/centurion-intel-uv "${T}"/50-centurion-intel-uv
	exeinto /usr/lib/elogind/system-sleep
	doexe "${T}"/50-centurion-intel-uv
	newexe packaging/sleep/centurion-sleep-log 40-centurion-sleep-log
	: "$(systemd_get_sleepdir)"
	exeinto "${_#"${EPREFIX}"}"
	doexe packaging/sleep/centurion-sleep-log

	udev_dorules packaging/udev/70-centurion-lighting.rules
	_centurion_subst packaging/udev/90-centurion-dgpu.rules "${T}"/90-centurion-dgpu.rules
	udev_dorules "${T}"/90-centurion-dgpu.rules

	keepdir /etc/nvcurve/profiles

	dodoc README.md NOTICE DISCLAIMER.md docs/{HANDBOOK,TECHNICAL,AUTOTUNE,ROADMAP}.md
}

pkg_prerm() {
	# Removal (not upgrade): put every tuned value back while the helper is
	# still there (same as uninstall.sh and the Arch/Fedora packages).
	if [[ -z ${REPLACED_BY_VERSION} && -z ${ROOT} ]]; then
		printf '%s' '{"op":"restore"}' \
			| "${EPREFIX}"/usr/libexec/${PN}/tune-helper >/dev/null 2>&1
	fi
}

pkg_postinst() {
	xdg_pkg_postinst
	udev_reload

	if [[ -e ${EROOT}/etc/polkit-1/rules.d/49-centurion.rules ]]; then
		ewarn "${EROOT}/etc/polkit-1/rules.d/49-centurion.rules exists and overrides the packaged"
		ewarn "  ${EROOT}/usr/share/polkit-1/rules.d/49-centurion.rules. If it is a leftover from an"
		ewarn "  older install (it may still match the whole com.centurion.* namespace), remove it."
	fi
	if [[ -n $(find "${EROOT}"/$(get_libdir)/elogind/system-sleep -name '*centurion*' 2>/dev/null) ]]; then
		ewarn "Old Centurion sleep hooks found in ${EROOT}/$(get_libdir)/elogind/system-sleep;"
		ewarn "  they now live in ${EROOT}/usr/lib/elogind/system-sleep. Remove the old copies."
	fi

	elog "Nothing is enabled automatically; every boot service is opt-in:"
	elog "  centurion-boot-guard       records clean shutdowns (recommended)"
	elog "  nvcurve-autoload           boot-time GPU curve (★ Default in the NVIDIA tab)"
	elog "  centurion-tune             Optimizations boot preset (⏻ Apply at boot)"
	elog "  centurion-intel-uv         Intel undervolt boot/resume profile"
	elog "  centurion-intel-uv-daemon  AC/battery switch, periodic re-apply, hwphint"
	elog "  centurion-netguard         Health → Network (IP blacklist, Wine/.exe guard)"
	elog "  OpenRC:  rc-update add <name> default   (centurion-tune / -intel-uv: boot)"
	elog "  systemd: systemctl enable <name>.service"
	elog "centurion-calibrate-boot is one-shot: scheduling it from Autotune adds it to the"
	elog "  next boot and it removes itself afterwards (no rc-update needed)."
	elog "Keyboard lighting: re-login or re-plug once so the uaccess udev rule applies to"
	elog "  the keyboard's hidraw node; until then the Lighting tab goes through pkexec."
	elog "Lutris hooks: /usr/bin/centurion-gamemode PRE / POST / RUN (Game launch sub-tab)."

	optfeature "GPU power limits, Custom fan curve, GPU mode and EC sensors" sys-power/acpi_call
	optfeature "the NVIDIA tab (NvAPI / NVML)" x11-drivers/nvidia-drivers
	optfeature "the Ryzen tab under Secure Boot / lockdown (needs ryzen_smu >= 0.1.7)" app-admin/ryzen_smu
	optfeature "extra AMD CPU sensors (overlays, e.g. ::guru)" sys-kernel/zenpower3 sys-kernel/zenergy
	optfeature "faster Health → Backup compression" app-arch/pigz
	optfeature "zstd Backup compression" app-arch/zstd
	optfeature "xz Backup compression" app-arch/xz-utils
	optfeature "Scenes → refresh rate on X11" x11-apps/xrandr
	optfeature "Scenes → refresh rate on KDE Plasma" kde-plasma/libkscreen
	optfeature "Scenes → refresh rate on wlroots compositors (::guru)" gui-apps/wlr-randr
	optfeature "Scenes → screen brightness without logind (::guru)" app-misc/brightnessctl
	optfeature "the active scene / power profile in games (::guru)" games-util/mangohud
	optfeature "keyboard legends from the X11 layout" x11-apps/setxkbmap
	optfeature "System tools → DMI" sys-apps/dmidecode
	optfeature "System tools → disk health" sys-apps/smartmontools sys-apps/nvme-cli
	optfeature "System tools → sensors" sys-apps/lm-sensors
	optfeature "System tools → cpupower" sys-power/cpupower
	optfeature "System tools → PCI devices" sys-apps/pciutils
	optfeature "System tools → USB devices" sys-apps/usbutils
	optfeature "System tools → NUMA" sys-process/numactl
	optfeature "Health → Curve stability → GPU test (Vulkan loader)" media-libs/vulkan-loader
	optfeature "System tools → Vulkan info" dev-util/vulkan-tools
	optfeature "System tools → OpenGL info" x11-apps/mesa-progs
}

pkg_postrm() {
	xdg_pkg_postrm
	udev_reload
}
