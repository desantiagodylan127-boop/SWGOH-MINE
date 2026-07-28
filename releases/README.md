# APK builds

## Current phone-test build

`HeroesOffline-playtest-shadowlogin.zip` packages the playtest APK (the member of
the Drive pair that already contains the 11,356 UnityBundles) with a rebuilt
ARM64 `libofflinecore.so`.

What changed versus the uploaded offline12/playtest pair:

- Replaced the Dobby hook backend (same one offline12-final ships) with the
  linker-free ShadowHook engine.
- Added the post-EA-splash hooks offline12 relies on: forced guest
  `DoGameServiceLogin`, local `env-list.ini` rewrite, and
  `ImportAccountViewReady` continue.
- Rewrites AssetBundle URLs to `jar:file://…/UnityBundles/…` inside the installed
  APK, and extracts `offline-content.pack` from assets on first launch.

Install only this APK for the phone test (uninstall any previous
`local.swgoh.heroesoffline2` first if the signature differs). The separate
offline12-final APK is not required for this build because playtest already
contains the UnityBundles.

- ZIP SHA-256: `5b12565be0823c2334ae78881e6b284ff5461cbd5d35408b0e67a52045951d8d`
- Contained APK SHA-256: `b040880031b8b3aa3db2e1c235ce2f936419dcb2927a62aaf7296b57a6ea31ab`
- Package: `local.swgoh.heroesoffline2`
- Version: `0.40.2044608-offline.11` (playtest shell) with source ShadowHook core

## Previous experimental build

`HeroesOffline-sourcebridge-shadowengine.zip` was an earlier offline6-shell
packaging of the ShadowHook bridge without the login/INI/import hooks and
without UnityBundles embedded. Prefer the playtest-shadowlogin build above.

All older binary-patched APKs remain withdrawn.
