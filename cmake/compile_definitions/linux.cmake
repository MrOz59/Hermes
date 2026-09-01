# linux specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="linux")

# AppImage
if(${SUNSHINE_BUILD_APPIMAGE})
    # use relative assets path for AppImage
    string(REPLACE "${CMAKE_INSTALL_PREFIX}" ".${CMAKE_INSTALL_PREFIX}" SUNSHINE_ASSETS_DIR_DEF ${SUNSHINE_ASSETS_DIR})
endif()

# cuda
set(CUDA_FOUND OFF)
if(${SUNSHINE_ENABLE_CUDA})
    include(CheckLanguage)
    check_language(CUDA)

    if(NOT CMAKE_CUDA_COMPILER)
        # Coming up empty usually means the environment never exported the
        # toolkit rather than that there is none: Arch installs nvcc as
        # /opt/cuda/bin/nvcc and only puts it on PATH from
        # /etc/profile.d/cuda.sh, which a build environment that is not a login
        # shell never sources. Look where the distros actually install it, then
        # let CheckLanguage judge again, so a toolkit that really is missing
        # still reports as missing.
        FIND_CUDA_TOOLKIT(SUNSHINE_CUDA_COMPILER SUNSHINE_CUDA_HOST_COMPILER)
        if(SUNSHINE_CUDA_COMPILER)
            message(STATUS "Found a CUDA compiler outside PATH: ${SUNSHINE_CUDA_COMPILER}")
            set(ENV{CUDACXX} "${SUNSHINE_CUDA_COMPILER}")
            if(SUNSHINE_CUDA_HOST_COMPILER)
                message(STATUS "CUDA host compiler: ${SUNSHINE_CUDA_HOST_COMPILER}")
                set(ENV{CUDAHOSTCXX} "${SUNSHINE_CUDA_HOST_COMPILER}")
            endif()

            # Drop the failed result, cached and local, or the check is skipped.
            unset(CMAKE_CUDA_COMPILER)
            unset(CMAKE_CUDA_COMPILER CACHE)
            check_language(CUDA)
        endif()
    endif()

    if(CMAKE_CUDA_COMPILER)
        set(CUDA_FOUND ON)
        enable_language(CUDA)

        # Distro-packaged CUDA toolkits live in /usr, so dependency include dirs
        # resolve to /usr/include and CMake passes `-isystem /usr/include` to
        # nvcc, breaking GCC's include_next chain (cmath -> math.h not found).
        # Marking it implicit makes CMake drop it from the command line.
        list(APPEND CMAKE_CUDA_IMPLICIT_INCLUDE_DIRECTORIES "/usr/include")

        message(STATUS "CUDA Compiler Version: ${CMAKE_CUDA_COMPILER_VERSION}")
        set(CMAKE_CUDA_ARCHITECTURES "")

        # https://docs.nvidia.com/cuda/archive/12.0.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 75 80 86 87 89 90)
        else()
            message(FATAL_ERROR
                    "Sunshine requires a minimum CUDA Compiler version of 12.0.
                    Found version: ${CMAKE_CUDA_COMPILER_VERSION}"
            )
        endif()

        # https://docs.nvidia.com/cuda/archive/12.8.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 100 101 120)
        endif()

        # https://docs.nvidia.com/cuda/archive/12.9.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.9)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 103 121)
        endif()

        # https://docs.nvidia.com/cuda/archive/13.0.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
            list(REMOVE_ITEM CMAKE_CUDA_ARCHITECTURES 101)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 110)
        else()
            list(APPEND CMAKE_CUDA_ARCHITECTURES 50 52 53 60 61 62 70 72)
        endif()

        # sort the architectures
        list(SORT CMAKE_CUDA_ARCHITECTURES COMPARE NATURAL)

        # message(STATUS "CUDA NVCC Flags: ${CUDA_NVCC_FLAGS}")
        message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    elseif(${CUDA_FAIL_ON_MISSING})
        message(FATAL_ERROR
                "CUDA not found.
                Point the build at a toolkit with '-DCMAKE_CUDA_COMPILER=/path/to/nvcc', adding
                '-DCMAKE_CUDA_HOST_COMPILER=/path/to/g++-<version>' if your GCC is newer than the toolkit
                supports. CMakeFiles/CMakeConfigureLog.yaml records why the CUDA check failed.
                If this is intentional, set '-DSUNSHINE_ENABLE_CUDA=OFF' or '-DCUDA_FAIL_ON_MISSING=OFF'"
        )
    endif()
endif()
if(CUDA_FOUND)
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvfbc")
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cu"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/nvfbc/NvFBC.h")

    add_compile_definitions(SUNSHINE_BUILD_CUDA)
endif()

# libdrm is required by KMS, Wayland, and the EVDI virtual-display backend.
# The backend is compiled unconditionally on Linux, so keep this dependency
# available even in minimal builds with the KMS and Wayland capture options off.
find_package(LIBDRM REQUIRED)
if(LIBDRM_FOUND)
    include_directories(SYSTEM ${LIBDRM_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBDRM_LIBRARIES})
endif()

# drm
if(${SUNSHINE_ENABLE_DRM})
    find_package(LIBCAP REQUIRED)
else()
    set(LIBCAP_FOUND OFF)
