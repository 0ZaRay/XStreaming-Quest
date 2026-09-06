#!/usr/bin/env python3
"""Apply Quest V0.6 patches on top of the V0.5 CI preparation.

V0.6 goals:
- close remaining Xbox stream credential redaction gaps;
- make the process-level Qualcomm decoder toggle restartable from the Quest UI;
- collect comparable WebRTC latency/quality telemetry without logging SDP, ICE
  credentials, Xbox access keys, account identifiers, or candidate addresses.

This script intentionally fails loudly when an expected V0.5/upstream anchor is
missing. CI should never publish a partially patched APK.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def patch_safe_console() -> None:
    path = "src/utils/safeConsole.js"
    text = read(path)

    text = replace_once(
        text,
        "    normalized.includes('cookie') ||\n",
        "    normalized.includes('cookie') ||\n"
        "    normalized.includes('accesskey') ||\n"
        "    normalized.includes('installid') ||\n",
        "safeConsole sensitive-key expansion",
    )

    text = replace_once(
        text,
        "authorizationtoken|gstoken|webtoken|xcloudtoken|xhometoken|credential|server_credential|password|client_secret|secret|sessionid|device_code|user_code|code_verifier",
        "authorizationtoken|accesskey|streamingaccesskey|gstoken|webtoken|xcloudtoken|xhometoken|credential|server_credential|password|client_secret|secret|sessionid|device_code|user_code|code_verifier|clientappinstallid",
        "safeConsole flattened-text redaction",
    )

    write(path, text)


def patch_low_latency_config() -> None:
    path = "android/app/src/main/java/com/xstreaming/LowLatencyDecoderConfig.java"
    write(
        path,
        """package com.xstreaming;

import android.content.Context;
import android.content.SharedPreferences;

public final class LowLatencyDecoderConfig {
    private static final String PREF_NAME = "xstreaming_low_latency_decoder_config";
    private static final String KEY_ENABLED = "native_low_latency_decoder";

    private LowLatencyDecoderConfig() {}

    public static boolean isEnabled(Context context) {
        SharedPreferences preferences = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        return preferences.getBoolean(KEY_ENABLED, false);
    }

    public static void setEnabled(Context context, boolean enabled) {
        SharedPreferences preferences = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        preferences.edit().putBoolean(KEY_ENABLED, enabled).apply();
    }

    /**
     * Persist synchronously immediately before a deliberate process restart.
     * This avoids racing SharedPreferences.apply() against Process.killProcess().
     */
    public static boolean setEnabledSync(Context context, boolean enabled) {
        SharedPreferences preferences = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        return preferences.edit().putBoolean(KEY_ENABLED, enabled).commit();
    }
}
""",
    )


def patch_audio_setting_module() -> None:
    path = "android/app/src/main/java/com/xstreaming/AudioSettingModule.java"
    write(
        path,
        """package com.xstreaming;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;

public class AudioSettingModule extends ReactContextBaseJavaModule {
    private static final int RESTART_REQUEST_CODE = 90606;
    private final ReactApplicationContext reactContext;
    private final boolean lowLatencyDecoderActiveAtProcessStart;

    public AudioSettingModule(ReactApplicationContext reactContext) {
        super(reactContext);
        this.reactContext = reactContext;
        // MainApplication selects its VideoDecoderFactory before the React
        // bridge is created. Capture that process-level choice now so the UI
        // can distinguish the active decoder from a newly saved preference.
        this.lowLatencyDecoderActiveAtProcessStart =
                LowLatencyDecoderConfig.isEnabled(reactContext);
    }

    @Override
    public String getName() {
        return "AudioSettingModule";
    }

    @ReactMethod
    public void setStereoEnabled(boolean enabled) {
        AudioConfig.setStereoEnabled(reactContext, enabled);
    }

    @ReactMethod
    public void setLowLatencyDecoderEnabled(boolean enabled) {
        LowLatencyDecoderConfig.setEnabled(reactContext, enabled);
    }

    @ReactMethod
    public void getLowLatencyDecoderActive(Promise promise) {
        promise.resolve(lowLatencyDecoderActiveAtProcessStart);
    }

