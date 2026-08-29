#include <nix/util/error.hh>
#include <nix/util/logging.hh>
#include <nix/util/terminal.hh>
#include <string>
#include <string_view>
#include <utility>

#include "eval-log.hh"

namespace {
constexpr std::string_view TRACE_PREFIX = "trace: ";
}

EvalLogCapture::EvalLogCapture(nix::Logger *inner) : inner(inner) {}

/* Lives for the rest of the worker process, like the logger it wraps. */
auto EvalLogCapture::install() -> EvalLogCapture & {
    auto *self = new EvalLogCapture(
        nix::logger); // NOLINT(cppcoreguidelines-owning-memory)
    nix::logger = self;
    return *self;
}

auto EvalLogCapture::take() -> Logs { return std::exchange(logs, {}); }

/* builtins.trace → printError("trace: …") */
void EvalLogCapture::log(nix::Verbosity lvl, std::string_view msg) {
    auto plain = nix::filterANSIEscapes(msg, true);
    if (plain.starts_with(TRACE_PREFIX)) {
        logs.traces.push_back(plain.substr(TRACE_PREFIX.size()));
    }
    inner->log(lvl, msg);
}

/* builtins.warn → logWarning(ErrorInfo{lvlWarn, …}) */
void EvalLogCapture::logEI(const nix::ErrorInfo &info) {
    if (info.level == nix::lvlWarn) {
        logs.warnings.push_back(nix::filterANSIEscapes(info.msg.str(), true));
    }
    inner->logEI(info);
}

/* nix::warn(), e.g. deprecated settings or impure fetches */
void EvalLogCapture::warn(const std::string &msg) {
    logs.warnings.push_back(nix::filterANSIEscapes(msg, true));
    inner->warn(msg);
}
