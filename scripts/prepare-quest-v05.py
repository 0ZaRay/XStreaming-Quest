#!/usr/bin/env python3
"""Apply the Quest V0.5 in-stream UI patch to the upstream NativeStream screen.

The fork deliberately keeps the large upstream NativeStream.tsx close to upstream
while V0.x is still proving the Quest UX. This script performs small, guarded,
exact-string edits in CI. Every replacement must match exactly once so upstream
changes fail loudly instead of silently producing a partially patched APK.
"""

from pathlib import Path

NATIVE_STREAM = Path("src/pages/NativeStream.tsx")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"Expected exactly one {label} block, found {count}. "
            "Upstream/branch source changed; inspect the Quest V0.5 patch before building."
        )
    return text.replace(old, new, 1)


def main() -> None:
    text = NATIVE_STREAM.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "import NativeTouchOverlay from '../components/NativeTouchOverlay';\n",
        "import NativeTouchOverlay from '../components/NativeTouchOverlay';\n"
        "import QuestQuickSettingsPanel from '../components/QuestQuickSettingsPanel';\n",
        "Quest quick-settings import",
    )

    text = replace_once(
        text,
        "  const [showModal, setShowModal] = React.useState(false);\n"
        "  const [showVirtualGamepad, setShowVirtualGamepad] = React.useState(false);",
        "  const [showModal, setShowModal] = React.useState(false);\n"
        "  const [showQuestQuickSettings, setShowQuestQuickSettings] =\n"
        "    React.useState(false);\n"
        "  const [showVirtualGamepad, setShowVirtualGamepad] = React.useState(false);",
        "Quest quick-settings state",
    )

    old_audio_block = '''  const handleAudioGainChange = React.useCallback(
    (value: number) => {
      const nextGain = Math.max(0, Math.min(10, Math.round(value)));
      audioGainRef.current = nextGain;
      setAudioGain(nextGain);
      applyRemoteAudioGain(nextGain);
    },
    [applyRemoteAudioGain],
  );
'''

    new_audio_block = old_audio_block + '''
  const applyQuestSettingsPatch = React.useCallback((patch: any) => {
    const nextSettings = {
      ...getSettings(),
      ...patch,
    };
    saveSettings(nextSettings);
    setSettings(nextSettings);
  }, []);
'''

    text = replace_once(
        text,
        old_audio_block,
        new_audio_block,
        "Quest live-settings updater",
    )

    old_menu = '''  const renderMenu = () => {
    if (!portraitMode && settings.show_menu && !isInPictureInPicture) {
      return (
        <View style={styles.quickMenu}>
          <IconButton
            icon="menu"
            size={28}
            onPress={() => {
              setShowModal(true);
            }}
          />
        </View>
      );
    } else {
      return null;
    }
  };
'''

    new_menu = '''  const renderMenu = () => {
    if (
      !portraitMode &&
      connectState === CONNECTED &&
      !isInPictureInPicture
    ) {
      return (
        <View style={styles.quickMenu}>
          <IconButton
            accessibilityLabel="Quick settings"
            icon={showQuestQuickSettings ? 'close' : 'cog-outline'}
            iconColor="#ffffff"
            size={28}
            onPress={() => {
              setShowQuestQuickSettings(current => !current);
            }}
          />
        </View>
      );
    } else {
      return null;
    }
  };
'''

    text = replace_once(text, old_menu, new_menu, "Quest quick-settings button")

    old_editor_and_menu = '''      <VirtualGamepadEditor
        visible={!portraitMode && showGamepadEditor && !isInPictureInPicture}
        profileName={editorProfile || getActiveProfileName()}
        onSave={handleSaveGamepadLayout}
        onCancel={() => setShowGamepadEditor(false)}
      />

      {renderMenu()}
'''

    new_editor_and_menu = '''      <VirtualGamepadEditor
        visible={!portraitMode && showGamepadEditor && !isInPictureInPicture}
        profileName={editorProfile || getActiveProfileName()}
        onSave={handleSaveGamepadLayout}
        onCancel={() => setShowGamepadEditor(false)}
      />

      <QuestQuickSettingsPanel
        visible={
          showQuestQuickSettings &&
          connectState === CONNECTED &&
          !isInPictureInPicture
        }
        connected={connectState === CONNECTED}
        settings={settings}
        showPerformance={showPerformance}
        audioGain={audioGain}
        onClose={() => setShowQuestQuickSettings(false)}
        onPatchSettings={applyQuestSettingsPatch}
        onTogglePerformance={() => {
          const nextShowPerformance = !showPerformance;
          setShowPerformance(nextShowPerformance);
          applyQuestSettingsPatch({show_performance: nextShowPerformance});
        }}
        onAudioGainChange={handleAudioGainChange}
        onOpenActions={() => {
          setShowQuestQuickSettings(false);
          setShowModal(true);
        }}
      />

      {renderMenu()}
'''

    text = replace_once(
        text,
        old_editor_and_menu,
        new_editor_and_menu,
        "Quest quick-settings panel render",
    )

    old_quick_menu_style = '''  quickMenu: {
    position: 'absolute',
    right: 5,
    bottom: 5,
    zIndex: 99,
  },
'''

    new_quick_menu_style = '''  quickMenu: {
    position: 'absolute',
    right: 8,
    top: '44%',
    zIndex: 300,
    elevation: 30,
    borderRadius: 28,
    backgroundColor: 'rgba(7, 9, 11, 0.78)',
    borderWidth: 1,
    borderColor: 'rgba(255, 255, 255, 0.15)',
  },
'''

    text = replace_once(
        text,
        old_quick_menu_style,
        new_quick_menu_style,
        "Quest quick-settings button style",
    )

    NATIVE_STREAM.write_text(text, encoding="utf-8")
    print("Quest V0.5 stream UI patch complete:")
    print("  - always-visible in-stream settings tab while connected")
    print("  - right-side live quick-settings panel")
    print("  - live FSR, sharpness, video fit, audio and performance controls")
    print("  - next-connection resolution/codec controls")
    print("  - app-restart low-latency decoder control")
    print("  - existing stream actions remain reachable from the panel")


if __name__ == "__main__":
    main()