    /**
     * Relaunch XStreaming through a system-held PendingIntent, then terminate
     * this process. Horizon closing a 2D panel does not reliably kill the app,
     * but the decoder factory is selected only during process startup.
     */
    @ReactMethod
    public void restartApp(boolean lowLatencyDecoderEnabled, Promise promise) {
        try {
            if (!LowLatencyDecoderConfig.setEnabledSync(
                    reactContext, lowLatencyDecoderEnabled)) {
                promise.reject("PREF_WRITE_FAILED", "Could not persist decoder preference");
                return;
            }

            Intent launchIntent = reactContext
                    .getPackageManager()
                    .getLaunchIntentForPackage(reactContext.getPackageName());
            if (launchIntent == null) {
                promise.reject("NO_LAUNCH_INTENT", "Could not resolve XStreaming launch intent");
                return;
            }

            launchIntent.addFlags(
                    Intent.FLAG_ACTIVITY_NEW_TASK
                            | Intent.FLAG_ACTIVITY_CLEAR_TASK
                            | Intent.FLAG_ACTIVITY_CLEAR_TOP);

            PendingIntent pendingIntent = PendingIntent.getActivity(
                    reactContext,
                    RESTART_REQUEST_CODE,
                    launchIntent,
                    PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);

            AlarmManager alarmManager =
                    (AlarmManager) reactContext.getSystemService(Context.ALARM_SERVICE);
            if (alarmManager == null) {
                promise.reject("NO_ALARM_MANAGER", "AlarmManager unavailable");
                return;
            }

            // set(), rather than setExact(), deliberately avoids exact-alarm
            // permission requirements. A short restart delay is all we need.
            alarmManager.set(
                    AlarmManager.ELAPSED_REALTIME_WAKEUP,
                    SystemClock.elapsedRealtime() + 900,
                    pendingIntent);

            promise.resolve(true);

            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                Activity activity = getCurrentActivity();
                if (activity != null) {
                    activity.finishAffinity();
                }
                android.os.Process.killProcess(android.os.Process.myPid());
                System.exit(0);
            }, 180);
        } catch (Exception error) {
            promise.reject("RESTART_FAILED", error);
        }
    }
}
""",
    )


def patch_quick_settings() -> None:
    path = "src/components/QuestQuickSettingsPanel.tsx"
    text = read(path)

    text = replace_once(
        text,
        "  Pressable,\n  ScrollView,\n  StyleSheet,\n  Text,\n  useWindowDimensions,\n",
        "  NativeModules,\n  Pressable,\n  ScrollView,\n  StyleSheet,\n  Text,\n  useWindowDimensions,\n",
        "QuickSettings NativeModules import",
    )

    text = replace_once(
        text,
        "  const {width} = useWindowDimensions();\n\n  if (!visible) {\n",
        """  const {width} = useWindowDimensions();
  const [activeLowLatencyDecoder, setActiveLowLatencyDecoder] =
    React.useState<boolean | null>(null);
  const [restartBusy, setRestartBusy] = React.useState(false);
  const [restartError, setRestartError] = React.useState('');

  React.useEffect(() => {
    let cancelled = false;
    if (!visible) {
      return () => {
        cancelled = true;
      };
    }

    const getter = NativeModules.AudioSettingModule?.getLowLatencyDecoderActive;
    if (typeof getter !== 'function') {
      setActiveLowLatencyDecoder(null);
      return () => {
        cancelled = true;
      };
    }

    Promise.resolve(getter())
      .then((active: boolean) => {
        if (!cancelled) {
          setActiveLowLatencyDecoder(!!active);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setActiveLowLatencyDecoder(null);
        }
      });

    return () => {
      cancelled = true;
    };
  }, [visible]);

  const requestedLowLatencyDecoder = !!settings?.native_low_latency_decoder;
  const restartRequired =
    activeLowLatencyDecoder !== null &&
    activeLowLatencyDecoder !== requestedLowLatencyDecoder;

  const restartForDecoderChange = async () => {
    const restart = NativeModules.AudioSettingModule?.restartApp;
    if (typeof restart !== 'function' || restartBusy) {
      return;
    }
    setRestartBusy(true);
    setRestartError('');
    try {
      await restart(requestedLowLatencyDecoder);
    } catch (error: any) {
      setRestartBusy(false);
      setRestartError(error?.message || 'Could not restart XStreaming.');
    }
  };

  if (!visible) {
""",
        "QuickSettings decoder process-state hook",
    )

    old_restart = """        <SectionTitle>APP RESTART</SectionTitle>
        <ToggleRow
          title="Ultra-low-latency decoder"
          description="Selects the Quest/Qualcomm decoder path on the next app process start."
          enabled={!!settings?.native_low_latency_decoder}
          onChange={native_low_latency_decoder =>
            onPatchSettings({native_low_latency_decoder})
          }
        />

"""
    new_restart = """        <SectionTitle>DECODER</SectionTitle>
        <ToggleRow
          title="Ultra-low-latency decoder"
          description="Uses the Quest 2 Qualcomm low-latency H.264 path. Changing it requires a real Android process restart."
          enabled={requestedLowLatencyDecoder}
          onChange={native_low_latency_decoder =>
            onPatchSettings({native_low_latency_decoder})
          }
        />

        <Text style={styles.restartStatus}>
          Active now:{' '}
          {activeLowLatencyDecoder === null
            ? 'checking…'
            : activeLowLatencyDecoder
            ? 'Qualcomm low latency'
            : 'standard decoder'}
        </Text>

        {restartRequired && (
          <Pressable
            accessibilityRole="button"
            disabled={restartBusy}
            onPress={restartForDecoderChange}
            style={({pressed}) => [
              styles.restartButton,
              (pressed || restartBusy) && styles.rowPressed,
            ]}>
            <Text style={styles.restartButtonText}>
              {restartBusy ? 'Restarting…' : 'Restart XStreaming to apply'}
            </Text>
            <Text style={styles.actionsButtonHint}>
              Remote Play will disconnect. XStreaming will reopen automatically.
            </Text>
          </Pressable>
        )}

        {!!restartError && (
          <Text style={styles.restartError}>{restartError}</Text>
        )}

        <SectionTitle>LATENCY LAB</SectionTitle>
        <Text style={styles.sectionNote}>
          V0.6 records safe one-second WebRTC samples while connected: RTT,
          network jitter, jitter-buffer delay, raw decoder time, bitrate and
          frame delivery. The Performance HUD can be enabled above; diagnostic
          logs use the [QuestPerf] tag.
        </Text>

"""
    text = replace_once(text, old_restart, new_restart, "QuickSettings decoder section")

    text = replace_once(
        text,
        "  actionsButton: {\n",
        """  restartStatus: {
    color: 'rgba(255, 255, 255, 0.70)',
    fontSize: 11,
    marginTop: -2,
    marginBottom: 8,
    paddingHorizontal: 4,
  },
  restartButton: {
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'rgba(128, 216, 135, 0.50)',
    paddingHorizontal: 14,
    paddingVertical: 12,
    marginBottom: 8,
    backgroundColor: 'rgba(16, 124, 16, 0.20)',
  },
  restartButtonText: {
    color: '#ffffff',
    fontSize: 13,
    fontWeight: '800',
  },
  restartError: {
    color: '#ff9c9c',
    fontSize: 11,
    lineHeight: 15,
    marginBottom: 8,
    paddingHorizontal: 4,
  },
  actionsButton: {
""",
        "QuickSettings restart styles",
    )

    write(path, text)


def patch_webrtc_metrics() -> None:
    path = "src/webrtc/index.ts"
    text = read(path)

    text = replace_once(
        text,
        "    this._resetAudioLevelTracking();\n    this._resetVideoTrackState();\n\n    // Use custom STUN/TURN server\n",
        "    this._resetAudioLevelTracking();\n    this._resetVideoTrackState();\n    globalThis._lastStat = null;\n\n    // Use custom STUN/TURN server\n",
        "WebRTC stats reset",
    )

    start = text.find("  getStreamState() {")
    end = text.find("\n  setPollRate(value: number) {", start)
    if start < 0 or end < 0:
        raise RuntimeError("WebRTC getStreamState anchors not found")

    replacement = r'''  getStreamState() {
    return new Promise(resolve => {
      const performances: any = {
        resolution: '',
        rtt: '--',
        rttMs: null,
        jit: '--',
        jitterBufferMs: null,
        networkJitter: '--',
        networkJitterMs: null,
        fps: 0,
        pl: '--',
        fl: '--',
        br: '--',
        bitrateMbps: null,
        decode: '--',
        decodeMs: null,
        receiveFps: null,
        decodeFps: null,
        framesReceivedDelta: null,
        framesDecodedDelta: null,
        framesDroppedDelta: null,
        packetsLostDelta: null,
        availableIncomingMbps: null,
      };

      if (!this._webrtcClient) {
        resolve(performances);
        return;
      }

      this._webrtcClient
        .getStats()
        .then(stats => {
          let sampledVideo = false;
          let selectedCandidateSeen = false;

          stats.forEach((stat: any) => {
            if (
              stat.type === 'inbound-rtp' &&
              (stat.kind === 'video' || stat.mediaType === 'video')
            ) {
              sampledVideo = true;
              if (stat.frameWidth && stat.frameHeight) {
                performances.resolution = `${stat.frameWidth} X ${stat.frameHeight}`;
              }

              performances.fps = Number(stat.framesPerSecond || 0);

              if (typeof stat.jitter === 'number') {
                performances.networkJitterMs = stat.jitter * 1000;
                performances.networkJitter = `${performances.networkJitterMs.toFixed(
                  2,
                )}ms`;
              }

              const framesDropped = Number(stat.framesDropped || 0);
              const framesReceived = Number(stat.framesReceived || 0);
              const totalFrames = framesDropped + framesReceived;
              const droppedPercent = totalFrames > 0
                ? (framesDropped * 100) / totalFrames
                : 0;
              performances.fl = `${framesDropped} (${droppedPercent.toFixed(2)}%)`;

              const packetsLost = Number(stat.packetsLost || 0);
              const packetsReceived = Number(stat.packetsReceived || 0);
              const totalPackets = packetsLost + packetsReceived;
              const lostPercent = totalPackets > 0
                ? (packetsLost * 100) / totalPackets
                : 0;
              performances.pl = `${packetsLost} (${lostPercent.toFixed(2)}%)`;

              const lastStat: any = globalThis._lastStat;
              const sameStream =
                !!lastStat && (!lastStat.id || !stat.id || lastStat.id === stat.id);

              if (sameStream) {
                const timeDiffMs = Number(stat.timestamp) - Number(lastStat.timestamp);
                if (timeDiffMs > 0) {
                  const seconds = timeDiffMs / 1000;

                  const bytesDelta =
                    Number(stat.bytesReceived || 0) -
                    Number(lastStat.bytesReceived || 0);
                  if (bytesDelta >= 0) {
                    performances.bitrateMbps =
                      (bytesDelta * 8) / seconds / 1_000_000;
                    performances.br = `${performances.bitrateMbps.toFixed(2)} Mbps`;
                  }

                  const receivedDelta =
                    Number(stat.framesReceived || 0) -
                    Number(lastStat.framesReceived || 0);
                  const decodedDelta =
                    Number(stat.framesDecoded || 0) -
                    Number(lastStat.framesDecoded || 0);
                  const droppedDelta =
                    Number(stat.framesDropped || 0) -
                    Number(lastStat.framesDropped || 0);
                  const lostDelta =
                    Number(stat.packetsLost || 0) -
                    Number(lastStat.packetsLost || 0);

                  performances.framesReceivedDelta = Math.max(0, receivedDelta);
                  performances.framesDecodedDelta = Math.max(0, decodedDelta);
                  performances.framesDroppedDelta = Math.max(0, droppedDelta);
                  performances.packetsLostDelta = Math.max(0, lostDelta);
                  performances.receiveFps = Math.max(0, receivedDelta / seconds);
                  performances.decodeFps = Math.max(0, decodedDelta / seconds);

                  const jitterBufferDelayDelta =
                    Number(stat.jitterBufferDelay || 0) -
                    Number(lastStat.jitterBufferDelay || 0);
                  const emittedDelta =
                    Number(stat.jitterBufferEmittedCount || 0) -
                    Number(lastStat.jitterBufferEmittedCount || 0);
                  if (emittedDelta > 0 && jitterBufferDelayDelta >= 0) {
                    performances.jitterBufferMs =
                      (jitterBufferDelayDelta / emittedDelta) * 1000;
                    performances.jit = `${performances.jitterBufferMs.toFixed(2)}ms`;
                  }

                  const decodeTimeDelta =
                    Number(stat.totalDecodeTime || 0) -
                    Number(lastStat.totalDecodeTime || 0);
                  if (decodedDelta > 0 && decodeTimeDelta >= 0) {
                    // Deliberately expose the raw W3C WebRTC statistic. V0.6
                    // does not subtract guessed Android correction constants.
                    performances.decodeMs =
                      (decodeTimeDelta / decodedDelta) * 1000;
                    performances.decode = `${performances.decodeMs.toFixed(2)}ms`;
                  }
                }
              }

              globalThis._lastStat = {
                id: stat.id,
                timestamp: Number(stat.timestamp),
                bytesReceived: Number(stat.bytesReceived || 0),
                framesReceived: Number(stat.framesReceived || 0),
                framesDecoded: Number(stat.framesDecoded || 0),
                framesDropped: Number(stat.framesDropped || 0),
                packetsLost: Number(stat.packetsLost || 0),
                jitterBufferDelay: Number(stat.jitterBufferDelay || 0),
                jitterBufferEmittedCount: Number(
                  stat.jitterBufferEmittedCount || 0,
                ),
                totalDecodeTime: Number(stat.totalDecodeTime || 0),
              };
            } else if (
              stat.type === 'candidate-pair' &&
              stat.state === 'succeeded'
            ) {
              const preferred = stat.nominated === true || stat.selected === true;
              if (!selectedCandidateSeen || preferred) {
                if (typeof stat.currentRoundTripTime === 'number') {
                  performances.rttMs = stat.currentRoundTripTime * 1000;
                  performances.rtt = `${performances.rttMs.toFixed(2)}ms`;
                }
                if (typeof stat.availableIncomingBitrate === 'number') {
                  performances.availableIncomingMbps =
                    stat.availableIncomingBitrate / 1_000_000;
                }
                if (preferred) {
                  selectedCandidateSeen = true;
                }
              }
            }
          });

          if (sampledVideo) {
            const settings = getSettings();
            // Keep this object intentionally narrow. Never add SDP, ICE
            // candidates, remote addresses, Xbox identifiers or auth values.
            console.info('[QuestPerf]', {
              decoderRequested: settings.native_low_latency_decoder
                ? 'qcom-low-latency'
                : 'standard',
              resolution: performances.resolution || null,
              fps: performances.fps,
              receiveFps: performances.receiveFps,
              decodeFps: performances.decodeFps,
              rttMs: performances.rttMs,
              networkJitterMs: performances.networkJitterMs,
              jitterBufferMs: performances.jitterBufferMs,
              decodeMs: performances.decodeMs,
              bitrateMbps: performances.bitrateMbps,
              availableIncomingMbps: performances.availableIncomingMbps,
              framesReceivedDelta: performances.framesReceivedDelta,
              framesDecodedDelta: performances.framesDecodedDelta,
              framesDroppedDelta: performances.framesDroppedDelta,
              packetsLostDelta: performances.packetsLostDelta,
            });
          }

          resolve(performances);
        })
        .catch(error => {
          console.log('getStreamState getStats error:', error);
          resolve(performances);
        });
    });
  }
'''

    text = text[:start] + replacement + text[end:]
    write(path, text)


def patch_native_stream_sampling() -> None:
    path = "src/pages/NativeStream.tsx"
    text = read(path)
    text = replace_once(
        text,
        "      connectState !== CONNECTED ||\n      !showPerformance ||\n      !webrtcClient ||\n",
        "      connectState !== CONNECTED ||\n      !webrtcClient ||\n",
        "NativeStream always-on V0.6 telemetry",
    )
    write(path, text)


def patch_perf_panel() -> None:
    path = "src/components/PerfPanel.tsx"
    text = read(path)
    anchor = """        <View>
          <Text style={styles.text}>
            {t('JIT')}: {performance.jit || '-1'} {isHorizon ? '| ' : ''}
          </Text>
        </View>
"""
    addition = anchor + """        <View>
          <Text style={styles.text}>
            NJ: {performance.networkJitter || '-1'} {isHorizon ? '| ' : ''}
          </Text>
        </View>
"""
    text = replace_once(text, anchor, addition, "PerfPanel network jitter row")
    write(path, text)


def main() -> None:
    patch_safe_console()
    patch_low_latency_config()
    patch_audio_setting_module()
    patch_quick_settings()
    patch_webrtc_metrics()
    patch_native_stream_sampling()
    patch_perf_panel()
    print("[Quest V0.6] Applied redaction, restart UX, and latency instrumentation patches.")


if __name__ == "__main__":
    main()
