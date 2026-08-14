import SwiftSyntax

/// A `@JS init` collected for direct JSI binding. A shared-object type has at most one (JS classes
/// have a single constructor). Instead of a `Constructor { … }` DSL entry, the macro synthesizes an
/// override of `SharedObject._constructSharedObject(...)` that decodes the JS arguments and returns a
/// fresh instance; unlike the method/property bindings it produces the native instance rather than
/// recovering one.
internal struct JSConstructor {
  let parameters: [FunctionParameterSyntax]

  init(initDecl: InitializerDeclSyntax) {
    self.parameters = Array(initDecl.signature.parameterClause.parameters)
  }

  /// The body statements, indented with `indent`: arity guard, per-argument decode through
  /// `JavaScriptDecodable.decode` on a zero-copy `arguments.unownedValue(at:)` (the arity guard proves
  /// each index is in bounds), then `return <Type>(label: arg0, …)`.
  private func bodyStatements(typeName: String, indent: String) -> String {
    var lines: [String] = []

    lines.append(
      """
      guard arguments.count == \(parameters.count) else {
        throw Exceptions.ArgumentsRangeMismatch((functionName: "\(typeName)", received: arguments.count, required: \(parameters.count), maximum: \(parameters.count)))
      }
      """)

    var callArguments: [String] = []
    for (index, parameter) in parameters.enumerated() {
      let exprType = expressionType(parameter.type.trimmedDescription)
      lines.append("let arg\(index) = try \(exprType).decode(arguments.unownedValue(at: \(index)), in: runtime)")

      let label = parameter.firstName.text
      callArguments.append(label == "_" ? "arg\(index)" : "\(label): arg\(index)")
    }

    lines.append("return \(typeName)(\(callArguments.joined(separator: ", ")))")

    return lines
      .flatMap { $0.split(separator: "\n", omittingEmptySubsequences: false) }
      .map { indent + $0 }
      .joined(separator: "\n")
  }

  /// Builds an instance from the JS arguments. Overrides the base `SharedObject` class method; returns
  /// the concrete instance, which promotes to the base `SharedObject?` return type.
  func buildConstructor(typeName: String) -> DeclSyntax {
    return """
      @JavaScriptActor
      public override class func _constructSharedObject(this: JavaScriptValue, arguments: borrowing JavaScriptValuesBuffer, in runtime: JavaScriptRuntime) throws -> SharedObject? {
      \(raw: bodyStatements(typeName: typeName, indent: "  "))
      }
      """
  }
}
