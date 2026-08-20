from __future__ import annotations

from typing import final

__all__ = ["Colors"]


@final
class Colors:
    GREEN: str = "\033[92m"
    RED: str = "\033[91m"
    YELLOW: str = "\033[93m"
    CYAN: str = "\033[96m"
    MAGENTA: str = "\033[95m"
    BOLD: str = "\033[1m"
    DIM: str = "\033[2m"
    RESET: str = "\033[0m"

    @classmethod
    def green(cls, text: str) -> str:
        return f"{cls.GREEN}{text}{cls.RESET}"

    @classmethod
    def red(cls, text: str) -> str:
        return f"{cls.RED}{text}{cls.RESET}"

    @classmethod
    def yellow(cls, text: str) -> str:
        return f"{cls.YELLOW}{text}{cls.RESET}"

    @classmethod
    def cyan(cls, text: str) -> str:
        return f"{cls.CYAN}{text}{cls.RESET}"

    @classmethod
    def bold(cls, text: str) -> str:
        return f"{cls.BOLD}{text}{cls.RESET}"

    @classmethod
    def dim(cls, text: str) -> str:
        return f"{cls.DIM}{text}{cls.RESET}"
