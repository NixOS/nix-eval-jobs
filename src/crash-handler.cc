#include <boost/core/demangle.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <typeinfo>

// Darwin and FreeBSD stdenv do not define _GNU_SOURCE but do have
// _Unwind_Backtrace.
#if defined(__APPLE__) || defined(__FreeBSD__)
#define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
#endif
#include <boost/stacktrace/stacktrace.hpp>

#include "crash-handler.hh"

/* Modeled on nix's crash handler: report unhandled exceptions via
   std::set_terminate. Crash signals are left to coredumps; in-process
   signal handlers cannot produce reliable traces. */

namespace {

void onTerminate() {
    std::cerr << "nix-eval-jobs crashed. This is a bug.\n";
    try {
        if (auto exc = std::current_exception()) {
            std::rethrow_exception(exc);
        }
        std::cerr << "std::terminate() called without exception\n";
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << boost::core::demangle(typeid(e).name())
                  << ": " << e.what() << "\n";
    } catch (...) {
        std::cerr << "Unknown exception!\n";
    }
    std::cerr << "Stack trace:\n" << boost::stacktrace::stacktrace();
    std::abort();
}

} // namespace

void registerCrashHandler() { std::set_terminate(onTerminate); }
