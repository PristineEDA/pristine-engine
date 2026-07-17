from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_script(name: str):
    path = ROOT / "scripts" / name
    spec = importlib.util.spec_from_file_location(path.stem, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load script: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[path.stem] = module
    spec.loader.exec_module(module)
    return module


def load_path(path: Path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[path.stem] = module
    spec.loader.exec_module(module)
    return module


def initialize_git_repo(root: Path) -> None:
    subprocess.run(["git", "init"], cwd=root, check=True, stdout=subprocess.DEVNULL)
    (root / ".gitignore").write_text("build/\n", encoding="utf-8")
    (root / "tracked.txt").write_text("baseline\n", encoding="utf-8")
    subprocess.run(["git", "add", "."], cwd=root, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=Pristine Tests",
            "-c",
            "user.email=tests@pristine.invalid",
            "commit",
            "-m",
            "baseline",
        ],
        cwd=root,
        check=True,
        stdout=subprocess.DEVNULL,
    )


class GateContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = load_script("gate_contract.py")

    def test_source_fingerprint_tracks_source_but_ignores_build_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            initialize_git_repo(root)
            baseline = self.contract.source_fingerprint(root)

            build_dir = root / "build"
            build_dir.mkdir()
            (build_dir / "generated.bin").write_bytes(b"ignored")
            self.assertEqual(self.contract.source_fingerprint(root), baseline)

            (root / "tracked.txt").write_text("changed\n", encoding="utf-8")
            tracked_changed = self.contract.source_fingerprint(root)
            self.assertNotEqual(tracked_changed, baseline)

            (root / "new_source.txt").write_text("untracked\n", encoding="utf-8")
            self.assertNotEqual(self.contract.source_fingerprint(root), tracked_changed)

    def test_cmake_tool_version_uses_executable_version_not_install_name(self) -> None:
        with mock.patch.object(self.contract.subprocess, "run") as run:
            run.return_value = mock.Mock(
                returncode=0,
                stdout="cmake version 4.2.1\n",
            )
            self.assertEqual(
                self.contract._cmake_tool_version(Path("C:/Visual Studio/18/cmake.exe")),
                (4, 2, 1),
            )

    def test_run_context_detects_source_change(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            initialize_git_repo(root)
            manifest = root / "gate_manifest.json"
            manifest.write_text('{"version":1}\n', encoding="utf-8")
            context = self.contract.create_run_context(root, manifest, run_id="test-run")

            self.assertEqual(
                self.contract.validate_run_context(context, root, manifest_path=manifest),
                [],
            )
            (root / "tracked.txt").write_text("changed\n", encoding="utf-8")
            errors = self.contract.validate_run_context(context, root, manifest_path=manifest)

        self.assertTrue(any("source fingerprint mismatch" in error for error in errors))


class DifferentialNormalizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.differential = load_path(ROOT / "tests" / "differential" / "slang_server_differential.py")

    def test_normalized_location_set_uses_relative_uri_and_utf16_range(self) -> None:
        response = {
            "result": [{
                "uri": "file:///tmp/work/top.sv",
                "range": {"start": {"line": 2, "character": 3}, "end": {"line": 2, "character": 8}},
            }]
        }
        normalized = self.differential.normalize_response(
            "definition", response, {"file:///tmp/work/top.sv": "top.sv"}
        )
        self.assertEqual(normalized, [{"uri": "top.sv", "range": {"start": [2, 3], "end": [2, 8]}}])

    def test_normalized_workspace_edit_sorts_uris_and_edits(self) -> None:
        response = {
            "result": {
                "changes": {
                    "file:///tmp/work/b.sv": [{"range": {"start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 1}}, "newText": "b"}],
                    "file:///tmp/work/a.sv": [{"range": {"start": {"line": 0, "character": 2}, "end": {"line": 0, "character": 3}}, "newText": "a"}],
                }
            }
        }
        normalized = self.differential.normalize_response(
            "rename", response, {"file:///tmp/work/a.sv": "a.sv", "file:///tmp/work/b.sv": "b.sv"}
        )
        self.assertEqual(list(normalized["changes"]), ["a.sv", "b.sv"])
        self.assertEqual(normalized["changes"]["a.sv"][0]["newText"], "a")

    def test_normalized_call_hierarchy_preserves_call_sites(self) -> None:
        response = {
            "result": [{
                "from": {
                    "name": "top",
                    "uri": "file:///tmp/work/top.sv",
                    "range": {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 9}},
                    "selectionRange": {"start": {"line": 0, "character": 7}, "end": {"line": 0, "character": 10}},
                },
                "fromRanges": [{"start": {"line": 1, "character": 2}, "end": {"line": 1, "character": 9}}],
            }]
        }
        normalized = self.differential.normalize_response(
            "callHierarchyIncoming", response, {"file:///tmp/work/top.sv": "top.sv"}
        )
        self.assertEqual(normalized[0]["name"], "top")
        self.assertEqual(normalized[0]["fromRanges"], [{"start": [1, 2], "end": [1, 9]}])

    def test_prepare_rename_requires_explicit_prepare_provider(self) -> None:
        boolean_rename = {"renameProvider": True}
        prepared_rename = {"renameProvider": {"prepareProvider": True}}

        self.assertTrue(self.differential.supports_differential_check(boolean_rename, "rename"))
        self.assertFalse(self.differential.supports_differential_check(boolean_rename, "prepareRename"))
        self.assertTrue(self.differential.supports_differential_check(prepared_rename, "prepareRename"))


class FullTestStatusRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_script("run_full_tests_with_status.py")

    def write_manifest(
        self,
        directory: Path,
        *,
        required: list[str],
        optional: list[str] | None = None,
        runner_self_test: str = "pristine_script_runner_tests",
    ):
        manifest_path = directory / "gate_manifest.json"
        manifest_path.write_text(
            json.dumps(
                {
                    "version": 7,
                    "runnerSelfTest": runner_self_test,
                    "requiredTests": required,
                    "optionalSkipTests": optional or [],
                    "requiredEnvironment": [{"name": "PRISTINE_REQUIRE_IHP_OPEN_PDK"}],
                    "groups": {"all": required},
                }
            ),
            encoding="utf-8",
        )
        return self.runner.load_gate_manifest(manifest_path)

    def test_differential_ctest_inherits_runtime_slang_root(self) -> None:
        cmake_text = (ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn('ENVIRONMENT "SLANG_SERVER_ROOT=', cmake_text)

    def test_summary_reports_required_missing_and_optional_skip(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(
                Path(temp),
                required=[
                    "pristine_unit_tests",
                    "pristine_differential_slang_server",
                    "pristine_missing_required_gate",
                ],
                optional=["pristine_differential_slang_server"],
            )
            all_tests = [
                "pristine_unit_tests",
                "pristine_differential_slang_server",
                "pristine_script_runner_tests",
            ]
            results = [
                self.runner.TestResult(
                    name="pristine_unit_tests",
                    status="passed",
                    returncode=0,
                    duration_seconds=1.0,
                    log_path=Path("unit.log"),
                ),
                self.runner.TestResult(
                    name="pristine_differential_slang_server",
                    status="skipped",
                    returncode=0,
                    duration_seconds=0.5,
                    log_path=Path("diff.log"),
                    skip_reason="SKIP: missing server",
                ),
            ]

            summary = self.runner.build_summary_payload(
                results,
                all_tests=all_tests,
                selected=all_tests,
                build_dir=Path("build/dev"),
                ctest="ctest",
                manifest=manifest,
                full_gate_enforced=True,
                environment={
                    "SLANG_SERVER_ROOT": "C:/slang-server",
                    "UNRELATED": "ignored",
                },
            )
            expected_manifest_hash = self.runner.file_sha256(manifest.path)

        self.assertIn("pristine_missing_required_gate", summary["requiredMissing"])
        self.assertEqual(summary["requiredPassed"], 1)
        self.assertEqual(summary["optionalSkipped"], 1)
        self.assertEqual(summary["requiredSkipped"], 0)
        self.assertEqual(summary["environment"], {"SLANG_SERVER_ROOT": "C:/slang-server"})
        self.assertEqual(summary["ctestPath"], "ctest")
        self.assertEqual(summary["manifestVersion"], 7)
        self.assertEqual(summary["requiredGateCount"], 3)
        self.assertEqual(summary["optionalGateCount"], 1)
        self.assertTrue(summary["runnerSelfTestPresent"])
        self.assertTrue(summary["fullGateEnforced"])
        self.assertEqual(summary["schemaVersion"], self.runner.SUMMARY_SCHEMA_VERSION)
        self.assertEqual(summary["buildPreset"], "dev")
        self.assertEqual(summary["buildType"], "debug")
        self.assertIn("startedAt", summary)
        self.assertIn("endedAt", summary)
        self.assertIn("durationSeconds", summary)
        self.assertEqual(summary["manifestHash"], expected_manifest_hash)
        self.assertEqual(summary["artifactRoot"], str((Path("build/dev") / "test-status-logs").resolve()))
        self.assertFalse(summary["requiredEnvironmentSatisfied"])

    def test_required_skip_results_reject_non_optional_skips(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(
                Path(temp),
                required=["pristine_lsp_core_e2e", "pristine_differential_slang_server"],
                optional=["pristine_differential_slang_server"],
            )
            results = [
                self.runner.TestResult(
                    name="pristine_lsp_core_e2e",
                    status="skipped",
                    returncode=0,
                    duration_seconds=0.1,
                    log_path=Path("lsp.log"),
                    skip_reason="SKIP: unexpected",
                ),
                self.runner.TestResult(
                    name="pristine_differential_slang_server",
                    status="skipped",
                    returncode=0,
                    duration_seconds=0.1,
                    log_path=Path("diff.log"),
                    skip_reason="SKIP: optional",
                ),
            ]

            required_skips = self.runner.required_skip_results(results, manifest, {})

        self.assertEqual([result.name for result in required_skips], ["pristine_lsp_core_e2e"])

    def test_required_slang_differential_promotes_optional_skip_to_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(
                Path(temp),
                required=["pristine_differential_slang_server"],
                optional=["pristine_differential_slang_server"],
            )
            result = self.runner.TestResult(
                name="pristine_differential_slang_server",
                status="skipped",
                returncode=0,
                duration_seconds=0.1,
                log_path=Path("differential.log"),
                skip_reason="SKIP: missing binary",
            )

            optional = self.runner.required_skip_results([result], manifest, {})
            required = self.runner.required_skip_results(
                [result],
                manifest,
                {"PRISTINE_REQUIRE_SLANG_DIFFERENTIAL": "1"},
            )

        self.assertEqual(optional, [])
        self.assertEqual(required, [result])

    def test_manifest_validation_rejects_optional_skip_outside_required(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest_path = Path(temp) / "gate_manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "runnerSelfTest": "pristine_script_runner_tests",
                        "requiredTests": ["pristine_unit_tests"],
                        "optionalSkipTests": ["pristine_differential_slang_server"],
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "optional skip tests"):
                self.runner.load_gate_manifest(manifest_path)

    def test_manifest_check_reports_missing_required_and_self_test(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(
                Path(temp),
                required=["pristine_unit_tests", "pristine_lsp_core_e2e"],
            )

            errors = self.runner.manifest_check_errors(
                ["pristine_unit_tests"],
                manifest,
                {"PRISTINE_REQUIRE_IHP_OPEN_PDK": "1"},
            )

        self.assertIn("required gate missing from CTest: pristine_lsp_core_e2e", errors)
        self.assertIn("runner self-test missing from CTest: pristine_script_runner_tests", errors)

    def test_manifest_check_reports_unclassified_ctest(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(Path(temp), required=["pristine_unit_tests"])

            errors = self.runner.manifest_check_errors(
                [
                    "pristine_unit_tests",
                    "pristine_script_runner_tests",
                    "pristine_new_unclassified_gate",
                ],
                manifest,
                {"PRISTINE_REQUIRE_IHP_OPEN_PDK": "1"},
            )

        self.assertEqual(
            errors,
            ["CTest is not classified in gate manifest: pristine_new_unclassified_gate"],
        )

    def test_runner_self_test_is_classified_but_not_product_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(
                Path(temp),
                required=["pristine_unit_tests", "pristine_differential_slang_server"],
                optional=["pristine_differential_slang_server"],
            )

            known = self.runner.known_manifest_tests(manifest)
            unclassified = self.runner.unclassified_tests(
                [
                    "pristine_unit_tests",
                    "pristine_differential_slang_server",
                    "pristine_script_runner_tests",
                ],
                manifest,
            )

        self.assertIn("pristine_differential_slang_server", known)
        self.assertIn("pristine_script_runner_tests", known)
        self.assertEqual(
            manifest.required_tests,
            ("pristine_unit_tests", "pristine_differential_slang_server"),
        )
        self.assertEqual(unclassified, [])

    def test_required_environment_preflight_reports_missing_and_wrong_values(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest_path = Path(temp) / "gate_manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "runnerSelfTest": "pristine_script_runner_tests",
                        "requiredTests": ["pristine_unit_tests"],
                        "optionalSkipTests": [],
                        "requiredEnvironment": [
                            {"name": "PRISTINE_REQUIRE_IHP_OPEN_PDK", "recommendedValue": "1"},
                            {"name": "PRISTINE_REQUIRE_TT_TINYQV_GDS", "recommendedValue": "1"},
                        ],
                    }
                ),
                encoding="utf-8",
            )
            manifest = self.runner.load_gate_manifest(manifest_path)

            missing = self.runner.missing_required_environment(
                manifest,
                {"PRISTINE_REQUIRE_IHP_OPEN_PDK": "0"},
            )
            valid = self.runner.missing_required_environment(
                manifest,
                {
                    "PRISTINE_REQUIRE_IHP_OPEN_PDK": "1",
                    "PRISTINE_REQUIRE_TT_TINYQV_GDS": "1",
                },
            )

        self.assertEqual(
            missing,
            [
                {
                    "name": "PRISTINE_REQUIRE_IHP_OPEN_PDK",
                    "expectedValue": "1",
                    "actualValue": "0",
                },
                {
                    "name": "PRISTINE_REQUIRE_TT_TINYQV_GDS",
                    "expectedValue": "1",
                    "actualValue": "",
                },
            ],
        )
        self.assertEqual(valid, [])

    def test_summary_marks_required_environment_satisfied(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest_path = Path(temp) / "gate_manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "runnerSelfTest": "pristine_script_runner_tests",
                        "requiredTests": ["pristine_unit_tests"],
                        "optionalSkipTests": [],
                        "requiredEnvironment": [
                            {"name": "PRISTINE_REQUIRE_IHP_OPEN_PDK", "recommendedValue": "1"},
                        ],
                    }
                ),
                encoding="utf-8",
            )
            manifest = self.runner.load_gate_manifest(manifest_path)
            summary = self.runner.build_summary_payload(
                [],
                all_tests=["pristine_unit_tests", "pristine_script_runner_tests"],
                selected=["pristine_unit_tests"],
                build_dir=Path("build/dev"),
                ctest="ctest",
                manifest=manifest,
                full_gate_enforced=False,
                environment={"PRISTINE_REQUIRE_IHP_OPEN_PDK": "1"},
            )

        self.assertTrue(summary["requiredEnvironmentSatisfied"])
        self.assertEqual(summary["missingRequiredEnvironment"], [])

    def test_focused_summary_marks_full_gate_not_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = self.write_manifest(Path(temp), required=["pristine_unit_tests"])

            summary = self.runner.build_summary_payload(
                [],
                all_tests=["pristine_unit_tests", "pristine_script_runner_tests"],
                selected=["pristine_unit_tests"],
                build_dir=Path("build/dev"),
                ctest="ctest",
                manifest=manifest,
                full_gate_enforced=False,
                environment={},
            )

        self.assertFalse(summary["fullGateEnforced"])
        self.assertEqual(summary["requiredMissing"], [])
        self.assertEqual(summary["unclassifiedTests"], [])
        self.assertEqual(summary["missingRequiredEnvironment"][0]["name"], "PRISTINE_REQUIRE_IHP_OPEN_PDK")


class CleanReleaseGateRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_script("run_clean_release_gate.py")

    def test_guarded_remove_release_dir_refuses_workspace_and_outside_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            workspace = Path(temp) / "repo"
            workspace.mkdir()
            release = workspace / "build" / "release"
            release.mkdir(parents=True)
            marker = release / "marker.txt"
            marker.write_text("old build", encoding="utf-8")

            dry_run = self.runner.guarded_remove_release_dir(workspace, release, dry_run=True)
            self.assertEqual(dry_run.status, "deleted")
            self.assertTrue(dry_run.existed_before)
            self.assertTrue(marker.exists())
            removed = self.runner.guarded_remove_release_dir(workspace, release, dry_run=False)
            self.assertEqual(removed.status, "deleted")
            self.assertFalse(release.exists())

            absent = self.runner.guarded_remove_release_dir(workspace, release, dry_run=False)
            self.assertEqual(absent.status, "alreadyAbsent")
            self.assertFalse(absent.existed_before)

            with self.assertRaises(ValueError):
                self.runner.guarded_remove_release_dir(workspace, workspace, dry_run=True)
            with self.assertRaises(ValueError):
                self.runner.guarded_remove_release_dir(workspace, Path(temp) / "outside", dry_run=True)

    def test_planned_steps_cover_version_and_release_smokes(self) -> None:
        steps = self.runner.planned_steps(
            "cmake",
            Path("build/release"),
            "release",
            Path("build/release/pristine-engine.exe"),
            cxx_compiler="clang++-21",
        )
        names = [name for name, _ in steps]

        self.assertEqual(
            names,
            [
                "configure",
                "build",
                "version",
                "lsp-smoke",
                "waveform-smoke",
                "layout-smoke",
            ],
        )
        self.assertIn("--version", steps[2][1])
        self.assertIn("-DCMAKE_CXX_COMPILER=clang++-21", steps[0][1])
        self.assertIn("tests/e2e/lsp_core_smoke.py", steps[3][1])
        self.assertIn("tests/e2e/waveform_pipe_smoke.py", steps[4][1])
        self.assertIn("tests/e2e/layout_pipe_smoke.py", steps[5][1])

    def test_summary_records_release_traceability_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            workspace = Path(temp) / "repo"
            workspace.mkdir()
            summary_path = workspace / "summary.json"
            build_dir = workspace / "build" / "release"
            step = self.runner.StepResult(
                name="version",
                command=["engine", "--version"],
                returncode=0,
                duration_seconds=0.1,
                log_path=workspace / "version.log",
                captured_output="pristine-engine 0.1.1 build=release",
            )

            provenance = {
                "runId": "run-1",
                "workspace": str(workspace),
                "gitHead": "abc123",
                "gitDirty": True,
                "sourceFingerprint": "fingerprint",
                "observedSourceFingerprint": "fingerprint",
                "sourceStable": True,
                "manifestHash": "manifest",
            }
            with mock.patch.object(
                self.runner.gate_contract,
                "summary_provenance",
                return_value=provenance,
            ):
                self.runner.write_summary(
                    summary_path,
                    status="passed",
                    workspace=workspace,
                    build_dir=build_dir,
                    cmake="cmake",
                    preset="release",
                    clean_preparation=self.runner.CleanPreparation(
                        "alreadyAbsent", build_dir, False
                    ),
                    dry_run=False,
                    steps=[step],
                    run_context={"runId": "run-1"},
                    started_at="2026-01-01T00:00:00Z",
                    duration_seconds=1.0,
                    release_version_output=step.captured_output,
                    planned=[("version", ["engine", "--version"])],
                )

            payload = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertEqual(payload["deletedPath"], str(build_dir))
        self.assertEqual(payload["cleanPreparation"]["status"], "alreadyAbsent")
        self.assertEqual(payload["releaseVersionOutput"], "pristine-engine 0.1.1 build=release")
        self.assertIn("gitHead", payload)
        self.assertIn("gitDirty", payload)
        self.assertEqual(payload["failedStep"], "")
        self.assertEqual(payload["failedLogPath"], "")


class WindowsClangGateRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_script("run_windows_clang_gate.py")

    def test_vs_amd64_preflight_rejects_wrong_shell_architecture(self) -> None:
        self.assertEqual(
            self.runner.vs_amd64_environment_errors(
                {"VSCMD_ARG_TGT_ARCH": "x64"},
                platform_name="nt",
            ),
            [],
        )
        errors = self.runner.vs_amd64_environment_errors(
            {"VSCMD_ARG_TGT_ARCH": "x86"},
            platform_name="nt",
        )
        self.assertTrue(any("VS amd64 shell is required" in error for error in errors))

    def test_clang_presets_pin_both_c_and_cxx_compilers(self) -> None:
        presets = json.loads(
            (ROOT / "CMakePresets.json").read_text(encoding="utf-8")
        )["configurePresets"]
        by_name = {preset["name"]: preset for preset in presets}
        for name in ("clang-cl", "clang-cl-release"):
            variables = by_name[name]["cacheVariables"]
            self.assertTrue(variables["CMAKE_C_COMPILER"].endswith("clang-cl.exe"))
            self.assertEqual(variables["CMAKE_C_COMPILER"], variables["CMAKE_CXX_COMPILER"])

    def test_planned_steps_enable_perf_and_run_complete_ctest(self) -> None:
        steps = self.runner.planned_steps(
            "cmake",
            "ctest",
            "clang-cl",
            Path("build/clang-cl"),
        )
        self.assertEqual([name for name, _ in steps], ["configure", "build", "ctest"])
        self.assertIn("-DPRISTINE_BUILD_PERF_TESTS=ON", steps[0][1])
        self.assertIn("--output-on-failure", steps[2][1])

    def test_release_steps_build_and_smoke_the_clang_release_binary(self) -> None:
        steps = self.runner.planned_release_steps(
            "cmake",
            "clang-cl-release",
            Path("build/clang-cl-release"),
        )
        self.assertEqual(
            [name for name, _ in steps],
            [
                "release-configure",
                "release-build",
                "release-version",
                "release-lsp-smoke",
                "release-waveform-smoke",
                "release-layout-smoke",
            ],
        )
        self.assertIn("clang-cl-release", steps[0][1])
        self.assertTrue(any(arg.endswith("lsp_core_smoke.py") for arg in steps[3][1]))
        self.assertTrue(any(arg.endswith("waveform_pipe_smoke.py") for arg in steps[4][1]))
        self.assertTrue(any(arg.endswith("layout_pipe_smoke.py") for arg in steps[5][1]))

    def test_release_clean_guard_rejects_paths_outside_workspace(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            workspace = Path(temp) / "workspace"
            workspace.mkdir()
            outside = Path(temp) / "outside"
            with self.assertRaisesRegex(ValueError, "outside workspace"):
                self.runner.guarded_remove_build_dir(workspace, outside, dry_run=True)


class RequiredGateSuiteRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_script("run_required_gate_suite.py")

    def test_suite_plan_binds_every_gate_to_one_run_context(self) -> None:
        workspace = Path("C:/repo").resolve()
        context = workspace / "build/gate-artifacts/run-context.json"
        commands = self.runner.planned_commands(workspace, "cmake", context)

        self.assertEqual(
            list(commands),
            [
                "dev-configure",
                "dev-build",
                "full-debug",
                "windows-clang",
                "clean-release",
                "artifact-index",
            ],
        )
        for name in ("full-debug", "windows-clang", "clean-release", "artifact-index"):
            self.assertIn("--run-context", commands[name])
            self.assertIn(str(context), commands[name])
        self.assertIn("--profile", commands["artifact-index"])
        self.assertIn("windows-local", commands["artifact-index"])


class GateArtifactIndexTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.indexer = load_script("write_gate_artifact_index.py")

    def write_json(self, path: Path, payload: dict) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def context(self, root: Path) -> dict:
        manifest = root / "gate_manifest.json"
        manifest.write_text("{}\n", encoding="utf-8")
        return {
            "schemaVersion": 1,
            "runId": "run-1",
            "createdAt": "2026-01-01T00:00:00Z",
            "workspace": str(root),
            "gitHead": "a" * 40,
            "gitDirty": True,
            "sourceFingerprint": "fingerprint",
            "manifestPath": str(manifest),
            "manifestHash": "manifest",
        }

    def provenance(self, root: Path, *, run_id: str = "run-1") -> dict:
        return {
            "runId": run_id,
            "workspace": str(root),
            "gitHead": "a" * 40,
            "gitDirty": True,
            "sourceFingerprint": "fingerprint",
            "observedSourceFingerprint": "fingerprint",
            "sourceStable": True,
            "manifestHash": "manifest",
        }

    def valid_full_summary(self, root: Path) -> dict:
        return {
            "schemaVersion": self.indexer.gate_contract.SUMMARY_SCHEMA_VERSION,
            "gateType": "full-debug",
            "status": "passed",
            "provenance": self.provenance(root),
            "failed": 0,
            "requiredSkipped": 0,
            "fullGateEnforced": True,
            "manifestErrors": [],
            "unclassifiedTests": [],
            "missingRequiredEnvironment": [],
            "gateErrors": [],
            "ctestPath": "ctest",
            "artifactRoot": "build/dev/test-status-logs",
            "total": 16,
            "passed": 15,
            "skipped": 1,
            "optionalSkipped": 1,
            "durationSeconds": 12.5,
            "manifestHash": "abc123",
            "build": {
                "compiler": {"id": "MSVC", "version": "19.0", "path": "cl.exe"},
                "binarySha256": "debug-sha",
            },
        }

    def valid_release_summary(self, root: Path, build_dir: Path) -> dict:
        return {
            "schemaVersion": self.indexer.gate_contract.SUMMARY_SCHEMA_VERSION,
            "gateType": "clean-release",
            "status": "passed",
            "provenance": self.provenance(root),
            "gateErrors": [],
            "buildDir": str(build_dir),
            "cmakePath": "cmake",
            "deletedPath": str(build_dir),
            "cleanPreparation": {
                "status": "alreadyAbsent",
                "path": str(build_dir),
                "existedBefore": False,
            },
            "releaseVersionOutput": "pristine-engine 0.1.4 build=release",
            "failedStep": "",
            "failedLogPath": "",
            "build": {
                "binarySha256": "release-sha",
                "compiler": {"id": "MSVC", "version": "19.0", "path": "cl.exe"},
            },
        }

    def valid_clang_summary(self, root: Path) -> dict:
        debug_build = {
            "type": "debug",
            "compiler": {
                "id": "Clang",
                "version": "21.0.0",
                "path": "C:/Program Files/LLVM/bin/clang-cl.exe",
            },
            "binarySha256": "clang-debug-sha",
        }
        release_build_dir = root / "build" / "clang-cl-release"
        release_build = {
            "type": "release",
            "compiler": {
                "id": "Clang",
                "version": "21.0.0",
                "path": "C:/Program Files/LLVM/bin/clang-cl.exe",
            },
            "binarySha256": "clang-release-sha",
        }
        return {
            "schemaVersion": self.indexer.gate_contract.SUMMARY_SCHEMA_VERSION,
            "gateType": "windows-clang",
            "status": "passed",
            "provenance": self.provenance(root),
            "gateErrors": [],
            "releaseBuildDir": str(release_build_dir),
            "build": debug_build,
            "debugBuild": debug_build,
            "releaseBuild": release_build,
            "releaseCleanPreparation": {
                "status": "alreadyAbsent",
                "path": str(release_build_dir),
                "existedBefore": False,
            },
            "releaseVersionOutput": "pristine-engine 0.1.4 build=release",
        }

    def write_valid_inputs(self, root: Path):
        context = self.write_json(root / "context.json", self.context(root))
        full = self.write_json(root / "full.json", self.valid_full_summary(root))
        release = self.write_json(
            root / "release.json",
            self.valid_release_summary(root, root / "build" / "release"),
        )
        clang = self.write_json(root / "clang.json", self.valid_clang_summary(root))
        return context, full, release, clang

    def test_artifact_index_requires_full_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.write_json(root / "context.json", self.context(root))
            release = self.write_json(
                root / "release.json",
                self.valid_release_summary(root, root / "build" / "release"),
            )

            with self.assertRaisesRegex(RuntimeError, "full Debug summary is missing"):
                self.indexer.write_index(
                    root / "index.json",
                    workspace=root,
                    run_context_path=context,
                    full_summary_path=root / "missing.json",
                    release_summary_path=release,
                )

    def test_artifact_index_requires_clean_release_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.write_json(root / "context.json", self.context(root))
            full = self.write_json(root / "full.json", self.valid_full_summary(root))

            with self.assertRaisesRegex(RuntimeError, "clean Release summary is missing"):
                self.indexer.write_index(
                    root / "index.json",
                    workspace=root,
                    run_context_path=context,
                    full_summary_path=full,
                    release_summary_path=root / "missing-release.json",
                )

    def test_artifact_index_rejects_cross_run_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.context(root)
            full = self.valid_full_summary(root)
            full["provenance"] = self.provenance(root, run_id="stale-run")
            release = self.valid_release_summary(root, root / "build" / "release")
            clang = self.valid_clang_summary(root)

            errors = self.indexer.validate_required_summaries(
                context,
                full,
                release,
                clang,
                profile="windows-local",
                system_name="Windows",
            )

        self.assertTrue(any("runId mismatch" in error for error in errors))

    def test_windows_profile_requires_structured_clang_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.context(root)
            full = self.valid_full_summary(root)
            release = self.valid_release_summary(root, root / "build" / "release")

            errors = self.indexer.validate_required_summaries(
                context,
                full,
                release,
                None,
                profile="windows-local",
                system_name="Windows",
            )

        self.assertIn("Windows local profile requires a structured clang-cl summary", errors)

    def test_windows_profile_requires_clang_release_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.context(root)
            full = self.valid_full_summary(root)
            release = self.valid_release_summary(root, root / "build" / "release")
            clang = self.valid_clang_summary(root)
            clang["releaseBuild"]["binarySha256"] = ""
            clang["releaseVersionOutput"] = ""

            errors = self.indexer.validate_required_summaries(
                context,
                full,
                release,
                clang,
                profile="windows-local",
                system_name="Windows",
            )

        self.assertTrue(any("Release binary SHA256 is missing" in error for error in errors))
        self.assertIn("clang-cl Release version output is missing", errors)

    def test_ci_linux_profile_requires_modern_clang(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.context(root)
            full = self.valid_full_summary(root)
            release = self.valid_release_summary(root, root / "build" / "release")

            errors = self.indexer.validate_required_summaries(
                context,
                full,
                release,
                None,
                profile="ci-matrix",
                system_name="Linux",
            )

        self.assertTrue(any("expected 'Clang'" in error for error in errors))

    def test_ci_linux_profile_rejects_release_compiler_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context = self.context(root)
            full = self.valid_full_summary(root)
            full["build"]["compiler"] = {
                "id": "Clang",
                "version": "21.0.0",
                "path": "/usr/bin/clang++-21",
            }
            release = self.valid_release_summary(root, root / "build" / "release")
            release["build"]["compiler"] = {
                "id": "GNU",
                "version": "11.4.0",
                "path": "/usr/bin/c++",
            }

            errors = self.indexer.validate_required_summaries(
                context,
                full,
                release,
                None,
                profile="ci-matrix",
                system_name="Linux",
            )

        self.assertTrue(any("clean Release: compiler id" in error for error in errors))

    def test_artifact_index_writes_valid_index(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            context, full, release, clang = self.write_valid_inputs(root)
            output = root / "index.json"

            with mock.patch.object(
                self.indexer.gate_contract,
                "validate_run_context",
                return_value=[],
            ):
                payload = self.indexer.write_index(
                    output,
                    workspace=root,
                    run_context_path=context,
                    full_summary_path=full,
                    release_summary_path=release,
                    clang_summary_path=clang,
                    profile="windows-local",
                    system_name="Windows",
                )

            written = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(payload["schemaVersion"], self.indexer.INDEX_SCHEMA_VERSION)
        self.assertEqual(written["provenance"]["runId"], "run-1")
        self.assertEqual(written["fullDebug"]["status"], "passed")
        self.assertEqual(written["cleanRelease"]["releaseVersionOutput"], "pristine-engine 0.1.4 build=release")
        self.assertEqual(written["clangCl"]["status"], "passed")
        self.assertEqual(written["clangCl"]["debugBuild"]["type"], "debug")
        self.assertEqual(written["clangCl"]["releaseBuild"]["type"], "release")


if __name__ == "__main__":
    unittest.main()
