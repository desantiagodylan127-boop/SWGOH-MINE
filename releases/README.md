# APK builds

`HeroesOffline-sourcebridge-shadowengine.zip` contains the reconstructed C++
OfflineCore and corrected Unity transport bridge using ShadowHook's original
function-hook engine with its incompatible linker monitor disabled.

- ZIP SHA-256:
  `c90a61c521beb3ab821a05c8e3be54ef9d4cc5e9f84f8678682c71b952b5c4c6`
- Contained APK SHA-256:
  `180e5c7745263b621c01793021790d80c8239f8043f75a78a3a12958e78f5d97`

The build passed host sanitizers, real content-pack verification, RPC/bridge
contracts, Android compilation, ELF inspection, alignment, signatures, ZIP
integrity, and no-INTERNET checks. It has not yet passed the target-phone
runtime test.

All older binary-patched APKs remain withdrawn.
