#!/usr/bin/env python3
"""Emit PlatformIO build flags that identify a firmware image."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parents[1]
SEMVER = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)
REVISION = re.compile(r"^(?:[0-9a-fA-F]{7,64}|unknown)$")


def git(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args], cwd=PROJECT_DIR, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> None:
    base_version = (PROJECT_DIR / "VERSION").read_text(encoding="utf-8").strip()
    revision = os.environ.get("FIRMWARE_GIT_SHA") or git("rev-parse", "--short=12", "HEAD")
    version = os.environ.get("FIRMWARE_VERSION")

    if not version:
        dirty = git("status", "--porcelain") != ""
        if git("describe", "--exact-match", "--tags", "HEAD") == f"v{base_version}" and not dirty:
            version = base_version
        else:
            suffix = ".dirty" if dirty else ""
            version = f"{base_version}-dev.{revision}{suffix}"

    if not SEMVER.fullmatch(version):
        raise ValueError("FIRMWARE_VERSION must be a semantic version")
    if not REVISION.fullmatch(revision):
        raise ValueError("FIRMWARE_GIT_SHA must be a hexadecimal Git revision")

    print(f'-DFIRMWARE_VERSION=\\"{version}\\"')
    print(f'-DFIRMWARE_GIT_SHA=\\"{revision}\\"')


if __name__ == "__main__":
    main()
