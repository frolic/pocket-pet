import ExpoModulesScanner
import Foundation

/// Command-line front end for the scanner. Parses the subcommand and paths, then delegates to the
/// matching library entry (which runs the scan and writes its JSON output). Each path may be a
/// `.swift` file or a directory (scanned recursively for `.swift` files).
///
/// Subcommands:
///   scan-modules   <path>...   fast: top-level `@ExpoModule` types, for autolinking
///   scan-exports   <path>...   deep: full JS-exported surface, for TS type generation

let toolName = "ExpoModulesScanner"

let usageText = """
  usage: \(toolName) <subcommand> <path> [<path> ...]

  subcommands:
    scan-modules   fast scan for top-level @ExpoModule types (autolinking)
    scan-exports   deep scan of the full JS-exported surface (type generation)

  options:
    -h, --help     print this help and exit

  """

/// Prints the usage text to the given handle. Goes to stdout when help was explicitly requested
/// (a successful action), stderr when it accompanies a usage error.
func printUsage(to handle: FileHandle = .standardError) {
  handle.write(Data(usageText.utf8))
}

func fail(_ message: String, usage: Bool = false, code: Int32 = 2) -> Never {
  FileHandle.standardError.write(Data("error: \(message)\n".utf8))
  if usage {
    printUsage()
  }
  exit(code)
}

var arguments = Array(CommandLine.arguments.dropFirst())

// `-h`/`--help` anywhere is treated as a help request: print usage to stdout and exit 0.
if arguments.contains(where: { $0 == "-h" || $0 == "--help" }) {
  printUsage(to: .standardOutput)
  exit(0)
}

guard !arguments.isEmpty else {
  printUsage()
  exit(2)
}

let subcommand = arguments.removeFirst()
let paths = arguments

switch subcommand {
case "scan-modules":
  guard !paths.isEmpty else {
    fail("scan-modules requires at least one path", usage: true)
  }
  exit(Scanner.runModules(paths: paths))

case "scan-exports":
  guard !paths.isEmpty else {
    fail("scan-exports requires at least one path", usage: true)
  }
  exit(Scanner.runExports(paths: paths))

default:
  fail("unknown subcommand '\(subcommand)'", usage: true)
}
