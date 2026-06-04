#!/usr/bin/env python3
import os
import signal
import shutil
import subprocess
import sys
from pathlib import Path


DEFAULT_CORPUS = "retrosoc"
CORPORA = {
    "retrosoc": {
        "repo_url": "https://github.com/retroSoC/retroSoC.git",
        "commit": "76651fd",
        "checkout_dir": "retroSoC",
    },
}


def env_value(name: str, legacy_name: str | None = None, default: str | None = None) -> str | None:
    value = os.environ.get(name)
    if value is not None and value.strip():
        return value.strip()
    if legacy_name is not None:
        legacy_value = os.environ.get(legacy_name)
        if legacy_value is not None and legacy_value.strip():
            return legacy_value.strip()
    return default


class CommandFailure(RuntimeError):
    def __init__(self, command: list[str], output: str):
        super().__init__(f"{command!r} failed\n{output}")


def run(command: list[str], cwd: Path | None = None) -> str:
    timeout = int(env_value("RTL_E2E_GIT_TIMEOUT_SECONDS", "RETROSOC_GIT_TIMEOUT_SECONDS", "120"))
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


def ensure_cached_checkout(cache_root: Path, checkout_dir: str, repo_url: str, commit: str) -> Path:
    checkout = cache_root / checkout_dir
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
    corpus_name = env_value("RTL_E2E_CORPUS", default=DEFAULT_CORPUS).lower()
    corpus = CORPORA.get(corpus_name)
    if corpus is None:
        print(f"ERROR: Unknown RTL_E2E_CORPUS '{corpus_name}'", file=sys.stderr)
        return 1
    expected = env_value("RTL_E2E_EXPECTED_COMMIT", "RETROSOC_EXPECTED_COMMIT", corpus["commit"])
    repo_url = env_value("RTL_E2E_REPO_URL", "RETROSOC_REPO_URL", corpus["repo_url"])
    explicit_root = env_value("RTL_E2E_ROOT", "RETROSOC_ROOT")

    try:
        if explicit_root:
            checkout = Path(explicit_root).resolve()
            if not checkout.exists():
                raise RuntimeError(f"RTL_E2E_ROOT does not exist: {checkout}")
            head = git_head(checkout)
            if not head.startswith(expected):
                raise RuntimeError(
                    f"RTL_E2E_ROOT commit {head} does not match expected prefix {expected}. "
                    "Use a checkout at the configured corpus commit, or unset RTL_E2E_ROOT to use the local cache."
                )
            print(checkout)
            return 0

        default_cache = repo_root / ".cache" / "rtl-e2e" / corpus_name
        cache_root = Path(env_value("RTL_E2E_CACHE_DIR", "RETROSOC_CACHE_DIR", str(default_cache))).resolve()
        checkout = ensure_cached_checkout(cache_root, corpus["checkout_dir"], repo_url, expected)
        head = git_head(checkout)
        if not head.startswith(expected):
            raise RuntimeError(f"Cached {corpus_name} commit {head} does not match expected prefix {expected}")
        print(checkout)
        return 0
    except (OSError, RuntimeError) as exc:
        if explicit_root:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        print(f"SKIP: {exc}")
        return 77


if __name__ == "__main__":
    raise SystemExit(main())
