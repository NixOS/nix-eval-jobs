#include <nix/store/path-with-outputs.hh>
#include <nix/store/store-api.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/store/globals.hh>
#include <nix/expr/value-to-json.hh>
#include <nix/store/derivations.hh>
#include <nix/store/derivation-options.hh>
#include <nix/expr/get-drvs.hh>
#include <nix/store/derived-path-map.hh>
#include <nix/expr/eval.hh>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <nix/store/path.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/util/ref.hh>
#include <nix/expr/value/context.hh>
#include <nix/util/error.hh>
#include <nix/expr/eval-error.hh>
#include <nix/util/experimental-features.hh>
#include <nix/util/configuration.hh> // for experimentalFeatureSettings
// required for std::optional
#include <nix/util/json-utils.hh> //NOLINT(misc-include-cleaner)
#include <nix/util/pos-idx.hh>
#include <nix/util/util.hh> // for get()
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include "drv.hh"
#include "eval-args.hh"

namespace {

auto queryOutputs(nix::PackageInfo &packageInfo, nix::EvalState &state,
                  const std::string &attrPath)
    -> std::map<std::string, std::optional<nix::StorePath>> {
    std::map<std::string, std::optional<nix::StorePath>> outputs;

    try {
        nix::PackageInfo::Outputs outputsQueried;

        // CA derivations do not have static output paths, so we have to
        // fallback if we encounter an error
        try {
            outputsQueried = packageInfo.queryOutputs(true);
        } catch (const nix::Error &e) {
            // Handle CA derivation errors
            if (!nix::experimentalFeatureSettings.isEnabled(
                    nix::Xp::CaDerivations)) {
                throw;
            }
            outputsQueried = packageInfo.queryOutputs(false);
        }
        for (auto &[outputName, optOutputPath] : outputsQueried) {
            outputs[outputName] = optOutputPath;
        }
    } catch (const std::exception &e) {
        state
            .error<nix::EvalError>(
                "derivation '%s' does not have valid outputs: %s", attrPath,
                e.what())
            .debugThrow();
    }

    return outputs;
}

auto queryMeta(nix::PackageInfo &packageInfo, nix::EvalState &state)
    -> std::optional<nlohmann::json> {
    nlohmann::json meta_;
    for (const auto &metaName : packageInfo.queryMetaNames()) {
        nix::NixStringContext context;
        std::stringstream stream;

        auto *metaValue = packageInfo.queryMeta(metaName);
        // Skip non-serialisable types
        if (metaValue == nullptr) {
            continue;
        }

        nix::printValueAsJSON(state, true, *metaValue, nix::noPos, stream,
                              context);

        meta_[metaName] = nlohmann::json::parse(stream.str());
    }
    return meta_;
}

auto queryInputDrvs(const nix::Derivation &drv)
    -> std::map<nix::StorePath, std::set<std::string>> {
    std::map<nix::StorePath, std::set<std::string>> drvs;
    for (const auto &[inputDrvPath, inputNode] : drv.inputDrvs.map) {
        std::set<std::string> inputDrvOutputs;
        for (const auto &outputName : inputNode.value) {
            inputDrvOutputs.insert(outputName);
        }
        drvs.emplace(inputDrvPath, std::move(inputDrvOutputs));
    }
    return drvs;
}

auto queryCacheStatus(
    nix::Store &store,
    std::map<std::string, std::optional<nix::StorePath>> &outputs,
    nix::StorePaths &neededBuilds,
    std::vector<nix::StorePath> &neededSubstitutes,
    std::vector<nix::StorePath> &unknownPaths, const nix::Derivation &drv)
    -> Drv::CacheStatus {

    // queryMissing's per-input traversal flags an FOD as willBuild whenever a
    // build-time-only input is missing from substituters, even though `nix
    // build` would just download the FOD's own output. Short-circuit when
    // every output is already obtainable; fall through otherwise so the
    // queryMissing breakdown still populates neededBuilds for cross-system
    // jobs (issue #369).
    nix::StorePathCAMap toQuery;
    bool allOutputsKnown = true;
    for (auto const &[_, val] : outputs) {
        if (!val) {
            allOutputsKnown = false;
            break;
        }
        if (!store.isValidPath(*val)) {
            toQuery.insert({*val, std::nullopt});
        }
    }
    if (allOutputsKnown) {
        nix::SubstitutablePathInfos infos;
        if (!toQuery.empty()) {
            store.querySubstitutablePathInfos(toQuery, infos);
        }
        if (infos.size() == toQuery.size()) {
            for (auto const &[p, _] : infos) {
                neededSubstitutes.push_back(p);
            }
            std::ranges::sort(
                neededSubstitutes,
                [](const nix::StorePath &a, const nix::StorePath &b) {
                    return a.name() != b.name() ? a.name() < b.name()
                                                : a.to_string() < b.to_string();
                });
            return infos.empty() ? Drv::CacheStatus::Local
                                 : Drv::CacheStatus::Cached;
        }
    }

    std::vector<nix::StorePathWithOutputs> paths;
    for (auto const &[key, val] : outputs) {
        if (val) {
            paths.push_back(nix::StorePathWithOutputs{*val, {}});
        }
    }
    // Input drvs go in too so neededBuilds is populated for cross-system jobs
    // whose own output paths are unknown (issue #369).
    for (const auto &[inputDrvPath, inputNode] : drv.inputDrvs.map) {
        paths.push_back(
            nix::StorePathWithOutputs(inputDrvPath, inputNode.value));
    }

    auto missing = store.queryMissing(toDerivedPaths(paths));

    if (!missing.willBuild.empty()) {
        // TODO: can we expose the topological sort order as a graph?
        auto sorted = store.topoSortPaths(missing.willBuild);
        std::ranges::reverse(sorted.begin(), sorted.end());
        for (auto &path : sorted) {
            neededBuilds.push_back(std::move(path));
        }
    }
    if (!missing.willSubstitute.empty()) {
        std::vector<const nix::StorePath *> willSubstituteSorted = {};
        std::ranges::for_each(missing.willSubstitute.begin(),
                              missing.willSubstitute.end(),
                              [&](const nix::StorePath &path) -> void {
                                  willSubstituteSorted.push_back(&path);
                              });
        std::ranges::sort(
            willSubstituteSorted.begin(), willSubstituteSorted.end(),
            [](const nix::StorePath *lhs, const nix::StorePath *rhs) -> bool {
                if (lhs->name() == rhs->name()) {
                    return lhs->to_string() < rhs->to_string();
                }
                return lhs->name() < rhs->name();
            });
        for (const auto *path : willSubstituteSorted) {
            neededSubstitutes.push_back(*path);
        }
    }

    if (!missing.unknown.empty()) {
        for (const auto &path : missing.unknown) {
            unknownPaths.push_back(path);
        }
    }

    if (missing.willBuild.empty() && missing.unknown.empty()) {
        if (missing.willSubstitute.empty()) {
            // cacheStatus is Local if:
            //  - there's nothing to build
            //  - there's nothing to substitute
            return Drv::CacheStatus::Local;
        }
        // cacheStatus is Cached if:
        //  - there's nothing to build
        //  - there are paths to substitute
        return Drv::CacheStatus::Cached;
    }
    return Drv::CacheStatus::NotBuilt;
};

} // namespace

