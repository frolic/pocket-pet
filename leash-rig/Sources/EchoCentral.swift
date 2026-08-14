import CoreBluetooth
import os

/// The thing under test: a CoreBluetooth central whose one job is to bounce
/// every frame the watch sends straight back, as fast as iOS allows, in
/// every app state — foreground, background, locked, and relaunched from
/// termination via State Restoration. The watch measures; this only echoes.
final class EchoCentral: NSObject, ObservableObject {
    static let service = CBUUID(string: "F0C0F00D-1EAF-4C02-8A5C-000000000001")
    static let infoCharacteristic = CBUUID(string: "F0C0F00D-1EAF-4C02-8A5C-000000000002")
    static let txCharacteristic = CBUUID(string: "F0C0F00D-1EAF-4C02-8A5C-000000000003")
    static let rxCharacteristic = CBUUID(string: "F0C0F00D-1EAF-4C02-8A5C-000000000004")
    static let restoreIdentifier = "dev.frolic.leashrig.central"
    static let savedPeripheralKey = "savedPeripheralIdentifier"

    @Published var status = "starting"
    @Published var echoCount = 0
    @Published var deviceInfo = "—"

    private let log = Logger(subsystem: "dev.frolic.leashrig", category: "echo")
    /// Dedicated serial queue: echo turnaround never waits on the main thread.
    private let queue = DispatchQueue(label: "leash.echo")
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var echoed = 0

    override init() {
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: queue,
            options: [CBCentralManagerOptionRestoreIdentifierKey: Self.restoreIdentifier]
        )
    }

    private func setStatus(_ text: String) {
        log.info("\(text, privacy: .public)")
        DispatchQueue.main.async { self.status = text }
    }

    private func connectToSavedOrScan() {
        if let saved = UserDefaults.standard.string(forKey: Self.savedPeripheralKey),
           let identifier = UUID(uuidString: saved),
           let known = central.retrievePeripherals(withIdentifiers: [identifier]).first {
            adopt(known)
            return
        }
        setStatus("scanning…")
        central.scanForPeripherals(withServices: [Self.service])
    }

    /// Take ownership of a peripheral and issue the pending connect — which
    /// never times out and survives at the system level, so the watch
    /// reappearing is what brings this app back to life.
    private func adopt(_ found: CBPeripheral) {
        peripheral = found
        found.delegate = self
        UserDefaults.standard.set(found.identifier.uuidString, forKey: Self.savedPeripheralKey)
        setStatus("connecting to \(found.name ?? "leash")…")
        central.connect(found)
    }
}

extension EchoCentral: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            setStatus("bluetooth \(String(describing: central.state.rawValue))")
            return
        }
        /* Restored peripherals arrive before poweredOn; (re)connect now. */
        if let existing = peripheral {
            if existing.state != .connected { central.connect(existing) }
            else { existing.discoverServices([Self.service]) }
        } else {
            connectToSavedOrScan()
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState state: [String: Any]) {
        /* iOS relaunched us (backgrounded or post-termination) because the
           watch spoke. Re-adopt whatever the system preserved. */
        if let restored = (state[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            peripheral = restored
            restored.delegate = self
            setStatus("restored by iOS")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover found: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        central.stopScan()
        adopt(found)
    }

    func centralManager(_ central: CBCentralManager, didConnect connected: CBPeripheral) {
        setStatus("connected, discovering…")
        connected.discoverServices([Self.service])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral lost: CBPeripheral,
                        error: Error?) {
        setStatus("disconnected — pending reconnect")
        rx = nil
        central.connect(lost) /* pending connect: fires whenever it returns */
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect failed: CBPeripheral,
                        error: Error?) {
        setStatus("connect failed — retrying")
        central.connect(failed)
    }
}

extension EchoCentral: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
            setStatus("leash service missing")
            return
        }
        peripheral.discoverCharacteristics(
            [Self.infoCharacteristic, Self.txCharacteristic, Self.rxCharacteristic],
            for: service
        )
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case Self.infoCharacteristic:
                peripheral.readValue(for: characteristic)
            case Self.txCharacteristic:
                peripheral.setNotifyValue(true, for: characteristic)
            case Self.rxCharacteristic:
                rx = characteristic
            default:
                break
            }
        }
        setStatus("echoing")
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard error == nil, let data = characteristic.value else { return }
        if characteristic.uuid == Self.infoCharacteristic {
            let info = String(data: data, encoding: .utf8) ?? "?"
            DispatchQueue.main.async { self.deviceInfo = info }
            return
        }
        /* The entire job: bounce it back immediately, still on our queue. */
        guard let rx else { return }
        peripheral.writeValue(data, for: rx, type: .withoutResponse)
        echoed += 1
        if echoed % 16 == 0 || echoed < 4 {
            let count = echoed
            DispatchQueue.main.async { self.echoCount = count }
        }
    }
}
