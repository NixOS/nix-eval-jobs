#include <filesystem>
#include <gtest/gtest.h>

#include <nix/util/tests/characterization.hh>
#include <nix/util/tests/json-characterization.hh>
#include <nix/util/tests/test-data.hh>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string_view>
#include <utility>

#include "drv.hh"
#include "response.hh"

using nlohmann::json;

/* --------------------------------------------------------------------------
 * Constituents
 * --------------------------------------------------------------------------*/

class ConstituentsTest : public virtual nix::CharacterizationTest {
    std::filesystem::path unitTestData =
        nix::getUnitTestData() / "constituents";

  public:
    [[nodiscard]] auto goldenMaster(std::string_view testStem) const
        -> std::filesystem::path override {
        return unitTestData / testStem;
    }
};

struct ConstituentsJsonTest : ConstituentsTest,
                              nix::JsonCharacterizationTest<Constituents> {};

// NOLINTBEGIN(google-readability-avoid-underscore-in-googletest-name)

TEST_F(ConstituentsJsonTest, roundTrip) {
    Constituents const val{
        .constituents = {"/nix/store/abc-foo.drv", "/nix/store/def-bar.drv"},
        .namedConstituents = {"foo", "bar"},
        .globConstituents = true,
    };
    readJsonTest("full", val);
    writeJsonTest("full", val);
}

/* --------------------------------------------------------------------------
 * Drv
 * --------------------------------------------------------------------------*/

class DrvTest : public virtual nix::CharacterizationTest {
    std::filesystem::path unitTestData = nix::getUnitTestData() / "drv";

  public:
    [[nodiscard]] auto goldenMaster(std::string_view testStem) const
        -> std::filesystem::path override {
        return unitTestData / testStem;
    }
};

struct DrvJsonTest
    : DrvTest,
      nix::JsonCharacterizationTest<Drv>,
      ::testing::WithParamInterface<std::pair<std::string_view, Drv>> {};

