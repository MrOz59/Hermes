# linux specific macros

# GEN_WAYLAND: args = `filename`
macro(GEN_WAYLAND wayland_directory subdirectory filename)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated-src)

    message("wayland-scanner private-code \
${wayland_directory}/${subdirectory}/${filename}.xml \
${CMAKE_BINARY_DIR}/generated-src/${filename}.c")
    message("wayland-scanner client-header \
${wayland_directory}/${subdirectory}/${filename}.xml \
${CMAKE_BINARY_DIR}/generated-src/${filename}.h")
    execute_process(
            COMMAND wayland-scanner private-code
            ${wayland_directory}/${subdirectory}/${filename}.xml
            ${CMAKE_BINARY_DIR}/generated-src/${filename}.c
            COMMAND wayland-scanner client-header
            ${wayland_directory}/${subdirectory}/${filename}.xml
            ${CMAKE_BINARY_DIR}/generated-src/${filename}.h

            RESULT_VARIABLE EXIT_INT
    )

    if(NOT ${EXIT_INT} EQUAL 0)
        message(FATAL_ERROR "wayland-scanner failed")
    endif()

    list(APPEND PLATFORM_TARGET_FILES
            ${CMAKE_BINARY_DIR}/generated-src/${filename}.c
            ${CMAKE_BINARY_DIR}/generated-src/${filename}.h)
endmacro()

# FIND_CUDA_TOOLKIT: args = `nvcc_variable` `host_compiler_variable`
#
# Locates a CUDA toolkit that the environment never exported, for when
# `check_language(CUDA)` comes up empty. `nvcc_variable` receives the compiler,
# and `host_compiler_variable` a GCC old enough to run under it, left empty
# when the system one will do.
function(FIND_CUDA_TOOLKIT nvcc_variable host_compiler_variable)
    set(${nvcc_variable} "" PARENT_SCOPE)
    set(${host_compiler_variable} "" PARENT_SCOPE)

    # Newest first, so a machine carrying several toolkits gets the latest.
    file(GLOB cuda_versioned_prefixes "/opt/cuda-*" "/usr/local/cuda-*")
    list(SORT cuda_versioned_prefixes COMPARE NATURAL ORDER DESCENDING)

    find_program(CUDA_NVCC_EXECUTABLE
            NAMES nvcc
            HINTS
            ENV CUDA_PATH
            ENV CUDA_HOME
            ENV CUDAToolkit_ROOT
            PATHS
            /opt/cuda
            /usr/local/cuda
            ${cuda_versioned_prefixes}
            PATH_SUFFIXES bin
            DOC "CUDA compiler, for toolkits that are not on PATH")
    mark_as_advanced(CUDA_NVCC_EXECUTABLE)
    if(NOT CUDA_NVCC_EXECUTABLE)
        return()
    endif()
    set(${nvcc_variable} "${CUDA_NVCC_EXECUTABLE}" PARENT_SCOPE)

    # An explicitly chosen host compiler always wins, whether it was aimed at
    # CMake or at nvcc itself.
    if(DEFINED CMAKE_CUDA_HOST_COMPILER
            OR NOT "$ENV{CUDAHOSTCXX}" STREQUAL ""
            OR NOT "$ENV{NVCC_CCBIN}" STREQUAL "")
        return()
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()

    # nvcc refuses to run under a GCC newer than the toolkit knows about. The
    # cutoff sits in the toolkit's own header, as an `#error` guarded by
    # `#if __GNUC__ > <major>`; distros that patch that error out (Arch is one)
    # build fine with a newer GCC, so a stripped header means no cutoff.
    cmake_path(GET CUDA_NVCC_EXECUTABLE PARENT_PATH cuda_bin_dir)
    cmake_path(GET cuda_bin_dir PARENT_PATH cuda_root)
    set(host_config "${cuda_root}/include/crt/host_config.h")
    if(NOT EXISTS "${host_config}")
        return()
    endif()

    file(STRINGS "${host_config}" gnu_version_error REGEX "^#error.*unsupported GNU version")
    file(STRINGS "${host_config}" gnu_version_guard REGEX "^#if __GNUC__ > [0-9]+$" LIMIT_COUNT 1)
    if(NOT gnu_version_error OR NOT gnu_version_guard)
        return()
    endif()

    string(REGEX MATCH "[0-9]+" newest_supported_gnu "${gnu_version_guard}")
    string(REGEX MATCH "^[0-9]+" host_gnu "${CMAKE_CXX_COMPILER_VERSION}")
    if(NOT host_gnu GREATER newest_supported_gnu)
        return()
    endif()

    # Distros shipping a GCC this new package the older releases next to it.
    math(EXPR oldest_wanted_gnu "${newest_supported_gnu} - 3")
    set(host_compiler_names "")
    foreach(major RANGE ${oldest_wanted_gnu} ${newest_supported_gnu})
        list(APPEND host_compiler_names "g++-${major}")
    endforeach()
    list(REVERSE host_compiler_names)

    find_program(CUDA_HOST_CXX_EXECUTABLE
            NAMES ${host_compiler_names}
            DOC "C++ compiler nvcc runs, when the system one is too new for the toolkit")
    mark_as_advanced(CUDA_HOST_CXX_EXECUTABLE)
    if(CUDA_HOST_CXX_EXECUTABLE)
        set(${host_compiler_variable} "${CUDA_HOST_CXX_EXECUTABLE}" PARENT_SCOPE)
    else()
        message(WARNING
                "GCC ${host_gnu} is newer than this CUDA toolkit supports (up to GCC \
${newest_supported_gnu}), and no g++-${newest_supported_gnu} is installed to build the CUDA code with.")
    endif()
endfunction()
