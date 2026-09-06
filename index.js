/**
 * @format
 */

import {AppRegistry} from 'react-native';
import {name as appName} from './app.json';

// Install the Quest log sanitizer before loading App or any of its modules.
// Several upstream modules emit verbose diagnostics during module initialization,
// so a static `import App` here would be too late to protect those messages.
const {installSafeConsole} = require('./src/utils/safeConsole');
installSafeConsole();

const App = require('./src/App').default;

AppRegistry.registerComponent(appName, () => App);
