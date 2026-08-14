import SwiftSyntax

/// A boundary type parsed into a structured, JS-oriented tree, so the consumer walks a tagged tree
/// instead of re-parsing Swift type syntax. The single Swift type parser lives here. Encoded as a
/// `kind` discriminator plus per-kind fields and a `typeof` on every node, e.g.
/// `{ "kind": "array", "typeof": "object", "element": { "kind": "primitive", "name": "Int",
/// "typeof": "number" } }`. An unmodeled spelling becomes `.unknown` (verbatim text, no `typeof`),
/// never silently dropped.

/// The runtime category mirroring JavaScript's `typeof`. A coarse companion to `TypeNode.kind`: every
/// structured object kind (array, dictionary, promise, ref) reports `object`. `bigint`/`symbol` are
/// included for completeness; the scanner doesn't produce them.
enum JSType: String, Encodable {
  case undefined
  case object
  case boolean
  case number
  case bigint
  case string
  case symbol
  case function
}

indirect enum TypeNode: Equatable {
  /// `Bool`, `Int`, `Double`, `String`: the types core fast-decodes. `name` is the Swift spelling;
  /// `jsType` is `boolean`/`number`/`string`.
  case primitive(name: String, jsType: JSType)

  /// `T?` / `T!` / `Optional<T>`.
  case optional(wrapped: TypeNode)

  /// `[T]` / `Array<T>`.
  case array(element: TypeNode)

  /// `[K: V]` / `Dictionary<K, V>`.
  case dictionary(key: TypeNode, value: TypeNode)

  /// `Promise<T>`.
  case promise(value: TypeNode)

  /// A closure `(A, B) async throws -> R`. `returns` is `nil` for `Void`; `isAsync`/`isThrowing` carry
  /// the effects (encoded `async`/`throws`).
  case function(parameters: [TypeNode], returns: TypeNode?, isAsync: Bool, isThrowing: Bool)

  /// Any other named type (record, shared object, enum, …). `name` is the possibly-qualified spelling;
  /// the generator resolves it against the scanned types or treats it as opaque.
  case ref(name: String)

  /// A spelling the parser doesn't model (generic parameter, tuple, metatype, …), kept verbatim so
  /// nothing is lost. Has no `typeof`.
  case unknown(text: String)

  /// The `typeof` category, or `nil` for `.unknown`. An optional reports its *present* value's
  /// category; the absent (`undefined`) case is carried by the `.optional` wrapper itself.
  var jsType: JSType? {
    switch self {
    case .primitive(_, let jsType):
      return jsType
    case .array, .dictionary, .promise, .ref:
      return .object
    case .function:
      return .function
    case .optional(let wrapped):
      return wrapped.jsType
    case .unknown:
      return nil
    }
  }
}

extension TypeNode: Encodable {
  private enum CodingKeys: String, CodingKey {
    case kind
    case name
    // The `typeof` category. Spelled `typeof` in JSON (what the consumer reads); the Swift property is
    // `jsType` since it's the cleaner Swift name.
    case jsType = "typeof"
    case wrapped, element, key, value, parameters, returns, text
    // A closure type's effects, matching `ExportedFunction`'s TS-keyword spellings.
    case isAsync = "async"
    case isThrowing = "throws"
  }

  func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    // Every node carries its `typeof` category (except `.unknown`, whose `jsType` is nil). Encoded up
    // front so it sits beside `kind` on each node.
    try container.encodeIfPresent(jsType, forKey: .jsType)
    switch self {
    case .primitive(let name, _):
      try container.encode("primitive", forKey: .kind)
      try container.encode(name, forKey: .name)
    case .optional(let wrapped):
      try container.encode("optional", forKey: .kind)
      try container.encode(wrapped, forKey: .wrapped)
    case .array(let element):
      try container.encode("array", forKey: .kind)
      try container.encode(element, forKey: .element)
    case .dictionary(let key, let value):
      try container.encode("dictionary", forKey: .kind)
      try container.encode(key, forKey: .key)
      try container.encode(value, forKey: .value)
    case .promise(let value):
      try container.encode("promise", forKey: .kind)
      try container.encode(value, forKey: .value)
    case .function(let parameters, let returns, let isAsync, let isThrowing):
      try container.encode("function", forKey: .kind)
      try container.encode(parameters, forKey: .parameters)
      try container.encodeIfPresent(returns, forKey: .returns)
      try container.encode(isAsync, forKey: .isAsync)
      try container.encode(isThrowing, forKey: .isThrowing)
    case .ref(let name):
      try container.encode("ref", forKey: .kind)
      try container.encode(name, forKey: .name)
    case .unknown(let text):
      try container.encode("unknown", forKey: .kind)
      try container.encode(text, forKey: .text)
    }
  }
}

/// The primitive Swift types with a dedicated JS mapping (the set core fast-decodes), each paired with
/// its JS primitive. A bare name here is a `.primitive`; anything else is a `.ref`.
private let primitiveJSTypes: [String: JSType] = [
  "Bool": .boolean,
  "Int": .number,
  "Double": .number,
  "String": .string,
]

