import SwiftSyntax

/**
 Reads the first string-literal argument of an attribute, e.g. `@JS("doWork")` -> "doWork".
 Returns nil if the attribute has no arguments or the first argument is not a string literal.
 */
internal func jsNameArgument(of attribute: AttributeSyntax) -> String? {
  guard let args = attribute.arguments?.as(LabeledExprListSyntax.self),
    let first = args.first,
    let str = first.expression.as(StringLiteralExprSyntax.self),
    let segment = str.segments.first?.as(StringSegmentSyntax.self) else {
    return nil
  }
  return segment.content.text
}

/**
 Reads a labeled boolean-literal argument of an attribute, e.g. `@Event(sync: true)` -> true.
 Returns nil if the attribute has no argument with that label or its value isn't a boolean literal.
 */
internal func boolArgument(of attribute: AttributeSyntax, label: String) -> Bool? {
  guard let args = attribute.arguments?.as(LabeledExprListSyntax.self) else {
    return nil
  }
  for arg in args where arg.label?.text == label {
    guard let literal = arg.expression.as(BooleanLiteralExprSyntax.self) else {
      return nil
    }
    return literal.literal.tokenKind == .keyword(.true)
  }
  return nil
}

/// True if the type is written as an optional: `T?`, `T!`, or the explicit `Optional<T>`. Used to
/// decide argument requiredness (an optional parameter may be omitted) and record-field nullability.
internal func isOptionalType(_ type: TypeSyntax) -> Bool {
  if type.is(OptionalTypeSyntax.self) || type.is(ImplicitlyUnwrappedOptionalTypeSyntax.self) {
    return true
  }
  if let identifier = type.as(IdentifierTypeSyntax.self), identifier.name.text == "Optional" {
    return true
  }
  return false
}

/// True if a trailing occurrence of this parameter may be omitted by the JS caller: it either has a
/// default value (Swift applies it) or is an optional type (an absent slot becomes `nil`). The arity
/// range and the per-arity call branches are derived from this.
internal func isOmittable(_ parameter: FunctionParameterSyntax) -> Bool {
  return hasDefaultValue(parameter) || isOptionalType(parameter.type)
}

/// True if the parameter declares a default value (`b: Int = 5`). An omitted defaulted slot is left
/// out of the call so Swift fills in the default, distinguishing it from an omitted optional slot
/// (passed `nil`).
internal func hasDefaultValue(_ parameter: FunctionParameterSyntax) -> Bool {
  return parameter.defaultValue != nil
}

/**
 True if the declaration is a `@Event(sync: true)` property. A sync event dispatches inline on the
 JS thread instead of scheduling, so `@ExpoModule`/`@SharedObject` stamp it with `@JavaScriptActor`,
 making "must be called on the JS thread" a compile-time guarantee at the call site. Async events
 (the default) are deliberately not stamped: their `emit` schedules onto the JS thread itself, so
 they stay callable from any thread.
 */
internal func isSyncEventMember(_ decl: DeclSyntaxProtocol) -> Bool {
  guard let varDecl = decl.as(VariableDeclSyntax.self),
    let attribute = varDecl.attributes.firstAttribute(named: "Event") else {
    return false
  }
  return boolArgument(of: attribute, label: "sync") == true
}

/**
 Reads a labeled array-literal argument of an attribute, e.g. `@ExpoModule(classes: [Foo.self, Bar.self])`,
 and returns the type names referenced (e.g. `["Foo", "Bar"]`). Each element must be a
 `<TypeName>.self` member-access expression; non-conforming elements are skipped silently.
 */
internal func classListArgument(of attribute: AttributeSyntax, label: String) -> [String] {
  guard let args = attribute.arguments?.as(LabeledExprListSyntax.self) else {
    return []
  }
  for arg in args where arg.label?.text == label {
    guard let array = arg.expression.as(ArrayExprSyntax.self) else {
      return []
    }
    return array.elements.compactMap { element -> String? in
      guard let memberAccess = element.expression.as(MemberAccessExprSyntax.self),
        memberAccess.declName.baseName.text == "self",
        let base = memberAccess.base?.as(DeclReferenceExprSyntax.self) else {
        return nil
      }
      return base.baseName.text
    }
  }
  return []
}

