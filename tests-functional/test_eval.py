#!/usr/bin/env python3

import contextlib
import functools
import http.server
import json
import os
import shutil
import subprocess
import threading
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

import pytest

TEST_ROOT = Path(__file__).parent.resolve()
PROJECT_ROOT = TEST_ROOT.parent
# Allow overriding the binary path with environment variable
BIN = Path(
    os.environ.get("NIX_EVAL_JOBS_BIN", str(PROJECT_ROOT.joinpath("build", "src", "nix-eval-jobs")))
)
# Common flags for all test invocations
COMMON_FLAGS = ["--extra-experimental-features", "nix-command flakes"]


def check_gc_root(gcRootDir: str, drvPath: str) -> None:
    """
    Make sure the expected GC root exists in the given dir
    """
    link_name = os.path.basename(drvPath)
    symlink_path = os.path.join(gcRootDir, link_name)
    assert os.path.islink(symlink_path) and drvPath == os.readlink(symlink_path)


def common_test(extra_args: list[str]) -> list[dict[str, Any]]:
    with TemporaryDirectory() as tempdir:
        cmd = [str(BIN), "--gc-roots-dir", tempdir, "--meta", *COMMON_FLAGS, *extra_args]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 4

        built_job = results[0]
        assert built_job["attr"] == "builtJob"
        assert built_job["name"] == "job1"
        assert built_job["outputs"]["out"].endswith("-job1")
        assert built_job["drvPath"].endswith(".drv")
        # No meta field in bare derivations

        dotted_job = results[1]
        assert dotted_job["attr"] == '"dotted.attr"'
        assert dotted_job["attrPath"] == ["dotted.attr"]

        package_with_deps = results[2]
        assert package_with_deps["attr"] == "package-with-deps"
        assert package_with_deps["name"] == "package-with-deps"

        recurse_drv = results[3]
        assert recurse_drv["attr"] == "recurse.drvB"
        assert recurse_drv["name"] == "drvB"

        assert len(list(Path(tempdir).iterdir())) == 4
        return results


def test_flake() -> None:
    results = common_test(["--flake", ".#hydraJobs"])
    for result in results:
        assert "isCached" not in result  # legacy
        assert "cacheStatus" not in result
        assert "neededBuilds" not in result
        assert "neededSubstitutes" not in result
        assert "requiredSystemFeatures" in result


def test_query_cache_status() -> None:
    results = common_test(["--flake", ".#hydraJobs", "--check-cache-status"])
    # FIXME in the nix sandbox we cannot query binary caches
    # this would need some local one
    for result in results:
        assert "isCached" in result  # legacy
        assert "cacheStatus" in result
        assert "neededBuilds" in result
        assert "neededSubstitutes" in result


def test_expression() -> None:
    results = common_test(["ci.nix"])
    for result in results:
        assert "isCached" not in result  # legacy
        assert "cacheStatus" not in result
        assert "requiredSystemFeatures" in result
        if result["attr"] == "builtJob":
            assert isinstance(result["requiredSystemFeatures"], list)
            assert "big-parallel" in result["requiredSystemFeatures"]

    with open(TEST_ROOT.joinpath("assets/ci.nix")) as ci_nix:
        common_test(["-E", ci_nix.read()])


def test_input_drvs() -> None:
    results = common_test(["ci.nix", "--show-input-drvs"])
    for result in results:
        assert "inputDrvs" in result


def test_eval_error() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.brokenPkgs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        attrs = json.loads(res.stdout)
        assert attrs["attr"] == "brokenPackage"
        assert "this is an evaluation error" in attrs["error"]


def test_no_gcroot_dir() -> None:
    cmd = [
        str(BIN),
        "--meta",
        "--workers",
        "1",
        *COMMON_FLAGS,
        "--flake",
        ".#legacyPackages.x86_64-linux.brokenPkgs",
    ]
    res = subprocess.run(
        cmd,
        cwd=TEST_ROOT.joinpath("assets"),
        text=True,
        stdout=subprocess.PIPE,
    )
    print(res.stdout)
    attrs = json.loads(res.stdout)
    assert attrs["attr"] == "brokenPackage"
    assert "this is an evaluation error" in attrs["error"]


