package local.swgoh.heroesoffline.source;

import android.content.ContentResolver;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.net.Uri;
import android.os.StatFs;
import android.system.Os;
import android.system.OsConstants;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class AssetImporter {
    public static final String EXPECTED_SHA256 =
            "8af543c20775dd27407ef1d8373a6bd569bd72ca5c5bc9506ea05b092cea728b";
    public static final int EXPECTED_BUNDLE_COUNT = 11_356;
    public static final String PACK_PATH = "assets/offline/offline-content.pack";
    public static final String BUNDLE_PREFIX = "assets/offline/UnityBundles/";

    static final int BUFFER_SIZE = 1024 * 1024;
    static final long MIN_PACK_BYTES = 190L * 1024L * 1024L;
    private static final long MAX_SOURCE_BYTES = 2L * 1024L * 1024L * 1024L;
    private static final long MAX_ENTRY_BYTES = 4L * 1024L * 1024L * 1024L;
    private static final long MAX_CONTENT_BYTES = 8L * 1024L * 1024L * 1024L;
    private static final long MAX_ZIP_DECLARED_BYTES = 16L * 1024L * 1024L * 1024L;
    private static final long SPACE_RESERVE = 128L * 1024L * 1024L;
    private static final byte[] UNITY_MAGIC =
            new byte[]{'U', 'n', 'i', 't', 'y', 'F', 'S', 0};
    private static final byte[] PACK_MAGIC =
            new byte[]{'H', 'O', 'P', 'A', 'C', 'K', '1', 0};

    private final ContentResolver resolver;
    private final File root;
    private final AtomicBoolean cancellation;
    private final Progress progress;

    @FunctionalInterface
    public interface Progress {
        void onProgress(String stage, long completed, long total);

        default boolean isCancelled() {
            return false;
        }
    }

    public static final class Result {
        public final File pack;
        public final File bundleDirectory;
        public final File manifest;

        Result(File pack, File bundleDirectory, File manifest) {
            this.pack = pack;
            this.bundleDirectory = bundleDirectory;
            this.manifest = manifest;
        }
    }

    public static final class CancelledException extends IOException {
        CancelledException() {
            super("Import cancelled");
        }
    }

    public AssetImporter(Context context, AtomicBoolean cancellation, Progress progress) {
        if (context == null || cancellation == null || progress == null) {
            throw new NullPointerException("context, cancellation, and progress are required");
        }
        Context application = context.getApplicationContext();
        resolver = application.getContentResolver();
        root = application.getNoBackupFilesDir();
        this.cancellation = cancellation;
        this.progress = progress;
    }

    public File importApk(Uri uri) throws IOException {
        Progress combined = new Progress() {
            @Override
            public void onProgress(String stage, long completed, long total) {
                progress.onProgress(stage, completed, total);
            }

            @Override
            public boolean isCancelled() {
                return cancellation.get() || progress.isCancelled();
            }
        };
        return importFrom(resolver, uri, root, combined).pack;
    }

    public static Result importFrom(ContentResolver resolver, Uri uri, File root,
                                    Progress progress) throws IOException {
        if (resolver == null || uri == null || root == null) {
            throw new NullPointerException("resolver, uri, and root are required");
        }
        Progress reporter = progress == null ? (stage, completed, total) -> { } : progress;
        ensureDirectory(root);
        File generations = new File(root, "asset-imports");
        ensureDirectory(generations);

        String id = UUID.randomUUID().toString();
        File staging = new File(generations, ".pending-" + id);
        File generation = new File(generations, "generation-" + id);
        if (!staging.mkdir()) {
            throw new IOException("Could not create import staging directory");
        }

        boolean published = false;
        boolean committed = false;
        try {
            checkCancelled(reporter);
            long advertisedLength = sourceLength(resolver, uri);
            if (advertisedLength > MAX_SOURCE_BYTES) {
                throw new IOException("Selected APK is larger than the safe import limit");
            }
            requireSpace(staging, checkedAdd(
                    advertisedLength < 0 ? MAX_SOURCE_BYTES : advertisedLength, SPACE_RESERVE));

            File source = new File(staging, "source.apk.pending");
            String sourceHash = copySource(
                    resolver, uri, source, advertisedLength, reporter);
            if (!EXPECTED_SHA256.equals(sourceHash)) {
                throw new IOException("Wrong APK: SHA-256 was " + sourceHash);
            }
            checkCancelled(reporter);

            ImportPlan plan;
            try (ZipFile zip = new ZipFile(source)) {
                plan = inspect(zip, reporter);
                // The source and extracted files coexist until extraction is complete.
                requireSpace(staging, checkedAdd(plan.totalSize, SPACE_RESERVE));
                extract(zip, plan, staging, reporter);
            }

            if (!source.delete()) {
                throw new IOException("Could not remove the temporary APK");
            }
            writeManifest(staging, plan, sourceHash);
            checkCancelled(reporter);

            syncDirectory(new File(staging, "assets/offline/UnityBundles"));
            syncDirectory(new File(staging, "assets/offline"));
            syncDirectory(new File(staging, "assets"));
            syncDirectory(staging);
            syncDirectory(generations);
            if (!staging.renameTo(generation)) {
                throw new IOException("Could not atomically publish imported content");
            }
            published = true;
            syncDirectory(generations);
            committed = true;

            File offline = new File(generation, "assets/offline");
            return new Result(new File(offline, "offline-content.pack"),
                    new File(offline, "UnityBundles"),
                    new File(generation, "import-manifest.txt"));
        } finally {
            if (!committed) {
                deleteTree(published ? generation : staging);
            }
        }
    }

    private static long sourceLength(ContentResolver resolver, Uri uri) {
        try (AssetFileDescriptor descriptor = resolver.openAssetFileDescriptor(uri, "r")) {
            return descriptor == null ? -1 : descriptor.getLength();
        } catch (IOException | SecurityException ignored) {
            return -1;
        }
    }

    private static String copySource(ContentResolver resolver, Uri uri, File destination,
                                     long total, Progress progress) throws IOException {
        InputStream opened = resolver.openInputStream(uri);
        if (opened == null) {
            throw new IOException("The selected document could not be opened");
        }
        MessageDigest digest = sha256();
        try (InputStream input = new BufferedInputStream(opened, BUFFER_SIZE);
             FileOutputStream file = new FileOutputStream(destination);
             OutputStream output = new BufferedOutputStream(file, BUFFER_SIZE)) {
            streamCopy(input, output, null, total, MAX_SOURCE_BYTES, digest, copied -> {
                checkCancelled(progress);
                progress.onProgress("Verifying APK", copied, total);
                if (usableBytes(destination) < SPACE_RESERVE) {
                    throw new IOException("Not enough free space to continue");
                }
            });
            output.flush();
            file.getFD().sync();
        } catch (IOException error) {
            destination.delete();
            throw error;
        }
        return hex(digest.digest());
    }

    private static ImportPlan inspect(ZipFile zip, Progress progress) throws IOException {
        ZipEntry pack = null;
        List<ZipEntry> bundles = new ArrayList<>(EXPECTED_BUNDLE_COUNT);
        Set<String> allPaths = new HashSet<>();
        Set<String> bundleNames = new HashSet<>(EXPECTED_BUNDLE_COUNT * 2);
        long contentSize = 0;
        long allDeclaredSize = 0;

        Enumeration<? extends ZipEntry> entries = zip.entries();
        while (entries.hasMoreElements()) {
            checkCancelled(progress);
            ZipEntry entry = entries.nextElement();
            String path = entry.getName();
            if (!isSafeZipPath(path, entry.isDirectory())) {
                throw new IOException("Unsafe ZIP entry path: " + path);
            }
            if (!allPaths.add(path)) {
                throw new IOException("Duplicate ZIP entry: " + path);
            }
            long size = checkedSize(entry);
            if (size > MAX_ENTRY_BYTES) {
                throw new IOException("Oversized ZIP entry: " + path);
            }
            long compressedSize = entry.getCompressedSize();
            if (compressedSize < 0 || compressedSize > MAX_SOURCE_BYTES) {
                throw new IOException("Invalid compressed ZIP entry size: " + path);
            }
            allDeclaredSize = checkedAdd(allDeclaredSize, size);
            if (allDeclaredSize > MAX_ZIP_DECLARED_BYTES) {
                throw new IOException("APK declares too much uncompressed data");
            }

            if (PACK_PATH.equals(path)) {
                if (entry.isDirectory()) {
                    throw new IOException("Content pack is a directory");
                }
                if (size < MIN_PACK_BYTES) {
                    throw new IOException("Content pack is smaller than 190 MiB");
                }
                pack = entry;
                contentSize = checkedAdd(contentSize, size);
            } else if (path.startsWith(BUNDLE_PREFIX)) {
                String name = path.substring(BUNDLE_PREFIX.length());
                if (name.isEmpty() && entry.isDirectory()) {
                    continue;
                }
                if (entry.isDirectory() || !isSafeBundleName(name)) {
                    throw new IOException(
                            "Unity bundle is not a direct, safe file: " + path);
                }
                if (!bundleNames.add(name)) {
                    throw new IOException("Duplicate Unity bundle name: " + name);
                }
                if (size < UNITY_MAGIC.length) {
                    throw new IOException("Unity bundle is too short: " + name);
                }
                contentSize = checkedAdd(contentSize, size);
                bundles.add(entry);
            }
            if (contentSize > MAX_CONTENT_BYTES) {
                throw new IOException("Imported content exceeds the safe size limit");
            }
        }
        if (pack == null) {
            throw new IOException("APK does not contain " + PACK_PATH);
        }
        if (bundles.size() != EXPECTED_BUNDLE_COUNT) {
            throw new IOException("Expected " + EXPECTED_BUNDLE_COUNT
                    + " Unity bundles, found " + bundles.size());
        }
        return new ImportPlan(pack, bundles, contentSize);
    }

    private static void extract(ZipFile zip, ImportPlan plan, File staging,
                                Progress progress) throws IOException {
        File offline = new File(staging, "assets/offline");
        File bundleDirectory = new File(offline, "UnityBundles");
        ensureDirectory(bundleDirectory);

        long completed = extractEntry(zip, plan.pack,
                new File(offline, "offline-content.pack"), PACK_MAGIC,
                0, plan.totalSize, progress, 0);
        int pendingNumber = 1;
        long bundleBytes = 0;
        for (ZipEntry bundle : plan.bundles) {
            long copied = extractEntry(zip, bundle,
                    new File(bundleDirectory,
                            bundle.getName().substring(BUNDLE_PREFIX.length())),
                    UNITY_MAGIC, completed, plan.totalSize, progress, pendingNumber++);
            completed = checkedAdd(completed, copied);
            bundleBytes = checkedAdd(bundleBytes, copied);
        }
        plan.bundleBytes = bundleBytes;
    }

    private static long extractEntry(ZipFile zip, ZipEntry entry, File target,
                                     byte[] expectedMagic, long base, long total,
                                     Progress progress, int pendingNumber) throws IOException {
        File pending = new File(
                target.getParentFile(), ".file-" + pendingNumber + ".pending");
        long copied;
        try (InputStream input =
                     new BufferedInputStream(zip.getInputStream(entry), BUFFER_SIZE);
             FileOutputStream file = new FileOutputStream(pending);
             OutputStream output = new BufferedOutputStream(file, BUFFER_SIZE)) {
            copied = streamCopy(input, output, expectedMagic, entry.getSize(),
                    entry.getSize(), null, amount -> {
                        checkCancelled(progress);
                        progress.onProgress("Importing verified content",
                                checkedAdd(base, amount), total);
                        if (usableBytes(pending) < SPACE_RESERVE) {
                            throw new IOException("Not enough free space to continue");
                        }
                    });
            output.flush();
            file.getFD().sync();
        } catch (IOException error) {
            pending.delete();
            throw error;
        }
        if (!pending.renameTo(target)) {
            pending.delete();
            throw new IOException("Could not commit staged file: " + target.getName());
        }
        return copied;
    }

    private static void writeManifest(File staging, ImportPlan plan, String sourceHash)
            throws IOException {
        String text = "sourceSha256=" + sourceHash + "\n"
                + "bundleCount=" + plan.bundles.size() + "\n"
                + "bundleBytes=" + plan.bundleBytes + "\n"
                + "packBytes=" + plan.pack.getSize() + "\n"
                + "contentBytes=" + plan.totalSize + "\n";
        File pending = new File(staging, ".manifest.pending");
        File manifest = new File(staging, "import-manifest.txt");
        try (FileOutputStream output = new FileOutputStream(pending)) {
            output.write(text.getBytes(StandardCharsets.UTF_8));
            output.flush();
            output.getFD().sync();
        } catch (IOException error) {
            pending.delete();
            throw error;
        }
        if (!pending.renameTo(manifest)) {
            pending.delete();
            throw new IOException("Could not commit import manifest");
        }
    }

    static long streamCopy(InputStream input, OutputStream output, byte[] expectedPrefix,
                           long expectedSize, long maximumSize, MessageDigest digest,
                           CopyObserver observer) throws IOException {
        if (maximumSize < 0 || expectedSize < -1 || expectedSize > maximumSize) {
            throw new IOException("Invalid stream size limit");
        }
        byte[] prefix = expectedPrefix == null ? null : new byte[expectedPrefix.length];
        int prefixLength = 0;
        boolean prefixChecked = prefix == null || prefix.length == 0;
        long copied = 0;
        byte[] buffer = new byte[BUFFER_SIZE];
        int read;
        while ((read = input.read(buffer)) != -1) {
            if (read == 0) {
                continue;
            }
            if (copied > maximumSize - read) {
                throw new IOException("Stream exceeds its safe size limit");
            }
            if (prefix != null && prefixLength < prefix.length) {
                int amount = Math.min(read, prefix.length - prefixLength);
                System.arraycopy(buffer, 0, prefix, prefixLength, amount);
                prefixLength += amount;
                if (prefixLength == prefix.length) {
                    if (!MessageDigest.isEqual(prefix, expectedPrefix)) {
                        throw new IOException("Stream has an invalid file signature");
                    }
                    prefixChecked = true;
                }
            }
            output.write(buffer, 0, read);
            if (digest != null) {
                digest.update(buffer, 0, read);
            }
            copied += read;
            if (observer != null) {
                observer.onBytes(copied);
            }
        }
        if (expectedSize >= 0 && copied != expectedSize) {
            throw new IOException("Stream size does not match its ZIP declaration");
        }
        if (!prefixChecked) {
            throw new IOException("Stream has an invalid file signature");
        }
        return copied;
    }

    interface CopyObserver {
        void onBytes(long copied) throws IOException;
    }

    public static boolean isSafeBundleName(String name) {
        if (name == null || name.isEmpty() || name.length() > 255
                || ".".equals(name) || "..".equals(name) || name.charAt(0) == '.') {
            return false;
        }
        for (int i = 0; i < name.length(); i++) {
            char value = name.charAt(i);
            boolean safe = value >= 'a' && value <= 'z'
                    || value >= 'A' && value <= 'Z'
                    || value >= '0' && value <= '9'
                    || value == '.' || value == '_' || value == '-';
            if (!safe) {
                return false;
            }
        }
        return true;
    }

    public static boolean isSafeZipPath(String path, boolean directory) {
        if (path == null || path.isEmpty() || path.length() > 4096
                || path.charAt(0) == '/' || path.indexOf('\\') >= 0
                || path.indexOf('\0') >= 0 || path.indexOf(':') >= 0
                || directory != path.endsWith("/")) {
            return false;
        }
        int end = directory ? path.length() - 1 : path.length();
        if (end == 0) {
            return false;
        }
        int componentStart = 0;
        for (int i = 0; i <= end; i++) {
            if (i == end || path.charAt(i) == '/') {
                int length = i - componentStart;
                if (length == 0 || length > 255) {
                    return false;
                }
                String component = path.substring(componentStart, i);
                if (".".equals(component) || "..".equals(component)) {
                    return false;
                }
                componentStart = i + 1;
            }
        }
        return true;
    }

    private static void ensureDirectory(File directory) throws IOException {
        if (directory.isDirectory()) {
            return;
        }
        if (directory.exists() || !directory.mkdirs()) {
            throw new IOException("Could not create directory: " + directory);
        }
    }

    private static void requireSpace(File path, long needed) throws IOException {
        if (usableBytes(path) < needed) {
            throw new IOException("Not enough free space (need at least "
                    + needed + " bytes)");
        }
    }

    private static long usableBytes(File path) {
        return new StatFs(path.getAbsolutePath()).getAvailableBytes();
    }

    private static long checkedSize(ZipEntry entry) throws IOException {
        if (entry.getSize() < 0) {
            throw new IOException(
                    "ZIP entry has no declared size: " + entry.getName());
        }
        return entry.getSize();
    }

    static long checkedAdd(long left, long right) throws IOException {
        if (left < 0 || right < 0 || left > Long.MAX_VALUE - right) {
            throw new IOException("Imported content size overflow");
        }
        return left + right;
    }

    private static void checkCancelled(Progress progress) throws CancelledException {
        if (progress.isCancelled() || Thread.currentThread().isInterrupted()) {
            throw new CancelledException();
        }
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] output = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            output[i * 2] = digits[(bytes[i] >>> 4) & 0x0f];
            output[i * 2 + 1] = digits[bytes[i] & 0x0f];
        }
        return new String(output);
    }

    private static void syncDirectory(File directory) throws IOException {
        FileDescriptor descriptor = null;
        try {
            descriptor = Os.open(directory.getAbsolutePath(),
                    OsConstants.O_RDONLY, 0);
            Os.fsync(descriptor);
        } catch (Exception error) {
            throw new IOException("Could not sync directory: " + directory, error);
        } finally {
            if (descriptor != null) {
                try {
                    Os.close(descriptor);
                } catch (Exception ignored) {
                    // Preserve an open/fsync failure.
                }
            }
        }
    }

    private static void deleteTree(File path) {
        if (path == null || !path.exists()) {
            return;
        }
        File[] children = path.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        path.delete();
    }

    private static final class ImportPlan {
        final ZipEntry pack;
        final List<ZipEntry> bundles;
        final long totalSize;
        long bundleBytes;

        ImportPlan(ZipEntry pack, List<ZipEntry> bundles, long totalSize) {
            this.pack = pack;
            this.bundles = bundles;
            this.totalSize = totalSize;
        }
    }
}
