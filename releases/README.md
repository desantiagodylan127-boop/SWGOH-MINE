# APK builds

`HeroesOffline-offline6-dobby-universal.apk` contains the ARM64 Dobby
compatibility fix. It does not contain the full offline bundle cache; import
assets from the separate 1.34 GB asset APK after installation.

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
