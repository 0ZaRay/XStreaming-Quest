package com.oney.WebRTCModule;

import android.util.Log;

import com.facebook.react.bridge.ReactContext;
import com.xstreaming.QuestOpenXrActivity;

import org.webrtc.MediaStream;
import org.webrtc.VideoFrame;
import org.webrtc.VideoSink;
import org.webrtc.VideoTrack;

import java.nio.ByteBuffer;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Package-local access bridge into react-native-webrtc 124.x.
 *
 * WebRTCModule#getStreamForReactTag and ThreadUtils are intentionally package
 * scoped. Keeping this helper in com.oney.WebRTCModule lets the Quest fork add
 * a second sink to the already-negotiated remote VideoTrack without forking the
 * whole react-native-webrtc dependency.
 */
public final class QuestXrStreamBridge {
    private static final String TAG = "QuestXRBridge";
    private static final Object LOCK = new Object();

    private static volatile VideoTrack attachedTrack;
    private static final AtomicBoolean active = new AtomicBoolean(false);
    private static final AtomicLong convertedFrames = new AtomicLong(0);
    private static final AtomicLong conversionNanos = new AtomicLong(0);
    private static final AtomicLong maxConversionNanos = new AtomicLong(0);

    private QuestXrStreamBridge() {}

    private static final VideoSink SINK = frame -> {
        if (!active.get()) {
            return;
        }

        final long startNs = System.nanoTime();
        VideoFrame.I420Buffer i420 = null;
        try {
            i420 = frame.getBuffer().toI420();
            if (i420 == null) {
                return;
            }

            // slice() makes the JNI direct-buffer address begin at the current
            // plane position rather than assuming every WebRTC buffer starts at 0.
            ByteBuffer y = i420.getDataY().slice();
            ByteBuffer u = i420.getDataU().slice();
            ByteBuffer v = i420.getDataV().slice();

            QuestOpenXrActivity.submitI420Frame(
                    y,
                    u,
                    v,
                    i420.getWidth(),
                    i420.getHeight(),
                    i420.getStrideY(),
                    i420.getStrideU(),
                    i420.getStrideV(),
                    frame.getRotation(),
                    frame.getTimestampNs());
        } catch (Throwable error) {
            Log.w(TAG, "Failed to convert/forward a WebRTC frame", error);
        } finally {
            if (i420 != null) {
                i420.release();
            }

            final long elapsedNs = System.nanoTime() - startNs;
            final long count = convertedFrames.incrementAndGet();
            conversionNanos.addAndGet(elapsedNs);
            updateMax(maxConversionNanos, elapsedNs);

            // Keep logs sparse and credential-free. This is specifically here
            // to tell us whether the correctness-first CPU bridge is too costly
            // to keep for the latency-focused production path.
            if (count % 120 == 0) {
                long total = conversionNanos.getAndSet(0);
                long max = maxConversionNanos.getAndSet(0);
                double avgMs = total / 120.0 / 1_000_000.0;
                double maxMs = max / 1_000_000.0;
                Log.i(TAG, String.format(
                        "[QuestXRBridgeCpu] frames=120 avgMs=%.3f maxMs=%.3f",
                        avgMs,
                        maxMs));
            }
        }
    };

    private static void updateMax(AtomicLong maxHolder, long value) {
        long current;
        do {
            current = maxHolder.get();
            if (value <= current) {
                return;
            }
        } while (!maxHolder.compareAndSet(current, value));
    }

    /**
     * Schedules attachment on react-native-webrtc's executor. A true return
     * means the request was valid and scheduled; the actual stream lookup is
     * asynchronous and logged when it completes.
     */
    public static boolean attach(ReactContext reactContext, String streamReactTag) {
        if (reactContext == null || streamReactTag == null || streamReactTag.isEmpty()) {
            return false;
        }

        WebRTCModule module = reactContext.getNativeModule(WebRTCModule.class);
        if (module == null) {
            Log.w(TAG, "WebRTCModule is unavailable");
            return false;
        }

        ThreadUtils.runOnExecutor(() -> {
            synchronized (LOCK) {
                detachLocked();

                MediaStream stream = module.getStreamForReactTag(streamReactTag);
                if (stream == null) {
                    Log.w(TAG, "Remote MediaStream was not found for spatial mode");
                    return;
                }

                List<VideoTrack> tracks = stream.videoTracks;
                if (tracks == null || tracks.isEmpty()) {
                    Log.w(TAG, "Remote MediaStream has no video track");
                    return;
                }

                VideoTrack track = tracks.get(0);
                try {
                    active.set(true);
                    track.addSink(SINK);
                    attachedTrack = track;
                    convertedFrames.set(0);
                    conversionNanos.set(0);
                    maxConversionNanos.set(0);
                    Log.i(TAG, "Attached OpenXR sink to active remote video track");
                } catch (Throwable error) {
                    active.set(false);
                    attachedTrack = null;
                    Log.w(TAG, "Could not attach OpenXR video sink", error);
                }
            }
        });
        return true;
    }

    public static void detach() {
        active.set(false);
        ThreadUtils.runOnExecutor(() -> {
            synchronized (LOCK) {
                detachLocked();
            }
        });
    }

    private static void detachLocked() {
        active.set(false);
        VideoTrack track = attachedTrack;
        attachedTrack = null;
        if (track != null) {
            try {
                track.removeSink(SINK);
                Log.i(TAG, "Detached OpenXR sink from remote video track");
            } catch (Throwable error) {
                Log.w(TAG, "Could not detach OpenXR video sink", error);
            }
        }
    }
}
