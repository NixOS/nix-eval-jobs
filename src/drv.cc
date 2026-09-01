#include <nix/store/path-info.hh>
#include <nix/store/path-with-outputs.hh>
#include <nix/store/store-api.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/store/remote-store.hh>
#include <atomic>
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

    /* Skip stores that would read the .drv over the wire. The daemon
       (UDSRemoteStore) is a LocalFSStore and reads locally. A store that
       fails to read is remembered. */
    static std::atomic<bool> storeReadsDrvs = true;
    const bool overTheWire = store.dynamic_pointer_cast<nix::RemoteStore>() &&
                             !store.dynamic_pointer_cast<nix::LocalFSStore>();
    const bool tryReadDerivation =
        !nix::settings.readOnlyMode && storeReadsDrvs && !overTheWire;

    bool precise = false;
    if (tryReadDerivation) {
        try {
            auto drv = store->readDerivation(packageInfo.requireDrvPath());
            result.system = drv.platform;
            if (args.showInputDrvs) {
                result.inputDrvs = queryInputDrvs(drv);
            }
            auto drvOptions = derivationOptionsFromStructuredAttrs(
                *store, drv.env, get(drv.structuredAttrs));
            result.requiredSystemFeatures =
                std::optional(drvOptions.getRequiredSystemFeatures(drv));
            precise = true;
        } catch (nix::Error &) {
            storeReadsDrvs = false;
        }
    }
    if (!precise) {
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
