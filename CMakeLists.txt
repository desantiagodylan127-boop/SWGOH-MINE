cmake_minimum_required(VERSION 3.22)
project(heroes_offline_core LANGUAGES C CXX)

option(HEROES_OFFLINE_BUILD_TESTS "Build host contract tests" ON)
option(HEROES_OFFLINE_SANITIZERS "Enable ASan and UBSan in host tests" OFF)

set(OFFLINE_CORE_SOURCES
  src/offline-core/src/content_pack.cpp
  src/offline-core/src/core.cpp
  src/offline-core/src/protobuf.cpp
  src/offline-core/src/rpc_registry.cpp
  src/offline-core/src/state.cpp
)

add_library(offlinecore SHARED ${OFFLINE_CORE_SOURCES})
add_library(heroes::offlinecore ALIAS offlinecore)
target_compile_features(offlinecore PUBLIC cxx_std_20)
target_include_directories(
  offlinecore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/offline-core/include>
)
target_compile_options(offlinecore PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(ANDROID)
  target_link_options(offlinecore PRIVATE -Wl,-z,max-page-size=16384)
endif()

if(HEROES_OFFLINE_BUILD_TESTS AND NOT ANDROID)
  enable_testing()
  add_executable(
    offlinecore_tests
    tests/test_main.cpp
    tests/content_pack_test.cpp
    tests/core_test.cpp
    tests/rpc_registry_test.cpp
    tests/state_test.cpp
  )
  target_compile_features(offlinecore_tests PRIVATE cxx_std_20)
  target_compile_options(
    offlinecore_tests
    PRIVATE -Wall -Wextra -Wpedantic -Werror
  )
  target_link_libraries(offlinecore_tests PRIVATE heroes::offlinecore)

  if(HEROES_OFFLINE_SANITIZERS)
    target_compile_options(
      offlinecore PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(offlinecore PRIVATE -fsanitize=address,undefined)
    target_compile_options(
      offlinecore_tests
      PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(offlinecore_tests PRIVATE -fsanitize=address,undefined)
  endif()

  add_test(NAME offlinecore_contracts COMMAND offlinecore_tests)
endif()
