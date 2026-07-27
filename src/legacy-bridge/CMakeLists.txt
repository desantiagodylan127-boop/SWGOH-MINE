cmake_minimum_required(VERSION 3.22)
project(legacy_offlinecore_android LANGUAGES C CXX)

if(NOT ANDROID)
  message(FATAL_ERROR "legacy-bridge supports Android only")
endif()
if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "legacy-bridge supports Android ARM64 (arm64-v8a) only")
endif()

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  Dobby
  GIT_REPOSITORY https://github.com/Linifadomra/Dobby.git
  GIT_TAG a418c6a7c493e6c599713a097ff823c84a40ba96
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(Dobby)
set_target_properties(dobby PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(OFFLINE_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../offline-core")

add_library(
  legacy_offlinecore
  SHARED
  src/legacy_jni.cpp
  src/il2cpp_bridge.cpp
  "${OFFLINE_CORE_ROOT}/src/content_pack.cpp"
  "${OFFLINE_CORE_ROOT}/src/core.cpp"
  "${OFFLINE_CORE_ROOT}/src/protobuf.cpp"
  "${OFFLINE_CORE_ROOT}/src/rpc_registry.cpp"
  "${OFFLINE_CORE_ROOT}/src/state.cpp"
)

set_target_properties(legacy_offlinecore PROPERTIES OUTPUT_NAME offlinecore)
target_compile_features(legacy_offlinecore PRIVATE cxx_std_20)
target_include_directories(
  legacy_offlinecore
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OFFLINE_CORE_ROOT}/include"
)
target_compile_options(
  legacy_offlinecore
  PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_options(
  legacy_offlinecore
  PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)

find_library(ANDROID_LIBRARY android REQUIRED)
find_library(LOG_LIBRARY log REQUIRED)
find_library(DL_LIBRARY dl REQUIRED)
target_link_libraries(
  legacy_offlinecore
  PRIVATE
    dobby::dobby
    "${ANDROID_LIBRARY}"
    "${LOG_LIBRARY}"
    "${DL_LIBRARY}"
)