def test_constituents() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.success",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 4
        child = results[0]
        assert child["attr"] == "anotherone"
        direct = results[1]
        assert direct["attr"] == "direct_aggregate"
        indirect = results[2]
        assert indirect["attr"] == "indirect_aggregate"
        mixed = results[3]
        assert mixed["attr"] == "mixed_aggregate"

        def absent_or_empty(f: str, d: dict) -> bool:
            return f not in d or len(d[f]) == 0

        assert absent_or_empty("namedConstituents", direct)
        assert absent_or_empty("namedConstituents", indirect)
        assert absent_or_empty("namedConstituents", mixed)

        assert direct["constituents"][0].endswith("-job1.drv")

        assert indirect["constituents"][0] == child["drvPath"]

        assert mixed["constituents"][0].endswith("-job1.drv")
        assert mixed["constituents"][1] == child["drvPath"]

        assert "error" not in direct
        assert "error" not in indirect
        assert "error" not in mixed

        check_gc_root(tempdir, direct["drvPath"])
        check_gc_root(tempdir, indirect["drvPath"])
        check_gc_root(tempdir, mixed["drvPath"])


def test_constituents_all() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.glob1",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 3
        assert [x["name"] for x in results] == [
            "constituentA",
            "constituentB",
            "aggregate",
        ]
        aggregate = results[2]
        assert len(aggregate["constituents"]) == 2
        assert aggregate["constituents"][0].endswith("constituentA.drv")
        assert aggregate["constituents"][1].endswith("constituentB.drv")


def test_constituents_glob_misc() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.glob2",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 6
        assert [x["name"] for x in results] == [
            "constituentA",
            "constituentB",
            "aggregate0",
            "aggregate1",
            "indirect_aggregate0",
            "mix_aggregate0",
        ]
        aggregate = results[2]
        assert len(aggregate["constituents"]) == 2
        assert aggregate["constituents"][0].endswith("constituentA.drv")
        assert aggregate["constituents"][1].endswith("constituentB.drv")
        aggregate = results[4]
        assert len(aggregate["constituents"]) == 1
        assert aggregate["constituents"][0].endswith("aggregate0.drv")
        failed = results[3]
        assert "constituents" in failed
        assert failed["error"] == "tests.*: constituent glob pattern had no matches\n"

        assert results[4]["constituents"][0] == results[2]["drvPath"]
        assert results[5]["constituents"][0] == results[0]["drvPath"]
        assert results[5]["constituents"][1] == results[2]["drvPath"]


def test_constituents_cycle() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.cycle",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 2
        assert [x["name"] for x in results] == ["aggregate0", "aggregate1"]
        for i in results:
            assert i["error"] == "Dependency cycle: aggregate0 <-> aggregate1"


def test_constituents_error() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.failures",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 2
        child = results[0]
        assert child["attr"] == "doesnteval"
        assert "error" in child
        aggregate = results[1]
        assert aggregate["attr"] == "aggregate"
        assert "namedConstituents" not in aggregate
        assert "doesntexist: does not exist\n" in aggregate["error"]
        assert "constituents" in aggregate


def test_empty_needed() -> None:
    """Test for issue #369 where neededBuilds and neededSubstitutes are empty when they shouldn't be"""
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--check-cache-status",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.emptyNeeded",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]

        # Should be 3 results - nginx, proxyWrapper, and webService
        assert len(results) == 3

        # Find the results for each attr
        web_service_result = next(r for r in results if r["attr"] == "webService")
        proxy_wrapper_result = next(r for r in results if r["attr"] == "proxyWrapper")
        nginx_result = next(r for r in results if r["attr"] == "nginx")

        # webService should have proxyWrapper.drv in its neededBuilds
        assert len(web_service_result["neededBuilds"]) > 0
        assert any(
            proxy_wrapper_result["drvPath"] in drv for drv in web_service_result["neededBuilds"]
        )

        # proxyWrapper should have nginx in its neededBuilds (since nginx is a derivation dependency)
        assert len(proxy_wrapper_result["neededBuilds"]) > 0
        assert any(nginx_result["drvPath"] in drv for drv in proxy_wrapper_result["neededBuilds"])