endif()
if(LIBDRM_FOUND AND LIBCAP_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_DRM)
    include_directories(SYSTEM ${LIBCAP_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBCAP_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/kmsgrab.cpp")
    list(APPEND SUNSHINE_DEFINITIONS EGL_NO_X11=1)
endif()

# The brokers' client halves are always built: they are a few hundred bytes of
# socket code each, and whether a broker is there to answer is a runtime
# question.
list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/card_broker.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/card_broker.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_broker.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_broker.cpp")

# evdev
include(dependencies/libevdev_Sunshine)

# vaapi
if(${SUNSHINE_ENABLE_VAAPI})
    find_package(Libva REQUIRED)
else()
    set(LIBVA_FOUND OFF)
endif()
if(LIBVA_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_VAAPI)
    include_directories(SYSTEM ${LIBVA_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${LIBVA_LIBRARIES} ${LIBVA_DRM_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.cpp")
endif()

# wayland
if(${SUNSHINE_ENABLE_WAYLAND})
    find_package(Wayland REQUIRED)
else()
    set(WAYLAND_FOUND OFF)
endif()
if(WAYLAND_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_WAYLAND)

    if(NOT SUNSHINE_SYSTEM_WAYLAND_PROTOCOLS)
        set(WAYLAND_PROTOCOLS_DIR "${CMAKE_SOURCE_DIR}/third-party/wayland-protocols")
    else()
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols REQUIRED)
    endif()

    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/xdg-output" xdg-output-unstable-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/linux-dmabuf" linux-dmabuf-unstable-v1)
    # ext-image-copy-capture is wlr-screencopy's successor and the only capture
    # protocol KWin and GNOME speak, so it is what makes a session on either of
    # them capturable at all. Both files come from the upstream wayland-protocols
    # checkout already vendored here; neither needs a fork.
    # ext-image-capture-source also declares a source that names a foreign
    # toplevel, so its generated code refers to that interface whether or not
    # Hermes ever captures a window. Nothing here binds it; it is here to link.
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "staging/ext-foreign-toplevel-list" ext-foreign-toplevel-list-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "staging/ext-image-capture-source" ext-image-capture-source-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "staging/ext-image-copy-capture" ext-image-copy-capture-v1)
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/third-party/wlr-protocols" "unstable" wlr-screencopy-unstable-v1)
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/third-party/wlr-protocols" "unstable" wlr-output-management-unstable-v1)

    include_directories(
            SYSTEM
            ${WAYLAND_INCLUDE_DIRS}
            ${CMAKE_BINARY_DIR}/generated-src
    )

    list(APPEND PLATFORM_LIBRARIES ${WAYLAND_LIBRARIES} gbm)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wlgrab.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.cpp")
endif()

# x11
if(${SUNSHINE_ENABLE_X11})
    find_package(X11 REQUIRED)
else()
    set(X11_FOUND OFF)
endif()
if(X11_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_X11)
    include_directories(SYSTEM ${X11_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${X11_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.cpp")
endif()

if(NOT ${CUDA_FOUND}
        AND NOT ${WAYLAND_FOUND}
        AND NOT ${X11_FOUND}
        AND NOT (${LIBDRM_FOUND} AND ${LIBCAP_FOUND})
        AND NOT ${LIBVA_FOUND})
    message(FATAL_ERROR "Couldn't find either cuda, wayland, x11, (libdrm and libcap), or libva")
endif()

# tray icon
if(${SUNSHINE_ENABLE_TRAY})
    # flatpak icons must be prefixed with the app id or they will not be included in the flatpak
    if(${SUNSHINE_BUILD_FLATPAK})
        set(SUNSHINE_TRAY_PREFIX "${PROJECT_FQDN}")
    else()
        # Must match the RENAME used for the status icons in cmake/packaging/linux.cmake.
        set(SUNSHINE_TRAY_PREFIX "hermes")
    endif()
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_TRAY_PREFIX="${SUNSHINE_TRAY_PREFIX}")
else()
    set(SUNSHINE_TRAY 0)
    message(STATUS "Tray icon disabled")
endif()

# These need to be set before adding the inputtino subdirectory in order for them to be picked up
set(LIBEVDEV_CUSTOM_INCLUDE_DIR "${EVDEV_INCLUDE_DIR}")
set(LIBEVDEV_CUSTOM_LIBRARY "${EVDEV_LIBRARY}")

add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/inputtino")
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES inputtino::libinputtino)
file(GLOB_RECURSE INPUTTINO_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/linux/input/inputtino*.h
        ${CMAKE_SOURCE_DIR}/src/platform/linux/input/inputtino*.cpp)
list(APPEND PLATFORM_TARGET_FILES ${INPUTTINO_SOURCES})

# build libevdev before the libinputtino target
if(EXTERNAL_PROJECT_LIBEVDEV_USED)
    add_dependencies(libinputtino libevdev)
endif()

# AppImage and Flatpak
if (${SUNSHINE_BUILD_APPIMAGE})
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_BUILD_APPIMAGE=1)
endif ()
if (${SUNSHINE_BUILD_FLATPAK})
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_BUILD_FLATPAK=1)
endif ()

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/virtual_display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/virtual_display.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/glad/src/egl.c"
        "${CMAKE_SOURCE_DIR}/third-party/glad/src/gl.c"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/EGL/eglplatform.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/KHR/khrplatform.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/glad/gl.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/glad/egl.h")

list(APPEND PLATFORM_LIBRARIES
        dl
        pulse
        pulse-simple)

include_directories(
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party/glad/include")
