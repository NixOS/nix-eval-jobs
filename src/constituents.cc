#include <filesystem>
#include <fnmatch.h>
#include <functional>
#include <map>
#include <nix/store/derivations.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/store/path.hh>
#include <nix/util/error.hh>
#include <nix/util/file-system.hh>
#include <nix/util/fmt.hh>
#include <nix/util/logging.hh>
#include <nix/util/ref.hh>
#include <nix/util/types.hh>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "constituents.hh"
#include "output-stream-lock.hh"

namespace {
// This is copied from `libutil/topo-sort.hh` in Nix and slightly modified.
// However, I needed a way to use strings as identifiers to sort, but still be
// able to put AggregateJob objects into this function since I'd rather not have
// to transform back and forth between a list of strings and AggregateJobs in
// resolveNamedConstituents.
auto topoSort(const std::set<AggregateJob> &items)
    -> std::vector<AggregateJob> {
    std::vector<AggregateJob> sorted;
    std::set<std::string> visited;
    std::set<std::string> parents;

    std::map<std::string, AggregateJob> dictIdentToObject;
    for (const auto &item : items) {
        dictIdentToObject.insert({item.name, item});
    }

    std::function<void(const std::string &path, const std::string *parent)>
        dfsVisit;

    dfsVisit = [&](const std::string &path, const std::string *parent) -> void {
        if (parents.contains(path)) {
            dictIdentToObject.erase(path);
            dictIdentToObject.erase(*parent);
            std::set<std::string> remaining;
            for (auto &[key, value] : dictIdentToObject) {
                remaining.insert(key);
            }
            throw DependencyCycle(path, *parent, remaining);
        }

        if (!visited.insert(path).second) {
            return;
        }
        parents.insert(path);

        const std::set<std::string> references =
            dictIdentToObject[path].dependencies;

        for (const auto &ref : references) {
            /* Don't traverse into items that don't exist in our starting set.
             */
            if (ref != path && dictIdentToObject.contains(ref)) {
                dfsVisit(ref, &path);
            }
        }

        sorted.push_back(dictIdentToObject[path]);
        parents.erase(path);
    };

    for (auto &[key, value] : dictIdentToObject) {
        dfsVisit(key, nullptr);
    }

    return sorted;
}

auto insertMatchingConstituents(
    const std::string &childJobName, const std::string &jobName,
    const std::function<bool(const std::string &, const nlohmann::json &)>
        &isBroken,
    const std::map<std::string, nlohmann::json> &jobs,
    std::set<std::string> &results) -> bool {
    bool expansionFound = false;
    for (const auto &[currentJobName, job] : jobs) {
        // Never select the job itself as constituent. Trivial way
        // to avoid obvious cycles.
        if (currentJobName == jobName) {
            continue;
        }
        auto jobName = currentJobName;
        if (fnmatch(childJobName.c_str(), jobName.c_str(), 0) == 0 &&
            !isBroken(jobName, job)) {
            results.insert(jobName);
            expansionFound = true;
        }
    }

    return expansionFound;
}
} // namespace

namespace {
void addConstituents(nlohmann::json &job, nix::Derivation &drv,
                     const std::set<std::string> &dependencies,
                     const std::map<std::string, nlohmann::json> &jobs,
                     const nix::ref<nix::Store> &store) {
    for (const auto &childJobName : dependencies) {
        auto childDrvPath = store->parseStorePath(
            std::string(jobs.find(childJobName)->second["drvPath"]));
        auto childDrv = store->readDerivation(childDrvPath);
        job["constituents"].push_back(store->printStorePath(childDrvPath));
        drv.inputDrvs.map[childDrvPath].value = {
            childDrv.outputs.begin()->first};
    }
}

void rewriteAndRegisterDrv(nlohmann::json &job, nix::Derivation &drv,
                           const nix::StorePath &drvPath,
                           const nix::ref<nix::Store> &store,
                           const std::filesystem::path &gcRootsDir) {
    // Reset outputs so fillInOutputPaths recomputes them
    // with the updated inputDrvs (constituents).
    for (auto &[name, output] : drv.outputs) {
        if (std::holds_alternative<nix::DerivationOutput::InputAddressed>(
                output.raw)) {
            output = nix::DerivationOutput::Deferred{};
            drv.env[name] = "";
        }
    }
    drv.fillInOutputPaths(*store);

    auto newDrvPath = store->writeDerivation(drv);
    if (newDrvPath == drvPath) {
        return;
    }

    auto newDrvPathS = store->printStorePath(newDrvPath);

    /* Only file system stores support gc roots. Stores with their own
       retention (e.g. an eval store plugin) keep the drv alive themselves. */
    auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>();
    if (!gcRootsDir.empty() && localStore) {
        const auto root =
            gcRootsDir / std::string(nix::baseNameOf(newDrvPathS));
        if (!nix::pathExists(root)) {
            localStore->addPermRoot(newDrvPath, root);
        }
    }

    nix::logger->log(nix::lvlDebug,
                     nix::fmt("rewrote aggregate derivation %s -> %s",
                              store->printStorePath(drvPath), newDrvPathS));

    job["drvPath"] = newDrvPathS;
    for (const auto &[name, output] : drv.outputs) {
        job["outputs"][name] = drv.env.at(name);
    }
}

void addBrokenJobsError(
    nlohmann::json &job,
    const std::unordered_map<std::string, std::string> &brokenJobs) {
    std::stringstream errorStream;
    for (const auto &[jobName, error] : brokenJobs) {
        errorStream << jobName << ": " << error << "\n";
    }
    job["error"] = errorStream.str();
}
} // namespace

