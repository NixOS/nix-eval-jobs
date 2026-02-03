#pragma once

#include <nix/util/error.hh>
#include <nix/util/logging.hh>
#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nix_eval_jobs {

/**
 * A logger that captures evaluation warnings while delegating
 * all other logging to the original logger.
 */
class WarningCapturingLogger : public nix::Logger {
  public:
    explicit WarningCapturingLogger(std::unique_ptr<nix::Logger> delegate);

    void stop() override;
    void pause() override;
    void resume() override;
    auto isVerbose() -> bool override;

    void log(nix::Verbosity lvl, std::string_view msg) override;
    void logEI(const nix::ErrorInfo &errInfo) override;
    void warn(const std::string &msg) override;

    void startActivity(nix::ActivityId act, nix::Verbosity lvl,
                       nix::ActivityType type, const std::string &msg,
                       const Fields &fields, nix::ActivityId parent) override;
    void stopActivity(nix::ActivityId act) override;
    void result(nix::ActivityId act, nix::ResultType type,
                const Fields &fields) override;
    void writeToStdout(std::string_view msg) override;
    auto ask(std::string_view msg) -> std::optional<char> override;
    void setPrintBuildLogs(bool printBuildLogs) override;

    /**
     * Clear all captured warnings and return them.
     * Thread-safe.
     */
    auto takeWarnings() -> nlohmann::json;

    /**
     * Attach trace information from a caught error to the last warning.
     * Used when abort-on-warn is set: the error following a warning
     * contains position info in its traces.
     * Thread-safe.
     */
    void attachTracesToLastWarning(const nix::ErrorInfo &errInfo);

  private:
    std::unique_ptr<nix::Logger> delegate;
    std::mutex mutex;
    std::vector<nlohmann::json> warnings;
};

/**
 * Install a warning-capturing logger as the global nix::logger.
 * Returns a pointer to the installed logger for later retrieval of warnings.
 */
auto installWarningCapturingLogger() -> WarningCapturingLogger *;

} // namespace nix_eval_jobs
