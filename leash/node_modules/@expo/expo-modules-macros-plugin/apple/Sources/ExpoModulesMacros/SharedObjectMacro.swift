import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

/**
 Member macro applied to a `SharedObject` subclass. Scans the class body for
 declarations marked with `@JS` and synthesizes `_synthesizedClassDefinition()`,
 returning a `ClassDefinition` ready to drop into a module's `Class { ... }` slot.

   @SharedObject
   final class Cache: SharedObject {
     @JS
     init(name: String) { self.name = name }

     @JS
     func get(_ key: String) -> String? { ... }

     @JS
     var size: Int { 42 }
   }

 The companion `@ExpoModule(classes: [Cache.self])` wires the resulting
 definition into the module's exposed surface.
 */
public struct SharedObjectMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let classDecl = declaration.as(ClassDeclSyntax.self) else {
      throw MacroExpansionErrorMessage("@SharedObject can only be applied to a class")
    }

    guard inheritsFromSharedObject(classDecl) else {
      throw MacroExpansionErrorMessage(
        "@SharedObject class must inherit from SharedObject. Add `: SharedObject` to the class declaration.")
    }

    let typeName = classDecl.name.text
    let jsName = jsNameArgument(of: node) ?? typeName

    // `@JS func`s/`var`s and the `@JS init` are bound directly into the shared object's JS object by
    // the synthesized `_decorateSharedObject` / `_constructSharedObject` rather than described with a
    // `Function(...)` / `Property(...)` / `Constructor { … }` DSL entry, so they're collected here
    // instead of appended to the `Class` block. The block keeps only non-`@JS` definitions (none are
    // collected today), so it's empty when every member is `@JS`.
    let entries: [String] = []
    var functions: [JSFunction] = []
    var properties: [JSProperty] = []
    var constructor: JSConstructor?

    for member in classDecl.memberBlock.members {
      let decl = member.decl

      if let initDecl = decl.as(InitializerDeclSyntax.self),
        initDecl.attributes.firstAttribute(named: "JS") != nil {
        if constructor != nil {
          throw MacroExpansionErrorMessage(
            "@SharedObject classes can have at most one @JS initializer; JavaScript classes have a single constructor.")
        }
        constructor = JSConstructor(initDecl: initDecl)
        continue
      }

      if let funcDecl = decl.as(FunctionDeclSyntax.self),
        let attribute = funcDecl.attributes.firstAttribute(named: "JS") {
        functions.append(JSFunction(funcDecl: funcDecl, attribute: attribute))
        continue
      }

      if let varDecl = decl.as(VariableDeclSyntax.self),
        let attribute = varDecl.attributes.firstAttribute(named: "JS") {
        properties.append(contentsOf: collectProperties(varDecl: varDecl, attribute: attribute))
      }
    }

    let lines = entries.map { "    \($0)" }.joined(separator: "\n")
    let body = entries.isEmpty
      ? "  return Class(\"\(jsName)\", \(typeName).self) {\n  }"
      : "  return Class(\"\(jsName)\", \(typeName).self) {\n\(lines)\n  }"

    var emitted: [DeclSyntax] = [
      """
      public static func _synthesizedClassDefinition() -> ClassDefinition {
      \(raw: body)
      }
      """
    ]

    // Direct JSI binding: one `_decorateSharedObject` that binds each `@JS func`/`var` onto the JS
    // object (unwrapping the per-call receiver from `this`), and a `_constructSharedObject` that
    // builds an instance from the `@JS init` arguments. Each is emitted only when it has something
    // to do.
    if !functions.isEmpty || !properties.isEmpty {
      emitted.append(
        buildDecorateSharedObject(functions: functions, properties: properties, typeName: typeName))
    }
    if let constructor {
      emitted.append(constructor.buildConstructor(typeName: typeName))
    }

    return emitted
  }
}

extension SharedObjectMacro: MemberAttributeMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AttributeSyntax] {
    // `@Event(sync: true)` members are stamped alongside `@JS` ones: a sync event dispatches
    // inline, so the isolation forces its call site onto the JS thread. Async events (the
    // default) stay unstamped — their `emit` schedules onto the JS thread itself.
    guard memberHasJSAttribute(member) || isSyncEventMember(member),
      shouldStampJavaScriptActor(on: member, enclosedBy: declaration) else {
      return []
    }
    return ["@JavaScriptActor"]
  }
}

// MARK: - Inheritance check

private func inheritsFromSharedObject(_ classDecl: ClassDeclSyntax) -> Bool {
  return inheritsFromAny(classDecl, names: ["SharedObject"])
}
