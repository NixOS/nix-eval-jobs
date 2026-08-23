// doesn't exist on macOS
// IWYU pragma: no_include <bits/types/struct_rusage.h>

#include <nix/expr/eval-error.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/terminal.hh>
#include <nix/expr/attr-path.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/store/globals.hh>
#include <nix/cmd/installable-flake.hh>
#include <nix/expr/value-to-json.hh>
#include <sys/resource.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <iostream>
// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants this header rather than the C++ version
#include <stdlib.h>
// NOLINTEND(modernize-deprecated-headers)
#include <exception>
#include <filesystem>
#include <nix/expr/attr-set.hh>
#include <nix/cmd/common-eval-args.hh>
#include <nix/util/error.hh>
#include <nix/expr/eval.hh>
#include <nix/util/file-system.hh>
#include <nix/fetchers/attrs.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/flake.hh>
#include <nix/expr/get-drvs.hh>
#include <nix/util/logging.hh>
#include <nix/store/outputs-spec.hh>
#include <nix/util/ref.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/util/types.hh>
#include <nix/util/util.hh>
#include <nix/expr/value.hh>
#include <nix/expr/value/context.hh>
#include <nlohmann/json_fwd.hpp>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "worker.hh"
#include "drv.hh"
#include "response.hh"
#include "buffered-io.hh"
#include "eval-args.hh"
#include "store.hh"

namespace nix {
struct Expr;
} // namespace nix

