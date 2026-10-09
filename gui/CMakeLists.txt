cmake_minimum_required(VERSION 3.19)
project(centurion VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network Svg)

# Where the root helpers live. Must match the polkit exec.path annotations.
set(CENTURION_HELPER_DIR "/usr/libexec/centurion" CACHE PATH "Installed root helper directory")

add_executable(centurion
    src/main.cpp
    src/mainwindow.cpp   src/mainwindow.h
    src/theme.cpp        src/theme.h
    src/privileged.cpp   src/privileged.h
    src/platformprofile.cpp src/platformprofile.h
    src/sysinfo.cpp      src/sysinfo.h
    src/hometab.cpp      src/hometab.h
    src/fancurvedialog.cpp src/fancurvedialog.h
    src/memorydialog.cpp src/memorydialog.h
    src/deviceinfodialog.cpp src/deviceinfodialog.h
    src/fwattrtab.cpp    src/fwattrtab.h
    src/ryzentab.cpp     src/ryzentab.h
    src/inteltab.cpp     src/inteltab.h
    src/vfcurvewidget.cpp src/vfcurvewidget.h
    src/nvidiatab.cpp    src/nvidiatab.h
    src/optimizetab.cpp  src/optimizetab.h
    src/int64spinbox.cpp src/int64spinbox.h
    src/tray.cpp         src/tray.h
    src/scenes.cpp       src/scenes.h
    src/scenestab.cpp    src/scenestab.h
    src/bootadvisor.cpp  src/bootadvisor.h
    src/bundle.cpp       src/bundle.h
    src/lighting.cpp     src/lighting.h
    src/lightingtab.cpp  src/lightingtab.h
    src/zone4tab.cpp     src/zone4tab.h
    src/kblayout.cpp     src/kblayout.h
    src/amdgpu.cpp       src/amdgpu.h
    src/amdgputab.cpp    src/amdgputab.h
    src/healthtab.cpp    src/healthtab.h
    src/nvregdialog.cpp  src/nvregdialog.h
    src/systools.cpp     src/systools.h
    src/klogpage.cpp     src/klogpage.h
    src/dgpupage.cpp     src/dgpupage.h
    src/netpage.cpp      src/netpage.h
    src/backuppage.cpp   src/backuppage.h
)
# Theme glyphs (checkbox tick, spin/combo chevrons), @2x for HiDPI. Compiled
# with qt_add_resources, not AUTORCC: CMake's Makefile generator runs the
# AUTORCC step through /bin/sh without escaping '(' — a checkout under a path
# like "files (1)" failed with a shell syntax error.
qt_add_resources(centurion theme
    PREFIX /theme
    BASE data/theme
    FILES
        data/theme/check.png data/theme/check@2x.png
        data/theme/chev-down.png data/theme/chev-down@2x.png
        data/theme/chev-up.png data/theme/chev-up@2x.png
        data/theme/chev-down-off.png data/theme/chev-down-off@2x.png
        data/theme/chev-up-off.png data/theme/chev-up-off@2x.png)
# App icon (window, tray, header mark): the same SVG that is installed to hicolor.
qt_add_resources(centurion appicon
    PREFIX /
    BASE data
    FILES data/centurion.svg)

target_compile_definitions(centurion PRIVATE
    CENTURION_VERSION="${PROJECT_VERSION}"
    CENTURION_HELPER_DIR="${CENTURION_HELPER_DIR}"
    QT_NO_KEYWORDS)
target_compile_options(centurion PRIVATE -Wall -Wextra -Wpedantic)

# ── Optimization / hardening (driven by install.sh; all optional) ────────────
option(CENTURION_LTO "Link-time optimization" ON)
option(CENTURION_NATIVE "Tune for this CPU (-march=native); binary will not run on older CPUs" OFF)
option(CENTURION_HARDEN "Stack protector, FORTIFY_SOURCE=3, CET, full RELRO, PIE" ON)
set(CENTURION_PGO "" CACHE STRING "Profile-guided optimization: '' | generate | use")
set(CENTURION_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "PGO profile directory")

