#!/usr/bin/env python3
import os
import signal
import shutil
import subprocess
import sys
from pathlib import Path


DEFAULT_REPO_URL = "https://github.com/retroSoC/retroSoC.git"
DEFAULT_COMMIT = "76651fd"


class CommandFailure(RuntimeError):
    def __init__(self, command: list[str], output: str):
        super().__init__(f"{command!r} failed\n{output}")


def run(command: list[str], cwd: Path | None = None) -> str:
    timeout = int(os.environ.get("RETROSOC_GIT_TIMEOUT_SECONDS", "120"))
    env = os.environ.copy()
    env.setdefault("GIT_TERMINAL_PROMPT", "0")
    popen_kwargs = {
        "cwd": str(cwd) if cwd else None,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT,
        "text": True,
        "env": env,
    }
    if os.name == "nt":
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_kwargs["start_new_session"] = True

    process = subprocess.Popen(
        command,
        **popen_kwargs,
    )
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        else:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        output, _ = process.communicate()
        raise CommandFailure(
            command,
            f"Timed out after {timeout} seconds.\n{output}",
        )
    if process.returncode != 0:
        raise CommandFailure(command, output)
    return output.strip()


def git_head(root: Path) -> str:
    return run(["git", "-C", str(root), "rev-parse", "HEAD"])


def clone_cached_checkout(checkout: Path, repo_url: str) -> None:
    checkout.parent.mkdir(parents=True, exist_ok=True)
    try:
        run(["git", "clone", repo_url, str(checkout)])
    except RuntimeError:
        shutil.rmtree(checkout, ignore_errors=True)
        raise


def ensure_cached_checkout(cache_root: Path, repo_url: str, commit: str) -> Path:
    checkout = cache_root / "retroSoC"
    if not checkout.exists():
        clone_cached_checkout(checkout, repo_url)
    if not (checkout / ".git").exists():
        shutil.rmtree(checkout, ignore_errors=True)
        clone_cached_checkout(checkout, repo_url)
    if git_head(checkout).startswith(commit):
        return checkout
    run(["git", "-C", str(checkout), "fetch", "--tags", "origin"])
    run(["git", "-C", str(checkout), "checkout", "--detach", commit])
    return checkout


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    expected = os.environ.get("RETROSOC_EXPECTED_COMMIT", DEFAULT_COMMIT).strip() or DEFAULT_COMMIT
    repo_url = os.environ.get("RETROSOC_REPO_URL", DEFAULT_REPO_URL).strip() or DEFAULT_REPO_URL
    explicit_root = os.environ.get("RETROSOC_ROOT")

    try:
        if explicit_root:
            checkout = Path(explicit_root).resolve()
            if not checkout.exists():
                raise RuntimeError(f"RETROSOC_ROOT does not exist: {checkout}")
            head = git_head(checkout)
            if not head.startswith(expected):
                raise RuntimeError(
                    f"RETROSOC_ROOT commit {head} does not match expected prefix {expected}. "
                    "Use a checkout at commit 76651fd, or unset RETROSOC_ROOT to use the local cache."
                )
            print(checkout)
            return 0

        cache_root = Path(os.environ.get("RETROSOC_CACHE_DIR", repo_root / ".cache" / "retrosoc")).resolve()
        checkout = ensure_cached_checkout(cache_root, repo_url, expected)
        head = git_head(checkout)
        if not head.startswith(expected):
            raise RuntimeError(f"Cached retroSoC commit {head} does not match expected prefix {expected}")
        print(checkout)
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
