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
  "scenario": {
    "schema_version": 2
  }
}
```

The `scenario` object must follow the authoritative `signal_synth` scenario
schema. Valid documents are canonicalized and receive a SHA-256 document
fingerprint. Invalid documents are still saved as editable drafts and return
HTTP 422 with `code`, JSON `path`, and `message` validation entries plus the
new or updated draft ID.

The authoring helper routes expose core-owned metadata for the browser form:
field labels, groups, ranges, units, defaults, enum options, target support,
and complete valid templates. The preview route runs core pack analysis for a
single draft plus selected targets and returns scoreable/reference-only
targets, channel count, sample count, estimated package size, peak memory, and
compatibility messages. The curated-scenario route returns source scenario JSON
for clone/fork into an editable draft.

Drafts are scoped to the exact organization/user identity. Another user,
including one in the same organization, receives 404. Viewer-role keys can
read their own drafts but cannot create, update, or delete them.

Only synthetic engineering inputs are permitted. Do not place PHI, personal
data, patient identifiers, or clinical free text in names or scenario fields.