/// Parses a `TypeSyntax` into a `TypeNode`: the single place Swift type syntax is interpreted.
func typeNode(from type: TypeSyntax) -> TypeNode {
  // `T?` sugar.
  if let optional = type.as(OptionalTypeSyntax.self) {
    return .optional(wrapped: typeNode(from: optional.wrappedType))
  }
  // `T!`, JS treats it the same as `T?`.
  if let iuo = type.as(ImplicitlyUnwrappedOptionalTypeSyntax.self) {
    return .optional(wrapped: typeNode(from: iuo.wrappedType))
  }
  // `[T]` sugar.
  if let arrayType = type.as(ArrayTypeSyntax.self) {
    return .array(element: typeNode(from: arrayType.element))
  }
  // `[K: V]` sugar.
  if let dictionaryType = type.as(DictionaryTypeSyntax.self) {
    return .dictionary(key: typeNode(from: dictionaryType.key), value: typeNode(from: dictionaryType.value))
  }
  // `(A, B) -> R`.
  if let functionType = type.as(FunctionTypeSyntax.self) {
    return functionNode(from: functionType)
  }
  // `@escaping (…) -> …`, `@Sendable …`, etc.: strip the attributes and parse the underlying type.
  if let attributed = type.as(AttributedTypeSyntax.self) {
    return typeNode(from: attributed.baseType)
  }
  // A nominal type, possibly generic: `Int`, `Point`, `Optional<T>`, `Array<T>`, `Promise<T>`.
  if let identifier = type.as(IdentifierTypeSyntax.self) {
    return nominalNode(name: identifier.name.text, generics: identifier.genericArgumentClause)
  }
  // A qualified nominal type: `Foo.Bar`. Kept as a ref under its full spelling.
  if let member = type.as(MemberTypeSyntax.self) {
    return .ref(name: member.trimmedDescription)
  }
  // Tuples, metatypes, some/any types, etc.: not modeled, preserve the verbatim spelling.
  return .unknown(text: type.trimmedDescription)
}

/// A nominal type's node. The standard generic wrappers normalize to their sugared node
/// (`Optional<T>`/`Array<T>`/`Dictionary<K,V>`/`Promise<T>`); a bare name is a primitive when known,
/// else a ref; any other generic is a ref under its full spelling.
private func nominalNode(name: String, generics: GenericArgumentClauseSyntax?) -> TypeNode {
  guard let generics else {
    if let jsType = primitiveJSTypes[name] {
      return .primitive(name: name, jsType: jsType)
    }
    return .ref(name: name)
  }

  let arguments = genericArguments(generics)
  switch (name, arguments.count) {
  case ("Optional", 1):
    return .optional(wrapped: arguments[0])
  case ("Array", 1):
    return .array(element: arguments[0])
  case ("Dictionary", 2):
    return .dictionary(key: arguments[0], value: arguments[1])
  case ("Promise", 1):
    return .promise(value: arguments[0])
  default:
    // An unmodeled generic (`Set<Int>`, `Either<A, B>`, …). Keep the whole spelling as a ref so the
    // name and its arguments survive for the generator to interpret or flag.
    return .ref(name: "\(name)<\(arguments.map { $0.spelling }.joined(separator: ", "))>")
  }
}

/// The argument types of a generic clause, each parsed into a node.
private func genericArguments(_ clause: GenericArgumentClauseSyntax) -> [TypeNode] {
  clause.arguments.compactMap { argument in
    guard case .type(let type) = argument.argument else {
      return nil
    }
    return typeNode(from: type)
  }
}

/// A closure node: each parameter parsed, the result (`Void`/`()` collapsed to `nil`, matching how a
/// function's own return is reported), and the closure's `async`/`throws` effects.
private func functionNode(from type: FunctionTypeSyntax) -> TypeNode {
  let parameters = type.parameters.map { typeNode(from: $0.type) }
  let returnType = type.returnClause.type
  let returns = isVoidType(returnType) ? nil : typeNode(from: returnType)
  let effects = type.effectSpecifiers
  return .function(
    parameters: parameters,
    returns: returns,
    isAsync: effects?.asyncSpecifier != nil,
    isThrowing: effects?.throwsClause?.throwsSpecifier != nil
  )
}

/// True when a return clause is absent or written `Void` / `()`, so the surface reports `nil`. Shared
/// by the function-return read and the closure-type parser.
func isVoidType(_ type: TypeSyntax?) -> Bool {
  guard let type else {
    return true
  }
  let text = type.trimmedDescription
  return text == "Void" || text == "()"
}

extension TypeNode {
  /// A best-effort source-like spelling, used only to re-compose an unmodeled generic ref's name.
  /// Not a round-trip of arbitrary Swift; the structured node is the source of truth.
  fileprivate var spelling: String {
    switch self {
    case .primitive(let name, _):
      return name
    case .ref(let name):
      return name
    case .optional(let wrapped):
      return "\(wrapped.spelling)?"
    case .array(let element):
      return "[\(element.spelling)]"
    case .dictionary(let key, let value):
      return "[\(key.spelling): \(value.spelling)]"
    case .promise(let value):
      return "Promise<\(value.spelling)>"
    case .function(let parameters, let returns, _, _):
      return "(\(parameters.map { $0.spelling }.joined(separator: ", "))) -> \(returns?.spelling ?? "Void")"
    case .unknown(let text):
      return text
    }
  }
}
