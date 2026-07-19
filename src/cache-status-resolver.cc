#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp> // IWYU pragma: keep
#include <boost/asio/executor_work_guard.hpp>
#include <exception>
#include <map>
#include <mutex>
#include <nix/store/derivations.hh>
#include <nix/store/filetransfer.hh>
#include <nix/store/globals.hh>
#include <nix/store/http-binary-cache-store.hh>
#include <nix/store/path-info.hh>
#include <nix/store/path.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/async.hh>
#include <nix/util/callback.hh>
#include <nix/util/error.hh>
#include <nix/util/logging.hh>
#include <nix/util/ref.hh>
#include <nix/util/signals.hh>
#include <nix/util/types.hh>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "cache-status-resolver.hh"
#include "drv.hh"
#include "response.hh"

namespace asio = boost::asio;

namespace {

/* Opening an HTTP store normally uses Nix's process-global FileTransfer.
   CacheStatusResolver is constructed before the evaluator workers fork, so
   those children would inherit a FileTransfer whose worker thread no longer
   exists.  Keep the resolver's HTTP transport private instead, leaving the
   process-global transport uninitialised until a child needs it. */
auto openSubstituters() -> std::list<nix::ref<nix::Store>> {
    auto fileTransfer = nix::makeFileTransfer();
    std::list<nix::ref<nix::Store>> stores;
    std::set<nix::StoreReference> done;

    for (const auto &storeReference :
         nix::settings.getWorkerSettings().substituters.get()) {
        if (!done.insert(storeReference).second) {
            continue;
        }
        try {
            auto config =
                nix::resolveStoreConfig(nix::StoreReference{storeReference});
            nix::ref<nix::Store> store = [&]() -> nix::ref<nix::Store> {
                if (auto httpConfig = config.dynamic_pointer_cast<
                                      nix::HttpBinaryCacheStoreConfig>()) {
                    return nix::ref{httpConfig}->openStore(fileTransfer);
                }
                return config->openStore();
            }();
            store->init();
            stores.push_back(std::move(store));
        } catch (nix::Error &error) {
            nix::logger->logEI(nix::lvlWarn, error.info());
        }
    }

    stores.sort([](const nix::ref<nix::Store> &lhs,
                   const nix::ref<nix::Store> &rhs) -> bool {
        return lhs->config.priority < rhs->config.priority;
    });
    return stores;
}

/* Deterministic output order: by name, then full path (matches the
   previous queryMissing-based implementation). */
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
    : store(std::move(store)), substituters(openSubstituters()),
      sink(std::move(sink)), workGuard(asio::make_work_guard(ctx)) {
    ioThread = std::thread([this]() -> void { ctx.run(); });
}

void CacheStatusResolver::stopAndJoin() {
    workGuard.reset();
    if (ioThread.joinable()) {
        ioThread.join();
    }
}

CacheStatusResolver::~CacheStatusResolver() {
    {
        const std::scoped_lock lock(mutex);
        closed = true;
    }
    // The destructor runs on error paths (exception unwinding) where
    // draining the backlog would only delay shutdown. Stopping the
    // io_context would be unsafe: in-flight FileTransfer callbacks
    // post their completion to it from the curl thread (see
    // callbackToAwaitable). Instead `aborted` makes every coroutine
    // bail out right after its current await.
    aborted = true;
    stopAndJoin();
}

void CacheStatusResolver::push(Response response) {
    {
        const std::scoped_lock lock(mutex);
        // Drop new work after close or a failure: finish() rethrows
        // the failure anyway, more lookups would be wasted work.
        if (closed || exc) {
            return;
        }
        inFlight++;
    }
    // NOLINTNEXTLINE(misc-include-cleaner): provided by co_spawn.hpp
    asio::co_spawn(
        ctx, process(std::move(response)),
        [this](const std::exception_ptr &error) -> void { onJobDone(error); });
}

void CacheStatusResolver::finish() {
    {
        std::unique_lock<std::mutex> lock(mutex);
        closed = true;
        idle.wait(lock, [this]() -> bool { return inFlight == 0; });
    }
    stopAndJoin();
    const std::scoped_lock lock(mutex);
    if (exc) {
        std::rethrow_exception(exc);
    }
}

void CacheStatusResolver::onJobDone(const std::exception_ptr &error) {
    const std::scoped_lock lock(mutex);
    inFlight--;
    if (error && !exc) {
        exc = error;
        aborted = true;
    }
    if (inFlight == 0) {
        idle.notify_all();
    }
}

void CacheStatusResolver::throwIfAborted() const {
    nix::checkInterrupt();
    if (aborted) {
        throw nix::Error("cache-status lookup aborted");
    }
}

// Coroutine parameters are by value: references can dangle across
// suspension points.
auto CacheStatusResolver::substitutable(nix::StorePath path)
    -> asio::awaitable<bool> {
    throwIfAborted();
    if (auto cached = probeCache.find(path); cached != probeCache.end()) {
        co_return cached->second;
    }
    bool found = false;
    for (const auto &sub : substituters) {
        if (sub->storeDir != store->storeDir) {
            continue;
        }
        try {
            co_await nix::callbackToAwaitable<
                nix::ref<const nix::ValidPathInfo>>(
                [&](nix::Callback<nix::ref<const nix::ValidPathInfo>> callback)
                    -> void { sub->queryPathInfo(path, std::move(callback)); });
            found = true;
            break;
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (nix::InvalidPath &) {
            // not in this substituter
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (nix::Error &) {
            // unreachable/misconfigured substituter; queryMissing
            // also treats these as misses
        }
    }
    probeCache.emplace(path, found);
    co_return found;
}

auto CacheStatusResolver::allSubstitutable(std::vector<nix::StorePath> paths)
    -> asio::awaitable<bool> {
    for (const auto &path : paths) {
        if (!co_await substitutable(path)) {
            co_return false;
        }
    }
    co_return true;
}

/* The wanted (or all, when wantedOutputs is empty) output paths of a
   derivation that are not in the local store; nullopt when an output
   path is statically unknown (CA derivations). */
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
auto CacheStatusResolver::visitDrv(Traversal *traversal, nix::StorePath drvPath,
                                   nix::StringSet wantedOutputs)
    -> asio::awaitable<void> {
    throwIfAborted();
    if (!traversal->visited.insert(drvPath).second) {
        co_return;
    }
    auto derivation = store->readDerivation(drvPath);

    auto missing = missingOutputs(derivation, wantedOutputs);
    if (!missing) {
        traversal->drv.unknownPaths.push_back(drvPath);
        co_return;
    }
    if (missing->empty()) {
        co_return;
    }

    if (co_await allSubstitutable(*missing)) {
        // Unlike queryMissing we do not walk the references of
        // substitutable paths, so neededSubstitutes lists drv
        // outputs only, not their transitive closure.
        traversal->substitutePaths.insert(missing->begin(), missing->end());
        co_return;
    }

    for (const auto &[inputDrvPath, inputNode] : derivation.inputDrvs.map) {
        co_await visitDrv(traversal, inputDrvPath, inputNode.value);
    }
    /* Post-order: dependencies are appended before their dependants,
       matching the reversed topological sort of the queryMissing-based
       implementation. */
    traversal->drv.neededBuilds.push_back(drvPath);
}

auto CacheStatusResolver::process(Response response) -> asio::awaitable<void> {
    nix::checkInterrupt();
    if (aborted) {
        co_return;
    }

    auto *job = std::get_if<Response::Job>(&response.payload);
    if (job == nullptr) {
        sink(std::move(response));
        co_return;
    }
    auto &drv = job->drv;

    /* Fast path: all output paths are statically known, so the drv is
       obtainable iff every missing output is substitutable. This
       mirrors checkOutputsAvailable and avoids walking the inputs of
       FODs whose build-time-only deps are not cached (issue #413). */
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
        if (co_await allSubstitutable(missingJobOutputs)) {
            drv.neededSubstitutes = missingJobOutputs;
            sortPaths(drv.neededSubstitutes);
            drv.cacheStatus = missingJobOutputs.empty()
                                  ? Drv::CacheStatus::Local
                                  : Drv::CacheStatus::Cached;
            sink(std::move(response));
            co_return;
        }
    }

    /* Slow path: something needs building. Walk the input graph for
       the per-derivation breakdown (which inputs will build, which
       come from a cache), like Store::queryMissing. */
    Traversal traversal{.drv = drv, .visited = {}, .substitutePaths = {}};
    co_await visitDrv(&traversal, drv.drvPath, {});

    drv.neededSubstitutes.assign(traversal.substitutePaths.begin(),
                                 traversal.substitutePaths.end());
    sortPaths(drv.neededSubstitutes);
    drv.cacheStatus = Drv::CacheStatus::NotBuilt;
    sink(std::move(response));
}
