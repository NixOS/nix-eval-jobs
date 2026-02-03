
#include <cstdlib>
#include <nix/util/args.hh>
#include <nix/util/error.hh>
#include <nix/util/file-system.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/lockfile.hh>
#include <nix/util/canon-path.hh>
#include <nix/main/common-args.hh>
#include <nix/cmd/common-eval-args.hh>
#include <nix/util/source-accessor.hh>
#include <nix/flake/flakeref.hh>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "eval-args.hh"
#include "output-stream-lock.hh"

MyArgs::MyArgs() : MixCommonArgs("nix-eval-jobs") {
    addFlag({
        .longName = "help",
        .aliases = {},
        .shortName = 0,
        .description = "show usage information",
        .category = "",
        .labels = {},
        .handler = {[&]() -> void {
            getCoutLock().lock() << "USAGE: nix-eval-jobs [options] expr\n\n";
            for (const auto &[name, flag] : longFlags) {
                if (hiddenCategories.contains(flag->category)) {
                    continue;
                }
                static constexpr int FLAG_WIDTH = 20;
                getCoutLock().lock()
                    << "  --" << std::left << std::setw(FLAG_WIDTH) << name
                    << " " << flag->description << "\n";
            }

            ::exit(0); // NOLINT(concurrency-mt-unsafe)
        }},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "impure",
        .aliases = {},
        .shortName = 0,
        .description = "allow impure expressions",
        .category = "",
        .labels = {},
        .handler = {&impure, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "force-recurse",
        .aliases = {},
        .shortName = 0,
        .description = "force recursion (don't respect recurseIntoAttrs)",
        .category = "",
        .labels = {},
        .handler = {&forceRecurse, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "gc-roots-dir",
        .aliases = {},
        .shortName = 0,
        .description = "garbage collector roots directory",
        .category = "",
        .labels = {"path"},
        .handler = {&gcRootsDir},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "workers",
        .aliases = {},
        .shortName = 0,
        .description = "number of evaluate workers",
        .category = "",
        .labels = {"workers"},
        .handler = {[this](const std::string &str) -> void {
            nrWorkers = std::stoi(str);
        }},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "max-memory-size",
        .aliases = {},
        .shortName = 0,
        .description = "maximum evaluation memory size in megabyte "
                       "(4GiB per worker by default)",
        .category = "",
        .labels = {"size"},
        .handler = {[this](const std::string &str) -> void {
            maxMemorySize = std::stoi(str);
        }},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "flake",
        .aliases = {},
        .shortName = 0,
        .description = "build a flake",
        .category = "",
        .labels = {},
        .handler = {&flake, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "meta",
        .aliases = {},
        .shortName = 0,
        .description = "include derivation meta field in output",
        .category = "",
        .labels = {},
        .handler = {&meta, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "constituents",
        .aliases = {},
        .shortName = 0,
        .description =
            "whether to evaluate constituents for Hydra's aggregate feature",
        .category = "",
        .labels = {},
        .handler = {&constituents, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "check-cache-status",
        .aliases = {},
        .shortName = 0,
        .description = "Check if the derivations are present locally or in "
                       "any configured substituters (i.e. binary cache). The "
                       "information will be exposed in the `cacheStatus` field "
                       "of the JSON output.",
        .category = "",
        .labels = {},
        .handler = {&checkCacheStatus, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "show-input-drvs",
        .aliases = {},
        .shortName = 0,
        .description =
            "Show input derivations in the output for each derivation. "
            "This is useful to get direct dependencies of a derivation.",
        .category = "",
        .labels = {},
        .handler = {&showInputDrvs, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "show-trace",
        .aliases = {},
        .shortName = 0,
        .description = "print out a stack trace in case of evaluation errors",
        .category = "",
        .labels = {},
        .handler = {&showTrace, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "no-instantiate",
        .aliases = {},
        .shortName = 0,
        .description =
            "don't instantiate (write) derivations, only evaluate (faster)",
        .category = "",
        .labels = {},
        .handler = {&noInstantiate, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "expr",
        .aliases = {},
        .shortName = 'E',
        .description = "treat the argument as a Nix expression",
        .category = "",
        .labels = {},
        .handler = {&fromArgs, true},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "apply",
        .aliases = {},
        .shortName = 0,
        .description =
            "Apply provided Nix function to each derivation. "
            "The result of this function will be serialized as a JSON value "
            "and stored inside `\"extraValue\"` key of the json line output.",
        .category = "",
        .labels = {"expr"},
        .handler = {&applyExpr},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "select",
        .aliases = {},
        .shortName = 0,
        .description =
            "Apply provided Nix function to transform the evaluation root. "
            "This is applied before any attribute traversal begins. "
            "When used with --flake without a fragment, the function receives "
            "an attrset with 'outputs' and 'inputs'. "
            "When used with a flake fragment, it receives the selected "
            "attribute. "
            "Examples: "
            "--select 'flake: flake.outputs.packages' "
            "--select 'flake: flake.inputs.nixpkgs' "
            "--select 'outputs: outputs.packages.x86_64-linux'",
        .category = "",
        .labels = {"expr"},
        .handler = {&selectExpr},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    // usually in MixFlakeOptions
    addFlag({
        .longName = "override-input",
        .aliases = {},
        .shortName = 0,
        .description =
            "Override a specific flake input (e.g. `dwarffs/nixpkgs`).",
        .category = category,
        .labels = {"input-path", "flake-url"},
        .handler = {[&](const std::string &inputPath,
                        const std::string &flakeRef) -> void {
            // overriden inputs are unlocked
            lockFlags.allowUnlocked = true;
            auto path = nix::flake::NonEmptyInputAttrPath::parse(inputPath);
            if (!path) {
                throw nix::UsageError(
                    "--override-input requires a non-empty input path");
            }
            lockFlags.inputOverrides.insert_or_assign(
                std::move(*path),
                nix::parseFlakeRef(nix::fetchSettings, flakeRef,
                                   nix::absPath(std::filesystem::path(".")),
                                   true));
        }},
        .completer = nullptr,
        .experimentalFeature = std::nullopt,
    });

    addFlag({
        .longName = "reference-lock-file",
        .aliases = {},
        .shortName = 0,
        .description = "Read the given lock file instead of `flake.lock` "
                       "within the top-level flake.",
        .category = category,
        .labels = {"flake-lock-path"},
        .handler = {[&](const std::string &lockFilePath) -> void {
            lockFlags.referenceLockFilePath = {
                nix::getFSSourceAccessor(),
                nix::CanonPath(nix::absPath(lockFilePath))};
        }},
        .completer = completePath,
        .experimentalFeature = std::nullopt,
    });

    expectArg("expr", &releaseExpr);
}

void MyArgs::parseArgs(char **argv, int argc) {
    parseCmdline(nix::argvToStrings(argc, argv), false);
}
