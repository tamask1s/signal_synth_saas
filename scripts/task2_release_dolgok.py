#!/usr/bin/env python3
"""Stable release workflows for Codex approvals.

The wrapper validates every user-controlled value and delegates only to the
repository's checked scripts. It has no generic command-execution feature.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def safe_repo_file(value: str) -> str:
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or not (ROOT / path).is_file():
        raise argparse.ArgumentTypeError("file must be an existing file inside the repository")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("artifact")
    sub.add_parser("deploy")
    sub.add_parser("rollback")
    sub.add_parser("push")
    sub.add_parser("core-refresh")
    sub.add_parser("adopt-core")
    commit = sub.add_parser("commit")
    issue = commit.add_mutually_exclusive_group(required=True)
    issue.add_argument("--issue", type=int)
    issue.add_argument("--no-issue", action="store_true")
    commit.add_argument("--message", required=True)
    commit.add_argument("files", nargs="+", type=safe_repo_file)
    args = parser.parse_args()

    if args.action == "artifact":
        command = ["scripts/build_release_artifact.sh"]
    elif args.action == "deploy":
        command = ["scripts/deploy_live.sh"]
    elif args.action == "rollback":
        command = ["scripts/rollback_live.sh"]
    elif args.action == "push":
        command = ["git", "push", "origin", "master"]
    elif args.action == "core-refresh":
        command = [str(ROOT.parent / "signal_synth" / "scripts" / "refresh_curated_release.sh")]
    elif args.action == "adopt-core":
        command = ["scripts/adopt_core_release.py"]
    else:
        if args.issue is not None and args.issue < 1:
            parser.error("issue must be positive")
        if len(args.message) > 160 or not args.message.strip():
            parser.error("message must contain 1-160 characters")
        issue_value = str(args.issue) if args.issue is not None else "-"
        command = ["scripts/commit_checked.sh", issue_value, args.message, *args.files]
    subprocess.run(command, cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
