
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

// nix::Args::Flag has many default-valued fields we intentionally omit.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifdef __clang__
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif

MyArgs::MyArgs() : MixCommonArgs("nix-eval-jobs") {
    addFlag({
        .longName = "help",
        .description = "show usage information",
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
    });

    addFlag({
        .longName = "impure",
        .description = "allow impure expressions",
        .handler = {&impure, true},
    });

    addFlag({
        .longName = "force-recurse",
        .description = "force recursion (don't respect recurseIntoAttrs)",
        .handler = {&forceRecurse, true},
    });

    addFlag({
        .longName = "gc-roots-dir",
        .description = "garbage collector roots directory",
        .labels = {"path"},
        .handler = {&gcRootsDir},
    });

    addFlag({
        .longName = "workers",
        .description = "number of evaluate workers",
        .labels = {"workers"},
        .handler = {[this](const std::string &str) -> void {
            nrWorkers = std::stoi(str);
        }},
    });

    addFlag({
        .longName = "max-memory-size",
        .description =
            "memory per worker in MiB (4GiB by default). workers * "
            "max-memory-size is enforced as a budget for all workers "
            "combined: jobs are only dispatched while it has room, a "
            "worker above its share is restarted after its job, and if "
            "the budget is exceeded anyway the largest worker is killed "
            "and its job retried alone.",
        .labels = {"size"},
        .handler = {[this](const std::string &str) -> void {
            maxMemorySize = std::stoi(str);
        }},
    });

    addFlag({
        .longName = "flake",
        .description = "build a flake",
        .handler = {&flake, true},
    });

    addFlag({
        .longName = "meta",
        .description = "include derivation meta field in output",
        .handler = {&meta, true},
    });

    addFlag({
        .longName = "constituents",
        .description =
            "whether to evaluate constituents for Hydra's aggregate feature",
        .handler = {&constituents, true},
    });

    addFlag({
        .longName = "check-cache-status",
        .description = "Check if the derivations are present locally or in "
                       "any configured substituters (i.e. binary cache). The "
                       "information will be exposed in the `cacheStatus` field "
                       "of the JSON output.",
        .handler = {&checkCacheStatus, true},
    });

    addFlag({
        .longName = "show-input-drvs",
        .description =
            "Show input derivations in the output for each derivation. "
            "This is useful to get direct dependencies of a derivation.",
        .handler = {&showInputDrvs, true},
    });

    addFlag({
        .longName = "show-trace",
        .description = "print out a stack trace in case of evaluation errors",
        .handler = {&showTrace, true},
    });

    addFlag({
        .longName = "no-instantiate",
        .description =
            "don't instantiate (write) derivations, only evaluate (faster)",
        .handler = {&noInstantiate, true},
    });

    addFlag({
        .longName = "expr",
        .shortName = 'E',
        .description = "treat the argument as a Nix expression",
        .handler = {&fromArgs, true},
    });

    addFlag({
        .longName = "apply",
        .description =
            "Apply provided Nix function to each derivation. "
            "The result of this function will be serialized as a JSON value "
            "and stored inside `\"extraValue\"` key of the json line output.",
        .labels = {"expr"},
        .handler = {&applyExpr},
    });

    addFlag({
        .longName = "select",
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
        .labels = {"expr"},
        .handler = {&selectExpr},
    });

    // usually in MixFlakeOptions
    addFlag({
        .longName = "override-input",
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
    });

    addFlag({
        .longName = "reference-lock-file",
        .description = "Read the given lock file instead of `flake.lock` "
                       "within the top-level flake.",
        .category = category,
        .labels = {"flake-lock-path"},
        .handler = {[&](const std::string &lockFilePath) -> void {
            lockFlags.referenceLockFilePath = {
                nix::getFSSourceAccessor(),
                nix::CanonPath(nix::absPath(lockFilePath).string()),
            };
        }},
        .completer = completePath,
    });

    const std::string internalCategory = "Internal flags";
    addFlag({
        .longName = "worker",
        .description = "run as an evaluation worker process",
        .category = internalCategory,
        .handler = {&runAsWorker, true},
    });
    hiddenCategories.insert(internalCategory);

    expectArg("expr", &releaseExpr);
}

#pragma GCC diagnostic pop

void MyArgs::parseArgs(char **argv, int argc) {
    parseCmdline(nix::argvToStrings(argc, argv), false);
}