/**
 Returns true if the class's inheritance clause names any of the given identifiers.
 Matches by base identifier only, so `Module`, `ExpoModulesCore.Module`, and
 `Module<Foo>` all match an entry of "Module".
 */
internal func inheritsFromAny(_ classDecl: ClassDeclSyntax, names: Set<String>) -> Bool {
  guard let inherited = classDecl.inheritanceClause?.inheritedTypes else {
    return false
  }
  for entry in inherited {
    if let name = baseIdentifier(of: entry.type), names.contains(name) {
      return true
    }
  }
  return false
}

/**
 Returns the rightmost identifier of a type, e.g. `Foo` for `Foo`, `Foo` for
 `Module.Foo`, and nil for composed or generic shapes the macro doesn't need to handle.
 */
internal func baseIdentifier(of type: TypeSyntax) -> String? {
  if let identifier = type.as(IdentifierTypeSyntax.self) {
    return identifier.name.text
  }
  if let member = type.as(MemberTypeSyntax.self) {
    return member.name.text
  }
  return nil
}

/**
 True if the declaration carries a `@JS` attribute. Works for functions, properties, and inits;
 returns false for any other decl kind.
 */
internal func memberHasJSAttribute(_ decl: DeclSyntaxProtocol) -> Bool {
  if let funcDecl = decl.as(FunctionDeclSyntax.self) {
    return funcDecl.attributes.firstAttribute(named: "JS") != nil
  }
  if let varDecl = decl.as(VariableDeclSyntax.self) {
    return varDecl.attributes.firstAttribute(named: "JS") != nil
  }
  if let initDecl = decl.as(InitializerDeclSyntax.self) {
    return initDecl.attributes.firstAttribute(named: "JS") != nil
  }
  return false
}

/**
 Decides whether the macro should stamp `@JavaScriptActor` on a `@JS`-marked member.
 The macro defers to the user when they've already chosen an isolation:
 - the `nonisolated` modifier is present on the member
 - any attribute whose name matches a known global actor (`@MainActor`, `@JavaScriptActor`)
   or follows the `*Actor` naming convention is present on the member or its enclosing type
 Async members never get the stamp because `AsyncFunction` controls their dispatch separately.
 */
internal func shouldStampJavaScriptActor(
  on member: DeclSyntaxProtocol,
  enclosedBy enclosing: some DeclGroupSyntax
) -> Bool {
  let modifiers = memberModifiers(of: member)
  if modifiers.contains(where: { $0.name.text == "nonisolated" }) {
    return false
  }

  if let funcDecl = member.as(FunctionDeclSyntax.self),
    funcDecl.signature.effectSpecifiers?.asyncSpecifier != nil {
    return false
  }

  let memberAttributes = memberAttributes(of: member)
  if memberAttributes.contains(where: hasGlobalActorShape) {
    return false
  }

  if enclosing.attributes.contains(where: hasGlobalActorShape) {
    return false
  }

  return true
}

private func memberModifiers(of decl: DeclSyntaxProtocol) -> DeclModifierListSyntax {
  if let funcDecl = decl.as(FunctionDeclSyntax.self) {
    return funcDecl.modifiers
  }
  if let varDecl = decl.as(VariableDeclSyntax.self) {
    return varDecl.modifiers
  }
  if let initDecl = decl.as(InitializerDeclSyntax.self) {
    return initDecl.modifiers
  }
  return DeclModifierListSyntax()
}

internal func memberAttributes(of decl: DeclSyntaxProtocol) -> AttributeListSyntax {
  if let funcDecl = decl.as(FunctionDeclSyntax.self) {
    return funcDecl.attributes
  }
  if let varDecl = decl.as(VariableDeclSyntax.self) {
    return varDecl.attributes
  }
  if let initDecl = decl.as(InitializerDeclSyntax.self) {
    return initDecl.attributes
  }
  return AttributeListSyntax()
}

