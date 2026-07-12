# Scenario draft API

Scenario drafts are user-owned mutable documents. They never modify built-in
curated pack files. All validation is performed by the sibling
`signal_synth` public scenario API.

Routes:

- `GET /syn_sig_ra/v1/authoring/schema`
- `GET /syn_sig_ra/v1/authoring/templates`
- `POST /syn_sig_ra/v1/authoring/preview`
- `GET /syn_sig_ra/v1/authoring/curated-scenarios/{pack_id}/{case_id}`
- `GET /syn_sig_ra/v1/scenarios`
- `POST /syn_sig_ra/v1/scenarios`
- `GET /syn_sig_ra/v1/scenarios/{scenario_id}`
- `PUT /syn_sig_ra/v1/scenarios/{scenario_id}`
- `DELETE /syn_sig_ra/v1/scenarios/{scenario_id}`

Create or replace a draft:

```json
{
  "name": "My synthetic ECG",
  "target_intent": ["r_peak"],
  "scenario": {
    "schema_version": 2
  }
}
```

`target_intent` records what the user's algorithm is expected to output. It is
not part of the core scenario JSON or its fingerprint. The browser starts with
this goal, shows compatible templates and curated cases, and saves it beside
the canonical draft. Custom pack composition inherits the union of the selected
draft intents; an Advanced override remains available for deliberate expert use.

The `scenario` object must follow the authoritative `signal_synth` scenario
schema. Valid documents are canonicalized and receive a SHA-256 document
fingerprint. Invalid documents are still saved as editable drafts and return
HTTP 422 with `code`, JSON `path`, and `message` validation entries plus the
new or updated draft ID.

The target-first authoring flow uses core-owned metadata for the grouped browser
form: field labels, ranges, units, defaults, enum options, visibility rules,
condition and artifact catalogs, repeatable-item schemas, target support, and
complete valid templates. Conditions, artifacts, PPG perfusion episodes,
randomization envelopes, and tags can all be edited without writing JSON.
The preview route runs core pack analysis for a
single draft plus selected targets and returns scoreable/reference-only
targets, channel count, sample count, estimated package size, peak memory, and
compatibility messages. Validation paths focus the corresponding form control
where possible. Advanced JSON remains synchronized for expert use. The
curated-scenario route returns source scenario JSON for clone/fork into an
editable draft. **Show all starting points** and **Advanced JSON editor** keep
the complete core feature set available when the guided recommendations are too
narrow. Missing PPG, HRV duration/modulation, artifact, and morphology
requirements can be applied to the current draft in one action before preview.

Drafts are scoped to the exact organization/user identity. Another user,
including one in the same organization, receives 404. Viewer-role keys can
read their own drafts but cannot create, update, or delete them.

Only synthetic engineering inputs are permitted. Do not place PHI, personal
data, patient identifiers, or clinical free text in names or scenario fields.
