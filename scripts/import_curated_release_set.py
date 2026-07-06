#!/usr/bin/env python3
"""Import a signal_synth curated release-set artifact into SaaS pack files."""

import argparse
import json
import os
import shutil
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


def product_changelog(entries):
    output = []
    for entry in entries:
        changes = entry.get("changes", [])
        summary = entry.get("summary")
        if not summary:
            summary = " ".join(changes) if changes else ""
        output.append({
            "version": entry["version"],
            "date": entry["date"],
            "summary": summary,
        })
    return output


def compatible_generator_versions(metadata):
    compatibility = metadata.get("generator_compatibility", {})
    versions = []
    minimum = compatibility.get("minimum_generator_version")
    if minimum:
        versions.append(minimum)
    versions.append("signal_synth-cli")
    output = []
    for item in versions:
        if item not in output:
            output.append(item)
    return output


def import_pack(source_root, output_root, path_base, metadata):
    source = metadata["source"]
    source_catalog_path = os.path.join(source_root, source["catalog_path"])
    source_pack_path = os.path.normpath(os.path.join(os.path.dirname(source_catalog_path), source["pack_path"]))
    pack = read_json(source_pack_path)
    pack_dir = os.path.dirname(source_pack_path)
    for scenario in pack.get("scenarios", []):
        scenario_path = os.path.normpath(os.path.join(pack_dir, scenario["path"]))
        scenario["path"] = slash_relpath(scenario_path, path_base)
    pack_id = metadata["pack_id"]
    write_json(os.path.join(output_root, pack_id + ".json"), pack)
    product = {
        "schema_version": 1,
        "pack_id": pack_id,
        "version": metadata["version"],
        "release_status": metadata["release_status"],
        "released_at": metadata["release_date"],
        "expected_pack_fingerprint": source["pack_fingerprint"],
        "generator_contract": "signal-synth-cli/pack-challenge-v1",
        "compatible_generator_versions": compatible_generator_versions(metadata),
        "deprecation_message": metadata.get("deprecation_message", ""),
        "changelog": product_changelog(metadata.get("changelog", [])),
    }
    write_json(os.path.join(output_root, pack_id + ".product"), product)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Import signal_synth curated release-set metadata into SaaS pack files.")
    parser.add_argument("--metadata", default="../signal_synth/examples/catalog/curated_pack_metadata_v1.json", help="signal_synth curated release-set metadata JSON.")
    parser.add_argument("--source-root", default="../signal_synth", help="signal_synth checkout root.")
    parser.add_argument("--out", default="packs", help="SaaS pack output directory.")
    parser.add_argument("--path-base", default=None, help="Directory used as the base for scenario relative paths. Defaults to --out.")
    parser.add_argument("--clean", action="store_true", help="Delete existing JSON/product files in the output directory first.")
    args = parser.parse_args(argv)

    metadata_path = os.path.abspath(args.metadata)
    source_root = os.path.abspath(args.source_root)
    output_root = os.path.abspath(args.out)
    path_base = os.path.abspath(args.path_base or args.out)
    release_set = read_json(metadata_path)
    if release_set.get("metadata_type") != "synsigra_curated_pack_catalog":
        raise RuntimeError("metadata is not a Synsigra curated pack catalog")
    if args.clean and os.path.isdir(output_root):
        for name in os.listdir(output_root):
            if name.endswith(".json") or name.endswith(".product"):
                os.remove(os.path.join(output_root, name))
    if not os.path.isdir(output_root):
        os.makedirs(output_root)
    for pack in release_set.get("packs", []):
        import_pack(source_root, output_root, path_base, pack)
        print("imported %s" % pack["pack_id"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
