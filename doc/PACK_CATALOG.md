# Pack catalog release policy

Built-in packs are curated, immutable product releases. Each
`packs/<pack_id>.json` has a required `packs/<pack_id>.product` sidecar. The
authoritative `signal_synth` parser validates the pack and computes its
fingerprint; the SaaS then requires the sidecar's ID, semantic version, and
expected fingerprint to match exactly.

This prevents a scenario or pack edit from silently changing a deployed
release. A changed pack requires:

1. a semantic version increment;
2. a changelog entry;
3. an updated expected fingerprint after review;
4. an explicit generator compatibility declaration.

Major compatibility lines use stable IDs such as `r_peak_stress_v1`. Breaking
scenario/target changes require a new major pack ID (`..._v2`) so existing API
clients can continue requesting the old release. Additive, reviewed changes
may increment the semantic minor version, but published package artifacts and
their stored fingerprints remain immutable.

## Generator compatibility

`generator_contract` names the required CLI behavior and
`compatible_generator_versions` lists accepted generator version identities.
New jobs return `pack_generator_incompatible` rather than invoking an
unapproved generator. Completed jobs retain the exact generator build hash.

## Deprecation

Release status is `stable` or `deprecated`. Deprecated releases remain visible
for reproducibility and require a human-readable migration message. They are
not silently removed. Physical artifacts follow the retention policy, while
job, pack fingerprint, package fingerprint, and generator identity metadata
remain.

## Current curated beta set

- `r_peak_stress_v1` 1.0: R-peak smoke, rate-stress, and
  baseline/powerline signal-quality scenarios.
