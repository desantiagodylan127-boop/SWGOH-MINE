package local.swgoh.heroesoffline.source;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;

import org.junit.Test;

/**
 * Pure-Java checks for validation and streaming helpers. No Android runtime is used.
 */
public final class AssetImporterTest {
    @Test
    public void validationAndStreamingHelpersPass() throws Exception {
        testBundleNames();
        testZipPaths();
        testStreamSignatureAndSize();
        testBadSignature();
        testShortSignature();
        testOversize();
        testSizeMismatch();
        testStreamingDigest();
        testCheckedAddition();
    }

    static void testBundleNames() {
        check(AssetImporter.isSafeBundleName("0123abcdef.bundle"));
        check(AssetImporter.isSafeBundleName("data-name_01"));
        check(!AssetImporter.isSafeBundleName(""));
        check(!AssetImporter.isSafeBundleName("../escape"));
        check(!AssetImporter.isSafeBundleName("nested/file"));
        check(!AssetImporter.isSafeBundleName("nested\\file"));
        check(!AssetImporter.isSafeBundleName(".hidden"));
        check(!AssetImporter.isSafeBundleName("name with space"));
    }

    static void testZipPaths() {
        check(AssetImporter.isSafeZipPath("assets/offline/file", false));
        check(AssetImporter.isSafeZipPath("assets/offline/", true));
        check(!AssetImporter.isSafeZipPath("../escape", false));
        check(!AssetImporter.isSafeZipPath("assets/../escape", false));
        check(!AssetImporter.isSafeZipPath("/absolute", false));
        check(!AssetImporter.isSafeZipPath("C:/drive", false));
        check(!AssetImporter.isSafeZipPath("nested\\file", false));
        check(!AssetImporter.isSafeZipPath("double//slash", false));
        check(!AssetImporter.isSafeZipPath("file/", false));
        check(!AssetImporter.isSafeZipPath("directory", true));
    }

    static void testStreamSignatureAndSize() throws Exception {
        byte[] source = "UnityFS\0payload".getBytes(StandardCharsets.ISO_8859_1);
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        long copied = AssetImporter.streamCopy(new ByteArrayInputStream(source), output,
                "UnityFS\0".getBytes(StandardCharsets.ISO_8859_1),
                source.length, source.length, null, null);
        equal(source.length, copied);
        check(Arrays.equals(source, output.toByteArray()));
    }

    static void testBadSignature() {
        byte[] source = "NotUnitypayload".getBytes(StandardCharsets.ISO_8859_1);
        expectIOException(() -> AssetImporter.streamCopy(
                new ByteArrayInputStream(source), new ByteArrayOutputStream(),
                "UnityFS\0".getBytes(StandardCharsets.ISO_8859_1),
                source.length, source.length, null, null));
    }

    static void testShortSignature() {
        byte[] source = "Unity".getBytes(StandardCharsets.ISO_8859_1);
        expectIOException(() -> AssetImporter.streamCopy(
                new ByteArrayInputStream(source), new ByteArrayOutputStream(),
                "UnityFS\0".getBytes(StandardCharsets.ISO_8859_1),
                source.length, source.length, null, null));
    }

    static void testOversize() {
        byte[] source = new byte[17];
        expectIOException(() -> AssetImporter.streamCopy(
                new ByteArrayInputStream(source), new ByteArrayOutputStream(),
                null, -1, 16, null, null));
    }

    static void testSizeMismatch() {
        byte[] source = new byte[15];
        expectIOException(() -> AssetImporter.streamCopy(
                new ByteArrayInputStream(source), new ByteArrayOutputStream(),
                null, 16, 16, null, null));
    }

    static void testStreamingDigest() throws Exception {
        byte[] source = "abc".getBytes(StandardCharsets.US_ASCII);
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        AssetImporter.streamCopy(new ByteArrayInputStream(source),
                new ByteArrayOutputStream(), null,
                source.length, source.length, digest, null);
        equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                AssetImporter.hex(digest.digest()));
    }

    static void testCheckedAddition() throws Exception {
        equal(3L, AssetImporter.checkedAdd(1, 2));
        expectIOException(() -> AssetImporter.checkedAdd(Long.MAX_VALUE, 1));
        expectIOException(() -> AssetImporter.checkedAdd(0, -1));
    }

    private static void expectIOException(ThrowingRunnable action) {
        try {
            action.run();
            throw new AssertionError("Expected IOException");
        } catch (IOException expected) {
            // Expected.
        } catch (Exception error) {
            throw new AssertionError("Unexpected exception", error);
        }
    }

    private static void check(boolean value) {
        if (!value) {
            throw new AssertionError();
        }
    }

    private static void equal(long expected, long actual) {
        if (expected != actual) {
            throw new AssertionError("Expected " + expected + ", got " + actual);
        }
    }

    private static void equal(String expected, String actual) {
        if (!expected.equals(actual)) {
            throw new AssertionError("Expected " + expected + ", got " + actual);
        }
    }

    private interface ThrowingRunnable {
        void run() throws Exception;
    }
}
