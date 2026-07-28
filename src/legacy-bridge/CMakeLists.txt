cmake_minimum_required(VERSION 3.22)
project(legacy_offlinecore_android LANGUAGES C CXX ASM)

if(NOT ANDROID)
  message(FATAL_ERROR "legacy-bridge supports Android only")
endif()
if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "legacy-bridge supports Android ARM64 (arm64-v8a) only")
endif()

include(FetchContent)

FetchContent_Declare(
  shadowhook_upstream
  GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
  GIT_TAG 47302d5bd8e508d589d2f3dfd7536ece06c610a1
  PATCH_COMMAND
    sh -c
    "git apply --reverse --check '${CMAKE_CURRENT_LIST_DIR}/../../patches/shadowhook-engine-only.patch' >/dev/null 2>&1 || git apply --ignore-whitespace '${CMAKE_CURRENT_LIST_DIR}/../../patches/shadowhook-engine-only.patch'"
)
FetchContent_GetProperties(shadowhook_upstream)
if(NOT shadowhook_upstream_POPULATED)
  FetchContent_Populate(shadowhook_upstream)
endif()

set(SHADOWHOOK_DIR
    "${shadowhook_upstream_SOURCE_DIR}/shadowhook/src/main/cpp")
file(
  GLOB SHADOWHOOK_SOURCES
  "${SHADOWHOOK_DIR}/*.c"
  "${SHADOWHOOK_DIR}/arch/arm64/*.c"
  "${SHADOWHOOK_DIR}/arch/arm64/*.S"
  "${SHADOWHOOK_DIR}/common/*.c"
  "${SHADOWHOOK_DIR}/third_party/*/*.c"
)
add_library(shadowhook_engine STATIC ${SHADOWHOOK_SOURCES})
set_target_properties(
  shadowhook_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(shadowhook_engine PRIVATE c_std_11)
target_compile_options(
  shadowhook_engine PRIVATE -Os -ffunction-sections -fdata-sections)
target_include_directories(
  shadowhook_engine
  PUBLIC "${SHADOWHOOK_DIR}/include"
  PRIVATE
    "${SHADOWHOOK_DIR}"
    "${SHADOWHOOK_DIR}/arch/arm64"
    "${SHADOWHOOK_DIR}/common"
    "${SHADOWHOOK_DIR}/third_party/xdl"
    "${SHADOWHOOK_DIR}/third_party/bsd"
    "${SHADOWHOOK_DIR}/third_party/lss"
)
target_link_libraries(shadowhook_engine PRIVATE log dl)

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
    shadowhook_engine
    "${ANDROID_LIBRARY}"
    "${LOG_LIBRARY}"
    "${DL_LIBRARY}"
)