auto resolveNamedConstituents(const std::map<std::string, nlohmann::json> &jobs)
    -> std::variant<std::vector<AggregateJob>, DependencyCycle> {
    std::set<AggregateJob> aggregateJobs;
    for (auto const &[jobName, job] : jobs) {
        auto named = job.find("namedConstituents");
        if (named != job.end() && !named->empty()) {
            const bool globConstituents =
                job.value<bool>("globConstituents", false);
            std::unordered_map<std::string, std::string> brokenJobs;
            std::set<std::string> results;

            auto isBroken = [&brokenJobs,
                             &jobName](const std::string &childJobName,
                                       const nlohmann::json &job) -> bool {
                if (job.find("error") != job.end()) {
                    const std::string error = job["error"];
                    nix::logger->log(
                        nix::lvlError,
                        nix::fmt(
                            "aggregate job '%s' references broken job '%s': %s",
                            jobName, childJobName, error));
                    brokenJobs[childJobName] = error;
                    return true;
                }
                return false;
            };

            for (const std::string childJobName : *named) {
                auto childJobIter = jobs.find(childJobName);
                if (childJobIter == jobs.end()) {
                    if (!globConstituents) {
                        nix::logger->log(
                            nix::lvlError,
                            nix::fmt("aggregate job '%s' references "
                                     "non-existent job '%s'",
                                     jobName, childJobName));
                        brokenJobs[childJobName] = "does not exist";
                    } else if (!insertMatchingConstituents(childJobName,
                                                           jobName, isBroken,
                                                           jobs, results)) {
                        nix::warn("aggregate job '%s' references constituent "
                                  "glob pattern '%s' with no matches",
                                  jobName, childJobName);
                        brokenJobs[childJobName] =
                            "constituent glob pattern had no matches";
                    }
                } else if (!isBroken(childJobName, childJobIter->second)) {
                    results.insert(childJobName);
                }
            }

            aggregateJobs.insert(AggregateJob(jobName, results, brokenJobs));
        }
    }

    try {
        return topoSort(aggregateJobs);
    } catch (DependencyCycle &e) {
        return e;
    }
}

void rewriteAggregates(std::map<std::string, nlohmann::json> &jobs,
                       const std::vector<AggregateJob> &aggregateJobs,
                       const nix::ref<nix::Store> &store,
                       const std::filesystem::path &gcRootsDir) {
    for (const auto &aggregateJob : aggregateJobs) {
        auto &job = jobs.find(aggregateJob.name)->second;
        auto drvPath = store->parseStorePath(std::string(job["drvPath"]));
        auto drv = store->readDerivation(drvPath);

        if (aggregateJob.brokenJobs.empty()) {
            addConstituents(job, drv, aggregateJob.dependencies, jobs, store);
            rewriteAndRegisterDrv(job, drv, drvPath, store, gcRootsDir);
        }

        job.erase("namedConstituents");

        if (!aggregateJob.brokenJobs.empty()) {
            addBrokenJobsError(job, aggregateJob.brokenJobs);
        }

        getCoutLock().lock() << job.dump() << "\n";
    }
}
