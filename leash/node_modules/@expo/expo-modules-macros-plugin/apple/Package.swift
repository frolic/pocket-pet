// swift-tools-version: 6.2
// The swift-tools-version declares the minimum version of Swift required to build this package.

import CompilerPluginSupport
import Foundation
import PackageDescription

let package = Package(
  name: "ExpoModulesMacros",
  platforms: [.macOS(.v13)],
  products: [
    // The scanner CLI. Named `ExpoModulesScanner` (the user-facing tool name) while its target is
    // `ExpoModulesScannerCLI`; the detection logic lives in the importable `ExpoModulesScanner`
    // library that both the CLI and the tests depend on.
    .executable(name: "ExpoModulesScanner", targets: ["ExpoModulesScannerCLI"]),
  ],
  dependencies: [
    .package(url: "https://github.com/swiftlang/swift-syntax.git", from: "602.0.0-latest")
  ],
  targets: [
    .macro(
      name: "ExpoModulesMacros",
      dependencies: [
        .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
        .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
      ]
    ),
    .target(
      name: "ExpoModulesScanner",
      dependencies: [
        .product(name: "SwiftSyntax", package: "swift-syntax"),
        .product(name: "SwiftParser", package: "swift-syntax"),
      ]
    ),
    .executableTarget(
      name: "ExpoModulesScannerCLI",
      dependencies: ["ExpoModulesScanner"]
    ),
  ]
)

// The Tests directory is excluded from the published npm package,
// so only declare the test target when building from the repository.
if FileManager.default.fileExists(atPath: Context.packageDirectory + "/Tests") {
  package.targets.append(
    .testTarget(
      name: "ExpoModulesMacrosTests",
      dependencies: [
        "ExpoModulesMacros",
        .product(name: "SwiftSyntaxMacrosTestSupport", package: "swift-syntax"),
        .product(name: "SwiftSyntaxMacrosGenericTestSupport", package: "swift-syntax"),
      ]
    )
  )
  package.targets.append(
    .testTarget(
      name: "ExpoModulesScannerTests",
      dependencies: [
        "ExpoModulesScanner",
        .product(name: "SwiftParser", package: "swift-syntax"),
      ]
    )
  )
}
