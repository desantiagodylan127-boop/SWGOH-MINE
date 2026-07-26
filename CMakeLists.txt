cmake_minimum_required(VERSION 3.22)
project(heroes_offline_hook_compat LANGUAGES C ASM)

include(FetchContent)

FetchContent_Declare(
  shadowhook_upstream
  GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
  GIT_TAG 47302d5bd8e508d589d2f3dfd7536ece06c610a1
  PATCH_COMMAND
    sh -c
    "git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/shadowhook-no-linker.patch' >/dev/null 2>&1 || git apply --ignore-whitespace '${CMAKE_CURRENT_SOURCE_DIR}/patches/shadowhook-no-linker.patch'"
)
FetchContent_GetProperties(shadowhook_upstream)
if(NOT shadowhook_upstream_POPULATED)
  FetchContent_Populate(shadowhook_upstream)
endif()

set(SHADOWHOOK_DIR ${shadowhook_upstream_SOURCE_DIR}/shadowhook/src/main/cpp)
file(
  GLOB SHADOWHOOK_SOURCES
  ${SHADOWHOOK_DIR}/*.c
  ${SHADOWHOOK_DIR}/arch/arm64/*.c
  ${SHADOWHOOK_DIR}/arch/arm64/*.S
  ${SHADOWHOOK_DIR}/common/*.c
  ${SHADOWHOOK_DIR}/third_party/*/*.c
)

add_library(shadowhook SHARED ${SHADOWHOOK_SOURCES} src/sh_compat_loader.c)
target_compile_features(shadowhook PRIVATE c_std_11)
target_compile_options(
  shadowhook
  PRIVATE
    -Wall
    -Wextra
    -Werror
    -Os
    -ffunction-sections
    -fdata-sections
)
target_include_directories(
  shadowhook
  PRIVATE
    ${SHADOWHOOK_DIR}
    ${SHADOWHOOK_DIR}/include
    ${SHADOWHOOK_DIR}/arch/arm64
    ${SHADOWHOOK_DIR}/common
    ${SHADOWHOOK_DIR}/third_party/xdl
    ${SHADOWHOOK_DIR}/third_party/bsd
    ${SHADOWHOOK_DIR}/third_party/lss
)
target_link_libraries(shadowhook PRIVATE log)
target_link_options(
  shadowhook
  PRIVATE
    -Wl,-z,max-page-size=16384
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    -Wl,--undefined-version
    -Wl,--version-script=${SHADOWHOOK_DIR}/shadowhook.map.txt
)
set_target_properties(shadowhook PROPERTIES OUTPUT_NAME shadowhook)