TEST_P(DrvJsonTest, fromJson) {
    const auto &[name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(DrvJsonTest, toJson) {
    const auto &[name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    DrvJSON, DrvJsonTest,
    ::testing::Values(
        std::pair{
            "minimal",
            Drv{
                .name = "hello",
                .storeDir = "/nix/store",
                .system = "x86_64-linux",
                .drvPath = {"00000000000000000000000000000000-hello.drv"},
                .outputs = {{
                    "out",
                    nix::StorePath{"00000000000000000000000000000000-hello"},
                }},
                .neededBuilds = {},
                .neededSubstitutes = {},
                .unknownPaths = {},
                .constituents = {},
            },
        },
        std::pair{
            "cached",
            Drv{
                .name = "cached-pkg",
                .storeDir = "/nix/store",
                .system = "x86_64-linux",
                .drvPath = {"00000000000000000000000000000000-pkg.drv"},
                .outputs = {{
                    "out",
                    nix::StorePath{"00000000000000000000000000000000-def"},
                }},
                .neededBuilds = {},
                .neededSubstitutes =
                    {
                        {"00000000000000000000000000000000-sub1"},
                        {"00000000000000000000000000000000-sub2"},
                    },
                .unknownPaths = {},
                .cacheStatus = Drv::CacheStatus::Cached,
                .constituents = {},
            },
        },
        std::pair{
            "not-built",
            Drv{
                .name = "unbuilt-pkg",
                .storeDir = "/nix/store",
                .system = "x86_64-linux",
                .drvPath = {"00000000000000000000000000000000-pkg.drv"},
                .outputs = {{"out", std::nullopt}},
                .neededBuilds =
                    {
                        {"00000000000000000000000000000001-build1.drv"},
                    },
                .neededSubstitutes = {},
                .unknownPaths = {},
                .cacheStatus = Drv::CacheStatus::NotBuilt,
                .constituents = {},
            },
        },
        std::pair{
            "all-fields",
            Drv{
                .name = "full-pkg",
                .storeDir = "/nix/store",
                .system = "x86_64-linux",
                .drvPath = {"00000000000000000000000000000000-pkg.drv"},
                .outputs =
                    {
                        {
                            "out",
                            nix::StorePath{
                                "00000000000000000000000000000000-def"},
                        },
                        {
                            "dev",
                            nix::StorePath{
                                "00000000000000000000000000000000-ghi"},
                        },
                    },
                .inputDrvs = {{
                    {
                        {"00000000000000000000000000000000-input.drv"},
                        {"out"},
                    },
                }},
                .requiredSystemFeatures =
                    nix::StringSet{
                        "big-parallel",
                        "kvm",
                    },
                .neededBuilds = {},
                .neededSubstitutes = {},
                .unknownPaths = {},
                .cacheStatus = Drv::CacheStatus::Local,
                .meta =
                    json{
                        {"description", "A package"},
                        {"license", "MIT"},
                    },
                .constituents =
                    {
                        .constituents = {"/nix/store/child.drv"},
                        .namedConstituents = {"childJob"},
                        .globConstituents = false,
                    },
            },
        }));

/* --------------------------------------------------------------------------
 * Response
 * --------------------------------------------------------------------------*/

class ResponseTest : public virtual nix::CharacterizationTest {
    std::filesystem::path unitTestData = nix::getUnitTestData() / "response";

  public:
    [[nodiscard]] auto goldenMaster(std::string_view testStem) const
        -> std::filesystem::path override {
        return unitTestData / testStem;
    }
};

struct ResponseJsonTest
    : ResponseTest,
      nix::JsonCharacterizationTest<Response>,
      ::testing::WithParamInterface<std::pair<std::string_view, Response>> {};

TEST_P(ResponseJsonTest, fromJson) {
    const auto &[name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(ResponseJsonTest, toJson) {
    const auto &[name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    ResponseJSON, ResponseJsonTest,
    ::testing::Values(
        std::pair{
            "job",
            Response{
                .attr = "hello",
                .attrPath = {"hello"},
                .payload =
                    Response::Job{
                        .drv =
                            Drv{
                                .name = "hello",
                                .storeDir = "/nix/store",
                                .system = "x86_64-linux",
                                .drvPath = {"00000000000000000000000000000000-"
                                            "hello.drv"},
                                .outputs = {{"out",
                                             nix::StorePath{
                                                 "00000000000000000000000000000"
                                                 "000-hello",
                                             }}},
                                .neededBuilds = {},
                                .neededSubstitutes = {},
                                .unknownPaths = {},
                                .constituents = {},
                            },
                        .extraValue = std::nullopt,
                    },
            },
        },
        std::pair{
            "job-with-extra-value",
            Response{
                .attr = "pkg",
                .attrPath = {"pkg"},
                .payload =
                    Response::Job{
                        .drv =
                            Drv{
                                .name = "pkg",
                                .storeDir = "/nix/store",
                                .system = "x86_64-linux",
                                .drvPath = {"00000000000000000000000000000000-"
                                            "pkg.drv"},
                                .outputs = {{
                                    "out",
                                    nix::StorePath{
                                        "00000000000000000000000000000"
                                        "000-def"},
                                }},
                                .neededBuilds = {},
                                .neededSubstitutes = {},
                                .unknownPaths = {},
                                .constituents = {},
                            },
                        .extraValue =
                            json{
                                {"version", "1.0"},
                                {"custom", true},
                            },
                    },
            },
        },
        std::pair{
            "attrs",
            Response{
                .attr = "pkgs",
                .attrPath = {"pkgs"},
                .payload = Response::Attrs{.attrs = {"foo", "bar", "baz"}},
            },
        },
        std::pair{
            "error",
            Response{
                .attr = "broken",
                .attrPath = {"broken"},
                .payload = Response::Error{.error = "evaluation failed"},
            },
        },
        std::pair{
            "error-with-logs",
            Response{
                .attr = "broken",
                .attrPath = {"broken"},
                .payload = Response::Error{.error = "evaluation failed"},
                .warnings = {"deprecated", "also deprecated"},
                .traces = {"got here"},
            },
        }));

// NOLINTEND(google-readability-avoid-underscore-in-googletest-name)
