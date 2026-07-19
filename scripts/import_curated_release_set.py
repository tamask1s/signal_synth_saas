#!/usr/bin/env python3
"""Import a signal_synth curated release-set artifact into SaaS pack files."""

import argparse
import json
import os
import shutil
import subprocess
import sys


def read_json(path):
    with open(path, "r") as handle:
        return json.load(handle)


def write_json(path, value):
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    with open(path, "w") as handle:
        handle.write(json.dumps(value, indent=2, sort_keys=True))
        handle.write("\n")


def slash_relpath(path, base):
    return os.path.relpath(os.path.abspath(path), os.path.abspath(base)).replace(os.sep, "/")


def resolve_signal_synth_cli(cli):
    if cli:
        return cli
    env_cli = os.environ.get("SIGNAL_SYNTH_CLI")
    if env_cli:
        return env_cli
    path_cli = shutil.which("signal-synth")
    if path_cli:
        return path_cli
    default_cli = "/opt/signal_synth/bin/signal-synth"
    return default_cli if os.path.exists(default_cli) else None


def validate_pack(signal_synth_cli, pack_path):
    if not signal_synth_cli:
        raise RuntimeError("signal-synth CLI is required for a curated import")
    output = subprocess.check_output(
        [signal_synth_cli, "pack", "validate", pack_path],
        universal_newlines=True,
    )
    if not any(line.startswith("pack_fingerprint=sha256:") for line in output.splitlines()):
        raise RuntimeError("signal-synth did not validate %s" % pack_path)


def import_pack(source_root, output_root, path_base, metadata, signal_synth_cli):
    source = metadata["source"]
    source_catalog_path = os.path.join(source_root, source["catalog_path"])
    source_pack_path = os.path.normpath(os.path.join(os.path.dirname(source_catalog_path), source["pack_path"]))
    pack = read_json(source_pack_path)
    pack_dir = os.path.dirname(source_pack_path)
    scenario_root = os.path.join(source_root, "examples", "scenarios")
    for scenario in pack.get("scenarios", []):
        scenario_path = os.path.normpath(os.path.join(pack_dir, scenario["path"]))
        relative_scenario = slash_relpath(scenario_path, scenario_root)
        if relative_scenario.startswith("../"):
            raise RuntimeError("pack scenario is outside examples/scenarios: %s" % scenario_path)
        output_scenario_path = os.path.join(output_root, "scenarios", *relative_scenario.split("/"))
        parent = os.path.dirname(output_scenario_path)
        if parent and not os.path.isdir(parent):
            os.makedirs(parent)
        shutil.copyfile(scenario_path, output_scenario_path)
        scenario["path"] = slash_relpath(output_scenario_path, path_base)
    protocol_path = pack.get("verification_protocol_path")
    if protocol_path:
        source_protocol = os.path.normpath(os.path.join(pack_dir, protocol_path))
        output_protocol = os.path.join(output_root, os.path.basename(protocol_path))
        shutil.copyfile(source_protocol, output_protocol)
        pack["verification_protocol_path"] = os.path.basename(output_protocol)
    pack_id = metadata["pack_id"]
    output_pack_path = os.path.join(output_root, pack_id + ".json")
    write_json(output_pack_path, pack)
    validate_pack(signal_synth_cli, output_pack_path)


def import_noise_assets(source_root, output_root):
    source = os.path.join(source_root, "examples", "assets", "noise")
    destination = os.path.join(output_root, "noise_assets")
    if os.path.isdir(destination):
        shutil.rmtree(destination)
    shutil.copytree(source, destination)
    fixture = os.path.join(destination, "synsigra_project_noise_v1.csv")
    if not os.path.isfile(fixture):
        raise RuntimeError("approved external-noise fixture is missing")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Import signal_synth curated release-set metadata into SaaS pack files.")
    parser.add_argument("--metadata", default="../signal_synth/examples/catalog/curated_pack_metadata_v1.json", help="signal_synth curated release-set metadata JSON.")
    parser.add_argument("--source-root", default="../signal_synth", help="signal_synth checkout root.")
    parser.add_argument("--out", default="packs", help="SaaS pack output directory.")
    parser.add_argument("--path-base", default=None, help="Directory used as the base for scenario relative paths. Defaults to --out.")
    parser.add_argument("--signal-synth-cli", default=None, help="Optional signal-synth CLI used to validate imported pack fingerprints.")
    parser.add_argument("--clean", action="store_true", help="Delete existing JSON/product files in the output directory first.")
    args = parser.parse_args(argv)

    metadata_path = os.path.abspath(args.metadata)
    source_root = os.path.abspath(args.source_root)
    output_root = os.path.abspath(args.out)
    path_base = os.path.abspath(args.path_base or args.out)
    signal_synth_cli = resolve_signal_synth_cli(args.signal_synth_cli)
    release_set = read_json(metadata_path)
    if release_set.get("metadata_type") != "synsigra_curated_pack_catalog":
        raise RuntimeError("metadata is not a Synsigra curated pack catalog")
    if args.clean and os.path.isdir(output_root):
        shutil.rmtree(output_root)
    if not os.path.isdir(output_root):
        os.makedirs(output_root)
    write_json(os.path.join(output_root, "curated_pack_metadata_v1.catalog"), release_set)
    import_noise_assets(source_root, output_root)
    for pack in release_set.get("packs", []):
        import_pack(source_root, output_root, path_base, pack, signal_synth_cli)
        print("imported %s" % pack["pack_id"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
