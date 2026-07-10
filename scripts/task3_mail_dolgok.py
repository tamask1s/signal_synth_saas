#!/usr/bin/env python3
"""Stable mail-operation workflows for Codex approvals."""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
EMAIL = re.compile(r"^[^@\s]+@(gmail\.com|googlemail\.com)$", re.I)


def sender(value: str) -> str:
    if not EMAIL.fullmatch(value):
        raise argparse.ArgumentTypeError("sender must be a Gmail or Googlemail address")
    return value


def secret_source(value: str) -> str:
    path = pathlib.Path(value).resolve()
    allowed = pathlib.Path("/home/graphyt2/sec").resolve()
    if not path.is_file() or allowed not in path.parents:
        raise argparse.ArgumentTypeError(
            "source must be a regular file under /home/graphyt2/sec")
    return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("local-status")
    sub.add_parser("local-install")
    gmail = sub.add_parser("gmail-config")
    gmail.add_argument("sender", type=sender)
    gmail_file = sub.add_parser("gmail-config-file")
    gmail_file.add_argument("sender", type=sender)
    gmail_file.add_argument("--source", required=True, type=secret_source)
    verify = sub.add_parser("gmail-verify")
    verify.add_argument("sender", type=sender)
    args = parser.parse_args()

    if args.action == "local-status":
        command = ["scripts/mail/verify_local_mta.sh"]
    elif args.action == "local-install":
        command = ["sudo", "scripts/mail/install_local_mta.sh"]
    elif args.action == "gmail-config":
        command = ["sudo", "ops/mail/configure_gmail_smtp.sh", args.sender]
    elif args.action == "gmail-config-file":
        command = [
            "sudo", "env", "SYN_SIG_RA_GMAIL_SOURCE_FILE=" + args.source,
            "ops/mail/configure_gmail_smtp.sh", args.sender
        ]
    else:
        command = ["sudo", "ops/mail/verify_gmail_smtp.sh", args.sender]
    subprocess.run(command, cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
