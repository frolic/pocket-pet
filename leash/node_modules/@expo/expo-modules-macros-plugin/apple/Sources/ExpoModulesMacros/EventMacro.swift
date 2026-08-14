import SwiftDiagnostics
import SwiftSyntax
import SwiftSyntaxMacros

/// Accessor macro applied to a function-typed `var` on a module or shared object, turning it into a
/// typed JavaScript event. A function-typed `var` can't be a stored property without an initializer,
/// so the macro expands it into a computed getter returning a closure that dispatches by name into
/// the `EventEmitter` `emit` overloads (core conforms both `BaseModule` and `SharedObject` to that
/// protocol, so `self.emit` resolves on each):
///
///   @Event
///   var onProgress: (ProgressEvent) -> Void
///   // expands to:
///   var onProgress: (ProgressEvent) -> Void {
///     get {
///       { [weak self] payload in self?.emit(event: "progress", payload: payload) }
///     }
///   }
///
/// A no-payload event (`() -> Void`) dispatches through the dedicated `emit(event:)` overload.
/// The JS event name defaults to the property name with the conventional `on` prefix stripped
/// (see `defaultEventName(for:)`); `@Event("customName")` overrides it verbatim.
///
/// The closure captures `self` **weakly**: it's usually invoked inline (`self.onProgress(…)`), but an
/// author may store it or hand it to a delegate, and a strong capture would then extend the module's
/// lifetime. After the emitter deallocates the closure silently no-ops, which matches what `emit`
/// already does once the runtime is gone.
///
/// The synthesized property is deliberately **not** isolated to `@JavaScriptActor`, unlike `@JS`
/// members: `emit` is itself non-isolated and schedules the dispatch onto the JS thread internally,
/// so the event is callable from any thread or isolation with no actor hop at the call site. It is
/// also self-contained: `@ExpoModule`/`@SharedObject` neither collect `@Event` members nor register
/// their names anywhere.
///
/// `@Event(sync: true)` opts into **synchronous dispatch**: the closure calls `emitSync` (inline
/// conversion + dispatch, no scheduling) instead of `emit`, and `@ExpoModule`/`@SharedObject` stamp
/// the member `@JavaScriptActor` so the compiler forces the call site onto the JS thread, the
/// inverse of the async default. The isolation is on the property access, so it guards the inline
/// `self.onTick(…)` usage; a closure stored or handed off escapes it, after which `emitSync` runs
/// wherever the caller invokes it.
///
/// As a **peer**, the macro emits a never-called conformance assertion (see
/// `TypeConformanceAssertion.swift`) checking that the payload type is JS-convertible and that the
/// enclosing type conforms to `EventEmitter`, so both failure modes surface as clear conformance
/// errors on the user's own declaration.
public struct EventMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    let event = try validatedEvent(of: node, on: declaration)
    // The closure's parameter and return types are inferred from the property's declared type
    // through the getter, so the body never has to spell the payload type. A sync event calls
    // `emitSync` (inline dispatch, JS thread only); the default calls the scheduling `emit`.
    let emitMethod = event.isSync ? "emitSync" : "emit"
    let closure = event.hasPayload
      ? "{ [weak self] payload in self?.\(emitMethod)(event: \"\(event.jsName)\", payload: payload) }"
      : "{ [weak self] in self?.\(emitMethod)(event: \"\(event.jsName)\") }"
    return [
      """
      get {
        \(raw: closure)
      }
      """
    ]
  }
}

