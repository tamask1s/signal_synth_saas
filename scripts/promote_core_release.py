#!/usr/bin/env python3
"""Adopt, gate, commit, push, and deploy one clean core release.

There are deliberately no arguments and no arbitrary command execution. Both
repositories must start clean on pushed master commits. The release artifact is
built and tested once from the exact SaaS commit, then that same artifact is
deployed with the normal rollback guard.
"""

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = ROOT.parent / "signal_synth"


def run(command, cwd=ROOT):
    result = subprocess.run(
        [str(value) for value in command],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    if result.returncode:
        tail = "\n".join(result.stdout.splitlines()[-100:])
        raise RuntimeError(
            "{} failed ({}):\n{}".format(
                " ".join(str(value) for value in command),
                result.returncode,
                tail,
            ).rstrip()
        )
    return result.stdout.strip()


def require_pushed_master(repo, name):
    if run(["git", "status", "--porcelain"], repo):
        raise RuntimeError("{} must start clean".format(name))
    if run(["git", "branch", "--show-current"], repo) != "master":
        raise RuntimeError("{} must be on master".format(name))
    if run(["git", "rev-parse", "HEAD"], repo) != run(
            ["git", "rev-parse", "origin/master"], repo):
        raise RuntimeError("{} master must already match origin/master".format(name))


def value_from_lines(text, prefix):
    values = [line[len(prefix):] for line in text.splitlines()
              if line.startswith(prefix)]
    if len(values) != 1 or not values[0]:
        raise RuntimeError("release builder did not emit {}".format(prefix))
    return values[0]


def main():
    require_pushed_master(ROOT, "signal_synth_saas")
    require_pushed_master(CORE, "signal_synth")
    new_core = run(["git", "rev-parse", "HEAD"], CORE)

    run([ROOT / "scripts" / "adopt_core_release.py"])
    run([sys.executable, "-m", "py_compile",
         ROOT / "scripts" / "adopt_core_release.py",
         ROOT / "scripts" / "promote_core_release.py"])
    run(["sh", "-n", ROOT / "scripts" / "build_verifier_downloads.sh"])
    run(["git", "diff", "--check"])
    if not run(["git", "status", "--porcelain"]):
        raise RuntimeError("core adoption produced no SaaS changes")

    run(["git", "add", "--all"])
    run(["git", "diff", "--cached", "--check"])
    run(["git", "commit", "-m", "Adopt signal_synth {}".format(new_core[:12])])

    artifact_output = run([ROOT / "scripts" / "build_release_artifact.sh"])
    artifact = value_from_lines(artifact_output, "release_artifact=")
    release_id = value_from_lines(artifact_output, "release_id=")
    if run(["git", "status", "--porcelain"]):
        raise RuntimeError("release build modified tracked source files")
    run(["git", "push", "origin", "master"])
    deploy_output = run([ROOT / "scripts" / "deploy_release.sh", artifact])

    print("core_promotion=ok")
    print("core_commit={}".format(new_core))
    print("saas_commit={}".format(run(["git", "rev-parse", "HEAD"])))
    print("release_id={}".format(release_id))
    for line in deploy_output.splitlines():
        if line.startswith(("deployed_release=", "rollback_snapshot=")):
            print(line)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("promote_core_release: {}".format(error), file=sys.stderr)
        raise SystemExit(1)
