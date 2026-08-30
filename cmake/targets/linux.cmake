# linux specific target definitions

# The card broker reaches the Hermes-KMS driver's configfs group, which is
# root's, so it is a program of its own rather than anything the streaming host
# links: a few hundred lines over one socket, with none of Hermes' dependencies
# and no reason to grow any.
if(SUNSHINE_BUILD_CARD_BROKER)
    add_executable(hermes-kms-card-broker "${CMAKE_SOURCE_DIR}/tools/hermes-kms-card-broker.cpp")
    set_target_properties(hermes-kms-card-broker PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON)
    target_compile_options(hermes-kms-card-broker PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
endif()

# The session broker creates the Unix account a client's isolated session runs
# as, which is the boundary the whole mode rests on. Like the card broker it is
# a separate program on purpose: it runs as root, and the less it shares with
# the streaming host the smaller the surface that root is reachable through.
if(SUNSHINE_BUILD_SESSION_BROKER)
    add_executable(hermes-session-broker "${CMAKE_SOURCE_DIR}/tools/hermes-session-broker.cpp")
    set_target_properties(hermes-session-broker PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON)
    target_compile_options(hermes-session-broker PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
endif()

# The Game Mode console is a window, and the streaming host is a service: a
# service has no business owning a window, and a console that crashes must not
# take a stream down with it. Qt and libcurl are already required by the build -
# the tray links the first, the host links the second - so being a separate
# program costs a binary and no dependency.
if(SUNSHINE_BUILD_GAMEMODE_CONSOLE)
    # This project treats CMAKE_MODULE_PATH as one path and interpolates it -
    # `include(${CMAKE_MODULE_PATH}/packaging/common.cmake)` - while CMake
    # treats it as a list that any find_package may append to. Qt's config does
    # append to it, which turns every later include() into a path with a
    # semicolon in the middle of it. The tray gets away with calling
    # find_package(Qt6) because it does so inside add_subdirectory, where the
    # change is scoped to that directory; this call is at top level and is not.
    # Restoring the variable is narrower than renaming it everywhere it is
    # misused, which is a separate cleanup.
    set(_hermes_saved_module_path "${CMAKE_MODULE_PATH}")
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(NOT Qt6_FOUND)
        find_package(Qt5 QUIET COMPONENTS Widgets)
    endif()
    set(CMAKE_MODULE_PATH "${_hermes_saved_module_path}")
    unset(_hermes_saved_module_path)

    if(Qt6_FOUND OR Qt5_FOUND)
        add_executable(hermes-gamemode "${CMAKE_SOURCE_DIR}/tools/hermes-gamemode.cpp")
        set_target_properties(hermes-gamemode PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                POSITION_INDEPENDENT_CODE ON)
        target_compile_options(hermes-gamemode PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
        target_include_directories(hermes-gamemode SYSTEM PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_directories(hermes-gamemode PRIVATE ${CURL_LIBRARY_DIRS})
        if(Qt6_FOUND)
            target_link_libraries(hermes-gamemode PRIVATE Qt6::Widgets)
        else()
            target_link_libraries(hermes-gamemode PRIVATE Qt5::Widgets)
        endif()
        target_link_libraries(hermes-gamemode PRIVATE
                nlohmann_json::nlohmann_json
                ${CURL_LIBRARIES})
    else()
        message(STATUS "Qt not found; the Game Mode console will not be built")
        set(SUNSHINE_BUILD_GAMEMODE_CONSOLE OFF)
    endif()
endif()
