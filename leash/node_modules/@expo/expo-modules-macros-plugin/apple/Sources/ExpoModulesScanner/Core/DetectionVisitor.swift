import SwiftSyntax

/// Walks a parsed source file and records top-level declarations carrying `@ExpoModule`, `@JS`,
/// `@SharedObject`, or `@Record`. Only file-scope declarations are considered: these macros apply to
/// top-level types, so descending into type and function bodies would only surface false positives.
/// Recognition mirrors the macros themselves — a purely syntactic match on the spelled attribute
/// name — so it sees the same declarations the compiler would hand the plugin, without compiling
/// anything.
final class DetectionVisitor: SyntaxVisitor {
  private let file: String
  private let converter: SourceLocationConverter
  /// Only these macros are recorded; the rest are ignored. Lets a `modules` scan report just
  /// `@ExpoModule` while an `exports` scan covers them all.
  private let detectedMacros: Set<DetectedMacro>
  private(set) var detections: [Detection] = []

  init(file: String, tree: SourceFileSyntax, detectedMacros: Set<DetectedMacro>) {
    self.file = file
    self.converter = SourceLocationConverter(fileName: file, tree: tree)
    self.detectedMacros = detectedMacros
    super.init(viewMode: .sourceAccurate)
  }

  override func visit(_ node: ClassDeclSyntax) -> SyntaxVisitorContinueKind {
    if isTopLevel(node) {
      record(attributes: node.attributes, name: node.name.text, kind: "class", at: node)
    }
    // Members live in the type body; we never report them, so there's no reason to descend.
    return .skipChildren
  }

  override func visit(_ node: StructDeclSyntax) -> SyntaxVisitorContinueKind {
    if isTopLevel(node) {
      record(attributes: node.attributes, name: node.name.text, kind: "struct", at: node)
    }
    return .skipChildren
  }

  /// True when the declaration sits at file scope: its parent is a `CodeBlockItemSyntax` directly
  /// under the source file's top-level item list. Members of a type are nested in a
  /// `MemberBlockItemSyntax` instead, so they don't match.
  ///
  /// TODO: decide whether to support nested types. A macro on a type nested in another type/enum/
  /// extension is valid Swift but missed here; supporting it means descending into type bodies and
  /// recording the enclosing path for a qualified name (e.g. `Namespace.InnerModule`).
  private func isTopLevel(_ node: some SyntaxProtocol) -> Bool {
    guard let item = node.parent?.as(CodeBlockItemSyntax.self) else {
      return false
    }
    return item.parent?.parent?.is(SourceFileSyntax.self) == true
  }

  /// Emits one detection per recognized Expo attribute on the declaration. A declaration can in
  /// principle carry more than one (uncommon), so each is recorded independently.
  private func record(
    attributes: AttributeListSyntax,
    name: String,
    kind: String,
    at node: some SyntaxProtocol
  ) {
    for element in attributes {
      guard let attribute = element.as(AttributeSyntax.self),
        let macro = DetectedMacro(rawValue: attribute.attributeName.trimmedDescription),
        detectedMacros.contains(macro) else {
        continue
      }
      let location = converter.location(for: node.positionAfterSkippingLeadingTrivia)
      detections.append(
        Detection(
          macro: macro,
          name: name,
          declarationKind: kind,
          jsName: stringArgument(of: attribute),
          arguments: arguments(of: attribute),
          file: file,
          line: location.line,
          column: location.column
        )
      )
    }
  }
}

/// Every argument passed to the attribute, in source order, each as a label (or `nil` when
/// positional) plus the value expression's source text. Returns an empty array when the attribute
/// is written bare (`@ExpoModule`) or with empty parens.
private func arguments(of attribute: AttributeSyntax) -> [MacroArgument] {
  guard let args = attribute.arguments?.as(LabeledExprListSyntax.self) else {
    return []
  }
  return args.map { arg in
    MacroArgument(label: arg.label?.text, value: arg.expression.trimmedDescription)
  }
}

/// The first string-literal argument of an attribute, e.g. `@JS("doWork")` -> "doWork". Returns
/// `nil` when there's no argument or it isn't a plain string literal. (Same shape the
/// `jsNameArgument` helper reads inside the macros.) Shared with the `scan-exports` surface visitor,
/// which reads the same JS-name override off `@JS`/`@ExpoModule`/`@SharedObject`.
func stringArgument(of attribute: AttributeSyntax) -> String? {
  guard let args = attribute.arguments?.as(LabeledExprListSyntax.self),
    let first = args.first,
    first.label == nil,
    let str = first.expression.as(StringLiteralExprSyntax.self),
    let segment = str.segments.first?.as(StringSegmentSyntax.self),
    str.segments.count == 1 else {
    return nil
  }
  return segment.content.text
}
