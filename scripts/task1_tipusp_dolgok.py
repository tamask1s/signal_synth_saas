#!/usr/bin/env python3
"""Stable read/test workflows for Codex approvals.

Only fixed subcommands are supported. This intentionally never executes an
arbitrary shell command or arbitrary Python source supplied by a caller.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

COMMANDS = {
    "status": [["git", "status", "--short", "--branch"], ["git", "diff", "--check"]],
    "quality": [["/bin/bash", "-lc", "RUN_E2E=1 scripts/build_release.sh && git diff --check"]],
    "live-verify": [["scripts/verify_live.sh"]],
    "mail-status": [["scripts/mail/verify_local_mta.sh"]],
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=sorted(COMMANDS))
    args = parser.parse_args()
    for command in COMMANDS[args.action]:
        subprocess.run(command, cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
