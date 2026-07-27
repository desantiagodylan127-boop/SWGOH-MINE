cmake_minimum_required(VERSION 3.22)
project(offlinecore_jni LANGUAGES CXX)

set(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(OFFLINE_CORE_ROOT "${REPOSITORY_ROOT}/src/offline-core")

add_library(
    offlinecore-jni
    SHARED
    native_core_jni.cpp
    "${OFFLINE_CORE_ROOT}/src/content_pack.cpp"
    "${OFFLINE_CORE_ROOT}/src/core.cpp"
    "${OFFLINE_CORE_ROOT}/src/protobuf.cpp"
    "${OFFLINE_CORE_ROOT}/src/rpc_registry.cpp"
    "${OFFLINE_CORE_ROOT}/src/state.cpp"
)

target_compile_features(offlinecore-jni PRIVATE cxx_std_20)
target_include_directories(
    offlinecore-jni
    PRIVATE "${OFFLINE_CORE_ROOT}/include"
)
target_compile_options(
    offlinecore-jni
    PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_options(
    offlinecore-jni
    PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384"
)
