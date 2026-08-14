import SwiftSyntax

/// Extracts the full JS-exported surface of every top-level `@ExpoModule`, `@SharedObject`, and
/// `@Record` type: their `@JS` members and record properties. The deep counterpart to
/// `DetectionVisitor`. Recognition is purely syntactic and re-reads what the macros read (the macro
/// target can't be imported), so it stays in step with `JSFunction` / `JSProperty` / `JSConstructor` /
/// `RecordProperty`.
final class SurfaceVisitor: SyntaxVisitor {
  private let file: String
  private(set) var modules: [ExportedModule] = []
  private(set) var sharedObjects: [ExportedSharedObject] = []
  private(set) var records: [ExportedRecord] = []

  init(file: String) {
    self.file = file
    super.init(viewMode: .sourceAccurate)
  }

  override func visit(_ node: ClassDeclSyntax) -> SyntaxVisitorContinueKind {
    if isTopLevel(node) {
      classify(name: node.name.text, attributes: node.attributes, members: node.memberBlock.members)
    }
    // The member walk reads the body itself; nested types aren't part of this surface (matching
    // `DetectionVisitor`'s top-level-only scope), so there's no reason to descend.
    return .skipChildren
  }

  override func visit(_ node: StructDeclSyntax) -> SyntaxVisitorContinueKind {
    if isTopLevel(node) {
      classify(name: node.name.text, attributes: node.attributes, members: node.memberBlock.members)
    }
    return .skipChildren
  }

  /// Routes a top-level type to the right collector based on which Expo macro it carries. A type
  /// carrying none of them is ignored. `@Record` and `@ExpoModule`/`@SharedObject` are mutually
  /// exclusive in practice, so the first match wins.
  private func classify(name: String, attributes: AttributeListSyntax, members: MemberBlockItemListSyntax) {
    if let attribute = attributes.firstAttribute(named: DetectedMacro.expoModule.rawValue) {
      let (functions, properties, _) = collectJSMembers(members)
      modules.append(
        ExportedModule(
          name: name,
          jsName: stringArgument(of: attribute) ?? name,
          functions: functions,
          properties: properties,
          file: file
        ))
      return
    }

    if let attribute = attributes.firstAttribute(named: DetectedMacro.sharedObject.rawValue) {
      let (functions, properties, constructor) = collectJSMembers(members)
      sharedObjects.append(
        ExportedSharedObject(
          name: name,
          jsName: stringArgument(of: attribute) ?? name,
          constructorParameters: constructor,
          functions: functions,
          properties: properties,
          file: file
        ))
      return
    }

    if attributes.firstAttribute(named: DetectedMacro.record.rawValue) != nil {
      records.append(ExportedRecord(name: name, properties: collectRecordProperties(members), file: file))
    }
  }

  /// The `@JS` members of a module / shared-object body: functions, properties, and the single
  /// `@JS init` constructor parameters (`nil` when absent). Only declarations carrying `@JS` count.
  private func collectJSMembers(
    _ members: MemberBlockItemListSyntax
  ) -> (functions: [ExportedFunction], properties: [ExportedProperty], constructor: [ExportedParameter]?) {
    var functions: [ExportedFunction] = []
    var properties: [ExportedProperty] = []
    var constructor: [ExportedParameter]?

    for member in members {
      let decl = member.decl

      if let initDecl = decl.as(InitializerDeclSyntax.self),
        initDecl.attributes.firstAttribute(named: DetectedMacro.js.rawValue) != nil {
        // At most one `@JS init`; keep the first if a malformed source has more (the macro errors).
        if constructor == nil {
          constructor = parameters(of: initDecl.signature.parameterClause)
        }
        continue
      }

      if let funcDecl = decl.as(FunctionDeclSyntax.self),
        let attribute = funcDecl.attributes.firstAttribute(named: DetectedMacro.js.rawValue) {
        functions.append(makeFunction(funcDecl: funcDecl, attribute: attribute))
        continue
      }

      if let varDecl = decl.as(VariableDeclSyntax.self),
        let attribute = varDecl.attributes.firstAttribute(named: DetectedMacro.js.rawValue) {
        properties.append(contentsOf: makeProperties(varDecl: varDecl, attribute: attribute))
      }
    }

    return (functions, properties, constructor)
  }

