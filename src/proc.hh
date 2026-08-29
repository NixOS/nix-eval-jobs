#pragma once
///@file

#include <exception>
#include <nix/util/file-descriptor.hh>
#include <nix/util/processes.hh>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct WorkerSpawnConfig {
    std::vector<std::string> argv;
    /* First line sent to every worker. */
    std::string config = "{}";
};

struct WorkerKilled : std::exception {};

/* A worker: our own binary spawned with --worker so it starts from a
   clean process (no inherited logger/FileTransfer state). The read side
   is non-blocking and line-buffered for use with poll(). */
class Proc {
  public:
    explicit Proc(const WorkerSpawnConfig &spawn);
    Proc(const Proc &) = delete;
    Proc(Proc &&) = delete;
    auto operator=(const Proc &) -> Proc & = delete;
    auto operator=(Proc &&) -> Proc & = delete;
    ~Proc() = default;

    [[nodiscard]] auto pid() const -> pid_t { return pid_; }
    [[nodiscard]] auto readFd() const -> int { return from.get(); }

    /* False if the worker is gone. */
    [[nodiscard]] auto sendLine(std::string_view line) -> bool;

    /* Read what is available. False on EOF. */
    [[nodiscard]] auto fill() -> bool;
    auto popLine() -> std::optional<std::string>;

    /* Reap a worker whose pipe closed and throw a matching error, or
       WorkerKilled for SIGKILL. */
    [[noreturn]] void throwExited(std::string_view doing);

  private:
    nix::AutoCloseFD to;
    nix::AutoCloseFD from;
    nix::Pid proc;
    pid_t pid_ = -1;
    std::string buf;
    size_t scanned = 0;
};
