import Foundation
import SwiftParser
import SwiftSyntax

/// Walks `paths`, and for each `.swift` file that might contain one of `macros` (the pre-filter passes
/// it), reads the source and hands it to `process` along with the file path. Returns the run's stats.
/// The shared core every scan command builds on: the walk, read, pre-filter, and stats are identical;
/// only what each command does per parsed file differs (`scan-modules` collects `Detection`s,
/// `scan-exports` walks a `SurfaceVisitor`), and that lives in `process`.
func scanFiles(
  paths: [String],
  macros: Set<DetectedMacro>,
  process: (_ source: String, _ file: String) -> Void
) -> ScanStats {
  let clock = ContinuousClock()
  let start = clock.now

  var filesScanned = 0
  var filesParsed = 0

  // Compile the pre-filter regex once per run, not once per file.
  let prefilter = macroAttributeRegex(for: macros)

  for file in swiftFiles(in: paths) {
    guard let source = try? String(contentsOfFile: file, encoding: .utf8) else {
      FileHandle.standardError.write(Data("warning: could not read \(file)\n".utf8))
      continue
    }
    filesScanned += 1
    // Skip the (relatively expensive) parse for files that can't contain any of the macros. A plain
    // substring scan is far cheaper than a full parse, and most files in a large tree mention none
    // of these names. See `mightContainMacro` for why this never drops a real match.
    guard mightContainMacro(in: source, prefilter: prefilter) else {
      continue
    }
    filesParsed += 1
    process(source, file)
  }

  let elapsed = (clock.now - start).components
  let durationMs = Double(elapsed.seconds) * 1000 + Double(elapsed.attoseconds) / 1e15

  return ScanStats(filesScanned: filesScanned, filesParsed: filesParsed, durationMs: durationMs)
}

/// Walks `paths`, parses each `.swift` file that might contain one of `macros`, and returns every
/// detection (in file then source order) with the run's stats — the shape `scan-modules` projects.
/// A thin layer over `scanFiles` that accumulates the per-file detections.
func collectDetections(paths: [String], macros: Set<DetectedMacro>) -> (detections: [Detection], stats: ScanStats) {
  var detections: [Detection] = []
  let stats = scanFiles(paths: paths, macros: macros) { source, file in
    detections.append(contentsOf: detect(source: source, file: file, macros: macros))
  }
  return (detections, stats)
}

/// Parses one source string and returns its detections for the given macro set. The unit of work the
/// tests exercise.
func detect(source: String, file: String, macros: Set<DetectedMacro>) -> [Detection] {
  let tree = Parser.parse(source: source)
  let visitor = DetectionVisitor(file: file, tree: tree, detectedMacros: macros)
  visitor.walk(tree)
  return visitor.detections
}

// MARK: - Pre-filter

/// Builds the pre-filter regex for a macro set, e.g. `@(ExpoModule)` for a `modules` scan or
/// `@(ExpoModule|JS|Record|SharedObject)` for an `exports` scan. A precompiled `NSRegularExpression`
/// benchmarked ~20x faster over a large source tree than calling `String.contains` once per macro
/// name, because it scans each file in a single pass. Compiled once per run and reused per file.
func macroAttributeRegex(for macros: Set<DetectedMacro>) -> NSRegularExpression {
  // Sort for a stable pattern regardless of the set's iteration order.
  let alternation = macros.map(\.rawValue).sorted().joined(separator: "|")
  return try! NSRegularExpression(pattern: "@(\(alternation))")
}

/// True if the source text contains one of the pre-filter's spelled macro attributes, so it's worth
/// parsing. A deliberate over-approximation: the pattern can still match inside a comment or string,
/// in which case the file is parsed and correctly yields no detections — a wasted parse, never a
/// missed module. It assumes the attribute is written with no space after `@` (`@ExpoModule`, not
/// `@ ExpoModule`), which is universal in practice; the rare spaced form would be skipped.
func mightContainMacro(in source: String, prefilter: NSRegularExpression) -> Bool {
  let range = NSRange(source.startIndex..., in: source)
  return prefilter.firstMatch(in: source, range: range) != nil
}

// MARK: - File discovery

/// Directory names skipped during the recursive walk. These hold build products, dependencies, and
/// git internals — never source worth scanning — and pruning them keeps the walk from descending
/// into the bulk of a monorepo's files.
private let prunedDirectoryNames: Set<String> = [".build", "Pods", ".git"]

/// Expands the given paths into the list of `.swift` files to parse: a file path passes through,
/// a directory is enumerated recursively (skipping `prunedDirectoryNames`). Order is deterministic
/// so output is stable across runs.
///
/// Reported paths are absolute, so the output is unambiguous and independent of the caller's working
/// directory. (A future `--root` option could emit paths relative to a given base when a portable,
/// shorter form is wanted.)
func swiftFiles(in paths: [String]) -> [String] {
  let fileManager = FileManager.default
  var result: [String] = []

  for path in paths {
    var isDirectory: ObjCBool = false
    guard fileManager.fileExists(atPath: path, isDirectory: &isDirectory) else {
      FileHandle.standardError.write(Data("warning: no such path \(path)\n".utf8))
      continue
    }

    if isDirectory.boolValue {
      result.append(contentsOf: swiftFiles(inDirectory: URL(fileURLWithPath: path), fileManager: fileManager))
    } else if path.hasSuffix(".swift") {
      // A directory walk already yields absolute paths; resolve a directly-passed file the same way
      // so every reported path is absolute regardless of how it was spelled.
      result.append(URL(fileURLWithPath: path).standardizedFileURL.path)
    }
  }

  return result.sorted()
}

/// Recursively enumerates `.swift` files under a directory, calling `skipDescendants()` on any
/// pruned directory so its subtree is never read. Uses the URL enumerator (rather than the
/// path-based one) precisely because it supports skipping a subtree mid-walk.
///
/// Directory-ness is read from `hasDirectoryPath` (the enumerator sets a trailing slash on the URLs
/// it yields) rather than `resourceValues(forKeys: [.isDirectoryKey])`, which re-`stat`s each entry.
/// The walk is the dominant cost of a whole-tree scan, and skipping that per-entry stat measurably
/// shortens it.
private func swiftFiles(inDirectory directory: URL, fileManager: FileManager) -> [String] {
  guard let enumerator = fileManager.enumerator(
    at: directory,
    includingPropertiesForKeys: nil,
    options: [.skipsHiddenFiles]
  ) else {
    return []
  }

  var result: [String] = []
  for case let url as URL in enumerator {
    if url.hasDirectoryPath {
      if prunedDirectoryNames.contains(url.lastPathComponent) {
        enumerator.skipDescendants()
      }
    } else if url.pathExtension == "swift" {
      result.append(url.path)
    }
  }
  return result
}