  /// Builds an `ExportedFunction` from a `@JS func`: JS-name fallback, parameters, a `Void` return as
  /// `nil`, and the effect/static flags.
  private func makeFunction(funcDecl: FunctionDeclSyntax, attribute: AttributeSyntax) -> ExportedFunction {
    let effects = funcDecl.signature.effectSpecifiers
    let returnType = funcDecl.signature.returnClause?.type
    return ExportedFunction(
      name: funcDecl.name.text,
      jsName: stringArgument(of: attribute) ?? funcDecl.name.text,
      parameters: parameters(of: funcDecl.signature.parameterClause),
      returns: isVoidType(returnType) ? nil : returnType.map { typeNode(from: $0) },
      isAsync: effects?.asyncSpecifier != nil,
      isThrowing: effects?.throwsClause?.throwsSpecifier != nil,
      isStatic: isTypeLevel(funcDecl.modifiers)
    )
  }

  /// Builds the `ExportedProperty` entries for a `@JS var`/`let`. One declaration can introduce several
  /// bindings (`var a, b: Int`), so this returns an array. The value type is the annotation, else the
  /// literal default's inferred type, else `nil`.
  private func makeProperties(varDecl: VariableDeclSyntax, attribute: AttributeSyntax) -> [ExportedProperty] {
    let isLet = varDecl.bindingSpecifier.tokenKind == .keyword(.let)
    let isStatic = isTypeLevel(varDecl.modifiers)
    let override = stringArgument(of: attribute)
    var result: [ExportedProperty] = []

    for binding in varDecl.bindings {
      guard let ident = binding.pattern.as(IdentifierPatternSyntax.self) else {
        continue
      }
      let name = ident.identifier.text
      let type = valueTypeNode(annotation: binding.typeAnnotation?.type, initializer: binding.initializer?.value)
      result.append(
        ExportedProperty(
          name: name,
          jsName: override ?? name,
          type: type,
          isSettable: isSettable(binding: binding, isLet: isLet),
          isStatic: isStatic
        ))
    }
    return result
  }

  /// True when a binding is assignable from JS, mirroring the macro's `bindingIsSettable`: a `let` is
  /// never settable; a stored `var` is; a computed `var` is settable iff it declares `set`/`willSet`/
  /// `didSet` (a getter-only `var` is read-only).
  private func isSettable(binding: PatternBindingSyntax, isLet: Bool) -> Bool {
    if isLet {
      return false
    }
    guard let accessorBlock = binding.accessorBlock else {
      // Stored `var`, settable.
      return true
    }
    switch accessorBlock.accessors {
    case .accessors(let list):
      return list.contains { accessor in
        switch accessor.accessorSpecifier.tokenKind {
        case .keyword(.set), .keyword(.willSet), .keyword(.didSet):
          return true
        default:
          return false
        }
      }
    case .getter:
      // `var x: Int { ... }` shorthand getter, read-only.
      return false
    }
  }

  /// The `@Record` properties: every stored, non-excluded `var`/`let` binding, mirroring
  /// `RecordMacro.recordProperties`. Computed and modifier-excluded bindings are skipped, as is one
  /// whose type can't be determined (the macro would error, but the scan stays lenient).
  private func collectRecordProperties(_ members: MemberBlockItemListSyntax) -> [ExportedRecordProperty] {
    var properties: [ExportedRecordProperty] = []

    for member in members {
      guard let varDecl = member.decl.as(VariableDeclSyntax.self),
        !isExcludedRecordModifier(varDecl.modifiers) else {
        continue
      }
      for binding in varDecl.bindings {
        if binding.accessorBlock != nil {
          continue
        }
        guard let ident = binding.pattern.as(IdentifierPatternSyntax.self) else {
          continue
        }
        let annotation = binding.typeAnnotation?.type
        guard let type = valueTypeNode(annotation: annotation, initializer: binding.initializer?.value) else {
          continue
        }
        properties.append(
          ExportedRecordProperty(
            name: ident.identifier.text,
            type: type,
            isOptional: annotation.map { isOptionalType($0) } ?? false,
            hasDefault: binding.initializer != nil
          ))
      }
    }
    return properties
  }