extension EventMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    // Diagnostics are owned by the accessor expansion; an invalid declaration silently emits no
    // peer here so each error is reported once.
    guard let event = try? validatedEvent(of: node, on: declaration) else {
      return []
    }
    // One nested helper asserts everything in a single call: the payload type is JS-convertible
    // (the `P` parameter, dropped for no-payload events and known-conforming primitives) and the
    // enclosing type can emit (the `E` parameter, always present since any `@Event` dispatches
    // through `self.emit`). Named after the member so the member shows up in either diagnostic,
    // and asserting the enclosing type by its spelled name so the conformance error names the
    // user's type rather than 'Self'.
    let name = event.swiftName
    let payload = event.payloadType.flatMap(assertableBoundaryType)
    let owner = enclosingTypeName(in: context) ?? "Self"
    let helper = payload != nil
      ? "func \(name)<P: \(jsConvertibleProtocolName), E: \(eventEmitterProtocolName)>(_: P.Type, _: E.Type) {}"
      : "func \(name)<E: \(eventEmitterProtocolName)>(_: E.Type) {}"
    let call = payload.map { "\(name)(\($0).self, \(owner).self)" } ?? "\(name)(\(owner).self)"
    return [
      """
      private func _assertTypesConformance_\(raw: name)() {
      \(raw: helper)
      \(raw: call)
      }
      """
    ]
  }
}

/// What the expansions need to know about a validated `@Event` declaration: the property name, the
/// JS event name (after an `@Event("…")` override), and the payload type when the function type
/// takes one.
private struct EventMember {
  let swiftName: String
  let jsName: String
  /// The payload type as written, or `nil` for a no-payload `() -> Void` event.
  let payloadType: String?
  /// Whether the event dispatches synchronously (`@Event(sync: true)`) via `emitSync`.
  let isSync: Bool

  var hasPayload: Bool {
    return payloadType != nil
  }
}

/// Validates the declaration `@Event` is attached to and reads the event out of it. The checks
/// mirror what the expansion relies on: a single-binding instance `var` (the macro synthesizes a
/// computed getter, so `let`, accessors, and initializers are all incompatible) whose type is a
/// function type returning `Void` with at most one payload parameter.
private func validatedEvent(
  of node: AttributeSyntax,
  on declaration: some DeclSyntaxProtocol
) throws -> EventMember {
  guard let varDecl = declaration.as(VariableDeclSyntax.self) else {
    throw MacroExpansionErrorMessage("@Event can only be applied to a property")
  }
  if varDecl.attributes.firstAttribute(named: "JS") != nil {
    throw MacroExpansionErrorMessage(
      "@Event and @JS cannot be combined on the same property; an event is exposed to JS on its own, so remove one of the attributes")
  }
  // The compiler also rejects accessor macros on a `let`, but with a generic message; this one says
  // what to do instead and carries the fix-it doing it. The synthesized property is getter-only, so
  // switching to `var` loses nothing.
  if varDecl.bindingSpecifier.tokenKind == .keyword(.let) {
    throw letBindingDiagnostic(for: varDecl)
  }
  if varDecl.modifiers.contains(where: isTypeLevelModifier) {
    throw MacroExpansionErrorMessage(
      "@Event must be an instance property; events are emitted from a module or shared object instance.")
  }
  guard varDecl.bindings.count == 1, let binding = varDecl.bindings.first,
    let identifier = binding.pattern.as(IdentifierPatternSyntax.self) else {
    throw MacroExpansionErrorMessage(
      "@Event must be applied to a single named property; declare each event separately")
  }
  if binding.initializer != nil {
    throw MacroExpansionErrorMessage(
      "@Event property cannot have an initial value; the macro synthesizes the closure")
  }
  if binding.accessorBlock != nil {
    throw MacroExpansionErrorMessage(
      "@Event property cannot declare its own accessors; the macro synthesizes the getter")
  }
  guard let functionType = underlyingFunctionType(of: binding.typeAnnotation?.type) else {
    throw MacroExpansionErrorMessage(
      "@Event property must declare a function type, such as '(Payload) -> Void' or '() -> Void'")
  }
  guard isVoidReturn(functionType.returnClause.type) else {
    throw MacroExpansionErrorMessage(
      "@Event function type must return 'Void'; an event dispatches to JS and has no return value")
  }
  guard functionType.parameters.count <= 1 else {
    throw MacroExpansionErrorMessage(
      "@Event function type takes at most one payload parameter; combine multiple values into a single record")
  }

  let swiftName = identifier.identifier.text
  return EventMember(
    swiftName: swiftName,
    jsName: jsNameArgument(of: node) ?? defaultEventName(for: swiftName),
    payloadType: functionType.parameters.first?.type.trimmedDescription,
    isSync: boolArgument(of: node, label: "sync") == true
  )
}