namespace {

/* Render and print to STDERR. this is what's shown in the Hydra UI. */
auto showError(const nix::ErrorInfo &err) -> std::string {
    std::ostringstream oss;
    nix::showErrorInfo(oss, err, nix::loggerSettings.showTrace.get());
    auto msg = oss.str();
    std::cerr << msg << "\n";
    return msg;
}

auto releaseExprTopLevelValue(nix::EvalState &state,
                              const nix::Bindings &autoArgs, MyArgs &args)
    -> nix::Value * {
    nix::Value vTop;

    if (args.fromArgs) {
        nix::Expr *expr =
            state.parseExprFromString(args.releaseExpr, state.rootPath("."));
        state.eval(expr, vTop);
    } else {
        state.evalFile(lookupFileArg(state, args.releaseExpr), vTop);
    }

    auto *vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

auto evaluateFlake(const nix::ref<nix::EvalState> &state, const MyArgs &args)
    -> nix::Value * {
    auto [flakeRef, fragment, outputSpec] =
        nix::parseFlakeRefWithFragmentAndExtendedOutputsSpec(
            args.releaseExpr, nix::absPath(std::filesystem::path(".")));
    const auto &lockFlags = args.lockFlags;

    /* The collector passes the flakeref it already locked and fetched:
       final + narHash lets the fetcher skip the fetch lock entirely
       (issue #432). */
    if (!args.lockedFlakeAttrs.empty()) {
        flakeRef = nix::FlakeRef::fromAttrs(nix::fetchers::jsonToAttrs(
            nlohmann::json::parse(args.lockedFlakeAttrs)));
    }

    nix::InstallableFlake flake{{},       state,      std::move(flakeRef),
                                fragment, outputSpec, {},
                                {},       lockFlags};

    // If no fragment specified, use callFlake to get the full flake structure
    // (just like :lf in the REPL)
    if (fragment.empty()) {
        auto *value = state->allocValue();
        nix::flake::callFlake(*state, *flake.getLockedFlake(), *value);
        return value;
    }
    // Fragment specified, use normal evaluation
    return flake.toValue(*state, nix::AutoCall::No).first;
}

auto attrPathJoin(nlohmann::json input) -> std::string {
    return std::accumulate(
        input.begin(), input.end(), std::string(),
        [](const std::string &acc, std::string str) -> std::basic_string<char> {
            // Escape token if containing dots
            if (str.find('.') != std::string::npos) {
                str = "\"" + str + "\"";
            }
            return acc.empty() ? str : acc + "." + str;
        });
}

auto extractConstituents(nix::EvalState &state, nix::Value *value,
                         const MyArgs &args) -> Constituents {
    if (!args.constituents) {
        return {};
    }

    std::vector<std::string> constituents;
    std::vector<std::string> namedConstituents;
    bool globConstituents = false;

    const auto *aggregateAttr =
        value->attrs()->get(state.symbols.create("_hydraAggregate"));

    if (aggregateAttr != nullptr &&
        state.forceBool(*aggregateAttr->value, aggregateAttr->pos,
                        "while evaluating the `_hydraAggregate` attribute")) {

        const auto *constituentsAttr =
            value->attrs()->get(state.symbols.create("constituents"));

        if (constituentsAttr == nullptr) {
            state
                .error<nix::EvalError>(
                    "derivation must have a 'constituents' attribute")
                .debugThrow();
        }

        // Extract constituent paths from context
        nix::NixStringContext context;
        state.coerceToString(
            constituentsAttr->pos, *constituentsAttr->value, context,
            "while evaluating the `constituents` attribute", true, false);

        for (const auto &ctx : context) {
            std::visit(
                nix::overloaded{
                    [&](const nix::NixStringContextElem::Built &built) -> void {
                        constituents.push_back(
                            built.drvPath->to_string(*state.store));
                    },
                    [&](const nix::NixStringContextElem::Opaque &opaque
                        [[maybe_unused]]) -> void {},
                    [&](const nix::NixStringContextElem::DrvDeep &drvDeep
                        [[maybe_unused]]) -> void {},
                },
                ctx.raw);
        }

        // Extract named constituents
        state.forceList(*constituentsAttr->value, constituentsAttr->pos,
                        "while evaluating the `constituents` attribute");
        auto constituentsList = constituentsAttr->value->listView();

        for (const auto &val : constituentsList) {
            state.forceValue(*val, nix::noPos);
            if (val->type() == nix::nString) {
                namedConstituents.emplace_back(val->c_str());
            }
        }

        // Check for glob constituents
        const auto *glob =
            value->attrs()->get(state.symbols.create("_hydraGlobConstituents"));
        globConstituents =
            glob != nullptr &&
            state.forceBool(
                *glob->value, glob->pos,
                "while evaluating the `_hydraGlobConstituents` attribute");
    }

    return Constituents{
        .constituents = std::move(constituents),
        .namedConstituents = std::move(namedConstituents),
        .globConstituents = globConstituents,
    };
}

auto applyExprToValue(nix::EvalState &state, nix::Value *value,
                      const std::string &applyExpr) -> nlohmann::json {
    if (applyExpr.empty()) {
        return nlohmann::json{};
    }

    auto *expr = state.parseExprFromString(applyExpr, state.rootPath("."));

    nix::Value vApply;
    nix::Value vRes;

    state.eval(expr, vApply);
    state.callFunction(vApply, *value, vRes, nix::noPos);
    state.forceAttrs(vRes, nix::noPos, "apply needs to evaluate to an attrset");

    nix::NixStringContext context;
    std::stringstream stream;
    nix::printValueAsJSON(state, true, vRes, nix::noPos, stream, context);

    return nlohmann::json::parse(stream.str());
}

auto registerGCRoot(nix::EvalState &state, const Drv &drv, const MyArgs &args)
    -> void {
    if (args.gcRootsDir.empty() || nix::settings.readOnlyMode) {
        return;
    }

    const std::filesystem::path root =
        args.gcRootsDir / std::string(drv.drvPath.to_string());

    if (!nix::pathExists(root)) {
        auto localStore = state.store.dynamic_pointer_cast<nix::LocalFSStore>();
        if (localStore) {
            localStore->addPermRoot(drv.drvPath, root);
        }
        // If not a local store, we can't create GC roots
    }
}

auto collectAttrsForRecursion(nix::EvalState &state, nix::Value *value,
                              const nlohmann::json &path, const MyArgs &args)
    -> std::vector<std::string> {
    std::vector<std::string> attrs;
    bool recurse =
        args.forceRecurse ||
        path.empty(); // Don't require recurseForDerivations for top-level

    for (auto &attr : value->attrs()->lexicographicOrder(state.symbols)) {
        const std::string_view &name = state.symbols[attr->name];
        attrs.emplace_back(name);

        if (!args.forceRecurse && name == "recurseForDerivations") {
            const auto *attrv =
                value->attrs()->get(nix::EvalState::s.recurseForDerivations);
            recurse = state.forceBool(*attrv->value, attrv->pos,
                                      "while evaluating recurseForDerivations");
        }
    }

    return recurse ? attrs : std::vector<std::string>{};
}

auto processDerivation(nix::EvalState &state, nix::Value *value,
                       std::string &attrPathS, const nlohmann::json &path,
                       MyArgs &args) -> Response::Payload {
    auto packageInfo = nix::getDerivation(state, *value, false);
    if (!packageInfo) {
        auto attrs = collectAttrsForRecursion(state, value, path, args);
        return Response::Attrs{std::move(attrs)};
    }

    // Extract constituents if enabled
    auto constituents = extractConstituents(state, value, args);

    // Apply expression if provided
    std::optional<nlohmann::json> extraValue;
    if (!args.applyExpr.empty()) {
        extraValue = applyExprToValue(state, value, args.applyExpr);
    }

    // Create derivation info
    auto drv = Drv::fromPackageInfo(attrPathS, state, *packageInfo, args,
                                    std::move(constituents));

    // Register GC root
    registerGCRoot(state, drv, args);

    return Response::Job{std::move(drv), std::move(extraValue)};
}

auto initializeRootValue(const nix::ref<nix::EvalState> &state,
                         const nix::Bindings &autoArgs, MyArgs &args)
    -> nix::Value * {
    nix::Value *vEvaluated =
        args.flake ? evaluateFlake(state, args)
                   : releaseExprTopLevelValue(*state, autoArgs, args);

    if (args.selectExpr.empty()) {
        return vEvaluated;
    }

    // Apply the provided select function
    auto *selectExpr =
        state->parseExprFromString(args.selectExpr, state->rootPath("."));

    nix::Value vSelect;
    state->eval(selectExpr, vSelect);

    nix::Value *vSelected = state->allocValue();
    state->callFunction(vSelect, *vEvaluated, *vSelected, nix::noPos);
    state->forceAttrs(
        *vSelected, nix::noPos,
        "'--select' must evaluate to an attrset (the traversal root)");

    return vSelected;
}

auto shouldRestart(const MyArgs &args) -> bool {
    struct rusage resourceUsage = {}; // NOLINT(misc-include-cleaner)
    getrusage(RUSAGE_SELF, &resourceUsage);
    size_t maxrss =
        resourceUsage
            .ru_maxrss; // NOLINT(cppcoreguidelines-pro-type-union-access)
    static constexpr size_t KB_TO_BYTES = 1024;
#ifdef __APPLE__
    maxrss /= KB_TO_BYTES; // ru_maxrss is bytes on macOS instead of KiB
#endif
    return maxrss > args.maxMemorySize * KB_TO_BYTES;
}

auto processJobRequest(nix::EvalState &state, LineReader &fromReader,
                       nix::AutoCloseFD &toParent,
                       const nix::Bindings &autoArgs, nix::Value *vRoot,
                       MyArgs &args) -> bool {
    /* Wait for the collector to send us a job name. */
    if (tryWriteLine(toParent.get(), std::string(MSG_NEXT)) < 0) {
        return false; // main process died
    }

    auto line = fromReader.readLine();
    if (line == MSG_EXIT) {
        return false;
    }

    if (!nix::hasPrefix(line, MSG_DO)) {
        std::cerr << "worker error: received invalid command '" << line
                  << "'\n";
        abort();
    }

    auto path = nlohmann::json::parse(line.substr(MSG_DO.size()));
    auto attrPathS = attrPathJoin(path);

    /* Evaluate it and send info back to the collector. */
    Response::Payload payload = [&]() -> Response::Payload {
        try {
            auto *vTmp =
                nix::findAlongAttrPath(state, attrPathS, autoArgs, *vRoot)
                    .first;

            auto *value = state.allocValue();
            state.autoCallFunction(autoArgs, *vTmp, *value);

            if (value->type() == nix::nAttrs) {
                return processDerivation(state, value, attrPathS, path, args);
            }
            // We ignore everything that cannot be built
            return Response::Attrs{{}};
        } catch (nix::StackOverflowError &e) {
            /* Not an EvalError. Fatal aborts the whole evaluation. */
            auto msg = showError(e.info());
            return Response::Error{
                .error = nix::filterANSIEscapes(msg, true),
                .fatal = true,
            };
        } catch (nix::EvalError &e) {
            auto msg = showError(e.info());
            return Response::Error{nix::filterANSIEscapes(msg, true)};
        } catch (const std::exception &e) {
            const auto *msg = e.what();
            std::cerr << msg << '\n';
            return Response::Error{nix::filterANSIEscapes(msg, true)};
        }
    }();

    Response const response{
        .attr = attrPathS,
        .attrPath = path.get<std::vector<std::string>>(),
        .payload = std::move(payload),
    };
    nlohmann::json const reply = response;
    if (tryWriteLine(toParent.get(), reply.dump()) < 0) {
        return false; // main process died
    }

    /* Check if we should restart due to memory usage */
    return !shouldRestart(args);
}

} // namespace

auto prefetchFlake(MyArgs &args) -> std::optional<std::string> {
    auto evalStore = nix_eval_jobs::openStore(args.evalStoreUrl);
    auto state = nix::make_ref<nix::EvalState>(
        args.lookupPath, evalStore, nix::fetchSettings, nix::evalSettings);
    auto [flakeRef, fragment, outputSpec] =
        nix::parseFlakeRefWithFragmentAndExtendedOutputsSpec(
            args.releaseExpr, nix::absPath(std::filesystem::path(".")));
    auto locked = nix::flake::lockFlake(nix::flakeSettings, *state, flakeRef,
                                        args.lockFlags);

    auto lockedRef = locked.flake.lockedRef;
    if (!lockedRef.input.isFinal() || !lockedRef.input.getNarHash()) {
        return std::nullopt;
    }
    lockedRef.input =
        lockedRef.input.fetchToStore(nix::fetchSettings, *state->store).second;
    return nix::fetchers::attrsToJSON(lockedRef.toAttrs()).dump();
}

void worker(
    MyArgs &args,
    nix::AutoCloseFD &toParent, // NOLINT(bugprone-easily-swappable-parameters)
    nix::AutoCloseFD &fromParent) {

    LineReader fromReader(fromParent.release());

    auto config = nlohmann::json::parse(fromReader.readLine());
    args.lockedFlakeAttrs = config.value("lockedFlake", "");

    auto evalStore = nix_eval_jobs::openStore(args.evalStoreUrl);
    auto state = nix::make_ref<nix::EvalState>(
        args.lookupPath, evalStore, nix::fetchSettings, nix::evalSettings);
    const nix::Bindings &autoArgs = *args.getAutoArgs(*state);

    nix::Value *vRoot = initializeRootValue(state, autoArgs, args);

    while (processJobRequest(*state, fromReader, toParent, autoArgs, vRoot,
                             args)) {
        // Continue processing jobs until we need to exit
    }

    if (tryWriteLine(toParent.get(), std::string(MSG_RESTART)) < 0) {
        return; // main process died
    };
}