def test_apply() -> None:
    with TemporaryDirectory() as tempdir:
        applyExpr = """drv: {
            the-name = drv.name;
            version = drv.version or null;
        }"""

        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--workers",
            "1",
            "--apply",
            applyExpr,
            *COMMON_FLAGS,
            "--flake",
            ".#hydraJobs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]

        assert len(results) == 4  # sanity check that we assert against all jobs

        # Check that nix-eval-jobs applied the expression correctly
        # and extracted 'version' as 'version' and 'name' as 'the-name'
        assert results[0]["extraValue"]["the-name"] == "job1"
        assert results[0]["extraValue"]["version"] is None
        assert results[1]["extraValue"]["the-name"] == "dotted"
        assert results[1]["extraValue"]["version"] is None
        assert results[2]["extraValue"]["the-name"] == "package-with-deps"
        assert results[2]["extraValue"]["version"] is None
        assert results[3]["extraValue"]["the-name"] == "drvB"
        assert results[3]["extraValue"]["version"] is None


def test_select_flake() -> None:
    """Test the --select option to filter flake outputs before evaluation"""
    with TemporaryDirectory() as tempdir:
        # Test 1: Select specific attributes from hydraJobs
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            *COMMON_FLAGS,
            "--flake",
            ".#hydraJobs",
            "--select",
            "outputs: { inherit (outputs) builtJob recurse; }",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        # Should only have the two selected jobs
        assert len(results) == 2
        attrs = {r["attr"] for r in results}
        assert attrs == {"builtJob", "recurse.drvB"}

        # Test 2: Select from the whole flake (outputs and inputs)
        # When using --flake . we get a structure with 'outputs' and 'inputs'
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".",
            "--select",
            "flake: flake.outputs.hydraJobs",  # Select just hydraJobs from outputs
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        # Should get the 4 hydraJobs
        assert len(results) == 4
        attrs = {r["attr"] for r in results}
        assert "builtJob" in attrs
        assert '"dotted.attr"' in attrs


def test_recursion_error() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            *COMMON_FLAGS,
            "--flake",
            ".#legacyPackages.x86_64-linux.infiniteRecursionPkgs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            capture_output=True,
        )
        print(res.stdout)
        assert res.returncode == 1
        result = json.loads(res.stdout)
        assert result["attr"] == "packageWithInfiniteRecursion"
        assert result["fatal"] is True
        assert "max-call-depth exceeded" in result["error"]


def test_no_instantiate_mode() -> None:
    """Test that --no-instantiate flag works correctly"""
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--no-instantiate",
            *COMMON_FLAGS,
            "--flake",
            ".#hydraJobs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 4

        # Check that all results have the expected structure
        for result in results:
            # In no-instantiate mode, drvPath should still be present (from the attr)
            assert "drvPath" in result
            assert result["drvPath"].endswith(".drv")

            # System should still be present (from querySystem fallback)
            assert "system" in result
            assert result["system"] != ""

            # Name should still be present
            assert "name" in result

            # Outputs should still be present and populated
            assert "outputs" in result
            assert isinstance(result["outputs"], dict)
            # Most derivations should have at least an "out" output
            if result["attr"] != "recurse.drvB":  # This one might be special
                assert len(result["outputs"]) > 0
                assert "out" in result["outputs"]
                assert result["outputs"]["out"] != ""

            # Cache status should not be present (it's Unknown and not included)
            assert "cacheStatus" not in result
            assert "neededBuilds" not in result
            assert "neededSubstitutes" not in result

            # Input drvs should not be present (requires reading derivation from store)
            assert "inputDrvs" not in result

            # Required system features should not be present (requires reading derivation from store)
            assert "requiredSystemFeatures" not in result

        # Verify specific outputs for known derivations
        built_job = next(r for r in results if r["attr"] == "builtJob")
        assert built_job["outputs"]["out"].endswith("-job1")

        # No GC roots should be created in no-instantiate mode
        assert len(list(Path(tempdir).iterdir())) == 0


# sandbox=false: NIX_STORE_DIR under /build collides with sandbox-build-dir
# when these tests run inside a Nix build.
_HERMETIC_NIX_OPTS = [
    "--extra-experimental-features",
    "nix-command flakes",
    "--option",
    "substituters",
    "",
    "--option",
    "sandbox",
    "false",
    "--option",
    "require-sigs",
    "false",
]


