import { createRelayEngine } from "../protocol/createRelayEngine";
import { BleConnection, createBleTransport } from "../transport/createBleTransport";
import { sha256 } from "./sha256";

/* One live relay session: scan → connect → read INFO → run the real
   engine over the real wire. The watch drives everything from here
   (all intent originates on the device); this side just logs. */
export async function runBleSession(options: {
  manager: import("react-native-ble-plx").BleManager;
  log: (line: string) => void;
}): Promise<BleConnection> {
  const connection = await createBleTransport({
    manager: options.manager,
    onLog: options.log,
  });
  createRelayEngine({
    transport: connection.transport,
    fetchFn: fetch,
    sha256,
    log: options.log,
  });
  options.log(`relay live for ${connection.info.device_id} (proto ${connection.info.proto})`);
  return connection;
}
