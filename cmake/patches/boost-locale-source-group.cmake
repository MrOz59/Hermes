#
# PATCH_COMMAND for the Boost FetchContent fallback, run with `cmake -P` from the
# root of the extracted Boost sources.
#
# Boost.Locale (1.89 through at least 1.92) puts include/boost/locale.hpp into
# source_group(TREE include/boost/locale ...). Before 4.5, CMake checked that root
# only as a string prefix, which ".../boost/locale.hpp" happens to pass; CMake 4.5
# rejects it and configuring stops with "is not a prefix of file". Boost.Locale
# fixed this on develop (boostorg/locale#273) by dropping locale.hpp from that
# list; do the same here until a Boost release carries the fix.
#
set(locale_cmakelists "${CMAKE_CURRENT_SOURCE_DIR}/libs/locale/CMakeLists.txt")
if(NOT EXISTS "${locale_cmakelists}")
    message(FATAL_ERROR "Boost.Locale patch: ${locale_cmakelists} not found")
endif()

file(READ "${locale_cmakelists}" content)

set(fix "list(REMOVE_ITEM sources_inc \"\${CMAKE_CURRENT_SOURCE_DIR}/include/boost/locale.hpp\")")
string(FIND "${content}" "${fix}" already_fixed)
if(NOT already_fixed EQUAL -1)
    return()
endif()

set(anchor "list(FILTER sources_inc EXCLUDE REGEX \"^src/\")")
string(FIND "${content}" "${anchor}" anchor_at)
if(anchor_at EQUAL -1)
    message(STATUS "Boost.Locale patch: source_group block changed upstream, nothing to patch")
    return()
endif()

string(REPLACE "${anchor}" "${anchor}\n${fix}" content "${content}")
file(WRITE "${locale_cmakelists}" "${content}")
message(STATUS "Boost.Locale patch: removed locale.hpp from the headers source_group tree")
