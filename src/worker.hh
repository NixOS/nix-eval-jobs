#pragma once
///@file

#include "eval-args.hh"

class MyArgs;

namespace nix {
class AutoCloseFD;
class Bindings;
class EvalState;
template <typename T> class ref;
} // namespace nix

void worker(MyArgs &args, nix::AutoCloseFD &toParent,
            nix::AutoCloseFD &fromParent);

/// Load and return the root value (flake or file, with --select applied).
/// Exposed so the main process can pre-warm the store before forking workers.
auto initializeRootValue(const nix::ref<nix::EvalState> &state,
                         nix::Bindings &autoArgs, MyArgs &args)
    -> nix::Value *;