// MARK: - Default event name

/// The default JS event name for a property: the Swift name with the conventional `on` prefix
/// stripped and the remainder decapitalized (`onStatusChange` → `statusChange`). The two sides
/// idiomatically want different names: the Swift property reads as invoking a handler
/// (`self.onStatusChange(…)`) and the prefix keeps it from colliding with a state property
/// (`status`), while JS listens by bare name (`addListener("statusChange")`, the Node/DOM idiom
/// that module and shared-object events follow). Names without the prefix (`statusChange`,
/// `online`) pass through verbatim, and an explicit `@Event("name")` override is never
/// transformed — that's also the escape hatch for legacy `onX` wire names.
private func defaultEventName(for swiftName: String) -> String {
  guard swiftName.hasPrefix("on") else {
    return swiftName
  }
  let rest = swiftName.dropFirst(2)
  guard let first = rest.first, first.isUppercase else {
    return swiftName
  }
  return decapitalized(String(rest))
}

/// Lowercases the leading uppercase run the way Swift's API importer does: a single leading
/// capital is lowercased (`StatusChange` → `statusChange`); a longer acronym run keeps its last
/// capital when a lowercase letter follows it, since that capital starts the next word
/// (`URLChange` → `urlChange`, `URL` → `url`).
private func decapitalized(_ name: String) -> String {
  let runEnd = name.firstIndex { !$0.isUppercase } ?? name.endIndex
  if name[..<runEnd].count > 1 && runEnd != name.endIndex {
    let lastCapital = name.index(before: runEnd)
    return name[..<lastCapital].lowercased() + name[lastCapital...]
  }
  return name[..<runEnd].lowercased() + name[runEnd...]
}

// MARK: - The `let` diagnostic

/// The error for `@Event let`, attached to the `let` keyword itself and carrying a fix-it that
/// replaces it with `var`. Unlike the other checks (plain thrown messages located at the attribute),
/// this one is a structured `Diagnostic` so Xcode can offer the one-click fix.
private func letBindingDiagnostic(for varDecl: VariableDeclSyntax) -> DiagnosticsError {
  let specifier = varDecl.bindingSpecifier
  let fixIt = FixIt(
    message: EventFixItMessage("Replace 'let' with 'var'", id: "event-let-to-var"),
    changes: [
      // Rewriting just the token's kind keeps its surrounding trivia (indentation, the space
      // before the property name) intact.
      .replace(
        oldNode: Syntax(specifier),
        newNode: Syntax(specifier.with(\.tokenKind, .keyword(.var)))
      )
    ]
  )
  let message = EventDiagnosticMessage(
    "@Event must be applied to a 'var': it expands into a computed property, which a 'let' cannot be. The synthesized property is read-only anyway.",
    id: "event-on-let"
  )
  return DiagnosticsError(diagnostics: [
    Diagnostic(node: specifier, message: message, fixIts: [fixIt])
  ])
}

private struct EventDiagnosticMessage: DiagnosticMessage {
  let message: String
  let diagnosticID: MessageID
  let severity: DiagnosticSeverity = .error

  init(_ message: String, id: String) {
    self.message = message
    self.diagnosticID = MessageID(domain: "ExpoModulesMacros", id: id)
  }
}

private struct EventFixItMessage: FixItMessage {
  let message: String
  let fixItID: MessageID

