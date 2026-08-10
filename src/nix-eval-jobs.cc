// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants these header rather than the C++ versions
#include <signal.h>
#include <stdlib.h>
#include <string.h>
// NOLINTEND(modernize-deprecated-headers)
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <exception>
#include <filesystem>
#include <functional>
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
#include <nix/store/local-fs-store.hh>
#include <nix/util/configuration.hh>
#include <nix/util/error.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/fmt.hh>
#include <nix/util/logging.hh>
#include <nix/util/processes.hh>
#include <nix/util/signals.hh> // NOLINT(misc-header-include-cycle)
#include <nix/util/sync.hh>
#include <nix/util/terminal.hh>
#include <nix/util/util.hh>
#ifdef __linux__
#include <nix/util/linux-namespaces.hh>
#include <nix/util/users.hh>
#endif
#include <sys/signal.h>
#include <variant>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <pthread.h>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "eval-args.hh"
#include "buffered-io.hh"
#include "worker.hh"
#include "response.hh"
#include "strings-portable.hh"
#include "output-stream-lock.hh"
#include "constituents.hh"
#include "daemon-settings.hh"
#include "store.hh"
#include "cache-status-resolver.hh"

namespace {
MyArgs myArgs; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

using Processor = std::function<void(MyArgs &myArgs, nix::AutoCloseFD &toFd,
                                     nix::AutoCloseFD &fromFd)>;

void handleConstituents(std::map<std::string, nlohmann::json> &jobs,
                        const MyArgs &args) {

    auto store = nix_eval_jobs::openStore(args.evalStoreUrl);
    auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>();

    if (!localStore) {
        nix::warn("constituents feature requires a local store, skipping "
                  "aggregate rewriting");
        return;
    }

    auto localStoreRef = nix::ref<nix::LocalFSStore>(localStore);

    std::visit(
        nix::overloaded{
            [&](const std::vector<AggregateJob> &namedConstituents) -> void {
                rewriteAggregates(jobs, namedConstituents, localStoreRef,
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

/* Auto-cleanup of fork's process and fds. */
struct Proc {
    nix::AutoCloseFD to, from;
    nix::Pid pid;

    Proc(const Proc &) = delete;
    Proc(Proc &&) = delete;
    auto operator=(const Proc &) -> Proc & = delete;
    auto operator=(Proc &&) -> Proc & = delete;

    explicit Proc(const Processor &proc) {
        nix::Pipe toPipe;
        nix::Pipe fromPipe;
        toPipe.create();
        fromPipe.create();
        auto childPid = startProcess(
            [&,
             toFd{std::make_shared<nix::AutoCloseFD>(
                 std::move(fromPipe.writeSide))},
             fromFd{std::make_shared<nix::AutoCloseFD>(
                 std::move(toPipe.readSide))}]() -> void {
                nix::logger->log(
                    nix::lvlDebug,
                    nix::fmt("created worker process %d", getpid()));
                try {
                    proc(myArgs, *toFd, *fromFd);
                } catch (nix::Error &e) {
                    nlohmann::json err;
                    const auto &msg = e.msg();
                    err["error"] = nix::filterANSIEscapes(msg, true);
                    nix::logger->log(nix::lvlError, msg);
                    if (tryWriteLine(toFd->get(), err.dump()) < 0) {
                        return; // main process died
                    };
                    // Don't forget to print it into the STDERR log, this is
                    // what's shown in the Hydra UI.
                    if (tryWriteLine(toFd->get(), std::string(MSG_RESTART)) <
                        0) {
                        return; // main process died
                    }
                }
            },
            nix::ProcessOptions{.allowVfork = false});

        to = std::move(toPipe.writeSide);
        from = std::move(fromPipe.readSide);
        pid = childPid;
    }

    ~Proc() = default;
};

// We'd highly prefer using std::thread here; but this won't let us configure
// the stack size. macOS uses 512KiB size stacks for non-main threads, and musl
// defaults to 128k. While Nix configures a 64MiB size for the main thread, this
// doesn't propagate to the threads we launch here. It turns out, running the
// evaluator under an anemic stack of 0.5MiB has it overflow way too quickly.
// Hence, we have our own custom Thread struct.
// NOLINTBEGIN(misc-include-cleaner)
// False positive: pthread.h is included but clang-tidy doesn't recognize it
struct Thread {
    pthread_t thread = {};

    Thread(const Thread &) = delete;
    Thread(Thread &&) noexcept = default;
    ~Thread() = default;
    auto operator=(const Thread &) -> Thread & = delete;
    auto operator=(Thread &&) -> Thread & = delete;

    explicit Thread(std::function<void(void)> func) {
        pthread_attr_t attr = {};

        auto funcPtr =
            std::make_unique<std::function<void(void)>>(std::move(func));

        int status = pthread_attr_init(&attr);
        if (status != 0) {
            throw nix::SysError(status, "calling pthread_attr_init");
        }

        struct AttrGuard {
            pthread_attr_t &attr;
            explicit AttrGuard(pthread_attr_t &attribute) : attr(attribute) {}
            AttrGuard(const AttrGuard &) = delete;
            auto operator=(const AttrGuard &) -> AttrGuard & = delete;
            AttrGuard(AttrGuard &&) = delete;
            auto operator=(AttrGuard &&) -> AttrGuard & = delete;
            ~AttrGuard() { (void)pthread_attr_destroy(&attr); }
        };
        const AttrGuard attrGuard(attr);

        static constexpr size_t STACK_SIZE_MB = 64;
        static constexpr size_t KB_SIZE = 1024;
        status = pthread_attr_setstacksize(
            &attr, static_cast<size_t>(STACK_SIZE_MB) * KB_SIZE * KB_SIZE);
        if (status != 0) {
            throw nix::SysError(status, "calling pthread_attr_setstacksize");
        }
        status = pthread_create(&thread, &attr, Thread::init, funcPtr.get());
        if (status != 0) {
            throw nix::SysError(status, "calling pthread_launch");
        }
        [[maybe_unused]] auto *res =
            funcPtr.release(); // will be deleted in init()
    }

    void join() const {
        const int status = pthread_join(thread, nullptr);
        if (status != 0) {
            throw nix::SysError(status, "calling pthread_join");
        }
    }

  private:
    static auto init(void *ptr) -> void * {
        std::unique_ptr<std::function<void(void)>> func;
        func.reset(static_cast<std::function<void(void)> *>(ptr));

        (*func)();
        return nullptr;
    }
};
// NOLINTEND(misc-include-cleaner)

struct State {
    std::set<nlohmann::json> todo =
        nlohmann::json::array({nlohmann::json::array()});
    std::set<nlohmann::json> active;
    std::map<std::string, nlohmann::json> jobs;
    std::exception_ptr exc;
};

void handleBrokenWorkerPipe(Proc &proc, std::string_view msg) {
    // we already took the process status from Proc, no
    // need to wait for it again to avoid error messages
    // NOLINTNEXTLINE(misc-include-cleaner)
    const pid_t pid = proc.pid.release();
    while (true) {
        int status = 0;
        const int result = waitpid(pid, &status, WNOHANG);
        if (result == 0) {
            kill(pid, SIGKILL);
            throw nix::Error(
                "BUG: while %s, worker pipe got closed but evaluation "
                "worker still running?",
                msg);
        }

        if (result == -1) {
            kill(pid, SIGKILL);
            throw nix::Error(
                "BUG: while %s, waitpid for evaluation worker failed: %s", msg,
                get_error_name(errno));
        }
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 1) {
                throw nix::Error(
                    "while %s, evaluation worker exited with exit code 1, "
                    "(possible infinite recursion)",
                    msg);
            }
            throw nix::Error("while %s, evaluation worker exited with %d", msg,
                             WEXITSTATUS(status));
        }

        if (WIFSIGNALED(status)) {
            switch (WTERMSIG(status)) {
            case SIGKILL:
                throw nix::Error(
                    "while %s, evaluation worker got killed by SIGKILL, "
                    "maybe "
                    "memory limit reached?",
                    msg);
                break;
#ifdef __APPLE__
            case SIGBUS:
                throw nix::Error(
                    "while %s, evaluation worker got killed by SIGBUS, "
                    "(possible infinite recursion)",
                    msg);
                break;
#else
            case SIGSEGV:
                throw nix::Error(
                    "while %s, evaluation worker got killed by SIGSEGV, "
                    "(possible infinite recursion)",
                    msg);
#endif
            default:
                throw nix::Error("while %s, evaluation worker got killed by "
                                 "signal %d (%s)",
                                 msg, WTERMSIG(status),
                                 get_signal_name(WTERMSIG(status)));
            }
        } // else ignore WIFSTOPPED and WIFCONTINUED
    }
}

auto joinAttrPath(const nlohmann::json &attrPath) -> std::string {
    std::string joined;
    for (const auto &element : attrPath) {
        if (!joined.empty()) {
            joined += '.';
        }
        joined += element.get<std::string>();
    }
    return joined;
}

namespace {
auto checkWorkerStatus(LineReader *fromReader, Proc *proc) -> std::string_view {
    auto line = fromReader->readLine();
    if (line.empty()) {
        handleBrokenWorkerPipe(*proc, "checking worker process");
    }
    if (line != MSG_NEXT && line != MSG_RESTART) {
        try {
            auto json = nlohmann::json::parse(line);
            throw nix::Error("worker error: %s", std::string(json["error"]));
        } catch (const nlohmann::json::exception &e) {
            throw nix::Error(
                "Received invalid JSON from worker: %s\n json: '%s'", e.what(),
                line);
        }
    }
    return line;
}

auto getNextJob(nix::Sync<State> &state_, std::condition_variable &wakeup)
    -> std::optional<nlohmann::json> {
    nlohmann::json attrPath;
    while (true) {
        nix::checkInterrupt();
        auto state(state_.lock());
        if ((state->todo.empty() && state->active.empty()) || state->exc) {
            return std::nullopt;
        }
        if (!state->todo.empty()) {
            attrPath = *state->todo.begin();
            state->todo.erase(state->todo.begin());
            state->active.insert(attrPath);
            return attrPath;
        }
        state.wait(wakeup);
    }
}

/* Record a finished job/error in the shared state and print it,
   unless it is an aggregate that still awaits its constituents (then
   handleConstituents prints it later). */
void emitResponse(nix::Sync<State> &state_, const Response &response,
                  nlohmann::json jsonResponse, std::string_view dumped) {
    {
        auto state(state_.lock());
        state->jobs.insert_or_assign(response.attr, std::move(jsonResponse));
    }

    bool hasPendingConstituents = false;
    if (const auto *job = std::get_if<Response::Job>(&response.payload)) {
        hasPendingConstituents =
            !job->drv.constituents.namedConstituents.empty();
    }
    if (!hasPendingConstituents) {
        getCoutLock().lock() << dumped << "\n";
    }
}

auto processWorkerResponse(LineReader *fromReader,
                           const nlohmann::json &attrPath, Proc *proc,
                           nix::Sync<State> &state_,
                           CacheStatusResolver *cacheStatusResolver)
    -> std::vector<nlohmann::json> {
    // Read response from worker
    auto respString = fromReader->readLine();
    if (respString.empty()) {
        auto msg =
            "reading result for attrPath '" + joinAttrPath(attrPath) + "'";
        handleBrokenWorkerPipe(*proc, msg);
    }

    // Parse and deserialize the typed response
    nlohmann::json jsonResponse;
    try {
        jsonResponse = nlohmann::json::parse(respString);
    } catch (const nlohmann::json::exception &e) {
        throw nix::Error("Received invalid JSON from worker: %s\n json: '%s'",
                         e.what(), respString);
    }
    auto response = jsonResponse.get<Response>();

    // Dispatch on the response payload
    std::vector<nlohmann::json> newAttrs;
    if (auto *attrs = std::get_if<Response::Attrs>(&response.payload)) {
        for (const auto &attr : attrs->attrs) {
            nlohmann::json newAttr(response.attrPath);
            newAttr.emplace_back(attr);
            newAttrs.push_back(newAttr);
        }
    } else if ((cacheStatusResolver != nullptr) &&
               std::holds_alternative<Response::Job>(response.payload)) {
        // The resolver fills in cacheStatus, then records/prints the job
        // via its sink.
        cacheStatusResolver->push(std::move(response));
    } else {
        emitResponse(state_, response, std::move(jsonResponse), respString);

        if (auto *error = std::get_if<Response::Error>(&response.payload);
            (error != nullptr) && error->fatal) {
            throw nix::Error("%s", error->error);
        }
    }

    return newAttrs;
}

void updateJobQueue(nix::Sync<State> &state_, std::condition_variable &wakeup,
                    const nlohmann::json &attrPath,
                    const std::vector<nlohmann::json> &newAttrs) {
    auto state(state_.lock());
    state->active.erase(attrPath);
    for (const auto &newAttr : newAttrs) {
        state->todo.insert(newAttr);
    }
    wakeup.notify_all();
}
} // namespace

/* Rationale for the separate resolver: see CacheStatusResolver. */
auto makeCacheStatusResolver(const MyArgs &args, nix::Sync<State> &state_)
    -> std::optional<CacheStatusResolver> {
    if (!args.checkCacheStatus) {
        return std::nullopt;
    }
    return std::optional<CacheStatusResolver>(
        std::in_place, nix_eval_jobs::openStore(args.evalStoreUrl),
        [&state_](const Response &response) -> void {
            nlohmann::json jsonResponse = response;
            auto dumped = jsonResponse.dump();
            emitResponse(state_, response, std::move(jsonResponse), dumped);
        });
}

void collector(nix::Sync<State> &state_, std::condition_variable &wakeup,
               CacheStatusResolver *cacheStatusResolver) {
    try {
        std::unique_ptr<Proc> proc;
        std::unique_ptr<LineReader> fromReader;

        while (true) {
            // Claim a job before forking so over-provisioned workers stay idle
            auto maybeAttrPath = getNextJob(state_, wakeup);
            if (!maybeAttrPath.has_value()) {
                /* A worker whose pending status line is MSG_RESTART
                   has already closed its pipe and is exiting on its
                   own. */
                if (proc &&
                    checkWorkerStatus(fromReader.get(), proc.get()) !=
                        MSG_RESTART &&
                    tryWriteLine(proc->to.get(), std::string(MSG_EXIT)) < 0) {
                    handleBrokenWorkerPipe(*proc, "sending exit");
                }
                return;
            }
            const auto &attrPath = maybeAttrPath.value();

            // Ensure we have a worker that is ready for a job
            while (true) {
                if (!proc) {
                    proc = std::make_unique<Proc>(worker);
                    fromReader =
                        std::make_unique<LineReader>(proc->from.release());
                }
                auto line = checkWorkerStatus(fromReader.get(), proc.get());
                if (line != MSG_RESTART) {
                    break;
                }
                proc.reset();
                fromReader.reset();
            }

            if (tryWriteLine(proc->to.get(),
                             std::string(MSG_DO) + attrPath.dump()) < 0) {
                auto msg = "sending attrPath '" + joinAttrPath(attrPath) + "'";
                handleBrokenWorkerPipe(*proc, msg);
            }

            auto newAttrs =
                processWorkerResponse(fromReader.get(), attrPath, proc.get(),
                                      state_, cacheStatusResolver);

            updateJobQueue(state_, wakeup, attrPath, newAttrs);
        }
    } catch (...) {
        auto state(state_.lock());
        state->exc = std::current_exception();
        wakeup.notify_all();
    }
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
    /* We are doing the garbage collection by killing forks */
    setenv("GC_DONT_GC", "1", 1); // NOLINT(concurrency-mt-unsafe)

    /* Because of an objc quirk[1], calling curl_global_init for the first time
       after fork() will always result in a crash.
       Up until now the solution has been to set
       OBJC_DISABLE_INITIALIZE_FORK_SAFETY for every nix process to ignore that
       error. Instead of working around that error we address it at the core -
       by calling curl_global_init here, which should mean curl will already
       have been initialized by the time we try to do so in a forked process.

       [1]
       https://github.com/apple-oss-distributions/objc4/blob/01edf1705fbc3ff78a423cd21e03dfc21eb4d780/runtime/objc-initialize.mm#L614-L636
    */
    curl_global_init(CURL_GLOBAL_ALL);

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

        nix::Sync<State> state_;

        /* Pre-initialize the eval store (if specified) before spawning
           workers so that the SQLite database and schema are created
           exactly once.  Without this, forked workers race to create
           a fresh store and hit SQLite "busy" / "schema is corrupt"
           errors.  See https://github.com/NixOS/nix-eval-jobs/issues/401 */
        if (myArgs.evalStoreUrl.has_value()) {
            nix_eval_jobs::openStore(myArgs.evalStoreUrl);
        }

        /* The fetcher cache is opened lazily on first fetch, so forked
           workers would otherwise race to create fetcher-cache-v4.sqlite and
           fail with "unable to open database file". Open it once here. */
        nix::fetchSettings.getCache();

        auto cacheStatusResolver = makeCacheStatusResolver(myArgs, state_);
        auto *cacheStatusResolverPtr =
            cacheStatusResolver ? &*cacheStatusResolver : nullptr;

        /* Start a collector thread per worker process. */
        std::vector<Thread> threads;
        std::condition_variable wakeup;
        threads.reserve(myArgs.nrWorkers);
        for (size_t i = 0; i < myArgs.nrWorkers; i++) {
            threads.emplace_back(
                [&state_, &wakeup, cacheStatusResolverPtr] -> void {
                    collector(state_, wakeup, cacheStatusResolverPtr);
                });
        }

        for (auto &thread : threads) {
            thread.join();
        }

        /* A collector error must surface immediately: rethrowing it
           here lets the CacheStatusResolver destructor discard the
           backlog instead of finish() draining it first (which would
           also mask the eval error with any resolver error). */
        {
            auto state(state_.lock());
            if (state->exc) {
                std::rethrow_exception(state->exc);
            }
        }

        /* All eval results are in; wait for outstanding cache-status
           checks before constituents are aggregated. */
        if (cacheStatusResolver) {
            cacheStatusResolver->finish();
        }

        auto state(state_.lock());

        if (myArgs.constituents) {
            handleConstituents(state->jobs, myArgs);
        }
    });
}
