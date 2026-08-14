import Foundation

/// Which Expo macro was found on a declaration. The scanner recognizes the entry-point macros that
/// mark a type or member as part of a module's JS surface, plus `@Record` for convertible types.
enum DetectedMacro: String, Codable, CaseIterable {
  case expoModule = "ExpoModule"
  case js = "JS"
  case sharedObject = "SharedObject"
  case record = "Record"
}

/// A single argument passed to a macro, e.g. `"Foo"` or `classes: [Bar.self]`. The label is `nil`
/// for positional arguments; `value` is the argument expression's source text as written.
struct MacroArgument: Codable, Equatable {
  /// The argument label (`classes` in `classes: [Bar.self]`), or `nil` for a positional argument.
  let label: String?

  /// The argument value exactly as written in source, e.g. `"Foo"` (including the quotes) or
  /// `[Bar.self]`. Kept as text because a syntactic scan can't resolve these to runtime values.
  let value: String
}

/// A single annotated declaration the scanner found, with just enough to locate it and know
/// what it is. Member-level details (parameters, types) are intentionally out of scope for this
/// first prototype — see the `@JS` member walk in the macros for where that would live.
struct Detection: Codable, Equatable {
  /// The macro spelled on the declaration (without the leading `@`).
  let macro: DetectedMacro

  /// The declared name, e.g. the class name for `@ExpoModule`, or the func/var/init name for `@JS`.
  let name: String

  /// The kind of declaration the macro was attached to: `class`, `struct`, `func`, `var`, `init`, …
  let declarationKind: String

  /// The explicit JS name override when written as `@ExpoModule("Foo")` / `@JS("bar")` /
  /// `@SharedObject("Baz")`, otherwise `nil` (the name defaults to `name` at expansion time).
  let jsName: String?

  /// Every argument passed to the macro, in source order, e.g. `@ExpoModule("Foo", classes: [Bar.self])`
  /// yields a positional `"Foo"` and a `classes:` argument. Empty when the macro is written bare.
  let arguments: [MacroArgument]

  /// Source location, relative to the path the scanner was invoked with.
  let file: String
  let line: Int
  let column: Int
}

/// Counts describing how much work the scan did, so callers can see the pre-filter's effect: of all
/// the `.swift` files read, how many actually needed parsing, and how long the run took.
struct ScanStats: Codable, Equatable {
  /// `.swift` files the walk found and read (after directory pruning).
  let filesScanned: Int

  /// Of those, how many contained a macro attribute and so were parsed with SwiftSyntax.
  let filesParsed: Int

  /// Wall-clock duration of the scan, in milliseconds (walking, reading, filtering, and parsing).
  let durationMs: Double
}
