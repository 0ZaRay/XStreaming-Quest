import React from 'react';
import {StyleSheet, View} from 'react-native';

type Props = {
  isLight: boolean;
};

/**
 * Quest-safe page background.
 *
 * The upstream decorative background is implemented as a full-window
 * react-native-svg canvas. On Quest 2 / Horizon OS the Android window can
 * report very large spatial dimensions, causing react-native-svg to allocate
 * an enormous backing bitmap and crash in SvgView.onDraw before the app can be
 * used. V0 intentionally favors functionality over decoration, so keep the
 * same component contract but render a native View instead of SVG.
 */
const XboxSymbolBackground = ({isLight}: Props) => {
  return (
    <View
      pointerEvents="none"
      style={[
        styles.background,
        isLight ? styles.backgroundLight : styles.backgroundDark,
      ]}
    />
  );
};

const styles = StyleSheet.create({
  background: {
    ...StyleSheet.absoluteFillObject,
  },
  backgroundLight: {
    backgroundColor: '#FCFBFF',
  },
  backgroundDark: {
    backgroundColor: '#111320',
  },
});

export default XboxSymbolBackground;
