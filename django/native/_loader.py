"""
Load ``django._native`` when available and allowed.

Environment:
    DJANGO_NATIVE: set to ``0``, ``false``, ``no``, or ``off`` to force the
    pure-Python fallbacks even if the extension is built.
"""

from __future__ import annotations

import importlib
import os
from types import ModuleType
from typing import Final

__all__ = [
    "AVAILABLE",
    "DISABLED_BY_ENV",
    "get_native_module",
    "is_native_enabled",
]

_FALSEY: Final[frozenset[str]] = frozenset({"0", "false", "no", "off", ""})


def is_native_enabled() -> bool:
    """Return whether native acceleration is allowed by environment policy."""
    value = os.environ.get("DJANGO_NATIVE", "1").strip().lower()
    return value not in _FALSEY


def _try_import_native() -> ModuleType | None:
    """Import the compiled extension, or return ``None`` if unavailable."""
    if not is_native_enabled():
        return None
    try:
        return importlib.import_module("django._native")
    except ImportError:
        return None


DISABLED_BY_ENV: bool = not is_native_enabled()
_impl: ModuleType | None = _try_import_native()
AVAILABLE: bool = _impl is not None


def get_native_module() -> ModuleType | None:
    """Return the loaded ``django._native`` module, or ``None``."""
    return _impl
