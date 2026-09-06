import React from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  useWindowDimensions,
  View,
} from 'react-native';
import {IconButton} from 'react-native-paper';

type SettingsPatch = Record<string, any>;

type QuestQuickSettingsPanelProps = {
  visible: boolean;
  connected: boolean;
  settings: any;
  showPerformance: boolean;
  audioGain: number;
  onClose: () => void;
  onPatchSettings: (patch: SettingsPatch) => void;
  onTogglePerformance: () => void;
  onAudioGainChange: (value: number) => void;
  onOpenActions: () => void;
};

type Choice = {
  label: string;
  value: any;
};

const clamp = (value: number, min: number, max: number) =>
  Math.max(min, Math.min(max, value));

function SectionTitle({children}: {children: React.ReactNode}) {
  return <Text style={styles.sectionTitle}>{children}</Text>;
}

function ToggleRow({
  title,
  description,
  enabled,
  onChange,
}: {
  title: string;
  description?: string;
  enabled: boolean;
  onChange: (enabled: boolean) => void;
}) {
  return (
    <Pressable
      accessibilityRole="switch"
      accessibilityState={{checked: enabled}}
      onPress={() => onChange(!enabled)}
      style={({pressed}) => [styles.row, pressed && styles.rowPressed]}>
      <View style={styles.rowText}>
        <Text style={styles.rowTitle}>{title}</Text>
        {!!description && <Text style={styles.rowDescription}>{description}</Text>}
      </View>
      <View style={[styles.togglePill, enabled && styles.togglePillEnabled]}>
        <Text style={styles.toggleText}>{enabled ? 'ON' : 'OFF'}</Text>
      </View>
    </Pressable>
  );
}

function StepperRow({
  title,
  description,
  value,
  min,
  max,
  step = 1,
  valueLabel,
  onChange,
}: {
  title: string;
  description?: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  valueLabel?: (value: number) => string;
  onChange: (value: number) => void;
}) {
  const safeValue = clamp(Number.isFinite(value) ? value : min, min, max);
  const displayValue = valueLabel ? valueLabel(safeValue) : String(safeValue);

  return (
    <View style={styles.row}>
      <View style={styles.rowText}>
        <Text style={styles.rowTitle}>{title}</Text>
        {!!description && <Text style={styles.rowDescription}>{description}</Text>}
      </View>
      <View style={styles.stepper}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Decrease ${title}`}
          disabled={safeValue <= min}
          onPress={() => onChange(clamp(safeValue - step, min, max))}
          style={({pressed}) => [
            styles.stepButton,
            safeValue <= min && styles.controlDisabled,
            pressed && safeValue > min && styles.rowPressed,
          ]}>
          <Text style={styles.stepButtonText}>−</Text>
        </Pressable>
        <Text style={styles.stepValue}>{displayValue}</Text>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Increase ${title}`}
          disabled={safeValue >= max}
          onPress={() => onChange(clamp(safeValue + step, min, max))}
          style={({pressed}) => [
            styles.stepButton,
            safeValue >= max && styles.controlDisabled,
            pressed && safeValue < max && styles.rowPressed,
          ]}>
          <Text style={styles.stepButtonText}>+</Text>
        </Pressable>
      </View>
    </View>
  );
}

function ChoiceRow({
  title,
  description,
  value,
  choices,
  onChange,
}: {
  title: string;
  description?: string;
  value: any;
  choices: Choice[];
  onChange: (value: any) => void;
}) {
  return (
    <View style={styles.choiceBlock}>
      <Text style={styles.rowTitle}>{title}</Text>
      {!!description && <Text style={styles.rowDescription}>{description}</Text>}
      <View style={styles.choiceWrap}>
        {choices.map(choice => {
          const selected = choice.value === value;
          return (
            <Pressable
              accessibilityRole="button"
              accessibilityState={{selected}}
              key={`${title}-${String(choice.value)}`}
              onPress={() => onChange(choice.value)}
              style={({pressed}) => [
                styles.choice,
                selected && styles.choiceSelected,
                pressed && styles.rowPressed,
              ]}>
              <Text
                style={[
                  styles.choiceText,
                  selected && styles.choiceTextSelected,
                ]}>
                {choice.label}
              </Text>
            </Pressable>
          );
        })}
      </View>
    </View>
  );
}

