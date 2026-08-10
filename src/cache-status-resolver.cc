#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
#include <nix/store/derivations.hh>
#include <nix/store/path-info.hh>
#include <nix/store/path.hh>
#include <nix/store/store-api.hh>
#include <nix/util/error.hh>
#include <nix/util/ref.hh>
#include <nix/util/signals.hh>
#include <nix/util/types.hh>
#include <optional>
#include <set>
#include <deque>
#include <utility>
#include <variant>
#include <vector>

#include "cache-status-resolver.hh"
#include "drv.hh"
#include "response.hh"

namespace {

/* Deterministic output order: by name, then full path. */
void sortPaths(std::vector<nix::StorePath> &paths) {
    std::ranges::sort(
        paths,
        [](const nix::StorePath &lhs, const nix::StorePath &rhs) -> bool {
            return lhs.name() != rhs.name() ? lhs.name() < rhs.name()
                                            : lhs.to_string() < rhs.to_string();
        });
}

} // namespace

CacheStatusResolver::CacheStatusResolver(nix::ref<nix::Store> store, Sink sink)
    : store(std::move(store)), sink(std::move(sink)) {
    worker = std::thread([this]() -> void { run(); });
}

CacheStatusResolver::~CacheStatusResolver() {
    {
        const std::scoped_lock lock(mutex);
        closed = true;
    }
    /* Don't drain the backlog during unwinding. */
    aborted = true;
    inboxCv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void CacheStatusResolver::push(Response response) {
    {
        const std::scoped_lock lock(mutex);
        if (closed || exc) {
            return;
        }
        inbox.push_back(std::move(response));
    }
    inboxCv.notify_all();
}

void CacheStatusResolver::finish() {
    {
        const std::scoped_lock lock(mutex);
        closed = true;
    }
    inboxCv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    const std::scoped_lock lock(mutex);
    if (exc) {
        std::rethrow_exception(exc);
    }
}

void CacheStatusResolver::takeInbox(std::vector<Response> *jobs) {
    std::deque<Response> taken;
    {
        const std::scoped_lock lock(mutex);
        std::swap(taken, inbox);
    }
    for (auto &response : taken) {
        if (std::holds_alternative<Response::Job>(response.payload)) {
            jobs->push_back(std::move(response));
        } else {
            sink(std::move(response));
        }
    }
}

void CacheStatusResolver::run() {
    try {
        std::vector<Response> jobs;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                inboxCv.wait(lock, [this]() -> bool {
                    return !inbox.empty() || closed || aborted;
                });
                if (aborted || (closed && inbox.empty())) {
                    return;
                }
            }
            takeInbox(&jobs);
            while (!jobs.empty()) {
                nix::checkInterrupt();
                if (aborted) {
                    return;
                }
                std::erase_if(jobs, [this](Response &response) -> bool {
                    auto &job = std::get<Response::Job>(response.payload);
                    if (!tryResolve(job.drv)) {
                        return false;
                    }
                    sink(std::move(response));
                    return true;
                });
                /* Late arrivals widen the next batch. */
                takeInbox(&jobs);
                resolveWanted();
            }
        }
    } catch (...) {
        const std::scoped_lock lock(mutex);
        exc = std::current_exception();
        aborted = true;
    }
}

/* Resolve the paths wanted by all blocked jobs in one batched
 * querySubstitutablePathInfos call. */
void CacheStatusResolver::resolveWanted() {
    if (wanted.empty()) {
        return;
    }
    nix::StorePathCAMap batch;
    for (const auto &path : std::exchange(wanted, {})) {
        batch.insert_or_assign(path, std::nullopt);
    }
    nix::SubstitutablePathInfos infos;
    try {
        store->querySubstitutablePathInfos(batch, infos);
    } catch (nix::Error &) { // NOLINT(bugprone-empty-catch)
        /* Unreachable substituters count as misses. */
    }
    for (const auto &[path, _ca] : batch) {
        probeCache.emplace(path, infos.contains(path));
    }
}

auto CacheStatusResolver::allSubstitutable(
    const std::vector<nix::StorePath> &paths) -> bool {
    bool all = true;
    for (const auto &path : paths) {
        if (auto cached = probeCache.find(path); cached != probeCache.end()) {
            all = all && cached->second;
        } else {
            wanted.insert(path);
            attemptComplete = false;
            all = false;
        }
    }
    return all;
}

