import { BleManager, Device, Subscription } from "react-native-ble-plx";
import { Transport } from "../protocol/common";

/* The real wire. Everything above the Transport seam (engine, codec,
   tests, demo) is untouched by BLE's arrival — that was the seam's job. */

export const LEASH_SERVICE = "f0c0f00d-1eaf-4c02-8a5c-000000000001";
export const LEASH_INFO = "f0c0f00d-1eaf-4c02-8a5c-000000000002";
export const LEASH_TX = "f0c0f00d-1eaf-4c02-8a5c-000000000003";
export const LEASH_RX = "f0c0f00d-1eaf-4c02-8a5c-000000000004";

function base64ToBytes(data: string): Uint8Array {
  const binary = globalThis.atob(data);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return globalThis.btoa(binary);
}

export interface BleConnection {
  transport: Transport;
  device: Device;
  info: { device_id: string; proto: number };
  disconnect(): Promise<void>;
}

/** Scans for a leash peripheral, connects, reads INFO, and exposes the
    framed channel as a Transport. */
export async function createBleTransport(options: {
  manager: BleManager;
  timeoutMs?: number;
  onLog?: (line: string) => void;
}): Promise<BleConnection> {
  const { manager } = options;
  const log = options.onLog ?? (() => {});
  const timeoutMs = options.timeoutMs ?? 15000;

  log("scanning for leash…");
  const device = await new Promise<Device>((resolve, reject) => {
    const timer = setTimeout(() => {
      manager.stopDeviceScan();
      reject(new Error("no leash peripheral found"));
    }, timeoutMs);
    manager.startDeviceScan([LEASH_SERVICE], null, (error, found) => {
      if (error) {
        clearTimeout(timer);
        manager.stopDeviceScan();
        reject(error);
        return;
      }
      if (found) {
        clearTimeout(timer);
        manager.stopDeviceScan();
        resolve(found);
      }
    });
  });

  log(`found ${device.name ?? device.id}, connecting…`);
  const connected = await device.connect();
  await connected.discoverAllServicesAndCharacteristics();
  const mtu = connected.mtu;
  log(`connected, mtu=${mtu}`);

  const infoCharacteristic = await connected.readCharacteristicForService(LEASH_SERVICE, LEASH_INFO);
  const info = JSON.parse(
    new TextDecoder().decode(base64ToBytes(infoCharacteristic.value ?? ""))
  );
  log(`INFO: ${JSON.stringify(info)}`);

  let receive: ((bytes: Uint8Array) => void) | undefined;
  const subscription: Subscription = connected.monitorCharacteristicForService(
    LEASH_SERVICE,
    LEASH_TX,
    (error, characteristic) => {
      if (error) {
        log(`notify error: ${error.message}`);
        return;
      }
      if (characteristic?.value) receive?.(base64ToBytes(characteristic.value));
    }
  );

  const transport: Transport = {
    /* ATT write payload is MTU-3; our frame sizing works from this. */
    mtu: () => mtu - 3,
    send: async (bytes) => {
      await connected.writeCharacteristicWithoutResponseForService(
        LEASH_SERVICE,
        LEASH_RX,
        bytesToBase64(bytes)
      );
    },
    onReceive: (callback) => {
      receive = callback;
    },
  };

  return {
    transport,
    device: connected,
    info,
    disconnect: async () => {
      subscription.remove();
      await connected.cancelConnection();
    },
  };
}
