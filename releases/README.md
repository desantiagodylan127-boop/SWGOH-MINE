# APK builds

`HeroesOffline-offline6-dobby-universal.apk` now contains the ARM64 ShadowHook
engine with its incompatible linker monitor disabled. The legacy filename is
retained so existing download links continue to work. It does not contain the
full offline bundle cache; import assets from the separate 1.34 GB asset APK.

Because this build uses a different signing key, uninstall the previous
`offline6` installation before installing it.

## Small crash-fix update

`offline6-crash-fix.xdelta3` updates only the first Dobby APK:

- Required old APK SHA-256:
  `8862cb969579ade591cdbad51f1516d13582fd64499b25f3138b52b4edd237cc`
- Resulting revised APK SHA-256:
  `958e8434d7f04dc519dc8d5aa7a00c6f864ac39c290b5a358fc3fe304e372cec`

Apply it with xdelta3:

```bash
xdelta3 -d \
  -s HeroesOffline-offline6-dobby-universal.apk \
  offline6-crash-fix.xdelta3 \
  HeroesOffline-offline6-dobby-revised.apk
```

Android cannot install a delta file directly. The command reconstructs the
complete revised APK, which can then update the first Dobby build because both
are signed with the same key.

## Black-screen update

`offline6-black-screen-fix.xdelta3` updates the post-splash crashing revision:

- Required old APK SHA-256:
  `958e8434d7f04dc519dc8d5aa7a00c6f864ac39c290b5a358fc3fe304e372cec`
- Resulting ShadowHook APK SHA-256:
  `aa153b1f8d1d014a434f7e914b0352811cc72ce7bbdc7e6c3054b3d2c8a65e9d`

```bash
xdelta3 -d \
  -s HeroesOffline-offline6-dobby-universal.apk \
  offline6-black-screen-fix.xdelta3 \
  HeroesOffline-offline6-shadowhook.apk
```