/* The wanted (or all, when wantedOutputs is empty) output paths of a derivation
 * that are not in the local store. nullopt when an output path is statically
 * unknown (CA derivations).
 */
auto CacheStatusResolver::missingOutputs(const nix::Derivation &derivation,
                                         const nix::StringSet &wantedOutputs)
    -> std::optional<std::vector<nix::StorePath>> {
    std::vector<nix::StorePath> missing;
    for (const auto &[outputName, outputPathOpt] :
         derivation.outputsAndOptPaths(*store)) {
        if (!wantedOutputs.empty() && !wantedOutputs.contains(outputName)) {
            continue;
        }
        if (!outputPathOpt.second) {
            return std::nullopt;
        }
        if (!store->isValidPath(*outputPathOpt.second)) {
            missing.push_back(*outputPathOpt.second);
        }
    }
    return missing;
}

// NOLINTNEXTLINE(misc-no-recursion): walking a DAG of derivations
void CacheStatusResolver::visitDrv(Traversal *traversal,
                                   const nix::StorePath &drvPath,
                                   const nix::StringSet &wantedOutputs) {
    if (!attemptComplete || !traversal->visited.insert(drvPath).second) {
        return;
    }
    auto derivation = store->readDerivation(drvPath);

    auto missing = missingOutputs(derivation, wantedOutputs);
    if (!missing) {
        traversal->unknownPaths.push_back(drvPath);
        return;
    }
    if (missing->empty()) {
        return;
    }

    if (allSubstitutable(*missing)) {
        traversal->substitutePaths.insert(missing->begin(), missing->end());
        return;
    }
    if (!attemptComplete) {
        return;
    }

    for (const auto &[inputDrvPath, inputNode] : derivation.inputDrvs.map) {
        visitDrv(traversal, inputDrvPath, inputNode.value);
    }
    /* Post-order: dependencies before their dependants. */
    traversal->neededBuilds.push_back(drvPath);
}

/* Resolve a job using only cached probe results.
 * Returns false when a needed probe is still unknown.
 * The job is retried after the next resolveWanted() round.
 */
auto CacheStatusResolver::tryResolve(Drv &drv) -> bool {
    attemptComplete = true;

    /* Fast path: all output paths are statically known.
     * The drv is obtainable iff every missing output is substitutable.
     * This mirrors checkOutputsAvailable and avoids walking the inputs of
     * FODs whose build-time-only deps are not cached (issue #413).
     */
    std::vector<nix::StorePath> missingJobOutputs;
    bool outputsKnown = true;
    for (const auto &[outputName, outputPath] : drv.outputs) {
        if (!outputPath) {
            outputsKnown = false;
            break;
        }
        if (!store->isValidPath(*outputPath)) {
            missingJobOutputs.push_back(*outputPath);
        }
    }
    if (outputsKnown) {
        const bool all = allSubstitutable(missingJobOutputs);
        if (!attemptComplete) {
            return false;
        }
        if (all) {
            drv.neededSubstitutes = missingJobOutputs;
            sortPaths(drv.neededSubstitutes);
            drv.cacheStatus = missingJobOutputs.empty()
                                  ? Drv::CacheStatus::Local
                                  : Drv::CacheStatus::Cached;
            return true;
        }
    }

    /* Slow path: Something needs building.
     * Like Store::queryMissing walk the input graph for the per-derivation
     * breakdown (which inputs will build, which come from a cache)
     */
    Traversal traversal;
    visitDrv(&traversal, drv.drvPath, {});
    if (!attemptComplete) {
        return false;
    }

    drv.neededBuilds = std::move(traversal.neededBuilds);
    drv.unknownPaths = std::move(traversal.unknownPaths);
    /* Unlike queryMissing we do not walk the references of substitutable paths.
     * Only neededSubstitutes lists drv outputs, not their transitive closure.
     */
    drv.neededSubstitutes.assign(traversal.substitutePaths.begin(),
                                 traversal.substitutePaths.end());
    sortPaths(drv.neededSubstitutes);
    drv.cacheStatus = Drv::CacheStatus::NotBuilt;
    return true;
}
