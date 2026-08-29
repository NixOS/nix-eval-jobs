// NOLINTNEXTLINE(modernize-deprecated-headers) misc-include-cleaner wants this
// for setenv
#include <stdlib.h>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/eval-gc.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh> // NOLINT(misc-header-include-cycle)
#include <nix/flake/flake.hh>
#include <nix/flake/settings.hh>
#include <nix/main/shared.hh>
#include <nix/store/globals.hh>
#include <nix/util/configuration.hh>
#include <nix/util/error.hh>
#include <nix/util/fmt.hh>
#include <nix/util/logging.hh>
#include <nix/util/signals.hh> // NOLINT(misc-header-include-cycle)
#include <nix/util/sync.hh>
#include <nix/util/terminal.hh>
#ifdef __linux__
#include <nix/util/linux-namespaces.hh>
#include <nix/util/users.hh>
#endif
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "eval-args.hh"
#include "buffered-io.hh"
#include "worker.hh"
#include "response.hh"
#include "output-stream-lock.hh"
#include "constituents.hh"
#include "daemon-settings.hh"
#include "crash-handler.hh"
#include "store.hh"
#include "cache-status-resolver.hh"
#include "rss.hh"
#include "proc.hh"

namespace {
MyArgs myArgs; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void runWorker() {
    nix::AutoCloseFD toParent(WORKER_OUT_FD);
    nix::AutoCloseFD fromParent(WORKER_IN_FD);
    nix::logger->log(nix::lvlDebug,
                     nix::fmt("created worker process %d", getpid()));
    try {
        worker(myArgs, toParent, fromParent);
    } catch (nix::Error &e) {
        nlohmann::json err;
        const auto &msg = e.msg();
        err["error"] = nix::filterANSIEscapes(msg, true);
        // Also print it to the STDERR log; this is what's shown in
        // the Hydra UI.
        nix::logger->log(nix::lvlError, msg);
        if (tryWriteLine(toParent.get(), err.dump()) < 0) {
            return; // main process died
        }
        if (tryWriteLine(toParent.get(), std::string(MSG_RESTART)) < 0) {
            return; // main process died
        }
    }
}

void handleConstituents(std::map<std::string, nlohmann::json> &jobs,
                        const MyArgs &args) {

    auto store = nix_eval_jobs::openStore(args.evalStoreUrl);

    std::visit(
        nix::overloaded{
            [&](const std::vector<AggregateJob> &namedConstituents) -> void {
                rewriteAggregates(jobs, namedConstituents, store,
                                  args.gcRootsDir);
            },
            [&](const DependencyCycle &cycle) -> void {
                nix::logger->log(nix::lvlError,
                                 nix::fmt("Found dependency cycle "
                                          "between jobs '%s' and '%s'",
                                          cycle.a, cycle.b));
                jobs[cycle.a]["error"] = cycle.message();
                jobs[cycle.b]["error"] = cycle.message();

                getCoutLock().lock() << jobs[cycle.a].dump() << "\n"
                                     << jobs[cycle.b].dump() << "\n";

                for (const auto &jobName : cycle.remainingAggregates) {
                    jobs[jobName]["error"] =
                        "Skipping aggregate because of a dependency "
                        "cycle";
                    getCoutLock().lock() << jobs[jobName].dump() << "\n";
                }
            },
        },
        resolveNamedConstituents(jobs));
}

using JobMap = std::map<std::string, nlohmann::json>;

/* Record a finished job/error and print it, unless it is an aggregate
   that still awaits its constituents (handleConstituents prints it
   later). Also called from the CacheStatusResolver thread. */
void emitResponse(nix::Sync<JobMap> &jobs, const Response &response,
                  nlohmann::json json, std::string_view dumped) {
    jobs.lock()->insert_or_assign(response.attr, std::move(json));

    bool hasPendingConstituents = false;
    if (const auto *job = std::get_if<Response::Job>(&response.payload)) {
        hasPendingConstituents =
            !job->drv.constituents.namedConstituents.empty();
    }
    if (!hasPendingConstituents) {
        getCoutLock().lock() << dumped << "\n";
    }
}

void emitResponse(nix::Sync<JobMap> &jobs, const Response &response) {
    nlohmann::json json = response;
    auto dumped = json.dump();
    emitResponse(jobs, response, std::move(json), dumped);
}

void emitError(nix::Sync<JobMap> &jobs, const nlohmann::json &attrPath,
               const std::string &msg) {
    const Response response{
        .attr = joinAttrPath(attrPath),
        .attrPath = attrPath.get<std::vector<std::string>>(),
        .payload = Response::Error{msg},
    };
    nix::logger->log(nix::lvlError, msg + ": " + response.attr);
    emitResponse(jobs, response);
}

auto parseResponse(std::string_view line)
    -> std::pair<Response, nlohmann::json> {
    try {
        auto json = nlohmann::json::parse(line);
        auto response = json.get<Response>();
        return {std::move(response), std::move(json)};
    } catch (const nlohmann::json::exception &e) {
        throw nix::Error("Received invalid JSON from worker: %s\n json: '%s'",
                         e.what(), line);
    }
}

/* Single-threaded event loop over the worker pipes: spawns workers,
   hands out jobs and collects results.

   Memory: workers * max-memory-size is one budget for all workers. A
   job is only dispatched while every running job plus the new one can
   still grow by `estimate` (decayed max of observed job RSS growth)
   within the budget. If the budget is exceeded anyway the largest busy
   worker is SIGKILLed and its job queued for Mode::Solo: all workers are
   torn down and a single fresh one retries it. Sizes in MiB. */
class Scheduler {
  public:
    Scheduler(const MyArgs &args, const WorkerSpawnConfig &spawn,
              CacheStatusResolver *cacheStatusResolver, nix::Sync<JobMap> &jobs)
        : spawn(spawn), cacheStatusResolver(cacheStatusResolver), jobs(jobs),
          budget(args.nrWorkers * args.maxMemorySize), workers(args.nrWorkers) {
        todo.insert(nlohmann::json::array());
    }