export default function QuestQuickSettingsPanel({
  visible,
  connected,
  settings,
  showPerformance,
  audioGain,
  onClose,
  onPatchSettings,
  onTogglePerformance,
  onAudioGainChange,
  onOpenActions,
}: QuestQuickSettingsPanelProps) {
  const {width} = useWindowDimensions();

  if (!visible) {
    return null;
  }

  const panelWidth =
    width < 720 ? Math.max(260, width - 24) : Math.min(430, width * 0.42);
  const fsrSharpness = clamp(
    Number(settings?.fsr_display_options?.sharpness ?? 2),
    0,
    20,
  );

  const patchFsrSharpness = (sharpness: number) => {
    onPatchSettings({
      fsr_display_options: {
        ...(settings?.fsr_display_options || {}),
        sharpness,
      },
    });
  };

  return (
    <View style={[styles.panel, {width: panelWidth}]}>
      <View style={styles.header}>
        <View style={styles.headerText}>
          <Text style={styles.title}>Quick Settings</Text>
          <Text style={styles.subtitle}>
            {connected ? 'Xbox Remote Play • live' : 'Remote Play'}
          </Text>
        </View>
        <IconButton
          accessibilityLabel="Close quick settings"
          icon="close"
          iconColor="#ffffff"
          size={24}
          onPress={onClose}
        />
      </View>

      <ScrollView
        contentContainerStyle={styles.scrollContent}
        showsVerticalScrollIndicator={false}>
        <SectionTitle>LIVE</SectionTitle>

        <ToggleRow
          title="Performance HUD"
          description="Show stream statistics over the video."
          enabled={showPerformance}
          onChange={() => onTogglePerformance()}
        />

        <ToggleRow
          title="FSR upscaling"
          description="Switch the native renderer's FSR processing while streaming."
          enabled={!!settings?.fsr}
          onChange={enabled => onPatchSettings({fsr: enabled})}
        />

        {!!settings?.fsr && (
          <StepperRow
            title="FSR sharpness"
            description="0 is softest, 20 is strongest."
            value={fsrSharpness}
            min={0}
            max={20}
            onChange={patchFsrSharpness}
          />
        )}

        <ChoiceRow
          title="Video fit"
          description="Changes how the 16:9 Xbox image fills this Quest window."
          value={settings?.video_format || ''}
          choices={[
            {label: 'Aspect', value: ''},
            {label: 'Stretch', value: 'Stretch'},
            {label: 'Zoom', value: 'Zoom'},
          ]}
          onChange={video_format => onPatchSettings({video_format})}
        />

        <StepperRow
          title="Audio gain"
          description="Remote stream volume multiplier."
          value={audioGain}
          min={0}
          max={10}
          valueLabel={value => `${value}×`}
          onChange={onAudioGainChange}
        />

        <SectionTitle>NEXT CONNECTION</SectionTitle>
        <Text style={styles.sectionNote}>
          These are saved now, but the active Xbox session keeps its negotiated
          stream until the next connection.
        </Text>

        <ChoiceRow
          title="Resolution"
          value={settings?.resolution ?? 720}
          choices={[
            {label: '720p', value: 720},
            {label: '1080p', value: 1080},
            {label: 'HQ', value: 1081},
          ]}
          onChange={resolution => onPatchSettings({resolution})}
        />

        <ChoiceRow
          title="Codec"
          value={settings?.codec || ''}
          choices={[
            {label: 'Auto', value: ''},
            {label: 'H264 High', value: 'video/H264-4d'},
            {label: 'H264 Med', value: 'video/H264-42e'},
            {label: 'H264 Low', value: 'video/H264-420'},
          ]}
          onChange={codec => onPatchSettings({codec})}
        />

        <SectionTitle>APP RESTART</SectionTitle>
        <ToggleRow
          title="Ultra-low-latency decoder"
          description="Selects the Quest/Qualcomm decoder path on the next app process start."
          enabled={!!settings?.native_low_latency_decoder}
          onChange={native_low_latency_decoder =>
            onPatchSettings({native_low_latency_decoder})
          }
        />

        <Pressable
          accessibilityRole="button"
          onPress={onOpenActions}
          style={({pressed}) => [
            styles.actionsButton,
            pressed && styles.rowPressed,
          ]}>
          <Text style={styles.actionsButtonText}>More stream actions</Text>
          <Text style={styles.actionsButtonHint}>
            Xbox button, microphone, text input, disconnect and power controls
          </Text>
        </Pressable>

        <View style={styles.bottomSpacer} />
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: {
    position: 'absolute',
    right: 12,
    top: 12,
    bottom: 12,
    zIndex: 250,
    elevation: 24,
    borderRadius: 18,
    overflow: 'hidden',
    backgroundColor: 'rgba(7, 9, 11, 0.95)',
    borderWidth: 1,
    borderColor: 'rgba(255, 255, 255, 0.14)',
  },
  header: {
    minHeight: 68,
    paddingLeft: 18,
    paddingRight: 6,
    flexDirection: 'row',
    alignItems: 'center',
    borderBottomWidth: 1,
    borderBottomColor: 'rgba(255, 255, 255, 0.10)',
  },
  headerText: {
    flex: 1,
  },
  title: {
    color: '#ffffff',
    fontSize: 21,
    fontWeight: '700',
  },
  subtitle: {
    color: 'rgba(255, 255, 255, 0.58)',
    fontSize: 12,
    marginTop: 2,
  },
  scrollContent: {
    paddingHorizontal: 14,
    paddingBottom: 18,
  },
  sectionTitle: {
    color: '#80d887',
    fontSize: 11,
    fontWeight: '800',
    letterSpacing: 1.2,
    marginTop: 18,
    marginBottom: 8,
  },
  sectionNote: {
    color: 'rgba(255, 255, 255, 0.55)',
    fontSize: 12,
    lineHeight: 17,
    marginBottom: 8,
  },
  row: {
    minHeight: 62,
    borderRadius: 12,
    backgroundColor: 'rgba(255, 255, 255, 0.055)',
    paddingHorizontal: 12,
    paddingVertical: 10,
    marginBottom: 8,
    flexDirection: 'row',
    alignItems: 'center',
  },
  rowPressed: {
    opacity: 0.72,
  },
  rowText: {
    flex: 1,
    paddingRight: 10,
  },
  rowTitle: {
    color: '#ffffff',
    fontSize: 15,
    fontWeight: '600',
  },
  rowDescription: {
    color: 'rgba(255, 255, 255, 0.55)',
    fontSize: 11,
    lineHeight: 15,
    marginTop: 3,
  },
  togglePill: {
    minWidth: 50,
    height: 28,
    borderRadius: 14,
    backgroundColor: 'rgba(255, 255, 255, 0.13)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  togglePillEnabled: {
    backgroundColor: '#107c10',
  },
  toggleText: {
    color: '#ffffff',
    fontSize: 11,
    fontWeight: '800',
  },
  stepper: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  stepButton: {
    width: 36,
    height: 36,
    borderRadius: 10,
    backgroundColor: 'rgba(255, 255, 255, 0.12)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  stepButtonText: {
    color: '#ffffff',
    fontSize: 22,
    lineHeight: 24,
    fontWeight: '500',
  },
  stepValue: {
    minWidth: 42,
    color: '#ffffff',
    textAlign: 'center',
    fontSize: 14,
    fontWeight: '700',
  },
  controlDisabled: {
    opacity: 0.28,
  },
  choiceBlock: {
    borderRadius: 12,
    backgroundColor: 'rgba(255, 255, 255, 0.055)',
    padding: 12,
    marginBottom: 8,
  },
  choiceWrap: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    marginTop: 9,
    marginHorizontal: -3,
  },
  choice: {
    minHeight: 34,
    paddingHorizontal: 11,
    borderRadius: 10,
    backgroundColor: 'rgba(255, 255, 255, 0.09)',
    alignItems: 'center',
    justifyContent: 'center',
    margin: 3,
  },
  choiceSelected: {
    backgroundColor: '#107c10',
  },
  choiceText: {
    color: 'rgba(255, 255, 255, 0.72)',
    fontSize: 12,
    fontWeight: '600',
  },
  choiceTextSelected: {
    color: '#ffffff',
  },
  actionsButton: {
    marginTop: 18,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'rgba(255, 255, 255, 0.16)',
    paddingHorizontal: 14,
    paddingVertical: 13,
    backgroundColor: 'rgba(255, 255, 255, 0.07)',
  },
  actionsButtonText: {
    color: '#ffffff',
    fontSize: 14,
    fontWeight: '700',
  },
  actionsButtonHint: {
    color: 'rgba(255, 255, 255, 0.50)',
    fontSize: 10,
    lineHeight: 14,
    marginTop: 3,
  },
  bottomSpacer: {
    height: 4,
  },
});
