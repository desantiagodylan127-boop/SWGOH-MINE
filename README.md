# Heroes Offline hook compatibility

This repository contains the source-only native compatibility layer used to
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

On the affected Samsung device, ShadowHook fails before any game hook is
installed with `Init linker mod failed`. This library keeps that ABI but:

- uses Dobby for the four ARM/ARM64 inline hooks;
- monitors public `libdl` entry points instead of modifying linker internals;
- falls back to `dl_iterate_phdr` polling if a device bypasses those entry
  points; and
- reports failure to `libofflinecore.so` if any target hook cannot be installed.

The original offline core remains responsible for its pinned RVAs, request
handling, local state, and fail-closed behavior.

## Build

Requirements:

- CMake 3.22+
- Ninja
- Android NDK r29 (`29.0.14206865`) or newer
- Git access for the pinned Dobby source

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
./scripts/build-native.sh
```

This produces:

```text
dist/jni/arm64-v8a/libshadowhook.so
dist/jni/armeabi-v7a/libshadowhook.so
```

Dobby is pinned to commit
`a418c6a7c493e6c599713a097ff823c84a40ba96`, which includes the Android ARM64
and short-trampoline fixes proposed upstream in `jmpews/Dobby#302`.

## Patch a locally supplied offline6 APK

```bash
./scripts/patch-apk.py \
  /path/to/HeroesOffline-offline6-universal.apk \
  build/HeroesOffline-offline7-unsigned-unaligned.apk

zipalign -P 16 -f 4 \
  build/HeroesOffline-offline7-unsigned-unaligned.apk \
  build/HeroesOffline-offline7-unsigned.apk

apksigner sign --ks /path/to/heroes-offline.jks \
  --out build/HeroesOffline-offline7-universal.apk \
  build/HeroesOffline-offline7-unsigned.apk

apksigner verify --verbose --print-certs \
  build/HeroesOffline-offline7-universal.apk
```

Use the same signing key as `offline6` to install as an update. If that private
key is unavailable, uninstall `offline6` before installing a build signed with
a new key.

The 172 MB `offline6` APK is intentionally asset-light. Keep the existing
1.34 GB asset APK on the phone and select it from the bootstrap import screen.