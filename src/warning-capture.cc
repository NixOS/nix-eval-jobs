#include "warning-capture.hh"
#include <nix/util/error.hh>
#include <nix/util/logging.hh>
// NOLINTNEXTLINE(misc-include-cleaner)
#include <nix/util/position.hh>
#include <nix/util/terminal.hh>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nix_eval_jobs {

WarningCapturingLogger::WarningCapturingLogger(
    std::unique_ptr<nix::Logger> delegate)
    : delegate(std::move(delegate)) {}

void WarningCapturingLogger::stop() { delegate->stop(); }

void WarningCapturingLogger::pause() { delegate->pause(); }

void WarningCapturingLogger::resume() { delegate->resume(); }

auto WarningCapturingLogger::isVerbose() -> bool {
    return delegate->isVerbose();
}

void WarningCapturingLogger::log(nix::Verbosity lvl, std::string_view msg) {
    delegate->log(lvl, msg);
}

void WarningCapturingLogger::logEI(const nix::ErrorInfo &errInfo) {
    // Capture warnings from builtins.warn
    if (errInfo.level == nix::lvlWarn) {
        nlohmann::json warning;
        warning["msg"] = errInfo.msg.str();

        {
            const std::scoped_lock lock(mutex);
            warnings.push_back(std::move(warning));
        }
    }

    // Always delegate to original logger
    delegate->logEI(errInfo);
}

void WarningCapturingLogger::warn(const std::string &msg) {
    delegate->warn(msg);
}

void WarningCapturingLogger::startActivity(
    nix::ActivityId act, nix::Verbosity lvl, nix::ActivityType type,
    const std::string &msg, const Fields &fields, nix::ActivityId parent) {
    delegate->startActivity(act, lvl, type, msg, fields, parent);
}

void WarningCapturingLogger::stopActivity(nix::ActivityId act) {
    delegate->stopActivity(act);
}

void WarningCapturingLogger::result(nix::ActivityId act, nix::ResultType type,
                                    const Fields &fields) {
    delegate->result(act, type, fields);
}

void WarningCapturingLogger::writeToStdout(std::string_view msg) {
    delegate->writeToStdout(msg);
}

auto WarningCapturingLogger::ask(std::string_view msg) -> std::optional<char> {
    return delegate->ask(msg);
}

void WarningCapturingLogger::setPrintBuildLogs(bool printBuildLogs) {
    delegate->setPrintBuildLogs(printBuildLogs);
}

auto WarningCapturingLogger::takeWarnings() -> nlohmann::json {
    const std::scoped_lock lock(mutex);
    auto result = nlohmann::json(warnings);
    warnings.clear();
    return result;
}

void WarningCapturingLogger::attachTracesToLastWarning(
    const nix::ErrorInfo &errInfo) {
    if (errInfo.traces.empty()) {
        return;
    }

    const std::scoped_lock lock(mutex);
    if (warnings.empty()) {
        return;
    }

    auto &lastWarning = warnings.back();
    // Only add trace if this warning doesn't have one yet
    if (lastWarning.contains("trace")) {
        return;
    }

    auto traces = nlohmann::json::array();
    for (const auto &trace : errInfo.traces) {
        nlohmann::json traceJson;
        traceJson["msg"] = nix::filterANSIEscapes(trace.hint.str(), true);
        if (trace.pos && *trace.pos) {
            traceJson["line"] = trace.pos->line;
            traceJson["column"] = trace.pos->column;
            // Extract just the file path from origin
            if (auto path = trace.pos->getSourcePath()) {
                traceJson["file"] = path->to_string();
            }
        }
        traces.push_back(std::move(traceJson));
    }
    lastWarning["trace"] = std::move(traces);
}

auto installWarningCapturingLogger() -> WarningCapturingLogger * {
    auto capturingLogger =
        std::make_unique<WarningCapturingLogger>(std::move(nix::logger));
    auto *ptr = capturingLogger.get();
    nix::logger = std::move(capturingLogger);
    return ptr;
}

} // namespace nix_eval_jobs
