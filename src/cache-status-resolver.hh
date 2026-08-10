#pragma once
///@file

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <nix/store/derivations.hh>
#include <nix/store/path.hh>
#include <nix/store/store-api.hh>
#include <nix/util/ref.hh>
#include <nix/util/types.hh>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "drv.hh"
#include "response.hh"

/* Resolves the cache status (substituter narinfo lookups) of evaluated jobs on
 * a single worker thread. */
class CacheStatusResolver {
  public:
    /* Called with the job after its cache status has been filled in.
     * Invoked from the worker thread.
     * The sink must do its own locking.
     */
    using Sink = std::function<void(Response)>;

    CacheStatusResolver(nix::ref<nix::Store> store, Sink sink);
    ~CacheStatusResolver();

    CacheStatusResolver(const CacheStatusResolver &) = delete;
    CacheStatusResolver(CacheStatusResolver &&) = delete;
    auto operator=(const CacheStatusResolver &)
        -> CacheStatusResolver & = delete;
    auto operator=(CacheStatusResolver &&) -> CacheStatusResolver & = delete;

    void push(Response response);

    /* Drain remaining work, stop the worker thread and rethrow the first error
     * raised by a job. */
    void finish();

  private:
    /* Per-attempt state of the slow-path input graph walk. */
    struct Traversal {
        nix::StorePathSet visited;
        std::vector<nix::StorePath> neededBuilds;
        std::vector<nix::StorePath> unknownPaths;
        std::set<nix::StorePath> substitutePaths;
    };

    void run();
    void takeInbox(std::vector<Response> *jobs);
    auto tryResolve(Drv &drv) -> bool;
    void visitDrv(Traversal *traversal, const nix::StorePath &drvPath,
                  const nix::StringSet &wantedOutputs);
    auto missingOutputs(const nix::Derivation &derivation,
                        const nix::StringSet &wantedOutputs)
        -> std::optional<std::vector<nix::StorePath>>;
    auto allSubstitutable(const std::vector<nix::StorePath> &paths) -> bool;
    void resolveWanted();

    nix::ref<nix::Store> store;
    Sink sink;

    std::mutex mutex;
    std::condition_variable inboxCv;
    std::deque<Response> inbox;
    bool closed = false;
    std::exception_ptr exc;
    std::atomic<bool> aborted = false;
    std::thread worker;

    /* Jobs share most of their closure (e.g. stdenv). Probe results are
     * remembered across jobs. */
    std::map<nix::StorePath, bool> probeCache;
    std::set<nix::StorePath> wanted;
    /* False when the current tryResolve() hit a path missing from probeCache.
     */
    bool attemptComplete = true;
};
