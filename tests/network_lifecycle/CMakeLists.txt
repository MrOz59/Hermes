cmake_minimum_required(VERSION 3.20)
project(hermes_network_lifecycle LANGUAGES C CXX)

# A focused target using the production networking code. No display server,
# capture/encoder SDK, or GPU is needed to exercise socket ownership.
get_filename_component(HERMES_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Boost REQUIRED COMPONENTS log system)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${HERMES_ROOT}/third-party/googletest" googletest EXCLUDE_FROM_ALL)
set(ENET_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${HERMES_ROOT}/third-party/moonlight-common-c/enet" enet EXCLUDE_FROM_ALL)

add_executable(network_lifecycle_tests
  "${HERMES_ROOT}/src/network.cpp"
  "${HERMES_ROOT}/tests/unit/test_network.cpp"
  "${HERMES_ROOT}/tests/unit/test_thread_safe.cpp"
  "${HERMES_ROOT}/tests/unit/test_clipboard_process.cpp"
  support.cpp
)
target_compile_features(network_lifecycle_tests PRIVATE cxx_std_23)
target_compile_definitions(network_lifecycle_tests PRIVATE BOOST_LOG_DYN_LINK HERMES_NETWORK_LIFECYCLE_TESTS)
target_include_directories(network_lifecycle_tests PRIVATE
  "${HERMES_ROOT}"
  "${HERMES_ROOT}/third-party/moonlight-common-c/enet/include"
)
target_link_libraries(network_lifecycle_tests PRIVATE
  enet Boost::log Boost::system Threads::Threads nlohmann_json::nlohmann_json GTest::gtest_main
)

option(HERMES_TEST_SANITIZERS "Enable address, leak, and undefined-behavior checks" OFF)
if(HERMES_TEST_SANITIZERS)
  foreach(target network_lifecycle_tests enet)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

enable_testing()
add_test(NAME network_lifecycle COMMAND network_lifecycle_tests)