  init(_ message: String, id: String) {
    self.message = message
    self.fixItID = MessageID(domain: "ExpoModulesMacros", id: id)
  }
}

// MARK: - Declaration shape helpers

/// The spelled name of the innermost type declaration enclosing the macro, read from the lexical
/// context, so the emitter assertion can name the user's type in the conformance diagnostic
/// ("requires that 'MyModule' conform to 'EventEmitter'" instead of "'Self'"). Returns `nil` (the
/// caller falls back to `Self`) when there's no enclosing type, or when it's a generic type
/// declaration: `Foo.self` isn't valid for an unbound generic, while `Self` works anywhere. An
/// extension can't be detected as generic syntactically (see the extension case below).
private func enclosingTypeName(in context: some MacroExpansionContext) -> String? {
  for scope in context.lexicalContext {
    if let classDecl = scope.as(ClassDeclSyntax.self) {
      return classDecl.genericParameterClause == nil ? classDecl.name.text : nil
    }
    if let structDecl = scope.as(StructDeclSyntax.self) {
      return structDecl.genericParameterClause == nil ? structDecl.name.text : nil
    }
    if let actorDecl = scope.as(ActorDeclSyntax.self) {
      return actorDecl.genericParameterClause == nil ? actorDecl.name.text : nil
    }
    if let extensionDecl = scope.as(ExtensionDeclSyntax.self) {
      // An extension carries no generic-parameter clause of its own, so a bare extended type
      // (`extension Box`) is indistinguishable from a non-generic one (`extension Foo`); both read
      // as a plain identifier here. A written bound form (`extension Box<Int>`) is valid as `.self`,
      // and the common non-generic case keeps its spelled name in the diagnostic. The unguarded gap
      // is `extension <Generic>` with the parameters omitted, where the spelled name is an unbound
      // generic invalid as `.self`; events on generic types in an extension are rare enough that the
      // resulting compile error is an acceptable price for naming the user's type everywhere else.
      return extensionDecl.extendedType.trimmedDescription
    }
  }
  return nil
}

private func isTypeLevelModifier(_ modifier: DeclModifierSyntax) -> Bool {
  return modifier.name.tokenKind == .keyword(.static) || modifier.name.tokenKind == .keyword(.class)
}

/// The function type underlying a property's type annotation, unwrapping attributes
/// (`@Sendable (P) -> Void`) and single-element parentheses (`((P) -> Void)`). Returns `nil` when
/// the annotation is missing or isn't a function type, including an optional function type:
/// an event is always present, never `nil`.
private func underlyingFunctionType(of type: TypeSyntax?) -> FunctionTypeSyntax? {
  guard let type else {
    return nil
  }
  if let attributed = type.as(AttributedTypeSyntax.self) {
    return underlyingFunctionType(of: attributed.baseType)
  }
  if let tuple = type.as(TupleTypeSyntax.self),
    tuple.elements.count == 1, let element = tuple.elements.first, element.firstName == nil {
    return underlyingFunctionType(of: element.type)
  }
  return type.as(FunctionTypeSyntax.self)
}

/// True when the function type's return is written as `Void` / `()`. Function types always carry an
/// explicit return clause, so unlike a function declaration there's no "absent" case. A module-qualified
/// `Swift.Void` and redundant parentheses (`(Void)`, `(())`) are accepted too, so a valid void return
/// written one of those ways isn't rejected with a misleading "must return 'Void'" diagnostic.
private func isVoidReturn(_ type: TypeSyntax) -> Bool {
  // Peel single-element, unlabeled parentheses: `(Void)` and `(())` are the same type as their content.
  if let tuple = type.as(TupleTypeSyntax.self), tuple.elements.count == 1,
    let element = tuple.elements.first, element.firstName == nil {
    return isVoidReturn(element.type)
  }
  let text = type.trimmedDescription
  return text == "Void" || text == "()" || text == "Swift.Void"
}
