#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
signal_synth_root=${SIGNAL_SYNTH_ROOT:-"$repo_dir/../signal_synth"}
out_dir=${1:-"$repo_dir/downloads/verifier"}
work_dir=${TMPDIR:-/tmp}/synsigra_verifier_downloads_$$

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

if [ ! -f "$signal_synth_root/setup.cfg" ]; then
  echo "signal_synth Python package metadata not found at $signal_synth_root/setup.cfg" >&2
  exit 2
fi

version=$(
  awk '
    $1 == "version" && $2 == "=" {
      print $3
      exit
    }
  ' "$signal_synth_root/setup.cfg"
)
if [ -z "$version" ]; then
  echo "unable to read synsigra package version from setup.cfg" >&2
  exit 2
fi

mkdir -p "$work_dir/dist" "$work_dir/bundle/wheels" "$out_dir"
python3 - "$signal_synth_root" "$work_dir/dist" "$version" <<'PY'
import base64
import csv
import hashlib
import os
import pathlib
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
dist = pathlib.Path(sys.argv[2])
version = sys.argv[3]
package_root = root / "python" / "synsigra"
wheel_name = "synsigra-{}-py3-none-any.whl".format(version)
dist_info = "synsigra-{}.dist-info".format(version)
wheel_path = dist / wheel_name

def read_text(path):
    return pathlib.Path(path).read_text(encoding="utf-8")

def digest(data):
    raw = hashlib.sha256(data).digest()
    return "sha256=" + base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")

entries = []

for path in sorted(package_root.rglob("*")):
    if path.is_file():
        relative = pathlib.PurePosixPath("synsigra") / path.relative_to(package_root).as_posix()
        entries.append((str(relative), path.read_bytes()))

metadata = """Metadata-Version: 2.1
Name: synsigra
Version: {version}
Summary: Local synthetic biosignal challenge loading and verification SDK
Author: Synsigra
License: Proprietary engineering QA tooling
Requires-Python: >=3.8
Description-Content-Type: text/markdown

{description}
""".format(
    version=version,
    description=read_text(root / "python" / "README.md"),
)
entries.append((dist_info + "/METADATA", metadata.encode("utf-8")))
entries.append((dist_info + "/WHEEL", b"Wheel-Version: 1.0\nGenerator: signal_synth_saas\nRoot-Is-Purelib: true\nTag: py3-none-any\n"))
entries.append((dist_info + "/entry_points.txt", b"[console_scripts]\nsynsigra-verify = synsigra.cli:main\n"))

record_rows = []
with zipfile.ZipFile(str(wheel_path), "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for name, data in entries:
        archive.writestr(name, data)
        record_rows.append([name, digest(data), str(len(data))])
    record_name = dist_info + "/RECORD"
    record_rows.append([record_name, "", ""])
    import io
    record_text = io.StringIO()
    writer = csv.writer(record_text, lineterminator="\n")
    writer.writerows(record_rows)
    archive.writestr(record_name, record_text.getvalue().encode("utf-8"))

print(wheel_path)
PY

wheel=$(find "$work_dir/dist" -maxdepth 1 -type f -name 'synsigra-*.whl' | sort | tail -n 1)
if [ -z "$wheel" ] || [ ! -f "$wheel" ]; then
  echo "synsigra wheel was not built" >&2
  exit 1
fi
wheel_file=$(basename "$wheel")

cp "$wheel" "$work_dir/bundle/wheels/$wheel_file"
cat >"$work_dir/bundle/README.md" <<EOF
# Synsigra local verifier bundle

This bundle contains the generator-free Synsigra Python verifier package.

It does not contain the C++ signal generator, generator source code, or any
tool that can create new challenge packages. It only installs the local
\`synsigra-verify\` command used to score your own detector outputs against a
downloaded Synsigra challenge package.

## Install

\`\`\`sh
python -m pip install "wheels/$wheel_file"
synsigra-verify --help
\`\`\`

## Verify a downloaded package

1. Download \`package.zip\` and the detector-template ZIP from the Synsigra UI.
2. Unzip the detector templates and replace rows under \`detections/\` with
   your algorithm output.
3. Run:

\`\`\`sh
synsigra-verify package.zip detections/ verification-results/ --profile regression --force
\`\`\`

The verifier exits with:

- 0 when package integrity, scoring, and thresholds pass;
- 1 when package/input/scoring/threshold checks fail;
- 2 when command-line arguments are invalid.

Synthetic engineering QA only. This is not clinical validation software.
EOF

cat >"$work_dir/bundle/verify_smoke.sh" <<'EOF'
#!/bin/sh
set -eu

wheel=$(find "$(dirname "$0")/wheels" -maxdepth 1 -type f -name 'synsigra-*.whl' | sort | tail -n 1)
python -m pip install "$wheel"
synsigra-verify --help >/dev/null
echo "synsigra verifier smoke test passed"
EOF
chmod 0755 "$work_dir/bundle/verify_smoke.sh"

bundle_file="synsigra-verifier-$version.zip"
python3 - "$work_dir/bundle" "$work_dir/$bundle_file" "$wheel_file" <<'PY'
import pathlib
import sys
import zipfile

bundle_root = pathlib.Path(sys.argv[1])
archive_path = pathlib.Path(sys.argv[2])
wheel_file = sys.argv[3]
with zipfile.ZipFile(str(archive_path), "w", compression=zipfile.ZIP_DEFLATED) as archive:
    archive.write(str(bundle_root / "README.md"), "README.md")
    archive.write(str(bundle_root / "verify_smoke.sh"), "verify_smoke.sh")
    archive.write(str(bundle_root / "wheels" / wheel_file), "wheels/" + wheel_file)
PY

install -m 0644 "$wheel" "$out_dir/$wheel_file"
install -m 0644 "$wheel" "$out_dir/synsigra-wheel.whl"
install -m 0644 "$work_dir/$bundle_file" "$out_dir/$bundle_file"
install -m 0644 "$work_dir/$bundle_file" "$out_dir/synsigra-verifier.zip"

wheel_sha=$(sha256sum "$out_dir/synsigra-wheel.whl" | awk '{print $1}')
bundle_sha=$(sha256sum "$out_dir/synsigra-verifier.zip" | awk '{print $1}')
generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat >"$out_dir/metadata.json" <<EOF
{
  "schema_version": 1,
  "package": "synsigra",
  "version": "$version",
  "console_script": "synsigra-verify",
  "generator_included": false,
  "generated_at": "$generated_at",
  "files": [
    {
      "label": "Verifier bundle",
      "filename": "synsigra-verifier.zip",
      "versioned_filename": "$bundle_file",
      "content_type": "application/zip",
      "sha256": "sha256:$bundle_sha",
      "description": "ZIP containing README, smoke script, and the generator-free verifier wheel."
    },
    {
      "label": "Python wheel",
      "filename": "synsigra-wheel.whl",
      "versioned_filename": "$wheel_file",
      "content_type": "application/octet-stream",
      "sha256": "sha256:$wheel_sha",
      "description": "Pure-Python wheel that installs synsigra-verify without the C++ generator."
    }
  ],
  "install": [
    "unzip synsigra-verifier.zip",
    "python -m pip install wheels/$wheel_file",
    "synsigra-verify --help"
  ],
  "verify_example": "synsigra-verify package.zip detections/ verification-results/ --profile regression --force"
}
EOF

echo "Built verifier downloads in $out_dir"
ls -1 "$out_dir"