  /// Projects a parameter clause into `ExportedParameter`s: label = first name, name = second (else
  /// first), and `optional` when it has a default value or an optional type.
  private func parameters(of clause: FunctionParameterClauseSyntax) -> [ExportedParameter] {
    clause.parameters.map { parameter in
      let label = parameter.firstName.text
      let name = parameter.secondName?.text ?? label
      return ExportedParameter(
        label: label,
        name: name,
        type: typeNode(from: parameter.type),
        isOptional: parameter.defaultValue != nil || isOptionalType(parameter.type)
      )
    }
  }

  /// True when the declaration sits at file scope. Same rule as `DetectionVisitor.isTopLevel`: its
  /// parent is a `CodeBlockItemSyntax` directly under the source file's top-level item list.
  private func isTopLevel(_ node: some SyntaxProtocol) -> Bool {
    guard let item = node.parent?.as(CodeBlockItemSyntax.self) else {
      return false
    }
    return item.parent?.parent?.is(SourceFileSyntax.self) == true
  }
}

// MARK: - Syntactic helpers (shared spelling with the macros)

/// True when the modifiers make a member type-level (`static` or `class`).
private func isTypeLevel(_ modifiers: DeclModifierListSyntax) -> Bool {
  modifiers.contains {
    $0.name.tokenKind == .keyword(.static) || $0.name.tokenKind == .keyword(.class)
  }
}

/// True when a modifier excludes a property from being a `@Record` field (`static`, `class`,
/// `private`, `fileprivate`, `lazy`), mirroring `RecordMacro.isExcludedByModifier`.
private func isExcludedRecordModifier(_ modifiers: DeclModifierListSyntax) -> Bool {
  modifiers.contains { modifier in
    switch modifier.name.tokenKind {
    case .keyword(.static), .keyword(.class), .keyword(.private), .keyword(.fileprivate), .keyword(.lazy):
      return true
    default:
      return false
    }
  }
}

/// True when a type is written as an optional: `T?`, `T!`, or `Optional<T>`, mirroring the macros'
/// `isOptionalType`.
private func isOptionalType(_ type: TypeSyntax) -> Bool {
  if type.is(OptionalTypeSyntax.self) || type.is(ImplicitlyUnwrappedOptionalTypeSyntax.self) {
    return true
  }
  if let identifier = type.as(IdentifierTypeSyntax.self), identifier.name.text == "Optional" {
    return true
  }
  return false
}

/// The first attribute whose spelled name matches `name`. A local copy of the macros' helper (the
/// macro target can't be imported here).
extension AttributeListSyntax {
  fileprivate func firstAttribute(named name: String) -> AttributeSyntax? {
    for element in self {
      if let attribute = element.as(AttributeSyntax.self),
        attribute.attributeName.trimmedDescription == name {
        return attribute
      }
    }
    return nil
  }
}

/// The node for a property/field value type: the annotation, else the literal default's inferred
/// primitive (`var n = 1` -> `Int`), else `nil`.
private func valueTypeNode(annotation: TypeSyntax?, initializer: ExprSyntax?) -> TypeNode? {
  if let annotation {
    return typeNode(from: annotation)
  }
  return initializer.flatMap(inferredLiteralType)
}

/// The node for a simple literal default (`var n = 1` -> `Int`); `nil` for anything non-literal.
private func inferredLiteralType(of expression: ExprSyntax) -> TypeNode? {
  switch expression.kind {
  case .stringLiteralExpr:
    return .primitive(name: "String", jsType: .string)
  case .integerLiteralExpr:
    return .primitive(name: "Int", jsType: .number)
  case .floatLiteralExpr:
    return .primitive(name: "Double", jsType: .number)
  case .booleanLiteralExpr:
    return .primitive(name: "Bool", jsType: .boolean)
  default:
    return nil
  }
}
