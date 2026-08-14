import SwiftUI

@main
struct LeashRigApp: App {
    @StateObject private var echo = EchoCentral()

    var body: some Scene {
        WindowGroup {
            VStack(alignment: .leading, spacing: 12) {
                Text("leash rig").font(.title).bold().foregroundStyle(.yellow)
                Text("native echo — the watch measures")
                    .font(.caption).foregroundStyle(.secondary)
                Divider()
                LabeledContent("status", value: echo.status)
                LabeledContent("device", value: echo.deviceInfo)
                LabeledContent("echoed", value: "\(echo.echoCount)")
                Spacer()
                Text("Leave this running. Background it, lock the phone, even let iOS kill it — the watch's leashping tells the truth.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .padding()
            .monospaced()
        }
    }
}
