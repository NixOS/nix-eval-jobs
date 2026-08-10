#pragma once
///@file

#include <string>
#include <filesystem>

#include <nix/flake/flake.hh>
#include <nix/util/args/root.hh>
#include <nix/cmd/common-eval-args.hh>
#include <cstddef>
#include <nix/main/common-args.hh>
#include <nix/util/types.hh>

class MyArgs : virtual public nix::MixEvalArgs,
               virtual public nix::MixCommonArgs,
               virtual public nix::RootArgs {
  public:
    static constexpr size_t DEFAULT_MAX_MEMORY_SIZE = 4096;

    virtual ~MyArgs() = default;
    std::string releaseExpr;
    std::string applyExpr;
    std::string selectExpr;
    std::filesystem::path gcRootsDir;
    bool flake = false;
    bool fromArgs = false;
    bool meta = false;
    bool showTrace = false;
    bool impure = false;
    bool forceRecurse = false;
    bool checkCacheStatus = false;
    bool showInputDrvs = false;
    bool constituents = false;
    bool noInstantiate = false;
    size_t nrWorkers = 1;
    size_t maxMemorySize = DEFAULT_MAX_MEMORY_SIZE;

    // usually in MixFlakeOptions
    nix::flake::LockFlags lockFlags = {.updateLockFile = false,
                                       .writeLockFile = false,
                                       .useRegistries = false,
                                       .allowUnlocked = false,
                                       .referenceLockFilePath = {},
                                       .outputLockFilePath = {},
                                       .inputOverrides = {},
                                       .inputUpdates = {}};
    MyArgs();
    MyArgs(MyArgs &&) = delete;
    auto operator=(const MyArgs &) -> MyArgs & = default;
    auto operator=(MyArgs &&) -> MyArgs & = delete;
    MyArgs(const MyArgs &) = delete;

    void parseArgs(char **argv, int argc);
};
