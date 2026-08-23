#pragma once
///@file

#include <exception>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <nix/util/fmt.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/util/ref.hh>
#include <nix/util/types.hh>

struct DependencyCycle : public std::exception {
    std::string a;
    std::string b;
    std::set<std::string> remainingAggregates;

    DependencyCycle(std::string nodeA, std::string nodeB,
                    const std::set<std::string> &remainingAggregates)
        : a(std::move(nodeA)), b(std::move(nodeB)),
          remainingAggregates(remainingAggregates) {}

    [[nodiscard]] auto message() const -> std::string {
        return nix::fmt("Dependency cycle: %s <-> %s", a, b);
    }
};

struct AggregateJob {
    std::string name;
    std::set<std::string> dependencies;
    std::unordered_map<std::string, std::string> brokenJobs;

    auto operator<(const AggregateJob &other) const -> bool {
        return name < other.name;
    }
};

auto resolveNamedConstituents(const std::map<std::string, nlohmann::json> &jobs)
    -> std::variant<std::vector<AggregateJob>, DependencyCycle>;

void rewriteAggregates(std::map<std::string, nlohmann::json> &jobs,
                       const std::vector<AggregateJob> &aggregateJobs,
                       const nix::ref<nix::Store> &store,
                       const std::filesystem::path &gcRootsDir);
