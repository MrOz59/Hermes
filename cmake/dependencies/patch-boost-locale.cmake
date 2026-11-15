# Boost 1.89 includes include/boost/locale.hpp in sources_inc, outside the
# include/boost/locale source-group tree. Use the common parent of all headers.
# This only changes IDE grouping; it does not change the library's sources.
set(locale_cmake "${BOOST_SOURCE_DIR}/libs/locale/CMakeLists.txt")
file(READ "${locale_cmake}" contents)
set(old_group [=[source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/locale" PREFIX "headers" FILES ${sources_inc})]=])
set(new_group [=[source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost" PREFIX "headers" FILES ${sources_inc})]=])
string(FIND "${contents}" "${old_group}" old_position)
if(NOT old_position EQUAL -1)
    string(REPLACE "${old_group}" "${new_group}" contents "${contents}")
    file(WRITE "${locale_cmake}" "${contents}")
else()
    string(FIND "${contents}" "${new_group}" new_position)
    if(new_position EQUAL -1)
        message(FATAL_ERROR "Boost.Locale source-group layout changed; review the 1.89 workaround")
    endif()
endif()
