// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants these headers rather than the C++ versions
#include <signal.h>
#include <string.h>
// NOLINTEND(modernize-deprecated-headers)
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <nix/util/current-process.hh>
#include <nix/util/error.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/processes.hh>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "buffered-io.hh"
#include "proc.hh"
#include "worker.hh"

/* Not declared by <unistd.h> on all platforms. */
extern "C" char **environ; // NOLINT(readability-redundant-declaration)

Proc::Proc(const WorkerSpawnConfig &spawn) {
    nix::Pipe toPipe;
    nix::Pipe fromPipe;
    toPipe.create();
    fromPipe.create();

    const std::string selfExe =
        nix::getSelfExe().value_or(spawn.argv.front()).string();
    std::vector<std::string> args = spawn.argv;
    args.emplace_back("--worker");
    std::vector<char *> execArgv;
    execArgv.reserve(args.size() + 1);
    for (auto &arg : args) {
        execArgv.push_back(arg.data());
    }
    execArgv.push_back(nullptr);

    /* Dup above the target fds so the adddup2 sources cannot collide
       with WORKER_OUT_FD/WORKER_IN_FD. */
    constexpr int minFreeFd = WORKER_IN_FD + 1;
    const nix::AutoCloseFD outFd(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        fcntl(fromPipe.writeSide.get(), F_DUPFD, minFreeFd));
    const nix::AutoCloseFD inFd(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        fcntl(toPipe.readSide.get(), F_DUPFD, minFreeFd));
    if (!outFd || !inFd) {
        throw nix::SysError("duplicating worker pipe fds");
    }

    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_init(&fileActions);
    posix_spawn_file_actions_adddup2(&fileActions, outFd.get(), WORKER_OUT_FD);
    posix_spawn_file_actions_adddup2(&fileActions, inFd.get(), WORKER_IN_FD);
    pid_t childPid = -1;
    const int err = posix_spawn(&childPid, selfExe.c_str(), &fileActions,
                                nullptr, execArgv.data(), environ);
    posix_spawn_file_actions_destroy(&fileActions);
    if (err != 0) {
        throw nix::SysError(err, "spawning worker process");
    }
    proc = childPid;
    pid_ = childPid;

    to = std::move(toPipe.writeSide);
    from = std::move(fromPipe.readSide);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (fcntl(from.get(), F_SETFL, O_NONBLOCK) == -1) {
        throw nix::SysError("making worker pipe non-blocking");
    }
    if (!sendLine(spawn.config)) {
        throw nix::SysError("sending config to worker process");
    }
}

auto Proc::sendLine(std::string_view line) -> bool {
    return tryWriteLine(to.get(), std::string(line)) == 0;
}

auto Proc::fill() -> bool {
    static constexpr size_t CHUNK = 64 * 1024;
    while (true) {
        const size_t old = buf.size();
        buf.resize(old + CHUNK);
        const ssize_t n = ::read(from.get(), buf.data() + old, CHUNK);
        if (n > 0) {
            buf.resize(old + static_cast<size_t>(n));
            continue;
        }
        buf.resize(old);
        if (n == 0) {
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        if (errno != EINTR) {
            throw nix::SysError("reading from worker");
        }
    }
}

auto Proc::popLine() -> std::optional<std::string> {
    const auto nl = buf.find('\n', scanned);
    if (nl == std::string::npos) {
        scanned = buf.size();
        return std::nullopt;
    }
    std::string line = buf.substr(0, nl);
    buf.erase(0, nl + 1);
    scanned = 0;
    return line;
}

void Proc::throwExited(std::string_view doing) {
    // NOLINTNEXTLINE(misc-include-cleaner)
    const pid_t child = proc.release();
    int status = 0;
    // Pipe EOF can be observed before a SIGKILLed worker is reapable.
    int result = waitpid(child, &status, WNOHANG);
    if (result == 0) {
        kill(child, SIGKILL);
        result = waitpid(child, &status, 0);
    }
    if (result == -1) {
        throw nix::SysError("while %s, waitpid for evaluation worker failed",
                            doing);
    }
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGKILL) {
            throw nix::Error("while %s, evaluation worker got killed by "
                             "SIGKILL, maybe memory limit reached?",
                             doing);
        }
        throw nix::Error("while %s, evaluation worker crashed with signal %d "
                         "(%s); enable coredumps for a backtrace",
                         doing, WTERMSIG(status),
                         // NOLINTNEXTLINE(concurrency-mt-unsafe)
                         strsignal(WTERMSIG(status)));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
        throw nix::Error("while %s, evaluation worker exited with exit code "
                         "1 (possible infinite recursion)",
                         doing);
    }
    throw nix::Error("while %s, evaluation worker exited with %d", doing,
                     WEXITSTATUS(status));
}
