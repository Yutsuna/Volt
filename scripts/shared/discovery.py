from __future__ import annotations

from pathlib import Path

__all__ = ["discover_source_files"]


def discover_source_files(
    root: Path | str,
    pattern: str = "**/*.vl",
    *,
    relative_to: Path | None = None,
) -> list[Path]:
    root_path = Path(root)
    if not root_path.exists():
        return []

    if root_path.is_file():
        return [root_path.resolve()]

    files = sorted(p.resolve() for p in root_path.glob(pattern) if p.is_file())
    if relative_to is not None:
        return [f.relative_to(relative_to) for f in files]
    return files
