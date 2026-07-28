#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [[ -z "${ndk}" || ! -f "${ndk}/build/cmake/android.toolchain.cmake" ]]; then
  echo "Set ANDROID_NDK_HOME to Android NDK r29 or newer." >&2
  exit 1
fi

build_dir="${repo_root}/build/legacy-arm64"
cmake \
  -S "${repo_root}/src/legacy-bridge" \
  -B "${build_dir}" \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ndk}/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}" --target legacy_offlinecore

mkdir -p "${repo_root}/dist/legacy/arm64-v8a"
cp "${build_dir}/libofflinecore.so" \
  "${repo_root}/dist/legacy/arm64-v8a/libofflinecore.so"
"${ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
  "${repo_root}/dist/legacy/arm64-v8a/libofflinecore.so"

echo "Built dist/legacy/arm64-v8a/libofflinecore.so"
