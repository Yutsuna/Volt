from __future__ import annotations

import asyncio
import subprocess
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

__all__ = ["ProcessResult", "run_process_async", "run_process_sync"]


@dataclass(frozen=True, slots=True)
class ProcessResult:
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float

    @property
    def success(self) -> bool:
        return self.returncode == 0

    @property
    def combined_output(self) -> str:
        if self.stderr and self.stdout:
            return f"{self.stderr}\n{self.stdout}"
        return self.stderr or self.stdout


async def run_process_async(
    cmd: Sequence[str | Path],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
) -> ProcessResult:
    cmd_str = [str(c) for c in cmd]
    start = time.perf_counter()
    proc = await asyncio.create_subprocess_exec(
        *cmd_str,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        cwd=cwd,
        env=env,
    )
    try:
        if timeout is not None:
            stdout_bytes, stderr_bytes = await asyncio.wait_for(
                proc.communicate(), timeout=timeout
            )
        else:
            stdout_bytes, stderr_bytes = await proc.communicate()
    except asyncio.TimeoutError:
        proc.kill()
        _ = await proc.wait()
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return ProcessResult(
            returncode=-1,
            stdout="",
            stderr=f"Process timed out after {timeout}s",
            elapsed_ms=elapsed_ms,
        )

    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return ProcessResult(
        returncode=proc.returncode if proc.returncode is not None else 0,
        stdout=stdout_bytes.decode(errors="ignore"),
        stderr=stderr_bytes.decode(errors="ignore"),
        elapsed_ms=elapsed_ms,
    )


def run_process_sync(
    cmd: Sequence[str | Path],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
) -> ProcessResult:
    cmd_str = [str(c) for c in cmd]
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd_str,
            capture_output=True,
            text=True,
            cwd=cwd,
            env=env,
            timeout=timeout,
            check=False,
        )
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return ProcessResult(
            returncode=proc.returncode,
            stdout=proc.stdout or "",
            stderr=proc.stderr or "",
            elapsed_ms=elapsed_ms,
        )
    except subprocess.TimeoutExpired as exc:
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        stdout_str = (
            exc.stdout.decode(errors="ignore")
            if isinstance(exc.stdout, bytes)
            else (exc.stdout or "")
        )
        stderr_str = (
            exc.stderr.decode(errors="ignore")
            if isinstance(exc.stderr, bytes)
            else (exc.stderr or f"Process timed out after {timeout}s")
        )
        return ProcessResult(
            returncode=-1,
            stdout=stdout_str,
            stderr=stderr_str,
            elapsed_ms=elapsed_ms,
        )
