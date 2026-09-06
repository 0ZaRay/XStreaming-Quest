package com.xstreaming;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import com.facebook.react.ReactInstanceManager;
import com.facebook.react.bridge.ReactContext;
import com.oney.WebRTCModule.QuestXrStreamBridge;

import java.nio.ByteBuffer;

/**
 * Experimental Quest V1 immersive shell.
 *
 * The existing React Native activity owns Xbox authentication/signalling and the
 * WebRTC PeerConnection. This activity is deliberately a separate immersive
 * OpenXR surface. QuestXrStreamBridge attaches an additional VideoSink to the
 * already-decoded remote VideoTrack and forwards the latest I420 frame to the
 * native OpenXR compositor layer.
 *
 * V1 starts with a correctness-first CPU bridge. It is instrumented so we can
 * measure its cost before replacing it with a shared-EGL/zero-copy texture path.
 */
public class QuestOpenXrActivity extends Activity {
    private static final String TAG = "QuestXRActivity";
    public static final String EXTRA_STREAM_URL = "xstreaming_stream_url";

    private String streamUrl = "";
    private boolean bridgeAttached = false;

    static {
        System.loadLibrary("openxr_loader");
        System.loadLibrary("xstreaming_xr");
    }

    private static native void nativeStart(Activity activity);
    private static native void nativeSetResumed(boolean resumed);
    private static native void nativeStop();
    private static native void nativeSetScreenConfig(
            float widthMeters,
            float distanceMeters,
            float tiltDegrees,
            boolean headLocked);
    private static native void nativeSubmitI420Frame(
            ByteBuffer y,
            ByteBuffer u,
            ByteBuffer v,
            int width,
            int height,
            int strideY,
            int strideU,
            int strideV,
            int rotation,
            long timestampNs);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);

        if (getIntent() != null) {
            String requestedStreamUrl = getIntent().getStringExtra(EXTRA_STREAM_URL);
            if (requestedStreamUrl != null) {
                streamUrl = requestedStreamUrl;
            }
        }

        // Conservative first spatial preset. V1 controls will expose these
        // values in-headset after the stream bridge itself is proven.
        nativeSetScreenConfig(2.40f, 2.00f, 0.0f, false);
        nativeStart(this);
        attachToReactStream();
    }

    private void attachToReactStream() {
        if (streamUrl.isEmpty()) {
            Log.w(TAG, "No WebRTC stream tag supplied; OpenXR will show its test surface");
            return;
        }

        MainApplication application = (MainApplication) getApplication();
        ReactInstanceManager manager =
                application.getReactNativeHost().getReactInstanceManager();
        ReactContext reactContext = manager.getCurrentReactContext();
        if (reactContext == null) {
            Log.w(TAG, "React context unavailable while entering spatial mode");
            return;
        }

        bridgeAttached = QuestXrStreamBridge.attach(reactContext, streamUrl);
        Log.i(TAG, bridgeAttached
                ? "Requested attachment to active Xbox video track"
                : "Could not request attachment to active Xbox video track");
    }

    /**
     * Called by the WebRTC-package bridge while the decoded frame is valid.
     * Native code copies the planes immediately into a latest-frame buffer.
     */
    public static void submitI420Frame(
            ByteBuffer y,
            ByteBuffer u,
            ByteBuffer v,
            int width,
            int height,
            int strideY,
            int strideU,
            int strideV,
            int rotation,
            long timestampNs) {
        nativeSubmitI420Frame(
                y,
                u,
                v,
                width,
                height,
                strideY,
                strideU,
                strideV,
                rotation,
                timestampNs);
    }

    @Override
    protected void onResume() {
        super.onResume();
        nativeSetResumed(true);
    }

    @Override
    protected void onPause() {
        nativeSetResumed(false);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (bridgeAttached) {
            QuestXrStreamBridge.detach();
            bridgeAttached = false;
        }
        nativeStop();
        super.onDestroy();
    }
}