def _hermetic_nix_env(tmp_path: Path) -> dict[str, str]:
    """Per-test nix env that confines all stores/state/logs to tmp_path.

    Mirrors nix's own functional-test setup (tests/functional/common/vars.sh)
    so nix-collect-garbage etc. can never touch the host store.
    """
    env = {
        **os.environ,
        "HOME": str(tmp_path / "home"),
        "NIX_STORE_DIR": str(tmp_path / "store"),
        "NIX_LOCALSTATE_DIR": str(tmp_path / "var"),
        "NIX_STATE_DIR": str(tmp_path / "var/nix"),
        "NIX_LOG_DIR": str(tmp_path / "var/log/nix"),
        "NIX_CONF_DIR": str(tmp_path / "conf"),
        "NIX_DAEMON_SOCKET_PATH": str(tmp_path / "daemon-socket"),
        # Force single-user mode so the *_DIR overrides take effect; otherwise
        # macOS (and any daemon-installed Linux) routes through the system
        # daemon and writes into the host store.
        "NIX_REMOTE": "",
        # /tmp is a symlink to /private/tmp on macOS; without this nix refuses
        # to use a store rooted under a symlinked path.
        "NIX_IGNORE_SYMLINK_STORE": "1",
    }
    for var in ("NIX_USER_CONF_FILES", "NIX_PATH"):
        env.pop(var, None)
    Path(env["NIX_CONF_DIR"]).mkdir()
    return env


def _make_nix_runner(env: dict[str, str], cwd: Path):
    def nix(*args: str, capture: bool = False) -> str:
        cmd = ["nix", *_HERMETIC_NIX_OPTS, *args]
        if capture:
            return subprocess.check_output(cmd, cwd=cwd, env=env, text=True).strip()
        subprocess.check_call(cmd, cwd=cwd, env=env)
        return ""

    return nix


@contextlib.contextmanager
def _http_server(directory: Path):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        thread.join()


@pytest.mark.skipif(shutil.which("nix") is None, reason="requires nix CLI")
@pytest.mark.parametrize("scheme", ["file", "http"])
def test_fod_with_uncached_input_issue413(tmp_path: Path, scheme: str) -> None:
    """An FOD whose output is in a substituter must report cacheStatus=cached,
    even when its build-time-only input drv is absent from every substituter.
    `nix build` would just download the FOD output and never invoke the input
    builder.

    Parametrised across the file:// and http:// substituter transports;
    nix-eval-jobs uses querySubstitutablePathInfos in both cases (per-path,
    no WantMassQuery gate), but the underlying store implementations differ.
    """
    env = _hermetic_nix_env(tmp_path)
    assets = TEST_ROOT.joinpath("assets")
    cache = tmp_path / "cache"
    nix = _make_nix_runner(env, assets)

    nix_system = nix("eval", "--impure", "--raw", "--expr", "builtins.currentSystem", capture=True)
    flake_attr = f".#legacyPackages.{nix_system}.fodIssue413"

    fod_out = nix("build", "--no-link", "--print-out-paths", f"{flake_attr}.fod", capture=True)

    # Only the FOD output reaches the cache; the build input stays un-pushed.
    nix("copy", "--to", f"file://{cache}", fod_out)
    subprocess.check_call(["nix-collect-garbage", "-d"], env=env)

    with contextlib.ExitStack() as stack:
        if scheme == "file":
            substituter = f"file://{cache}"
        else:
            substituter = stack.enter_context(_http_server(cache))
        res = subprocess.check_output(
            [
                str(BIN),
                "--gc-roots-dir",
                str(tmp_path / "gc"),
                *COMMON_FLAGS,
                "--option",
                "substituters",
                substituter,
                "--option",
                "require-sigs",
                "false",
                "--check-cache-status",
                "--flake",
                flake_attr,
            ],
            cwd=assets,
            env=env,
            text=True,
        )

    jobs = [json.loads(line) for line in res.splitlines() if line]
    fod = next(job for job in jobs if job["attr"] == "fod")
    assert fod["cacheStatus"] == "cached", fod
