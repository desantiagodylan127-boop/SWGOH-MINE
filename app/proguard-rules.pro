# NativeCore methods are reached directly from Java, but their native entry
# points must retain stable names if release minification is enabled later.
-keepclasseswithmembernames class local.swgoh.heroesoffline.source.NativeCore {
    native <methods>;
}
