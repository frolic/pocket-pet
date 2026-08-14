import SwiftSyntax

/// Where a directly-bound closure gets the Swift value it calls into. A module is a singleton, so its
/// bindings call `self` and ignore the JS `this`; a shared object has a distinct native instance per JS
/// object, so its bindings recover the typed receiver from `this`.
internal enum Receiver {
  /// The module singleton; the closure captures `self` strong.
  case module
  /// A shared object of the given concrete type; the closure captures nothing and recovers the receiver
  /// from `this` per call.
  case sharedObject(typeName: String)

  /// The expression the body calls members on: `self` for a module, `_self` (bound by `unwrapStatement`)
  /// for a shared object. The leading underscore avoids colliding with a user member like `var owner`.
  var callee: String {
    switch self {
    case .module:
      return "self"
    case .sharedObject:
      return "_self"
    }
  }

  /// The JS object the decorator binds members onto, matching its first parameter: `object` for a
  /// module (its own JS object), `prototype` for a shared object (the shared class prototype).
  var decoratedObject: String {
    switch self {
    case .module:
      return "object"
    case .sharedObject:
      return "prototype"
    }
  }

  /// The leading body line binding the receiver, or `nil` for a module (it reads `self` directly). For a
  /// shared object, `native(from:as:)` recovers the typed instance from the borrowed `this`, throwing on
  /// a foreign object or a type mismatch.
  var unwrapStatement: String? {
    switch self {
    case .module:
      return nil
    case .sharedObject(let typeName):
      return "let _self = try SharedObject.native(from: this.asObject(in: runtime), as: \(typeName).self)"
    }
  }

  /// The capture-clause fragment (with a trailing space, or empty when nothing is captured). A module
  /// captures `self` strong; a shared object captures nothing of the instance.
  var captureClause: String {
    switch self {
    case .module:
      return "[self] "
    case .sharedObject:
      return ""
    }
  }
}
