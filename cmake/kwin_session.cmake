# Native private KWin transport. Build without changing any installed desktop
# files or selecting this backend for existing streams.
function(hermes_add_kwin_transport)
    if(NOT SUNSHINE_ENABLE_WAYLAND)
        return()
    endif()
    pkg_check_modules(HERMES_KWIN_WAYLAND QUIET IMPORTED_TARGET wayland-client>=1.20)
    pkg_check_modules(HERMES_KWIN_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3>=0.3.44)
    find_package(PlasmaWaylandProtocols CONFIG QUIET)
    if(NOT HERMES_KWIN_WAYLAND_FOUND OR NOT HERMES_KWIN_PIPEWIRE_FOUND OR NOT PlasmaWaylandProtocols_FOUND)
        message(STATUS "Private KWin transport unavailable: requires Wayland, PipeWire, and plasma-wayland-protocols development files")
        return()
    endif()
    set(protocol_dir "${PLASMA_WAYLAND_PROTOCOLS_DIR}")
    set(capture_xml "${protocol_dir}/zkde-screencast-unstable-v1.xml")
    if(NOT EXISTS "${capture_xml}")
        message(STATUS "Private KWin transport unavailable: KDE screencast protocol is missing")
        return()
    endif()
    file(READ "${capture_xml}" protocol_text)
    if(NOT protocol_text MATCHES "name=\"zkde_screencast_unstable_v1\" version=\"([0-9]+)\"")
        message(FATAL_ERROR "Cannot determine KDE screencast protocol version")
    endif()
    if(CMAKE_MATCH_1 LESS 6)
        message(STATUS "Private KWin transport requires screencast version 6 for stable PipeWire object identities")
        return()
    endif()
    find_program(hermes_wayland_scanner wayland-scanner REQUIRED)
    find_package(Threads REQUIRED)
    set(generated "${CMAKE_BINARY_DIR}/generated-src/hermes-kwin")
    file(MAKE_DIRECTORY "${generated}")
    set(protocol_sources)
    foreach(protocol IN ITEMS fake-input zkde-screencast-unstable-v1)
        add_custom_command(
            OUTPUT "${generated}/${protocol}.h" "${generated}/${protocol}.c"
            COMMAND "${hermes_wayland_scanner}" client-header "${protocol_dir}/${protocol}.xml" "${generated}/${protocol}.h"
            COMMAND "${hermes_wayland_scanner}" private-code "${protocol_dir}/${protocol}.xml" "${generated}/${protocol}.c"
            DEPENDS "${protocol_dir}/${protocol}.xml"
            VERBATIM)
        list(APPEND protocol_sources "${generated}/${protocol}.c" "${generated}/${protocol}.h")
    endforeach()
    add_library(hermes-kwin-transport STATIC
        "${CMAKE_SOURCE_DIR}/src/platform/linux/kwin_session.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pipewire_session.cpp"
        ${protocol_sources})
    set_target_properties(hermes-kwin-transport PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
    target_include_directories(hermes-kwin-transport PRIVATE "${generated}")
    target_include_directories(hermes-kwin-transport PUBLIC "${CMAKE_SOURCE_DIR}/src/platform/linux")
    target_link_libraries(hermes-kwin-transport PUBLIC
        PkgConfig::HERMES_KWIN_WAYLAND PkgConfig::HERMES_KWIN_PIPEWIRE Threads::Threads)
    target_link_libraries(sunshine hermes-kwin-transport)
    set(adapter "${CMAKE_SOURCE_DIR}/src/platform/linux/kwin_display.cpp")
    target_sources(sunshine PRIVATE "${adapter}")
    # The unit-test executable compiles the platform source list separately.
    set(SUNSHINE_TARGET_FILES ${SUNSHINE_TARGET_FILES} "${adapter}" PARENT_SCOPE)
    set(SUNSHINE_EXTERNAL_LIBRARIES ${SUNSHINE_EXTERNAL_LIBRARIES} hermes-kwin-transport PARENT_SCOPE)
    set(SUNSHINE_DEFINITIONS ${SUNSHINE_DEFINITIONS} SUNSHINE_BUILD_KWIN_TRANSPORT PARENT_SCOPE)


    add_executable(hermes-detached-session
        "${CMAKE_SOURCE_DIR}/tools/hermes-detached-session.cpp")
    target_link_libraries(hermes-detached-session PRIVATE Threads::Threads)
    set_target_properties(hermes-detached-session PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

    # Explicit build/run targets; neither executable is installed or launched
    # automatically. The check needs authorization in the private desktop only.
    add_executable(hermes-kwin-transport-check EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tools/hermes-kwin-transport-check.cpp")
    target_link_libraries(hermes-kwin-transport-check PRIVATE hermes-kwin-transport)
    set_target_properties(hermes-kwin-transport-check PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_executable(hermes-session-frame-check EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tools/hermes-session-frame-check.cpp")
    target_include_directories(hermes-session-frame-check PRIVATE "${CMAKE_SOURCE_DIR}/src/platform/linux")
    set_target_properties(hermes-session-frame-check PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    message(STATUS "Private KWin transport enabled (explicit socket endpoints, screencast v6)")
endfunction()

hermes_add_kwin_transport()
