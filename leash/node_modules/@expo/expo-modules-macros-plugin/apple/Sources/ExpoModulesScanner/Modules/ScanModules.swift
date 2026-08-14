import Foundation

/// The scanner's public entry point. Argument parsing, subcommand dispatch, and usage text live in
/// the CLI target; this just runs a command and writes its JSON report to stdout.
///
/// The detection model (`Detection`, `DetectionVisitor`, …) stays `internal`: tests reach it via
/// `@testable import`, and the CLI only needs these entries, so nothing else is exposed.
public enum Scanner {
  /// Runs the `scan-modules` command over `paths`, prints the JSON report to stdout, and returns a
  /// process exit code: `0` on success, `1` if encoding fails. (The deep `scan-exports` command has
  /// its own `runExports` entry returning its own result type.)
  public static func runModules(paths: [String]) -> Int32 {
    let result = scanModules(paths: paths)

    do {
      let encoder = JSONEncoder()
      encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
      let data = try encoder.encode(result)
      FileHandle.standardOutput.write(data)
      FileHandle.standardOutput.write(Data("\n".utf8))
      return 0
    } catch {
      FileHandle.standardError.write(Data("error: failed to encode results: \(error)\n".utf8))
      return 1
    }
  }
}

/// One module in the `scan-modules` output. Trimmed to what `expo-modules-autolinking` needs to
/// register a module: the Swift class name, the JS name it registers under, and the file it's in.
/// The richer fields the visitor captures (declaration kind, raw macro arguments, line/column) are
/// dropped here — they're redundant for this command (the macro is always `@ExpoModule` on a class)
/// and the deep `scan-exports` surface carries the richer per-member detail instead.
struct ScannedModule: Codable, Equatable {
  /// The Swift class name the module is declared as.
  let name: String

  /// The fully-resolved JS module name: the `@ExpoModule("Foo")` override when present, otherwise the
  /// class name. Resolved here (rather than left `nil`) so it matches how the macro derives the name
  /// and the consumer never has to apply the fallback itself.
  let jsName: String

  /// Source file the module was found in, relative to the path the scanner was invoked with.
  let file: String
}

/// The `scan-modules` result: the detected modules plus the stats describing the run. Encoded as the
/// command's JSON output. (`scan-exports` returns its own `ScanExportsResult` shape; the two commands
/// serve different consumers and don't share an envelope.)
struct ScanModulesResult: Codable, Equatable {
  let modules: [ScannedModule]
  let stats: ScanStats
}

/// Scans the given paths for top-level `@ExpoModule` types and returns the modules (in file then
/// source order) plus the stats for the run — the `scan-modules` command. Kept separate from the
/// public entry (and `internal`) so tests can drive it without going through argv/stdout.
func scanModules(paths: [String]) -> ScanModulesResult {
  let scan = collectDetections(paths: paths, macros: [.expoModule])

  let modules = scan.detections.map {
    // Resolve the JS name the way the macro does: explicit `@ExpoModule("Foo")` override, else the
    // class name.
    ScannedModule(name: $0.name, jsName: $0.jsName ?? $0.name, file: $0.file)
  }

  return ScanModulesResult(modules: modules, stats: scan.stats)
}
