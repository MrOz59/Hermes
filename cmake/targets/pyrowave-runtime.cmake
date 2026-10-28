# Private PyroWave runtime installation, shared with the loader regression test.
function(sunshine_bundle_pyrowave_runtime target)
    pkg_get_variable(PYROWAVE_RUNTIME_DIR pyrowave-shared libdir)
    file(GLOB PYROWAVE_RUNTIME_FILES "${PYROWAVE_RUNTIME_DIR}/libpyrowave-shared.so*")
    if(NOT PYROWAVE_RUNTIME_FILES)
        message(FATAL_ERROR "No shared PyroWave runtime found to bundle")
    endif()
    install(FILES ${PYROWAVE_RUNTIME_FILES} DESTINATION "${CMAKE_INSTALL_LIBDIR}/hermes")
    # File capabilities enable glibc AT_SECURE. An $ORIGIN path can then be
    # discarded unless it resolves under that distro's trusted library roots.
    # Native packages have a fixed prefix: use the absolute private directory,
    # without DESTDIR or the build-time SDK prefix. Keep portable bundles relative.
    if(SUNSHINE_BUILD_APPIMAGE OR SUNSHINE_BUILD_FLATPAK)
        file(RELATIVE_PATH PYROWAVE_RELATIVE_LIBDIR
                "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/hermes")
        set(PYROWAVE_INSTALL_RPATH "$ORIGIN/${PYROWAVE_RELATIVE_LIBDIR}")
    else()
        set(PYROWAVE_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}/hermes")
    endif()
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${PYROWAVE_INSTALL_RPATH}")
    pkg_get_variable(PYROWAVE_SDK_PREFIX pyrowave-shared prefix)
    if(EXISTS "${PYROWAVE_SDK_PREFIX}/share/licenses/pyrowave")
        install(DIRECTORY "${PYROWAVE_SDK_PREFIX}/share/licenses/pyrowave"
                DESTINATION share/licenses/hermes)
    endif()
endfunction()
