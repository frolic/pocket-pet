import SwiftSyntax
import SwiftSyntaxMacros

/// Marker macro applied to module / shared-object members that should be exposed to JavaScript.
/// `@ExpoModule` and `@SharedObject` discover declarations carrying this attribute and generate the
/// corresponding `Function` / `AsyncFunction` / `Property` / `Constructor` registrations; that part
/// of the expansion lives in those macros.
///
/// On its own, `@JS` emits one thing: a never-called peer that asserts each type crossing the JS
/// boundary is convertible in the direction it travels — arguments are `JavaScriptDecodable`, return
/// values are `JavaScriptEncodable` (a settable property's value type is both). Because it's a
/// **peer** of the marked member, a non-conforming type produces a compile error located on the
/// user's own `@JS` declaration rather than on the enclosing `@ExpoModule`. The assertion mechanism
/// itself is shared (see `directionalConformanceAssertion`); `@JS` only supplies the boundary types,
/// split by direction, that it reads off the declaration.
///
/// Usage:
///
///   @JS
///   func greet(name: String) -> String { ... }
///
///   @JS("doWork")
///   func performWork() async throws { ... }
///
///   @JS
///   var status: String { "ok" }
public struct JSMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let member = boundaryMember(of: declaration),
      let assertion = directionalConformanceAssertion(
        name: member.name,
        decodableTypes: member.decodableTypes,
        encodableType: member.encodableType,
        isStatic: member.isStatic
      ) else {
      return []
    }
    return [assertion]
  }
}

/// What an assertion peer needs about the `@JS` member it sits beside: a name (to keep the peer unique
/// among siblings), the boundary types split by conversion direction, and whether the member is
/// type-level. Arguments (and a settable property's incoming value) are decoded; return values (and a
/// property's outgoing value) are encoded, so each is asserted against the protocol for its direction.
private struct BoundaryMember {
  let name: String
  /// Types decoded from JS: function/constructor arguments, and a settable property's value type.
  let decodableTypes: [String]
  /// The single type encoded to JS: a function's return type, or a property's value type on read;
  /// `nil` when the member produces nothing JS-visible (a `Void` function).
  let encodableType: String?
  /// True for `static`/`class` members, so the peer is emitted in the same metatype context.
  let isStatic: Bool
}

/// Reads the boundary member off a `@JS` declaration, splitting its types by conversion direction. A
/// function contributes its parameter types (decodable) and its return type when non-Void (encodable);
/// a property contributes its value type as encodable (the getter) and also as decodable when settable
/// (the setter). Composed types (`[Int]`, `String?`, …) are kept verbatim, their conditional
/// conformances transitively constraining the elements. Returns `nil` for declaration kinds `@JS`
/// doesn't read types from, or a property whose type isn't spelled out (a syntactic macro can't
/// recover it).
private func boundaryMember(of declaration: some DeclSyntaxProtocol) -> BoundaryMember? {
  if let funcDecl = declaration.as(FunctionDeclSyntax.self) {
    let decodableTypes = funcDecl.signature.parameterClause.parameters.map { $0.type.trimmedDescription }
    let returnType = funcDecl.signature.returnClause?.type
    let encodableType = returnType.flatMap { isVoidType($0) ? nil : $0.trimmedDescription }
    return BoundaryMember(
      name: funcDecl.name.text,
      decodableTypes: decodableTypes,
      encodableType: encodableType,
      isStatic: isTypeLevel(funcDecl.modifiers)
    )
  }

  if let varDecl = declaration.as(VariableDeclSyntax.self),
    let binding = varDecl.bindings.first,
    let identifier = binding.pattern.as(IdentifierPatternSyntax.self),
    let type = binding.typeAnnotation?.type {
    // A `let`, or a `var` with no setter, is read-only (encodable only). A settable `var` is also
    // decoded on write, so its value type is asserted in both directions.
    let typeText = type.trimmedDescription
    let isVar = varDecl.bindingSpecifier.tokenKind == .keyword(.var)
    let isSettable = isVar && bindingIsSettable(binding)
    return BoundaryMember(
      name: identifier.identifier.text,
      decodableTypes: isSettable ? [typeText] : [],
      encodableType: typeText,
      isStatic: isTypeLevel(varDecl.modifiers)
    )
  }

  return nil
}

/// True when the modifiers make the member type-level (`static` or `class`), so its assertion peer
/// must be emitted in the same metatype context rather than as an instance member.
private func isTypeLevel(_ modifiers: DeclModifierListSyntax) -> Bool {
  return modifiers.contains {
    $0.name.tokenKind == .keyword(.static) || $0.name.tokenKind == .keyword(.class)
  }
}

/// True when a return clause is written as `Void` / `()` — nothing crosses the boundary, so it needs
/// no conformance assertion. (A missing return clause never reaches here: `returnClause` is `nil`.)
private func isVoidType(_ type: TypeSyntax) -> Bool {
  let text = type.trimmedDescription
  return text == "Void" || text == "()"
}