    void run() {
        while (!finished() || anyWorkerAlive()) {
            nix::checkInterrupt();
            dispatch();
            pollWorkers();
        }
    }

  private:
    /* Killed: we SIGKILLed it for exceeding the budget, EOF pending. */
    enum class Phase { None, Starting, Idle, Busy, Killed, Exiting };
    enum class Mode { Parallel, Solo };
    struct Worker {
        std::unique_ptr<Proc> proc;
        Phase phase = Phase::None;
        nlohmann::json attrPath;
        size_t rss = 0;
        size_t startRss = 0;

        [[nodiscard]] auto running() const -> bool {
            return phase == Phase::Busy || phase == Phase::Killed;
        }

        void sampleRss() { rss = proc ? residentMemoryMiB(proc->pid()) : 0; }
        [[nodiscard]] auto growth() const -> size_t {
            return rss > startRss ? rss - startRss : 0;
        }
    };

    static constexpr auto SAMPLE_INTERVAL = std::chrono::milliseconds(200);
    static constexpr size_t INITIAL_ESTIMATE = 512;
    static constexpr size_t DECAY_PERCENT = 98;

    const WorkerSpawnConfig &spawn;
    CacheStatusResolver *cacheStatusResolver;
    nix::Sync<JobMap> &jobs;

    std::set<nlohmann::json> todo;
    std::vector<nlohmann::json> solo;
    Mode mode = Mode::Parallel;
    const size_t budget;
    size_t estimate = INITIAL_ESTIMATE;
    std::vector<Worker> workers;
    std::chrono::steady_clock::time_point lastSample;

    [[nodiscard]] auto running() const -> size_t {
        return static_cast<size_t>(std::ranges::count_if(
            workers, [](const Worker &w) -> bool { return w.running(); }));
    }

    [[nodiscard]] auto finished() const -> bool {
        return todo.empty() && solo.empty() && running() == 0;
    }

    [[nodiscard]] auto totalRss() const -> size_t {
        size_t sum = residentMemoryMiB(getpid());
        for (const auto &w : workers) {
            sum += w.rss;
        }
        return sum;
    }

    [[nodiscard]] auto fitsBudget() const -> bool {
        const size_t busy = count(Phase::Busy);
        return busy == 0 || totalRss() + (busy + 1) * estimate <= budget;
    }

    [[nodiscard]] auto anyWorkerAlive() const -> bool {
        return std::ranges::any_of(
            workers, [](const Worker &w) -> bool { return w.proc != nullptr; });
    }

