import SwiftSyntax

/// The protocol that every type crossing the JS boundary must conform to. Centralized here so the
/// eventual rename (this is a placeholder name) is a single edit, and shared by the macros whose
/// generated conversions go through the dynamic-type API (`@Record`, `@Event`).
internal let jsConvertibleProtocolName = "AnyArgument"

/// The constraint a `@JS` boundary type must satisfy, by direction: an argument (and a settable
/// property's incoming value) is decoded, so it must be `JavaScriptDecodable`; a return value (and a
/// property's outgoing value) is encoded, so it must be `JavaScriptEncodable`. Asserting each
/// direction on its own keeps a decode-only or encode-only type from being over-constrained, and
/// surfaces a missing conformance as a clear "requires that '…' conform to …" diagnostic on the
/// member instead of an opaque error inside the generated closure.
internal let javaScriptDecodableProtocolName = "JavaScriptDecodable"
internal let javaScriptEncodableProtocolName = "JavaScriptEncodable"

/// The constraint a `@Record` property type must satisfy: it must be both `AnyArgument` (the
/// `from(dictionary:)` / `toDictionary(appContext:)` paths still convert native `Any` through the
/// dynamic-type API) and `JavaScriptDecodable & JavaScriptEncodable` (the `from(object:)` /
/// `toObject(appContext:)` paths convert JS values through `decode`/`encode`). A record field has to
/// support both directions, so the assertion requires the intersection.
internal let recordFieldProtocolName = "AnyArgument & JavaScriptDecodable & JavaScriptEncodable"

/// The protocol a type must conform to for `self.emit(event:…)` to resolve; core conforms
/// `BaseModule` and `SharedObject` to it. Asserted by `@Event` so attaching it to a type that can't
/// emit fails with a conformance diagnostic instead of an opaque "no member 'emit'" error.
internal let eventEmitterProtocolName = "EventEmitter"

/// Types we never assert because they're statically known to conform: the JS primitives. Asserting
/// them would only add noise to the expansion.
private let knownConformingPrimitives: Set<String> = ["Bool", "Int", "Double", "String"]

/// One member's worth of conformance assertion: a name (the member it stands for) and the declared
/// types crossing the JS boundary for it. The name surfaces verbatim in the compiler's conformance
/// diagnostic ("local function '<name>' requires that '<Type>' conform to …"), so it identifies the
/// offending member in the error message on top of the location pointing at the user's declaration.
internal struct ConformanceAssertion {
  let name: String
  /// Declared types as written. Composed types (`[Int]`, `String?`, …) are kept verbatim — their
  /// conditional conformances transitively constrain the elements.
  let types: [String]
}

/// A directional conformance-assertion peer for a `@JS` member: a never-called `private func` whose
/// body asserts each argument type is `JavaScriptDecodable` and the return/getter type is
/// `JavaScriptEncodable`, matching how the generated binding converts each (decode on the way in,
/// encode on the way out). The check is a single member-named nested helper with one `A0…` generic
/// parameter per non-primitive argument and a single `Return` parameter for the value, called once with
/// every type's metatype; naming the helper after the member puts the member in the compiler's
/// diagnostic, and the per-slot constraint reports the offending type against the protocol for *its*
/// direction.
///
/// `decodableTypes` are the argument types (and a settable property's value type); `encodableType` is
/// the single return type (or a property's value type on read), or `nil` when there's none. Primitives
/// are dropped from each; when nothing is left in either, returns `nil` so the caller emits no peer.
/// `isStatic` mirrors a `static`/`class` member so the peer sits in the right metatype context.
internal func directionalConformanceAssertion(
  name: String,
  decodableTypes: [String],
  encodableType: String?,
  isStatic: Bool
) -> DeclSyntax? {
  let decodables = distinctAssertableTypes(decodableTypes)
  let encodable = encodableType.flatMap(assertableBoundaryType)
  guard !decodables.isEmpty || encodable != nil else {
    return nil
  }

  // Build the generic parameter list and the matching call arguments in lockstep: one `A0…` slot
  // constrained `JavaScriptDecodable` per argument, and a single `Return` slot constrained
  // `JavaScriptEncodable` for the return/getter value (there's only ever one, so it isn't indexed).
  var parameters: [String] = []
  var typeParameters: [String] = []
  var arguments: [String] = []
  for (index, type) in decodables.enumerated() {
    parameters.append("A\(index): \(javaScriptDecodableProtocolName)")
    typeParameters.append("_: A\(index).Type")
    arguments.append("\(type).self")
  }
  if let encodable {
    parameters.append("Return: \(javaScriptEncodableProtocolName)")
    typeParameters.append("_: Return.Type")
    arguments.append("\(encodable).self")
  }

  let helper = "func \(name)<\(parameters.joined(separator: ", "))>(\(typeParameters.joined(separator: ", "))) {}"
  let call = "\(name)(\(arguments.joined(separator: ", ")))"
  let staticKeyword = isStatic ? "static " : ""
  return """
    private \(raw: staticKeyword)func _assertTypesConformance_\(raw: name)() {
    \(raw: helper)
    \(raw: call)
    }
    """
}

