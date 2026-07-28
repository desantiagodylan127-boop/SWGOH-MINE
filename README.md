# Heroes Offline

Clean, source-owned native core for a private offline Android game profile.
This repository intentionally contains no Electronic Arts binaries, game
assets, APKs, signing keys, inline hooks, or executable-address patches.

## Implemented

- strict `HOPACK1` parsing with bounds, path, duplicate, overlap, and SHA-256
  validation;
- atomic content materialization;
- versioned local profile persistence and deterministic AI generation;
- dependency-free protobuf encoding;
- local authentication, initial profile, metadata, and tutorial battle RPCs;
- a stable C ABI that never lets C++ exceptions cross into its host; and
- host contract tests plus ARM64 and ARMv7 Android builds.

The source core does **not** yet launch the proprietary Unity client. Runtime
integration remains gated until it can be done without ShadowHook, Dobby,
pinned RVAs, or managed-object memory writes.

## Host build and tests

```bash
cmake -S . -B build/host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHEROES_OFFLINE_SANITIZERS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

## Android native build

Requires Android NDK r29 (`29.0.14206865`) or newer:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
./scripts/build-native.sh
```

Outputs:

```text
dist/jni/arm64-v8a/libofflinecore.so
dist/jni/armeabi-v7a/libofflinecore.so
```

These libraries contain only the stable source C API. They do not depend on or
modify Android’s linker, Unity, or IL2CPP.

## Safety status

Experimental phone builds live under `releases/`. Prefer
`HeroesOffline-playtest-shadowlogin.zip`: it keeps the playtest UnityBundles and
replaces Dobby with a linker-free ShadowHook engine plus the offline12
post-splash login/INI/import hooks. Older binary-patched APKs remain withdrawn.