    [[nodiscard]] auto count(Phase phase) const -> size_t {
        return static_cast<size_t>(
            std::ranges::count_if(workers, [phase](const Worker &w) -> bool {
                return w.phase == phase;
            }));
    }

    auto takeJob(Worker &w) -> std::optional<nlohmann::json> {
        if (running() > 0 && (mode == Mode::Solo || !solo.empty())) {
            return std::nullopt; // one at a time / drain before Solo
        }
        std::optional<nlohmann::json> attrPath;
        if (mode == Mode::Solo) {
            attrPath = solo.back();
            solo.pop_back();
        } else if (solo.empty() && !todo.empty() && fitsBudget()) {
            attrPath = *todo.begin();
            todo.erase(todo.begin());
        } else {
            return std::nullopt;
        }
        w.startRss = w.rss;
        return attrPath;
    }

    /* Each retry runs on one fresh worker with nobody else's heap
       around: drop every worker, dispatch() spawns one, and Solo ends
       with that job so the next retry starts over. */
    void enterSolo() {
        for (auto &w : workers) {
            w = Worker{};
        }
        mode = Mode::Solo;
    }

    void resetWorker(size_t idx) { workers[idx] = Worker{}; }

    void jobFinished(Worker &w) {
        w.sampleRss();
        estimate = std::max(estimate * DECAY_PERCENT / 100, w.growth());
        w.phase = Phase::Starting;
        w.attrPath = nullptr;
        mode = Mode::Parallel;
    }

    void sampleMemory() {
        for (auto &w : workers) {
            w.sampleRss();
            if (w.running()) {
                estimate = std::max(estimate, w.growth());
            }
        }
        const size_t total = totalRss();
        if (total <= budget) {
            return;
        }
        Worker *victim = nullptr;
        for (auto &w : workers) {
            if (w.phase == Phase::Busy &&
                (victim == nullptr || w.rss > victim->rss)) {
                victim = &w;
            }
        }
        if (victim == nullptr) {
            return;
        }
        nix::logger->log(
            nix::lvlInfo,
            nix::fmt("memory budget of %d MiB exceeded (%d MiB in use), "
                     "killing worker %d ('%s', %d MiB)",
                     budget, total, victim->proc->pid(),
                     joinAttrPath(victim->attrPath), victim->rss));
        victim->phase = Phase::Killed;
        kill(victim->proc->pid(), SIGKILL);
    }

    void dispatch() {
        /* Don't spawn more workers than there are jobs to hand out. In
           Solo mode exactly one worker may exist at all. */
        if (mode == Mode::Parallel && !solo.empty() && running() == 0) {
            enterSolo();
        }
        size_t spawnable = 0;
        if (mode == Mode::Solo) {
            spawnable = anyWorkerAlive() ? 0 : 1;
        } else if (solo.empty()) {
            spawnable = todo.size();
            spawnable -= std::min(spawnable,
                                  count(Phase::Starting) + count(Phase::Idle));
        }

        for (size_t idx = 0; idx < workers.size(); idx++) {
            Worker &w = workers[idx];
            if (w.phase == Phase::Idle && finished()) {
                (void)w.proc->sendLine(MSG_EXIT);
                w.phase = Phase::Exiting;
            } else if (w.phase == Phase::Idle) {
                if (auto attrPath = takeJob(w)) {
                    w.attrPath = std::move(*attrPath);
                    w.phase = Phase::Busy;
                    if (!w.proc->sendLine(std::string(MSG_DO) +
                                          w.attrPath.dump())) {
                        onEof(idx);
                    }
                }
            } else if (w.phase == Phase::None && spawnable > 0 && !finished()) {
                w.proc = std::make_unique<Proc>(spawn);
                w.phase = Phase::Starting;
                w.sampleRss();
                spawnable--;
            }
        }
    }

