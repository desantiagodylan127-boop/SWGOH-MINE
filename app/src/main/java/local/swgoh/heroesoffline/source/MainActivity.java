package local.swgoh.heroesoffline.source;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends Activity {
    private static final int REQUEST_ASSET_APK = 1340;
    private static final String STATE_STATUS = "status";

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService importerExecutor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean destroyed = new AtomicBoolean();
    private final Object nativeLifecycleLock = new Object();

    private volatile AtomicBoolean importCancellation;
    private volatile boolean nativeInitialized;
    private TextView statusView;
    private ProgressBar progressView;
    private Button chooseView;
    private Button cancelView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(createContentView());

        if (savedInstanceState != null) {
            statusView.setText(savedInstanceState.getString(
                    STATE_STATUS, getString(R.string.status_ready)));
        }
    }

    private View createContentView() {
        int padding = Math.round(24 * getResources().getDisplayMetrics().density);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, padding, padding, padding);

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextAppearance(android.R.style.TextAppearance_Material_Headline);
        content.addView(title);

        TextView safety = new TextView(this);
        safety.setText(R.string.safety_status);
        safety.setPadding(0, padding, 0, padding);
        content.addView(safety);

        chooseView = new Button(this);
        chooseView.setText(R.string.choose_asset_apk);
        chooseView.setOnClickListener(ignored -> chooseAssetApk());
        content.addView(chooseView);

        cancelView = new Button(this);
        cancelView.setText(R.string.cancel_import);
        cancelView.setVisibility(View.GONE);
        cancelView.setOnClickListener(ignored -> cancelImport());
        content.addView(cancelView);

        progressView = new ProgressBar(
                this, null, android.R.attr.progressBarStyleHorizontal);
        progressView.setMax(10_000);
        progressView.setVisibility(View.GONE);
        content.addView(progressView);

        statusView = new TextView(this);
        statusView.setText(R.string.status_ready);
        statusView.setPadding(0, padding, 0, 0);
        statusView.setTextIsSelectable(true);
        content.addView(statusView);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(content);
        return scroll;
    }

    private void chooseAssetApk() {
        Intent chooser = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        chooser.addCategory(Intent.CATEGORY_OPENABLE);
        chooser.setType("application/vnd.android.package-archive");
        chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(chooser, REQUEST_ASSET_APK);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ASSET_APK || resultCode != RESULT_OK
                || data == null || data.getData() == null) {
            return;
        }

        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // Some document providers grant access only for this Activity.
        }
        beginImport(uri);
    }

    private void beginImport(Uri uri) {
        AtomicBoolean cancellation = new AtomicBoolean();
        importCancellation = cancellation;
        setBusy(true);
        statusView.setText(R.string.status_opening);

        importerExecutor.execute(() -> {
            try {
                shutdownNativeCoreQuietly();
                AssetImporter importer =
                        new AssetImporter(getApplicationContext(), cancellation, this::showProgress);
                File contentPack = importer.importApk(uri);
                if (cancellation.get()) {
                    finishImport(getString(R.string.status_cancelled));
                    return;
                }

                File bundleDirectory =
                        new File(contentPack.getParentFile(), "UnityBundles");
                NativeCore.Result initialization = NativeCore.initialize(
                        getNoBackupFilesDir().getAbsolutePath(),
                        getCacheDir().getAbsolutePath(),
                        contentPack.getAbsolutePath(),
                        bundleDirectory.getAbsolutePath());
                requireSuccess("Native initialization", initialization);
                synchronized (nativeLifecycleLock) {
                    if (destroyed.get() || cancellation.get()) {
                        NativeCore.shutdown();
                        finishImport(getString(R.string.status_cancelled));
                        return;
                    }
                    nativeInitialized = true;
                }

                NativeCore.Result selfTest = NativeCore.selfTest();
                requireSuccess("Native self-test", selfTest);
                if (cancellation.get()) {
                    shutdownNativeCore();
                    finishImport(getString(R.string.status_cancelled));
                    return;
                }
                String exactResult = selfTest.getMessage();
                if (exactResult == null || exactResult.isEmpty()) {
                    throw new IllegalStateException("Native self-test returned no result");
                }
                finishImport(getString(R.string.status_success, exactResult));
            } catch (AssetImporter.CancelledException cancelled) {
                shutdownNativeCoreQuietly();
                finishImport(getString(R.string.status_cancelled));
            } catch (Throwable error) {
                shutdownNativeCoreQuietly();
                String detail = error.getMessage();
                if (detail == null || detail.isEmpty()) {
                    detail = error.getClass().getName();
                }
                finishImport(getString(R.string.status_error, detail));
            }
        });
    }

    private static void requireSuccess(String operation, NativeCore.Result result) {
        if (result == null) {
            throw new IllegalStateException(operation + " returned no result");
        }
        if (!result.isSuccess()) {
            String message = result.getMessage();
            throw new IllegalStateException(message == null || message.isEmpty()
                    ? operation + " failed" : message);
        }
    }

    private void shutdownNativeCore() {
        synchronized (nativeLifecycleLock) {
            if (!nativeInitialized) {
                return;
            }
            try {
                NativeCore.shutdown();
            } finally {
                nativeInitialized = false;
            }
        }
    }

    private void shutdownNativeCoreQuietly() {
        try {
            shutdownNativeCore();
        } catch (Throwable ignored) {
            // Preserve the import or self-test error that triggered cleanup.
        }
    }

    private void showProgress(String stage, long completed, long total) {
        mainHandler.post(() -> {
            if (destroyed.get()) {
                return;
            }
            progressView.setIndeterminate(total <= 0);
            if (total > 0) {
                long scaled = completed > Long.MAX_VALUE / 10_000
                        ? (completed / total) * 10_000
                        : completed * 10_000 / total;
                progressView.setProgress((int) Math.min(10_000, scaled));
            }
            statusView.setText(total > 0
                    ? getString(R.string.status_progress_total, stage,
                            formatBytes(completed), formatBytes(total))
                    : getString(R.string.status_progress, stage, formatBytes(completed)));
        });
    }

    private void finishImport(String result) {
        mainHandler.post(() -> {
            if (destroyed.get()) {
                return;
            }
            statusView.setText(result);
            importCancellation = null;
            setBusy(false);
        });
    }

    private void cancelImport() {
        AtomicBoolean cancellation = importCancellation;
        if (cancellation != null) {
            cancellation.set(true);
            statusView.setText(R.string.status_cancelling);
            cancelView.setEnabled(false);
        }
    }

    private void setBusy(boolean busy) {
        chooseView.setEnabled(!busy);
        cancelView.setEnabled(busy);
        cancelView.setVisibility(busy ? View.VISIBLE : View.GONE);
        progressView.setVisibility(busy ? View.VISIBLE : View.GONE);
        progressView.setIndeterminate(busy);
        if (busy) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    private static String formatBytes(long bytes) {
        if (bytes < 1024) {
            return bytes + " B";
        }
        double value = bytes;
        String[] units = {"KiB", "MiB", "GiB", "TiB"};
        int unit = -1;
        do {
            value /= 1024.0;
            unit++;
        } while (value >= 1024.0 && unit < units.length - 1);
        return String.format(Locale.ROOT, "%.2f %s", value, units[unit]);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // The scrollable, density-independent layout needs no reconstruction.
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putString(STATE_STATUS, statusView.getText().toString());
        super.onSaveInstanceState(outState);
    }

    @Override
    protected void onDestroy() {
        destroyed.set(true);
        AtomicBoolean cancellation = importCancellation;
        if (cancellation != null) {
            cancellation.set(true);
        }
        mainHandler.removeCallbacksAndMessages(null);
        importerExecutor.shutdownNow();
        try {
            shutdownNativeCore();
        } catch (Throwable ignored) {
            // The Activity is already terminating; no UI remains for this error.
        }
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        super.onDestroy();
    }
}
