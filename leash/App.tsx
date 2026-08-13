import { useRef, useState } from "react";
import {
  Pressable,
  SafeAreaView,
  ScrollView,
  StatusBar,
  StyleSheet,
  Text,
  View,
} from "react-native";
import { runDemoScenario } from "./src/demo/runDemoScenario";
import { sha256 } from "./src/relay/sha256";

/* Leash: the watch's modem. [connect] runs the real BLE relay (dev build
   only — ble-plx is a native module); [demo] runs the same engine against
   a scripted mock watch and works everywhere, Expo Go included. */

export default function App() {
  const [lines, setLines] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const scroll = useRef<ScrollView>(null);
  const log = (line: string) => setLines((previous) => [...previous, line]);

  async function startDemo() {
    setBusy(true);
    setLines([]);
    try {
      await runDemoScenario({ sha256, log });
    } catch (error) {
      log(`demo failed: ${error}`);
    }
    setBusy(false);
  }

  async function startBle() {
    setBusy(true);
    setLines([]);
    try {
      /* Loaded lazily: in Expo Go / web the native module is absent and
         this throws a readable error instead of crashing at import. */
      const { BleManager } = require("react-native-ble-plx");
      const { runBleSession } = require("./src/relay/runBleSession");
      const manager = new BleManager();
      await runBleSession({ manager, log });
      /* Session stays live; the watch drives it from here. */
    } catch (error) {
      log(`ble unavailable: ${error}`);
      log("(real relaying needs the dev build: npx expo run:ios --device)");
      setBusy(false);
    }
  }

  return (
    <SafeAreaView style={styles.screen}>
      <StatusBar barStyle="light-content" />
      <View style={styles.header}>
        <Text style={styles.title}>leash</Text>
        <Text style={styles.subtitle}>the watch's modem</Text>
      </View>
      <View style={styles.buttons}>
        <Pressable style={[styles.button, busy && styles.buttonDim]} onPress={startBle} disabled={busy}>
          <Text style={styles.buttonText}>connect to watch</Text>
        </Pressable>
        <Pressable style={[styles.button, busy && styles.buttonDim]} onPress={startDemo} disabled={busy}>
          <Text style={styles.buttonText}>mock demo</Text>
        </Pressable>
      </View>
      <ScrollView
        ref={scroll}
        style={styles.log}
        onContentSizeChange={() => scroll.current?.scrollToEnd({ animated: false })}
      >
        {lines.map((line, index) => (
          <Text key={index} style={[styles.line, line.startsWith("—") && styles.phase]}>
            {line}
          </Text>
        ))}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: "#101418" },
  header: { padding: 16, borderBottomWidth: 1, borderBottomColor: "#232a31" },
  title: { color: "#e8c33b", fontSize: 24, fontWeight: "700", letterSpacing: 1 },
  subtitle: { color: "#8a949e", fontSize: 12, marginTop: 2 },
  buttons: { flexDirection: "row", gap: 10, padding: 12 },
  button: {
    backgroundColor: "#1d2733",
    borderColor: "#2e3d4d",
    borderWidth: 1,
    borderRadius: 8,
    paddingVertical: 10,
    paddingHorizontal: 14,
  },
  buttonDim: { opacity: 0.4 },
  buttonText: { color: "#c7d0d9", fontSize: 13, fontWeight: "600" },
  log: { flex: 1, paddingHorizontal: 12 },
  line: { color: "#c7d0d9", fontFamily: "Menlo", fontSize: 11, marginBottom: 3 },
  phase: { color: "#e8c33b", marginTop: 10 },
});
