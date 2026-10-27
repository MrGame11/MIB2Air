"""Operation outcomes independent of terminal wording and formatting."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Result:
    status: str
    headline: str
    paragraphs: tuple[str, ...] = ()
