from __future__ import annotations

import importlib.util
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

    def test_summary_reports_required_missing_and_optional_skip(self) -> None:
        all_tests = list(self.runner.REQUIRED_CTEST_GATES[:-1])
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
            environment={
                "SLANG_SERVER_ROOT": "C:/slang-server",
                "UNRELATED": "ignored",
            },
        )

        self.assertIn(self.runner.REQUIRED_CTEST_GATES[-1], summary["requiredMissing"])
        self.assertEqual(summary["requiredPassed"], 1)
        self.assertEqual(summary["optionalSkipped"], 1)
        self.assertEqual(summary["requiredSkipped"], 0)
        self.assertEqual(summary["environment"], {"SLANG_SERVER_ROOT": "C:/slang-server"})
        self.assertEqual(summary["ctestPath"], "ctest")

    def test_required_skip_results_reject_non_optional_skips(self) -> None:
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

        required_skips = self.runner.required_skip_results(results)

        self.assertEqual([result.name for result in required_skips], ["pristine_lsp_core_e2e"])


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


if __name__ == "__main__":
    unittest.main()