/* The fields of a derivation that are printed in json form */
auto Drv::fromPackageInfo(std::string &attrPath, nix::EvalState &state,
                          nix::PackageInfo &packageInfo, MyArgs &args,
                          Constituents constituents) -> Drv {
    auto store = state.store;

    Drv result{
        .name = packageInfo.queryName(),
        .storeDir = store->storeDir,
        .system = {},
        .drvPath = packageInfo.requireDrvPath(),
        .outputs = queryOutputs(packageInfo, state, attrPath),
        .neededBuilds = {},
        .neededSubstitutes = {},
        .unknownPaths = {},
        .constituents = std::move(constituents),
    };

    // Check if we can read derivations (requires LocalFSStore and not in
    // read-only mode)
    auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>();
    const bool canReadDerivation = localStore && !nix::settings.readOnlyMode;

    if (canReadDerivation) {
        // We can read the derivation directly for precise information
        auto drv = localStore->readDerivation(packageInfo.requireDrvPath());

        // Use the more precise system from the derivation
        result.system = drv.platform;

        if (args.checkCacheStatus) {
            // TODO: is this a bottleneck, where we should batch these queries?
            result.cacheStatus = queryCacheStatus(
                *store, result.outputs, result.neededBuilds,
                result.neededSubstitutes, result.unknownPaths, drv);
        }

        if (args.showInputDrvs) {
            result.inputDrvs = queryInputDrvs(drv);
        }

        auto drvOptions = derivationOptionsFromStructuredAttrs(
            *store, drv.env, get(drv.structuredAttrs));
        result.requiredSystemFeatures =
            std::optional(drvOptions.getRequiredSystemFeatures(drv));
    } else {
        // Fall back to basic info from PackageInfo
        // This happens when:
        // - In read-only/no-instantiate mode
        // - Store is not a LocalFSStore (e.g., remote store)
        result.system = packageInfo.querySystem();
    }

    // Handle metadata (works in both modes)
    if (args.meta) {
        result.meta = queryMeta(packageInfo, state);
    }

    return result;
}

