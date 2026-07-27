# APK builds

`HeroesOffline-sourcebridge-test.zip` contains the first APK built from the
reconstructed C++ OfflineCore and Unity transport bridge source.

- ZIP SHA-256:
  `2f3c7bfe092f0fa0c1f8e4b7ae5a2ccd0885af3f56f32f40e278369260e616d9`
- Contained APK SHA-256:
  `0a4a294c6939fa3ba8cbc395a9a0379896a00ff78b3ab2beab07d49140610d7e`

The build passed host sanitizers, real content-pack verification, RPC and
bridge contracts, Android compilation, ELF inspection, alignment, signature,
ZIP integrity, and no-INTERNET checks. It has not yet passed the target-phone
runtime test.

All older binary-patched APKs remain withdrawn.
