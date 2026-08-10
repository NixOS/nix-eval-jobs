#pragma once
///@file

#include <string_view>

#include "eval-args.hh"

class MyArgs;

namespace nix {
class AutoCloseFD;
class Bindings;
class EvalState;
template <typename T> class ref;
} // namespace nix

/* Messages between eval worker and collector */
constexpr std::string_view MSG_NEXT = "next";
constexpr std::string_view MSG_RESTART = "restart";
constexpr std::string_view MSG_EXIT = "exit";
constexpr std::string_view MSG_DO = "do ";

/* Pipe ends inherited by the exec'ed worker process. */
constexpr int WORKER_OUT_FD = 3; // worker -> collector
constexpr int WORKER_IN_FD = 4;  // collector -> worker

void worker(MyArgs &args, nix::AutoCloseFD &toParent,
            nix::AutoCloseFD &fromParent);
