from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import final

from .process import ProcessResult, run_process_async, run_process_sync

__all__ = ["VoltCompiler"]


@dataclass(frozen=True, slots=True)
@final
class VoltCompiler:
    volt_bin: Path

    def exists(self) -> bool:
        return self.volt_bin.exists()

    def build_cmd(
        self,
        source_file: Path,
        output_binary: Path,
        *,
        opt_level: int = 2,
        extra_flags: Sequence[str] = (),
    ) -> list[str]:
        cmd = [
            str(self.volt_bin),
            "build",
            "-i",
            str(source_file),
            "-o",
            str(output_binary),
            "-O",
            str(opt_level),
        ]
        cmd.extend(extra_flags)
        return cmd

    async def compile_async(
        self,
        source_file: Path,
        output_binary: Path,
        *,
        opt_level: int = 2,
        extra_flags: Sequence[str] = (),
    ) -> ProcessResult:
        cmd = self.build_cmd(
            source_file, output_binary, opt_level=opt_level, extra_flags=extra_flags
        )
        return await run_process_async(cmd)

    def compile_sync(
        self,
        source_file: Path,
        output_binary: Path,
        *,
        opt_level: int = 2,
        extra_flags: Sequence[str] = (),
    ) -> ProcessResult:
        cmd = self.build_cmd(
            source_file, output_binary, opt_level=opt_level, extra_flags=extra_flags
        )
        return run_process_sync(cmd)