namespace nlohmann {

using nix::get;
using nix::getBoolean;
using nix::getObject;
using nix::getString;
using nix::valueAt;

void adl_serializer<Constituents>::to_json(json &res, const Constituents &val) {
    res = json{
        {"constituents", val.constituents},
        {"namedConstituents", val.namedConstituents},
        {"globConstituents", val.globConstituents},
    };
}

auto adl_serializer<Constituents>::from_json(const json &_json)
    -> Constituents {
    const auto &json = getObject(_json);

    Constituents result;
    if (const auto *constituents = get(json, "constituents")) {
        result.constituents = *constituents;
    }
    if (const auto *named = get(json, "namedConstituents")) {
        result.namedConstituents = *named;
    }
    if (const auto *glob = get(json, "globConstituents")) {
        result.globConstituents = getBoolean(*glob);
    }
    return result;
}

void adl_serializer<Drv>::to_json(json &res, const Drv &drv) {
    nix::StoreDirConfig const storeDirConfig{drv.storeDir};

    json outputs;
    for (const auto &[name, optPath] : drv.outputs) {
        if (optPath) {
            outputs[name] = storeDirConfig.printStorePath(*optPath);
        } else {
            outputs[name] = nullptr;
        }
    }

    res = json{
        {"name", drv.name},
        {"storeDir", drv.storeDir},
        {"system", drv.system},
        {"drvPath", storeDirConfig.printStorePath(drv.drvPath)},
        {"outputs", std::move(outputs)},
    };

    if (drv.meta.has_value()) {
        res["meta"] = drv.meta.value();
    }
    if (drv.inputDrvs) {
        json inputDrvs = json::object();
        for (const auto &[path, outputNames] : *drv.inputDrvs) {
            inputDrvs[storeDirConfig.printStorePath(path)] = outputNames;
        }
        res["inputDrvs"] = std::move(inputDrvs);
    }

    if (drv.requiredSystemFeatures) {
        res["requiredSystemFeatures"] = drv.requiredSystemFeatures.value();
    }

    res.update(json(drv.constituents));

    if (drv.cacheStatus != Drv::CacheStatus::Unknown) {
        // Deprecated field
        res["isCached"] = drv.cacheStatus == Drv::CacheStatus::Cached ||
                          drv.cacheStatus == Drv::CacheStatus::Local;

        switch (drv.cacheStatus) {
        case Drv::CacheStatus::Cached:
            res["cacheStatus"] = "cached";
            break;
        case Drv::CacheStatus::Local:
            res["cacheStatus"] = "local";
            break;
        default:
            res["cacheStatus"] = "notBuilt";
            break;
        }
        res["neededBuilds"] = json::array();
        for (const auto &path : drv.neededBuilds) {
            res["neededBuilds"].push_back(storeDirConfig.printStorePath(path));
        }
        res["neededSubstitutes"] = json::array();
        for (const auto &path : drv.neededSubstitutes) {
            res["neededSubstitutes"].push_back(
                storeDirConfig.printStorePath(path));
        }
        // TODO: is it useful to include "unknown" paths at all?
        // res["unknown"] = drv.unknownPaths;
    }
}

auto adl_serializer<Drv>::from_json(const json &_json) -> Drv {
    const auto &json = getObject(_json);

    auto storeDir = getString(valueAt(json, "storeDir"));
    nix::StoreDirConfig const storeDirConfig{storeDir};

    std::map<std::string, std::optional<nix::StorePath>> outputs;
    for (const auto &[name, val] : getObject(valueAt(json, "outputs"))) {
        if (val.is_null()) {
            outputs.emplace(name, std::nullopt);
        } else {
            outputs.emplace(name,
                            storeDirConfig.parseStorePath(getString(val)));
        }
    }

    Drv drv{
        .name = getString(valueAt(json, "name")),
        .storeDir = storeDir,
        .system = getString(valueAt(json, "system")),
        .drvPath =
            storeDirConfig.parseStorePath(getString(valueAt(json, "drvPath"))),
        .outputs = std::move(outputs),
        .neededBuilds = {},
        .neededSubstitutes = {},
        .unknownPaths = {},
        .constituents = {},
    };

    if (const auto *meta = get(json, "meta")) {
        drv.meta = *meta;
    }
    if (const auto *inputDrvsJson = get(json, "inputDrvs")) {
        std::map<nix::StorePath, std::set<std::string>> inputDrvs;
        for (const auto &[pathStr, outputNames] : getObject(*inputDrvsJson)) {
            inputDrvs.emplace(storeDirConfig.parseStorePath(pathStr),
                              outputNames.get<std::set<std::string>>());
        }
        drv.inputDrvs = std::move(inputDrvs);
    }
    if (const auto *rsf = get(json, "requiredSystemFeatures")) {
        drv.requiredSystemFeatures = *rsf;
    }

    drv.constituents = adl_serializer<Constituents>::from_json(_json);

    if (const auto *cacheStatus = get(json, "cacheStatus")) {
        auto status = getString(*cacheStatus);
        if (status == "cached") {
            drv.cacheStatus = Drv::CacheStatus::Cached;
        } else if (status == "local") {
            drv.cacheStatus = Drv::CacheStatus::Local;
        } else {
            drv.cacheStatus = Drv::CacheStatus::NotBuilt;
        }

        if (const auto *neededBuilds = get(json, "neededBuilds")) {
            for (const auto &path : *neededBuilds) {
                drv.neededBuilds.push_back(
                    storeDirConfig.parseStorePath(getString(path)));
            }
        }
        if (const auto *neededSubstitutes = get(json, "neededSubstitutes")) {
            for (const auto &path : *neededSubstitutes) {
                drv.neededSubstitutes.push_back(
                    storeDirConfig.parseStorePath(getString(path)));
            }
        }
    }

    return drv;
}

} // namespace nlohmann
