# Heroes Offline hook compatibility

This repository contains the source-only ARM64 compatibility layer used to
replace ShadowHook in the `HeroesOffline-offline6-universal.apk` bootstrap.
No Electronic Arts game binaries, assets, signing keys, or APKs are stored in
the repository.

## Why this exists

The offline bootstrap's existing `libofflinecore.so` uses these ShadowHook APIs:

- `shadowhook_init`
- `shadowhook_register_dl_init_callback`
- `shadowhook_hook_func_addr`
- `shadowhook_dlopen` / `shadowhook_dlsym`
- `shadowhook_get_errno` / `shadowhook_to_errmsg`

On the affected Samsung device, stock ShadowHook fails before any game hook is
installed with `Init linker mod failed`. This build keeps ShadowHook's tested
function-hook engine but:

- skips ShadowHook's incompatible linker-monitor and queued-symbol modules;
- avoids modifying both linker internals and public `libdl` trampolines;
- detects `libil2cpp.so` with `dl_iterate_phdr` from a lightweight worker; and
- waits for IL2CPP relocation to complete before installing hooks;
- only intercepts HTTP callbacks whose delegate target matches an observed
  `RPC<T>` object, falling back for unrelated startup requests; and
- reports failure to `libofflinecore.so` if any target hook cannot be installed.

The original offline core remains responsible for its pinned RVAs, request
handling, local state, and fail-closed behavior.

## Build

Requirements:

- CMake 3.22+
- Ninja
- Android NDK r29 (`29.0.14206865`) or newer
- Git access for the pinned ShadowHook source

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
./scripts/build-native.sh
```

This produces:

```text
dist/jni/arm64-v8a/libshadowhook.so
```

Create the guarded OfflineCore from the exact `offline6` ARM64 library:

```bash
./scripts/patch-offlinecore-arm64.py \
  /path/to/original/libofflinecore.so \
  dist/jni/arm64-v8a/libofflinecore.so
```

ShadowHook is pinned to commit
`47302d5bd8e508d589d2f3dfd7536ece06c610a1` (version 2.0.1 source).

The APK's original ARMv7 ShadowHook library is deliberately retained because
the reported linker failure is in the ARM64 process selected by the target
Samsung phone.

## Patch a locally supplied offline6 APK

```bash
./scripts/patch-apk.py \
  /path/to/HeroesOffline-offline6-universal.apk \
  build/HeroesOffline-offline6-dobby-unsigned-unaligned.apk

zipalign -P 16 -f 4 \
  build/HeroesOffline-offline6-dobby-unsigned-unaligned.apk \
  build/HeroesOffline-offline6-dobby-unsigned.apk

apksigner sign --ks /path/to/heroes-offline.jks \
  --out build/HeroesOffline-offline6-dobby-universal.apk \
  build/HeroesOffline-offline6-dobby-unsigned.apk

apksigner verify --verbose --print-certs \
  build/HeroesOffline-offline6-dobby-universal.apk
```

Use the same signing key as `offline6` to install as an update. If that private
key is unavailable, uninstall `offline6` before installing a build signed with
a new key.

The 172 MB `offline6` APK is intentionally asset-light. Keep the existing
1.34 GB asset APK on the phone and select it from the bootstrap import screen.