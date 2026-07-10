from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


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

            required_skips = self.runner.required_skip_results(results, manifest)

        self.assertEqual([result.name for result in required_skips], ["pristine_lsp_core_e2e"])

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

            self.assertTrue(
                self.runner.guarded_remove_release_dir(workspace, release, dry_run=True)
            )
            self.assertTrue(marker.exists())
            self.assertTrue(
                self.runner.guarded_remove_release_dir(workspace, release, dry_run=False)
            )
            self.assertFalse(release.exists())

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

            self.runner.write_summary(
                summary_path,
                status="passed",
                workspace=workspace,
                build_dir=build_dir,
                cmake="cmake",
                preset="release",
                removed_build_dir=True,
                deleted_path=str(build_dir),
                dry_run=False,
                steps=[step],
                release_version_output=step.captured_output,
                planned=[("version", ["engine", "--version"])],
            )

            payload = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertEqual(payload["deletedPath"], str(build_dir))
        self.assertEqual(payload["releaseVersionOutput"], "pristine-engine 0.1.1 build=release")
        self.assertIn("gitHead", payload)
        self.assertIn("gitDirty", payload)
        self.assertEqual(payload["failedStep"], "")
        self.assertEqual(payload["failedLogPath"], "")


if __name__ == "__main__":
    unittest.main()