    void pollWorkers() {
        std::vector<pollfd> fds;
        std::vector<size_t> owner;
        for (size_t idx = 0; idx < workers.size(); idx++) {
            if (workers[idx].proc) {
                fds.push_back({.fd = workers[idx].proc->readFd(),
                               .events = POLLIN,
                               .revents = 0});
                owner.push_back(idx);
            }
        }
        const int n = poll(fds.data(), fds.size(),
                           static_cast<int>(SAMPLE_INTERVAL.count()));
        if (n == -1 && errno != EINTR) {
            throw nix::SysError("polling workers");
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastSample >= SAMPLE_INTERVAL) {
            sampleMemory();
            lastSample = now;
        }

        for (size_t i = 0; i < fds.size(); i++) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                readWorker(owner[i]);
            }
        }
    }

    void readWorker(size_t idx) {
        Worker &w = workers[idx];
        const bool eof = !w.proc->fill();
        while (w.proc) {
            auto line = w.proc->popLine();
            if (!line) {
                break;
            }
            onLine(idx, *line);
        }
        if (eof && w.proc) {
            onEof(idx);
        }
    }

    void onLine(size_t idx, std::string_view line) {
        switch (workers[idx].phase) {
        case Phase::Starting:
            if (line == MSG_NEXT) {
                workers[idx].phase = Phase::Idle;
            } else if (line == MSG_RESTART) {
                resetWorker(idx);
            } else {
                auto json = nlohmann::json::parse(line, nullptr, false);
                if (json.is_object() && json.contains("error")) {
                    throw nix::Error("worker error: %s",
                                     std::string(json["error"]));
                }
                throw nix::Error("unexpected line from worker: '%s'", line);
            }
            break;
        case Phase::Busy:
            onResponse(idx, line);
            break;
        case Phase::Killed: // finished before the signal landed, retried anyway
        case Phase::Exiting:
            break;
        default:
            throw nix::Error("unexpected line from idle worker: '%s'", line);
        }
    }

    void onResponse(size_t idx, std::string_view line) {
        auto [response, json] = parseResponse(line);

        if (auto *attrs = std::get_if<Response::Attrs>(&response.payload)) {
            for (const auto &attr : attrs->attrs) {
                nlohmann::json child(response.attrPath);
                child.emplace_back(attr);
                todo.insert(std::move(child));
            }
        } else if (cacheStatusResolver != nullptr &&
                   std::holds_alternative<Response::Job>(response.payload)) {
            // The resolver fills in cacheStatus and emits via its sink.
            cacheStatusResolver->push(std::move(response));
        } else {
            emitResponse(jobs, response, std::move(json), line);
            if (auto *error = std::get_if<Response::Error>(&response.payload);
                error != nullptr && error->fatal) {
                throw nix::Error("%s", error->error);
            }
        }

        jobFinished(workers[idx]);
    }

    void onEof(size_t idx) {
        Worker &w = workers[idx];
        if (w.phase == Phase::Exiting) {
            resetWorker(idx);
            return;
        }
        const std::string doing =
            w.running() ? "evaluating '" + joinAttrPath(w.attrPath) + "'"
                        : "starting worker";
        try {
            w.proc->throwExited(doing);
        } catch (WorkerKilled &) {
            onKilled(idx);
        }
    }

    void onKilled(size_t idx) {
        Worker &w = workers[idx];
        if (w.phase != Phase::Killed) {
            nix::logger->log(
                nix::lvlError,
                nix::fmt("evaluation worker %d was killed (system out of "
                         "memory?)",
                         w.proc->pid()));
        }
        if (w.running()) {
            if (mode == Mode::Solo) {
                emitError(jobs, w.attrPath,
                          nix::fmt("evaluation exceeded the memory budget of "
                                   "%d MiB (workers * max-memory-size) even "
                                   "when run alone",
                                   budget));
            } else {
                solo.push_back(w.attrPath);
            }
        }
        resetWorker(idx);
        mode = Mode::Parallel;
    }
};

/* Rationale for the separate resolver: see CacheStatusResolver. */
auto makeCacheStatusResolver(const MyArgs &args, nix::Sync<JobMap> &jobs)
    -> std::optional<CacheStatusResolver> {
    if (!args.checkCacheStatus) {
        return std::nullopt;
    }
    return std::optional<CacheStatusResolver>(
        std::in_place, nix_eval_jobs::openStore(args.evalStoreUrl),
        nix_eval_jobs::openStore(), [&jobs](const Response &response) -> void {
            emitResponse(jobs, response);
        });
}

