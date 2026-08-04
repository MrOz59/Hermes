# linux specific packaging

install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}")

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)
# use symbolic link for shaders directory
file(CREATE_LINK "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/shaders"
        "${CMAKE_BINARY_DIR}/assets/shaders" COPY_ON_ERROR SYMBOLIC)

install(PROGRAMS "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/hermes-gamescope-launch"
        DESTINATION "bin")
install(PROGRAMS "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/hermes-monitor-recovery"
        DESTINATION "bin")

if(${SUNSHINE_BUILD_APPIMAGE} OR ${SUNSHINE_BUILD_FLATPAK})
    install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-hermes.rules"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/udev/rules.d")
    install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-hermes.conf"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/modules-load.d")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sunshine.service"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/systemd/user")
else()
    find_package(Systemd)
    find_package(Udev)

    if(UDEV_FOUND)
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-hermes.rules"
                DESTINATION "${UDEV_RULES_INSTALL_DIR}")
    endif()
    if(SYSTEMD_FOUND)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sunshine.service"
                DESTINATION "${SYSTEMD_USER_UNIT_INSTALL_DIR}")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-hermes.conf"
                DESTINATION "${SYSTEMD_MODULES_LOAD_DIR}")
    endif()
endif()

# Post install
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/postinst")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/postinst")

# Apply setcap for RPM
# https://github.com/coreos/rpm-ostree/discussions/5036#discussioncomment-10291071
set(CPACK_RPM_USER_FILELIST "%caps(cap_sys_admin+p) ${SUNSHINE_EXECUTABLE_PATH}")

# Dependencies
set(CPACK_DEB_COMPONENT_INSTALL ON)
# CPACK_DEBIAN_PACKAGE_SHLIBDEPS is off below, so nothing derives these from the
# binary — every library the executable links has to be listed by hand. Keep this
# in sync with `objdump -p <binary> | grep NEEDED`, and with the Requires block in
# packaging/linux/fedora/Sunshine.spec.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
            ${CPACK_DEB_PLATFORM_PACKAGE_DEPENDS} \
            debianutils, \
            libcap2, \
            libcurl4, \
            libdrm2, \
            libgbm1, \
            libevdev2, \
            libglx0, \
            libice6, \
            libnuma1, \
            libopengl0, \
            libopus0, \
            libpulse0, \
            libsm6, \
            libva2, \
            libva-drm2, \
            libva-x11-2, \
            libwayland-client0, \
            libwayland-cursor0, \
            libwayland-egl1, \
            libwayland-server0, \
            libx11-6, \
            libxext6, \
            miniupnpc, \
            openssl | libssl3")
set(CPACK_RPM_PACKAGE_REQUIRES "\
            ${CPACK_RPM_PLATFORM_PACKAGE_REQUIRES} \
            libcap >= 2.22, \
            libcurl >= 7.0, \
            libdrm >= 2.4.97, \
            libevdev >= 1.5.6, \
            libglvnd-glx, \
            libglvnd-opengl, \
            libICE, \
            libSM, \
            libva >= 2.14.0, \
            libwayland-client >= 1.20.0, \
            libwayland-cursor >= 1.20.0, \
            libwayland-egl >= 1.20.0, \
            libwayland-server >= 1.20.0, \
            libX11 >= 1.7.3.1, \
            libXext, \
            mesa-libgbm >= 25.0.7, \
            miniupnpc >= 2.2.4, \
            numactl-libs >= 2.0.14, \
            openssl >= 3.0.2, \
            opus >= 1.3, \
            pulseaudio-libs >= 10.0, \
            which >= 2.21")

if(NOT BOOST_USE_STATIC)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
                ${CPACK_DEBIAN_PACKAGE_DEPENDS}, \
                libboost-filesystem${Boost_VERSION}, \
                libboost-locale${Boost_VERSION}, \
                libboost-log${Boost_VERSION}, \
                libboost-program-options${Boost_VERSION}")
    set(CPACK_RPM_PACKAGE_REQUIRES "\
                ${CPACK_RPM_PACKAGE_REQUIRES}, \
                boost-filesystem >= ${Boost_VERSION}, \
                boost-locale >= ${Boost_VERSION}, \
                boost-log >= ${Boost_VERSION}, \
                boost-program-options >= ${Boost_VERSION}")
endif()

# This should automatically figure out dependencies, doesn't work with the current config
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

# application icon
# Installed as hermes.svg rather than apollo.svg so the package does not claim a
# path already owned by the apollo package.
if(NOT ${SUNSHINE_BUILD_FLATPAK})
    install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            RENAME "hermes.svg")
else()
    install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            RENAME "${PROJECT_FQDN}.svg")
endif()

# tray icon
if(${SUNSHINE_TRAY} STREQUAL 1)
    if(NOT ${SUNSHINE_BUILD_FLATPAK})
        install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "hermes-tray.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-playing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "hermes-playing.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-pausing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "hermes-pausing.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-locked.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "hermes-locked.svg")
    else()
        # flatpak icons must be prefixed with the app id or they will not be included in the flatpak
        install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-tray.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-playing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-playing.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-pausing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-pausing.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-locked.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-locked.svg")
    endif()

    set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
                    ${CPACK_DEBIAN_PACKAGE_DEPENDS}, \
                    libqt6svg6, \
                    libqt6widgets6")
    set(CPACK_RPM_PACKAGE_REQUIRES "\
                    ${CPACK_RPM_PACKAGE_REQUIRES}, \
                    qt6-qtbase, \
                    qt6-qtsvg")
endif()

# desktop file
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
if(NOT ${SUNSHINE_BUILD_APPIMAGE} AND NOT ${SUNSHINE_BUILD_FLATPAK})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.terminal.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
endif()

# metadata file
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")
