import Foundation

/// The deep-scan surface: the full JS-exported shape of every `@ExpoModule`, `@SharedObject`, and
/// `@Record` type, with the per-member detail a TypeScript type generator needs. Read syntactically
/// (like the macros): each boundary type becomes a structured `TypeNode` tree so the consumer walks a
/// tagged tree rather than re-parsing Swift type syntax.

/// One parameter of a `@JS` function or `@JS init`.
struct ExportedParameter: Encodable, Equatable {
  /// The argument label (the `first` name): `to` in `func move(to point: Point)`, or `_` if unlabeled.
  let label: String

  /// The internal parameter name (the `second` name, else the same as `label`): `point` above.
  let name: String

  let type: TypeNode

  /// True when the caller may omit it: a default value or an optional type. Distinct from the type
  /// being `.optional` (a defaulted non-optional is also omittable). Encoded as `optional`.
  let isOptional: Bool

  private enum CodingKeys: String, CodingKey {
    case label, name, type
    case isOptional = "optional"
  }
}

/// One `@JS func` on a module or shared object.
struct ExportedFunction: Encodable, Equatable {
  /// The Swift declaration name.
  let name: String

  /// The JS name it binds under: the `@JS("x")` override, else `name`.
  let jsName: String

  let parameters: [ExportedParameter]

  /// The return type, or `nil` for `Void`. Named `returns` to pair with `parameters`.
  let returns: TypeNode?

  /// `async` (an async function is promise-returning in JS).
  let isAsync: Bool

  /// `throws`.
  let isThrowing: Bool

  /// `static`/`class` member.
  let isStatic: Bool

  /// The flags are encoded under their TS-keyword spellings.
  private enum CodingKeys: String, CodingKey {
    case name, jsName, parameters, returns
    case isAsync = "async"
    case isThrowing = "throws"
    case isStatic = "static"
  }
}

/// One `@JS var` on a module or shared object.
struct ExportedProperty: Encodable, Equatable {
  /// The Swift declaration name.
  let name: String

  /// The JS name it binds under: the `@JS("x")` override, else `name`.
  let jsName: String

  /// The value type, or `nil` when undeterminable syntactically (no annotation and no literal
  /// default), the case the macro binds getter-only.
  let type: TypeNode?

  /// True when JS can assign to it: a stored `var` or a computed `var` with a `set`. Encoded as its
  /// inverse, `readonly`.
  let isSettable: Bool

  /// `static`/`class` member. Encoded as `static`.
  let isStatic: Bool

  private enum CodingKeys: String, CodingKey {
    case name, jsName, type
    case isReadonly = "readonly"
    case isStatic = "static"
  }

  func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    try container.encode(name, forKey: .name)
    try container.encode(jsName, forKey: .jsName)
    try container.encodeIfPresent(type, forKey: .type)
    try container.encode(!isSettable, forKey: .isReadonly)
    try container.encode(isStatic, forKey: .isStatic)
  }
}

/// One `@Record` property: a plain data slot exposing `optional`/`required` (vs. `ExportedProperty`,
/// a JS accessor exposing `readonly`/`static`). A record crosses the boundary by value, so the whole
/// type is read-only in JS; that's a record-level fact and isn't stamped per property.
struct ExportedRecordProperty: Encodable, Equatable {
  let name: String

  /// `@Record` requires a determinable type on every property, so this is never `nil`.
  let type: TypeNode

  /// Optional-typed. Encoded as `optional`.
  let isOptional: Bool

  /// Has a default value. Not encoded (derivable as `!isOptional && !isRequired`, and a Swift default
  /// never reaches JS); kept only to derive `isRequired`.
  let hasDefault: Bool

  /// Whether JS must supply this property, matching the macro's `RecordProperty.isRequired`. Encoded
  /// as `required`.
  var isRequired: Bool {
    return !hasDefault && !isOptional
  }

  private enum CodingKeys: String, CodingKey {
    case name, type
    case isOptional = "optional"
    case isRequired = "required"
  }

  func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    try container.encode(name, forKey: .name)
    try container.encode(type, forKey: .type)
    try container.encode(isOptional, forKey: .isOptional)
    try container.encode(isRequired, forKey: .isRequired)
  }
}

/// A `@ExpoModule` type and its `@JS` surface.
struct ExportedModule: Encodable, Equatable {
  /// The Swift class name.
  let name: String

  /// The JS module name: `@ExpoModule("Foo")` override, else the class name.
  let jsName: String

  let functions: [ExportedFunction]
  let properties: [ExportedProperty]

  /// Absolute source path, matching `scan-modules`.
  let file: String
}

/// A `@SharedObject` type: a JS class with an optional `@JS init` constructor plus its `@JS` members.
struct ExportedSharedObject: Encodable, Equatable {
  /// The Swift class name.
  let name: String

  /// The JS class name: `@SharedObject("Foo")` override, else the class name.
  let jsName: String

  /// The `@JS init` parameters, or `nil` when there's none. A shared object has at most one.
  let constructorParameters: [ExportedParameter]?

  let functions: [ExportedFunction]
  let properties: [ExportedProperty]

  let file: String
}

/// A `@Record` type and its properties (data only: no functions, accessors, or constructor).
struct ExportedRecord: Encodable, Equatable {
  /// The Swift type name (struct or class).
  let name: String

  let properties: [ExportedRecordProperty]

  let file: String
}

/// The exported types grouped by kind, nested under `exports` in the result so the surface is one
/// self-contained object separate from `stats`.
struct ExportedSurface: Encodable, Equatable {
  let modules: [ExportedModule]
  let sharedObjects: [ExportedSharedObject]
  let records: [ExportedRecord]
}

/// The `scan-exports` result: the surface plus the run's stats. A distinct envelope from
/// `ScanModulesResult` (different consumer: TS generation vs. autolinking).
struct ScanExportsResult: Encodable, Equatable {
  let exports: ExportedSurface
  let stats: ScanStats
}
