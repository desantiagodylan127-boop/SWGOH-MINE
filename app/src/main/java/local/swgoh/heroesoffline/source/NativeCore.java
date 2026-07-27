package local.swgoh.heroesoffline.source;

/** Java entry point for the source-owned offline core. */
public final class NativeCore {
    static {
        System.loadLibrary("offlinecore-jni");
    }

    private NativeCore() {
    }

    public static synchronized Result initialize(
            String storageDir, String cacheDir, String packPath, String bundleDir) {
        return nativeInitialize(storageDir, cacheDir, packPath, bundleDir);
    }

    public static synchronized Result selfTest() {
        return nativeSelfTest();
    }

    public static synchronized void shutdown() {
        nativeShutdown();
    }

    private static native Result nativeInitialize(
            String storageDir, String cacheDir, String packPath, String bundleDir);

    private static native Result nativeSelfTest();

    private static native void nativeShutdown();

    public static final class Result {
        private final boolean success;
        private final String message;
        private final int checkedRpcCount;

        private Result(boolean success, String message, int checkedRpcCount) {
            this.success = success;
            this.message = message;
            this.checkedRpcCount = checkedRpcCount;
        }

        public boolean isSuccess() {
            return success;
        }

        public String getMessage() {
            return message;
        }

        public int getCheckedRpcCount() {
            return checkedRpcCount;
        }

        @Override
        public String toString() {
            return "Result{success=" + success + ", message='" + message
                    + "', checkedRpcCount=" + checkedRpcCount + '}';
        }
    }
}