/// One conformance-assertion peer covering several members' assertions at once — used by `@Record`,
/// which folds every property into a single `_assertTypesConformance()` rather than emitting a peer
/// per property. Each assertion keeps its own named nested helper, so the per-member naming in the
/// diagnostic is preserved even though they share one peer.
///
/// Returns `nil` when no assertion has anything left to verify (all primitives / empty), so the
/// caller emits nothing.
internal func typeConformanceAssertions(
  for assertions: [ConformanceAssertion],
  constraint: String = jsConvertibleProtocolName
) -> DeclSyntax? {
  let bodies = assertions.compactMap { conformanceAssertionBody($0, constraint: constraint) }
  guard !bodies.isEmpty else {
    return nil
  }
  return """
    private func _assertTypesConformance() {
    \(raw: bodies.joined(separator: "\n"))
    }
    """
}

/// The type to assert for a boundary type as written: trailing optional markers are unwrapped to the
/// core type, and a known-conforming primitive returns `nil` (nothing to assert). Shared with
/// `@Event`, which folds its single payload type into a combined assertion of its own shape rather
/// than reusing the whole body fragment below.
internal func assertableBoundaryType(_ type: String) -> String? {
  let unwrapped = unwrappedOptional(type)
  return knownConformingPrimitives.contains(unwrapped) ? nil : unwrapped
}

/// The assertion's body fragment: a nested generic helper named after the member, plus one call per
/// distinct non-primitive type. Nesting the helper keeps the constraint entirely local — no shared
/// symbol, nothing to collide, nothing left in the type's namespace — and naming it after the member
/// puts the member's name in the compiler's conformance diagnostic. Returns `nil` when every type was
/// a known-conforming primitive or the list was empty.
private func conformanceAssertionBody(
  _ assertion: ConformanceAssertion,
  constraint: String = jsConvertibleProtocolName
) -> String? {
  let distinct = distinctAssertableTypes(assertion.types)
  guard !distinct.isEmpty else {
    return nil
  }

  var lines = ["func \(assertion.name)<T: \(constraint)>(_: T.Type) {}"]
  lines.append(contentsOf: distinct.map { "\(assertion.name)(\($0).self)" })
  return lines.joined(separator: "\n")
}

/// Normalizes a list of boundary types for assertion: unwraps each to its core type (trailing
/// optionals stripped), drops the known-conforming primitives that never need asserting, and dedups
/// while preserving first-seen order so a type is asserted once even when it appears more than once.
private func distinctAssertableTypes(_ types: [String]) -> [String] {
  var seen: Set<String> = []
  var distinct: [String] = []
  for type in types.map(unwrappedOptional)
  where !knownConformingPrimitives.contains(type) && seen.insert(type).inserted {
    distinct.append(type)
  }
  return distinct
}

/// Strips every trailing optional marker (`?`/`!`) so the assertion targets the core wrapped type.
/// `Optional<W>: AnyArgument` holds exactly when `W: AnyArgument` (and `T!` is just `T?`), so each
/// layer is conformance-equivalent to its wrapped type. Asserting the core gives a cleaner diagnostic
/// (the direct `requires that '<type>' conform`, not the conditional-conformance phrasing through
/// `Optional`) and sidesteps that `T!.self` is invalid in metatype position. Only *trailing* markers
/// are stripped, so `[Int?]` keeps its inner `?`; a longhand `Optional<W>` isn't peeled but is still
/// asserted whole, which remains correct.
private func unwrappedOptional(_ type: String) -> String {
  var result = Substring(type)
  while result.hasSuffix("?") || result.hasSuffix("!") {
    result = result.dropLast()
  }
  return String(result)
}
