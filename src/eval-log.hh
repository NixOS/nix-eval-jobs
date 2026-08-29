#pragma once
///@file

#include <nix/util/error.hh>
#include <nix/util/logging.hh>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/* Wraps the worker's logger to record `builtins.warn` and
   `builtins.trace` messages of the attribute currently being evaluated,
   so the collector can attach them to that attribute's JSON line.
   Everything is still forwarded to stderr unchanged. */
class EvalLogCapture : public nix::Logger {
  public:
    struct Logs {
        std::vector<std::string> warnings;
        std::vector<std::string> traces;
        bool operator==(const Logs &) const = default;
    };

    explicit EvalLogCapture(nix::Logger *inner);

    /* Install as nix::logger and return a handle for take(). */
    static auto install() -> EvalLogCapture &;

    auto take() -> Logs;

    void stop() override { inner->stop(); }
    void pause() override { inner->pause(); }
    void resume() override { inner->resume(); }
    auto isVerbose() -> bool override { return inner->isVerbose(); }
    void log(nix::Verbosity lvl, std::string_view msg) override;
    void logEI(const nix::ErrorInfo &info) override;
    void warn(const std::string &msg) override;
    void startActivity(nix::ActivityId act, nix::Verbosity lvl,
                       nix::ActivityType type, const std::string &text,
                       const Fields &fields, nix::ActivityId parent) override {
        inner->startActivity(act, lvl, type, text, fields, parent);
    }
    void stopActivity(nix::ActivityId act) override {
        inner->stopActivity(act);
    }
    void result(nix::ActivityId act, nix::ResultType type,
                const Fields &fields) override {
        inner->result(act, type, fields);
    }
    void writeToStdout(std::string_view data) override {
        inner->writeToStdout(data);
    }
    auto ask(std::string_view msg) -> std::optional<char> override {
        return inner->ask(msg);
    }
    void setPrintBuildLogs(bool on) override { inner->setPrintBuildLogs(on); }

  private:
    nix::Logger *inner;
    Logs logs;
};