void validateIncompatibleFlags(const MyArgs &args) {
    if (!args.noInstantiate) {
        return;
    }

    const std::vector<std::pair<bool, std::string_view>> flagChecks = {
        {args.showInputDrvs, "--show-input-drvs"},
        {args.checkCacheStatus, "--check-cache-status"},
        {args.constituents, "--constituents"}};

    std::string incompatibleFlags;
    for (const auto &[isSet, flagName] : flagChecks) {
        if (isSet) {
            incompatibleFlags +=
                (incompatibleFlags.empty() ? "" : ", ") + std::string(flagName);
        }
    }

    if (!incompatibleFlags.empty()) {
        throw nix::UsageError(
            nix::fmt("--no-instantiate is incompatible with: %s. "
                     "These features require instantiated derivations.",
                     incompatibleFlags));
    }
}
} // namespace

auto main(int argc, char **argv) -> int {
    /* We are doing the garbage collection by restarting workers */
    setenv("GC_DONT_GC", "1", 1); // NOLINT(concurrency-mt-unsafe)

    registerCrashHandler();

    auto args = std::span(argv, argc);

    return nix::handleExceptions(args[0], [&]() -> void {
        nix_eval_jobs::registerDaemonSettings();
        nix::initNix();
        nix::initGC();
        nix::flakeSettings.configureEvalSettings(nix::evalSettings);

#ifdef __linux__
        /* Mirrors src/nix/main.cc: prevent LocalStore::makeStoreWritable
           from stripping ro/nosuid/nodev on the host's /nix/store mount. */
        if (nix::isRootUser()) {
            try {
                nix::tryEnterPrivateMountNamespace();
            } catch (nix::Error &e) {
                nix::warn("failed to set up a private mount namespace: %s",
                          e.msg());
            }
        }
#endif

        myArgs.parseArgs(argv, argc);

        validateIncompatibleFlags(myArgs);

        /* FIXME: The build hook in conjunction with import-from-derivation is
         * causing "unexpected EOF" during eval */
        nix::settings.getWorkerSettings().builders = "";

        /* Set no-instantiate mode if requested (makes evaluation faster) */
        if (myArgs.noInstantiate) {
            nix::settings.readOnlyMode = true;
        }

        /* When building a flake, use pure evaluation (no access to
           'getEnv', 'currentSystem' etc. */
        if (myArgs.impure) {
            nix::evalSettings.pureEval = false;
        } else if (myArgs.flake) {
            nix::evalSettings.pureEval = true;
        }

        if (myArgs.releaseExpr.empty()) {
            throw nix::UsageError("no expression specified");
        }

        if (!myArgs.gcRootsDir.empty()) {
            myArgs.gcRootsDir = std::filesystem::absolute(myArgs.gcRootsDir);
        }

        if (myArgs.showTrace) {
            nix::loggerSettings.showTrace.assign(true);
        }

        if (myArgs.runAsWorker) {
            runWorker();
            return;
        }

        WorkerSpawnConfig spawn{.argv = {args.begin(), args.end()}};

        nix::Sync<JobMap> jobs;

        /* Pre-initialize the eval store (if specified) before spawning
           workers so that the SQLite database and schema are created
           exactly once.  Without this, workers race to create
           a fresh store and hit SQLite "busy" / "schema is corrupt"
           errors.  See https://github.com/NixOS/nix-eval-jobs/issues/401 */
        if (myArgs.evalStoreUrl.has_value()) {
            nix_eval_jobs::openStore(myArgs.evalStoreUrl);
        }

        /* The fetcher cache is opened lazily on first fetch, so
           workers would otherwise race to create fetcher-cache-v4.sqlite and
           fail with "unable to open database file". Open it once here. */
        nix::fetchSettings.getCache();

        if (myArgs.flake) {
            if (auto lockedAttrs = prefetchFlake(myArgs)) {
                spawn.config =
                    nlohmann::json{{"lockedFlake", *lockedAttrs}}.dump();
            }
        }

        auto cacheStatusResolver = makeCacheStatusResolver(myArgs, jobs);

        /* A scheduler error propagates from here so the
           CacheStatusResolver destructor discards its backlog instead
           of finish() draining it (and masking the eval error). */
        Scheduler(myArgs, spawn,
                  cacheStatusResolver ? &*cacheStatusResolver : nullptr, jobs)
            .run();

        if (cacheStatusResolver) {
            cacheStatusResolver->finish();
        }

        if (myArgs.constituents) {
            handleConstituents(*jobs.lock(), myArgs);
        }
    });
}
