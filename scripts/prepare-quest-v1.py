#!/usr/bin/env python3
"""Apply Quest V1 OpenXR integration after the V0.5/V0.6 preparation.

V1a intentionally keeps the proven Xbox authentication/signalling/WebRTC stack
inside React Native and adds a separate immersive OpenXR activity. The activity
attaches a second sink to the already-decoded remote VideoTrack and feeds a
native OpenXR quad compositor layer.

This first bridge is correctness-first I420 CPU transfer with explicit timing
logs. Once the spatial path is proven on Quest 2 we can replace the CPU transfer
with a shared-EGL/zero-copy texture bridge and measure the delta.
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


def patch_build_gradle() -> None:
    path = "android/app/build.gradle"
    text = read(path)

    text = replace_once(
        text,
        '    namespace "com.xstreaming"\n    sourceSets {\n',
        '    namespace "com.xstreaming"\n'
        '    buildFeatures {\n'
        '        prefab true\n'
        '    }\n'
        '    externalNativeBuild {\n'
        '        cmake {\n'
        '            path "src/main/cpp/CMakeLists.txt"\n'
        '            version "3.22.1"\n'
        '        }\n'
        '    }\n'
        '    sourceSets {\n',
        "OpenXR Gradle native-build block",
    )

    text = replace_once(
        text,
        '    implementation("com.facebook.react:react-android")\n',
        '    implementation("com.facebook.react:react-android")\n'
        '    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.63")\n',
        "OpenXR Android loader dependency",
    )

    write(path, text)


def patch_manifest() -> None:
    path = "android/app/src/main/AndroidManifest.xml"
    text = read(path)

    text = replace_once(
        text,
        '      <!-- Game Mode configuration -->\n',
        '      <meta-data\n'
        '            android:name="com.oculus.supportedDevices"\n'
        '            android:value="quest2|questpro|quest3|quest3s" />\n\n'
        '      <!-- Game Mode configuration -->\n',
        "Quest supported-devices metadata",
    )

    activity = '''      <activity
        android:name=".QuestOpenXrActivity"
        android:label="XStreaming Spatial"
        android:theme="@android:style/Theme.Black.NoTitleBar.Fullscreen"
        android:screenOrientation="landscape"
        android:configChanges="density|keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|uiMode"
        android:launchMode="singleTask"
        android:excludeFromRecents="true"
        android:resizeableActivity="false"
        android:hardwareAccelerated="true"
        android:exported="false">
        <intent-filter>
          <action android:name="android.intent.action.MAIN" />
          <category android:name="android.intent.category.DEFAULT" />
          <category android:name="com.oculus.intent.category.VR" />
        </intent-filter>
      </activity>

'''

    text = replace_once(
        text,
        '      <activity\n        android:name=".MainActivity"\n',
        activity + '      <activity\n        android:name=".MainActivity"\n',
        "Quest OpenXR activity",
    )

    write(path, text)


def patch_audio_setting_module() -> None:
    path = "android/app/src/main/java/com/xstreaming/AudioSettingModule.java"
    text = read(path)

    tail = '''        } catch (Exception error) {
            promise.reject("RESTART_FAILED", error);
        }
    }
}
'''
    replacement = '''        } catch (Exception error) {
            promise.reject("RESTART_FAILED", error);
        }
    }

    /** Launch the experimental immersive OpenXR activity around the live stream. */
    @ReactMethod
    public void launchSpatial(String streamUrl, Promise promise) {
        try {
            Activity activity = getCurrentActivity();
            if (activity == null) {
                promise.reject("NO_ACTIVITY", "No foreground XStreaming activity");
                return;
            }
            if (streamUrl == null || streamUrl.trim().isEmpty()) {
                promise.reject("NO_STREAM", "Remote video stream is not ready");
                return;
            }

            Intent intent = new Intent(activity, QuestOpenXrActivity.class);
            intent.putExtra(QuestOpenXrActivity.EXTRA_STREAM_URL, streamUrl);
            activity.startActivity(intent);
            promise.resolve(true);
        } catch (Exception error) {
            promise.reject("SPATIAL_LAUNCH_FAILED", error);
        }
    }
}
'''
    text = replace_once(text, tail, replacement, "AudioSettingModule spatial launcher")
    write(path, text)


def patch_quick_settings() -> None:
    path = "src/components/QuestQuickSettingsPanel.tsx"
    text = read(path)

    text = replace_once(
        text,
        '  audioGain: number;\n  onClose: () => void;\n',
        '  audioGain: number;\n  streamURL: string;\n  onClose: () => void;\n',
        "QuickSettings streamURL prop type",
    )

    text = replace_once(
        text,
        '  audioGain,\n  onClose,\n',
        '  audioGain,\n  streamURL,\n  onClose,\n',
        "QuickSettings streamURL destructuring",
    )

    spatial_section = '''        <SectionTitle>SPATIAL V1</SectionTitle>
        <Text style={styles.sectionNote}>
          Experimental native OpenXR compositor. It keeps the existing Xbox
          Remote Play session and adds a second video sink for a real spatial
          quad instead of Horizon's generic Android panel.
        </Text>
        <Pressable
          accessibilityRole="button"
          disabled={!connected || !streamURL}
          onPress={() => {
            const launchSpatial = NativeModules.AudioSettingModule?.launchSpatial;
            if (typeof launchSpatial === 'function') {
              Promise.resolve(launchSpatial(streamURL)).catch(() => {});
            }
          }}
          style={({pressed}) => [
            styles.restartButton,
            (!connected || !streamURL) && styles.controlDisabled,
            pressed && connected && !!streamURL && styles.rowPressed,
          ]}>
          <Text style={styles.restartButtonText}>Enter Spatial V1</Text>
          <Text style={styles.actionsButtonHint}>
            First proof uses a CPU I420 bridge so we can measure it before
            replacing it with a zero-copy shared-texture path.
          </Text>
        </Pressable>

'''

    text = replace_once(
        text,
        '        <SectionTitle>DECODER</SectionTitle>\n',
        spatial_section + '        <SectionTitle>DECODER</SectionTitle>\n',
        "QuickSettings Spatial V1 section",
    )

    write(path, text)


def patch_native_stream() -> None:
    path = "src/pages/NativeStream.tsx"
    text = read(path)

    text = replace_once(
        text,
        '        audioGain={audioGain}\n        onClose={() => setShowQuestQuickSettings(false)}\n',
        '        audioGain={audioGain}\n        streamURL={remote || \'\'}\n        onClose={() => setShowQuestQuickSettings(false)}\n',
        "NativeStream spatial streamURL wiring",
    )

    write(path, text)


def main() -> None:
    patch_build_gradle()
    patch_manifest()
    patch_audio_setting_module()
    patch_quick_settings()
    patch_native_stream()

    print("Quest V1 OpenXR preparation complete:")
    print("  - Khronos OpenXR Android loader 1.1.63 via Prefab")
    print("  - dedicated immersive QuestOpenXrActivity")
    print("  - active react-native-webrtc track bridged to OpenXR")
    print("  - 1280x720 compositor quad, 2.4m wide at 2m default")
    print("  - CPU I420 bridge timing + overwrite instrumentation")
    print("  - V0.6 decoder/latency instrumentation retained")


if __name__ == "__main__":
    main()