if(CENTURION_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo OUTPUT _ipo_msg LANGUAGES CXX)
    if(_ipo)
        set_property(TARGET centurion PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO not supported by this toolchain: ${_ipo_msg}")
    endif()
endif()
# Smaller binary / fewer relocations: drop unreferenced sections, link only
# libraries actually used, no PLT indirection for Qt calls.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # -O3 explicitly: the value must not depend on CMAKE_CXX_FLAGS_RELEASE, which distro
    # toolchain files and CXXFLAGS in the environment are free to change.
    target_compile_options(centurion PRIVATE -O3 -ffunction-sections -fdata-sections -fno-plt)
    target_link_options(centurion PRIVATE -Wl,--gc-sections -Wl,--as-needed -Wl,-O1)
endif()
if(CENTURION_NATIVE)
    target_compile_options(centurion PRIVATE -march=native -mtune=native)
endif()
if(CENTURION_HARDEN)
    include(CheckPIESupported)
    check_pie_supported(LANGUAGES CXX)
    set_property(TARGET centurion PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_compile_options(centurion PRIVATE
        -fstack-protector-strong -fstack-clash-protection
        $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>)
    include(CheckCXXCompilerFlag)
    # libstdc++ bounds checks (vector/string/array operator[], front/back on empty containers):
    # an out-of-range index aborts instead of corrupting memory. Ignored by other C++ libraries.
    target_compile_definitions(centurion PRIVATE _GLIBCXX_ASSERTIONS)
    # Zero-initialise automatic variables (GCC 12+, clang 16+); skipped where unsupported.
    check_cxx_compiler_flag(-ftrivial-auto-var-init=zero _autoinit)
    if(_autoinit)
        target_compile_options(centurion PRIVATE -ftrivial-auto-var-init=zero)
    endif()
    check_cxx_compiler_flag(-fcf-protection=full _cet)
    if(_cet)
        target_compile_options(centurion PRIVATE -fcf-protection=full)
    endif()
    target_link_options(centurion PRIVATE -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
endif()
# GCC names each .gcda after the *object's full path*, and the instrumented
# and the final build live in different build directories (build-pgo vs
# build): without stripping that prefix the final build looked for files the
# training run never wrote, and -Wno-missing-profile hid that PGO was a no-op.
set(_pgo_prefix "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
    set(_pgo_prefix "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CENTURION_PGO STREQUAL "")
    message(WARNING "GCC < 12 has no -fprofile-prefix-path: PGO only works if generate and use share one build directory")
endif()
if(CENTURION_PGO STREQUAL "generate")
    target_compile_options(centurion PRIVATE -fprofile-generate=${CENTURION_PGO_DIR} ${_pgo_prefix})
    target_link_options(centurion PRIVATE -fprofile-generate=${CENTURION_PGO_DIR})
elseif(CENTURION_PGO STREQUAL "use")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(centurion PRIVATE -fprofile-use=${CENTURION_PGO_DIR}/default.profdata -Wno-profile-instr-unprofiled)
    else()
        target_compile_options(centurion PRIVATE -fprofile-use=${CENTURION_PGO_DIR} ${_pgo_prefix} -fprofile-partial-training -Wno-missing-profile
            # moc output lives under each build dir, so its functions' source
            # locations differ between the two builds; GCC then drops the
            # profile for that glue code (harmless) instead of failing.
            -Wno-coverage-mismatch)
    endif()
elseif(NOT CENTURION_PGO STREQUAL "")
    message(FATAL_ERROR "CENTURION_PGO must be '', 'generate' or 'use'")
endif()
target_link_libraries(centurion PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg ${CMAKE_DL_LIBS})

# Lighting tab key legends from the active XKB layout (TR-Q İ/Ş/Ğ…). Optional:
# without it the keyboard drawing uses fixed US legends.
option(CENTURION_XKB "Key legends from the active keyboard layout (libxkbcommon)" ON)
if(CENTURION_XKB)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(XKBCOMMON IMPORTED_TARGET xkbcommon)
    endif()
    if(XKBCOMMON_FOUND)
        target_link_libraries(centurion PRIVATE PkgConfig::XKBCOMMON)
        target_compile_definitions(centurion PRIVATE CENTURION_HAVE_XKB)
    else()
        message(WARNING "libxkbcommon not found: keyboard legends fall back to US")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS centurion RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/centurion.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/centurion-autostart.desktop DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart
        RENAME centurion.desktop)
install(FILES data/centurion.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# GUI unit tests (64-bit editor path). Off by default; ctest runs them.
option(CENTURION_BUILD_TESTS "Build GUI unit tests" OFF)
if(CENTURION_BUILD_TESTS)
    enable_testing()
    add_executable(centurion-gui-tests tests/int64spinbox_test.cpp src/int64spinbox.cpp src/int64spinbox.h)
    target_link_libraries(centurion-gui-tests PRIVATE Qt6::Widgets)
    target_compile_definitions(centurion-gui-tests PRIVATE QT_NO_KEYWORDS)
    add_test(NAME int64spinbox COMMAND centurion-gui-tests)
endif()