private func hasGlobalActorShape(_ element: AttributeListSyntax.Element) -> Bool {
  guard let attribute = element.as(AttributeSyntax.self) else {
    return false
  }
  let name = attribute.attributeName.trimmedDescription
  return name.hasSuffix("Actor")
}

/// The Swift default type of a literal expression — `String`, `Double`, `Int`, or `Bool` — or `nil`
/// when the expression isn't one of those literals. Used to recover a property's type when it has no
/// annotation but does have a literal default (`var name = "foo"` → `String`). This matches the type
/// Swift itself would infer for the same un-annotated declaration; expressions whose type a syntactic
/// macro can't know (function calls, collection literals, member access) return `nil`.
internal func inferredLiteralType(of expression: ExprSyntax) -> String? {
  if expression.is(StringLiteralExprSyntax.self) {
    return "String"
  }
  if expression.is(FloatLiteralExprSyntax.self) {
    return "Double"
  }
  if expression.is(IntegerLiteralExprSyntax.self) {
    return "Int"
  }
  if expression.is(BooleanLiteralExprSyntax.self) {
    return "Bool"
  }
  return nil
}

extension AttributeListSyntax {
  internal func firstAttribute(named name: String) -> AttributeSyntax? {
    for element in self {
      if let attr = element.as(AttributeSyntax.self),
        attr.attributeName.trimmedDescription == name {
        return attr
      }
    }
    return nil
  }
}

/// A type spelled so it's valid in expression position (before `.decode`/`.encode`, before
/// `.getDynamicType()`, or after `as!`). Implicitly-unwrapped optionals (`T!`) are only allowed in
/// type-annotation position, so a trailing `!` is rewritten to `?` (`T!` and `T?` are both
/// `Optional<T>`, which the conversion layer treats identically). Other type spellings pass through
/// unchanged.
internal func expressionType(_ type: String) -> String {
  guard type.hasSuffix("!") else {
    return type
  }
  return type.dropLast() + "?"
}

// MARK: - @JS property collection

/// Collects the `@JS var` bindings of a declaration into `JSProperty` values for direct JSI binding.
/// Shared between `@ExpoModule` and `@SharedObject` — the resulting properties are receiver-agnostic;
/// the decorator that emits them picks the receiver (module `self` vs. shared-object `_self`).
internal func collectProperties(
  varDecl: VariableDeclSyntax,
  attribute: AttributeSyntax
) -> [JSProperty] {
  let jsNameOverride = jsNameArgument(of: attribute)
  // A `let` is never settable; only `var` bindings can carry a setter.
  let isVar = varDecl.bindingSpecifier.tokenKind == .keyword(.var)

  return varDecl.bindings.compactMap { binding in
    guard let ident = binding.pattern.as(IdentifierPatternSyntax.self) else {
      return nil
    }
    let swiftName = ident.identifier.text
    // Prefer the explicit annotation; recover the type from a literal default (`var x = false`)
    // when there's none. `nil` falls back to inference at the use site.
    let valueType = binding.typeAnnotation?.type.trimmedDescription
      ?? binding.initializer.flatMap { inferredLiteralType(of: $0.value) }
    return JSProperty(
      swiftName: swiftName,
      jsName: jsNameOverride ?? swiftName,
      valueType: valueType,
      isSettable: isVar && bindingIsSettable(binding)
    )
  }
}

/// Whether a `var` binding is settable from JS. A stored property (no accessor block) is settable;
/// a computed property is settable only when it declares an explicit `set` accessor. A getter-only
/// computed property (`{ get }` or a single getter body) stays read-only. `willSet`/`didSet`
/// observers imply stored storage, which is also settable.
internal func bindingIsSettable(_ binding: PatternBindingSyntax) -> Bool {
  guard let accessorBlock = binding.accessorBlock else {
    return true
  }
  switch accessorBlock.accessors {
  case .accessors(let accessors):
    return accessors.contains { accessor in
      switch accessor.accessorSpecifier.tokenKind {
      case .keyword(.set), .keyword(.willSet), .keyword(.didSet):
        return true
      default:
        return false
      }
    }
  case .getter:
    return false
  }
}
