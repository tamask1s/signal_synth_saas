# Custom pack composer

Custom packs are immutable snapshots assembled from valid scenario drafts.
They do not alter curated built-in packs.

Routes:

- `GET /syn_sig_ra/v1/custom-packs`
- `POST /syn_sig_ra/v1/custom-packs`
- `GET /syn_sig_ra/v1/custom-packs/{pack_id}`
- `DELETE /syn_sig_ra/v1/custom-packs/{pack_id}`

Composition request:

```json
{
  "name": "My R-peak validation pack",
  "description": "Synthetic clean and stress scenarios",
  "targets": ["r_peak"],
  "scenario_ids": ["scenario_..."]
}
```

Every selected draft must be valid and owned by the caller. Composition copies
the canonical scenario documents into an immutable data-root snapshot, builds
an authoritative schema-v1 pack, and stores its `signal_synth` fingerprint.
Before creating any files, the API runs authoritative core pack analysis and
returns HTTP 422 if any selected scenario is incompatible with a requested
target.
Later edits or deletion of the source drafts cannot change the custom pack.

`POST /syn_sig_ra/v1/jobs` accepts a curated or caller-owned custom `pack_id`.
Deleting a custom pack removes it from the composer and prevents new jobs, but
keeps its snapshot so existing queued jobs and retry records remain
reproducible.

The browser UI supports target cards, searchable draft selection, a core-backed
scenario-by-target coverage matrix, package-size/duration/memory estimates,
creating/deleting custom packs, choosing them in the normal job form, polling
the job, and downloading the resulting manifest and ZIP.
