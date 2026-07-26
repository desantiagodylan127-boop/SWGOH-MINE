#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [[ -z "${ndk}" || ! -f "${ndk}/build/cmake/android.toolchain.cmake" ]]; then
  echo "Set ANDROID_NDK_HOME to Android NDK r29 or newer." >&2
  exit 1
fi

for abi in arm64-v8a; do
  build_dir="${repo_root}/build/${abi}"
  cmake \
    -S "${repo_root}" \
    -B "${build_dir}" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ndk}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="${abi}" \
    -DANDROID_PLATFORM=android-24 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "${build_dir}" --target shadowhook

  mkdir -p "${repo_root}/dist/jni/${abi}"
  cp "${build_dir}/libshadowhook.so" "${repo_root}/dist/jni/${abi}/libshadowhook.so"
  "${ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
    "${repo_root}/dist/jni/${abi}/libshadowhook.so"
done

echo "Built compatibility libraries under dist/jni/"
