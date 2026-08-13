import { useEffect, useRef, useState } from "react";
import { SafeAreaView, ScrollView, StatusBar, StyleSheet, Text, View } from "react-native";
import * as Crypto from "expo-crypto";
import { runDemoScenario } from "./src/demo/runDemoScenario";

/* Leash v0: the relay engine running against a scripted mock watch.
   BLE replaces the mock at P2 — nothing above the Transport seam changes. */

async function sha256(bytes: Uint8Array): Promise<string> {
  const digest = await Crypto.digest(Crypto.CryptoDigestAlgorithm.SHA256, bytes.buffer as ArrayBuffer);
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

export default function App() {
  const [lines, setLines] = useState<string[]>([]);
  const scroll = useRef<ScrollView>(null);

  useEffect(() => {
    const log = (line: string) => setLines((prev) => [...prev, line]);
    runDemoScenario({ sha256, log }).catch((error) => log(`demo failed: ${error}`));
  }, []);

  return (
    <SafeAreaView style={styles.screen}>
      <StatusBar barStyle="light-content" />
      <View style={styles.header}>
        <Text style={styles.title}>leash</Text>
        <Text style={styles.subtitle}>mock watch → relay engine → fake server</Text>
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
  log: { flex: 1, padding: 12 },
  line: { color: "#c7d0d9", fontFamily: "Menlo", fontSize: 11, marginBottom: 3 },
  phase: { color: "#e8c33b", marginTop: 10 },
});
