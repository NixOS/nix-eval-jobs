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

CacheStatusResolver::CacheStatusResolver(nix::ref<nix::Store> evalStore,
                                         nix::ref<nix::Store> buildStore,
                                         Sink sink)
    : evalStore(std::move(evalStore)), buildStore(std::move(buildStore)),
      sink(std::move(sink)) {
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

/* Resolve the paths wanted by all blocked jobs in batched
 * queryValidPaths / querySubstitutablePathInfos calls. */
void CacheStatusResolver::resolveWanted() {
    if (!wantedValid.empty()) {
        const auto batch = std::exchange(wantedValid, {});
        const auto valid = buildStore->queryValidPaths(batch);
        for (const auto &path : batch) {
            validCache.emplace(path, valid.contains(path));
        }
    }
    if (wanted.empty()) {
        return;
    }
    nix::StorePathCAMap batch;
    for (const auto &path : std::exchange(wanted, {})) {
        batch.insert_or_assign(path, std::nullopt);
    }
    nix::SubstitutablePathInfos infos;
    try {
        buildStore->querySubstitutablePathInfos(batch, infos);
    } catch (nix::Error &) { // NOLINT(bugprone-empty-catch)
        /* Unreachable substituters count as misses. */
    }
    for (const auto &[path, _ca] : batch) {
        probeCache.emplace(path, infos.contains(path));
    }
}

auto CacheStatusResolver::allSubstitutable(
    const std::vector<nix::StorePath> &paths) -> std::optional<bool> {
    bool all = true;
    bool complete = true;
    for (const auto &path : paths) {
        if (auto cached = probeCache.find(path); cached != probeCache.end()) {
            all = all && cached->second;
        } else {
            wanted.insert(path);
            attemptComplete = false;
            complete = false;
        }
    }
    if (!complete) {
        return std::nullopt;
    }
    return all;
}

auto CacheStatusResolver::probeValidity(const nix::StorePath &path)
    -> std::optional<bool> {
    if (auto cached = validCache.find(path); cached != validCache.end()) {
        return cached->second;
    }
    wantedValid.insert(path);
    attemptComplete = false;
    return std::nullopt;
}

auto CacheStatusResolver::readDerivation(const nix::StorePath &drvPath)
    -> const nix::Derivation & {
    if (auto cached = drvCache.find(drvPath); cached != drvCache.end()) {
        return cached->second;
    }
    return drvCache.emplace(drvPath, evalStore->readDerivation(drvPath))
        .first->second;
}

/* The wanted (or all, when wantedOutputs is empty) output paths of a derivation
 * that are not in the local store. Probes past pending lookups so one retry
 * round batches as many paths as possible. */
auto CacheStatusResolver::missingOutputs(const nix::Derivation &derivation,
                                         const nix::StringSet &wantedOutputs)
    -> MissingOutputs {
    MissingOutputs result;
    for (const auto &[outputName, outputPathOpt] :
         derivation.outputsAndOptPaths(*evalStore)) {
        if (!wantedOutputs.empty() && !wantedOutputs.contains(outputName)) {
            continue;
        }
        if (!outputPathOpt.second) {
            result.known = false;
            return result;
        }
        auto valid = probeValidity(*outputPathOpt.second);
        if (!valid) {
            result.complete = false;
            continue;
        }
        if (!*valid) {
            result.missing.push_back(*outputPathOpt.second);
        }
    }
    return result;
}

// NOLINTNEXTLINE(misc-no-recursion): walking a DAG of derivations
void CacheStatusResolver::visitDrv(Traversal *traversal,
                                   const nix::StorePath &drvPath,
                                   const nix::StringSet &wantedOutputs) {
    /* Siblings are still visited after a pending probe: the traversal is
     * discarded, but every visit widens the next batch. */
    if (!traversal->visited.insert(drvPath).second) {
        return;
    }
    const auto &derivation = readDerivation(drvPath);

    auto outputs = missingOutputs(derivation, wantedOutputs);
    if (!outputs.known) {
        traversal->unknownPaths.push_back(drvPath);
        return;
    }
    /* Validity probes are cheap daemon lookups: recurse anyway so a single
     * queryValidPaths round covers the whole closure. */
    if (!outputs.complete) {
        for (const auto &[inputDrvPath, inputNode] : derivation.inputDrvs.map) {
            visitDrv(traversal, inputDrvPath, inputNode.value);
        }
        return;
    }
    if (outputs.missing.empty()) {
        return;
    }

    /* Substituter probes are per-path narinfo requests: stay pruned, a
     * substitutable derivation's inputs are never probed. */
    auto substitutable = allSubstitutable(outputs.missing);
    if (!substitutable) {
        return;
    }
    if (*substitutable) {
        traversal->substitutePaths.insert(outputs.missing.begin(),
                                          outputs.missing.end());
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
    bool outputsComplete = true;
    for (const auto &[outputName, outputPath] : drv.outputs) {
        if (!outputPath) {
            outputsKnown = false;
            break;
        }
        auto valid = probeValidity(*outputPath);
        if (!valid) {
            outputsComplete = false;
            continue;
        }
        if (!*valid) {
            missingJobOutputs.push_back(*outputPath);
        }
    }
    if (outputsKnown) {
        if (!outputsComplete) {
            return false;
        }
        const auto all = allSubstitutable(missingJobOutputs);
        if (!all) {
            return false;
        }
        if (*all) {
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
