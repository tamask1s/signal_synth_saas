#include "syn_sig_ra/route.h"

#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/build_info.h"
#include "syn_sig_ra/job_request.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/pack_catalog.h"

#include <jansson.h>

#include <cctype>
#include <cstdlib>
#include <sys/statvfs.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

bool owns_uri(const std::string& uri, const std::string& prefix) {
    return uri == prefix ||
           (uri.size() > prefix.size() &&
            uri.compare(0, prefix.size(), prefix) == 0 &&
            uri[prefix.size()] == '/');
}

bool path_at_or_below(const std::string& uri, const std::string& path) {
    return uri == path ||
           (uri.size() > path.size() &&
            uri.compare(0, path.size(), path) == 0 &&
            uri[path.size()] == '/');
}

syn_sig_ra::RouteResponse json_response(
    int status,
    const std::string& body
) {
    syn_sig_ra::RouteResponse response;
    response.disposition = syn_sig_ra::RouteDisposition::handled;
    response.status = status;
    response.content_type = "application/json";
    response.body = body;
    return response;
}

bool is_json_content_type(const std::string& content_type) {
    const std::string expected("application/json");
    if (content_type.size() < expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (std::tolower(
                static_cast<unsigned char>(content_type[index])
            ) != expected[index]) {
            return false;
        }
    }
    return content_type.size() == expected.size() ||
           content_type[expected.size()] == ';';
}

std::string json_dump_line(json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        return "{}\n";
    }
    std::string output(encoded);
    free(encoded);
    output += '\n';
    return output;
}

json_t* job_json_object(
    const syn_sig_ra::JobRecord& job,
    const std::string& public_base_path
) {
    json_t* root = json_object();
    json_object_set_new(root, "job_id", json_string(job.job_id.c_str()));
    json_object_set_new(root, "project_id", json_string(job.project_id.c_str()));
    json_object_set_new(root, "status", json_string(job.status.c_str()));
    json_object_set_new(
        root,
        "pack_id",
        json_string(job.selected_pack_id.c_str())
    );
    json_object_set_new(
        root,
        "created_at",
        json_string(job.created_at.c_str())
    );
    if (!job.started_at.empty()) {
        json_object_set_new(
            root,
            "started_at",
            json_string(job.started_at.c_str())
        );
    }
    if (!job.completed_at.empty()) {
        json_object_set_new(
            root,
            "completed_at",
            json_string(job.completed_at.c_str())
        );
    }
    if (job.status == "succeeded") {
        if (job.package_id.empty()) {
            json_object_set_new(
                root, "artifact_status", json_string("expired")
            );
            return root;
        }
        json_object_set_new(
            root, "artifact_status", json_string("available")
        );
        json_object_set_new(
            root,
            "package_id",
            json_string(job.package_id.c_str())
        );
        json_object_set_new(
            root,
            "package_fingerprint",
            json_string(job.package_fingerprint.c_str())
        );
        json_object_set_new(
            root,
            "generator_version",
            json_string(job.generator_version.c_str())
        );
        json_object_set_new(
            root,
            "generator_build_identity",
            json_string(job.generator_build_identity.c_str())
        );
        json_object_set_new(
            root,
            "manifest_url",
            json_string(
                (
                    public_base_path + "/v1/artifacts/" + job.package_id +
                    "/manifest.json"
                ).c_str()
            )
        );
        json_object_set_new(
            root,
            "archive_url",
            json_string(
                (
                    public_base_path + "/v1/artifacts/" + job.package_id +
                    "/package.zip"
                ).c_str()
            )
        );
    } else if (job.status == "failed") {
        json_t* error = json_object();
        json_object_set_new(
            error,
            "code",
            json_string(job.error_code.c_str())
        );
        json_object_set_new(
            error,
            "message",
            json_string(job.error_message.c_str())
        );
        json_object_set_new(root, "error", error);
    }
    return root;
}

std::string job_json(
    const syn_sig_ra::JobRecord& job,
    const std::string& public_base_path
) {
    json_t* root = job_json_object(job, public_base_path);
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

std::string job_list_json(
    const std::vector<syn_sig_ra::JobRecord>& jobs,
    const std::string& public_base_path,
    int limit,
    int offset
) {
    json_t* root = json_object();
    json_t* array = json_array();
    for (std::vector<syn_sig_ra::JobRecord>::const_iterator it = jobs.begin();
         it != jobs.end();
         ++it) {
        json_array_append_new(array, job_json_object(*it, public_base_path));
    }
    json_object_set_new(root, "jobs", array);
    json_object_set_new(
        root,
        "limit",
        json_integer(static_cast<json_int_t>(limit))
    );
    json_object_set_new(
        root,
        "count",
        json_integer(static_cast<json_int_t>(jobs.size()))
    );
    json_object_set_new(root, "offset", json_integer(offset));
    if (jobs.size() == static_cast<std::size_t>(limit)) {
        json_object_set_new(root, "next_offset", json_integer(offset + limit));
    }
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

bool query_integer(
    const std::string& query,
    const std::string& name,
    int default_value,
    int& value
) {
    value = default_value;
    if (query.empty()) return true;
    const std::string needle = name + "=";
    const std::string::size_type start = query.find(needle);
    if (start == std::string::npos ||
        (start != 0 && query[start - 1] != '&')) return true;
    const std::string::size_type value_start = start + needle.size();
    const std::string::size_type end = query.find('&', value_start);
    const std::string encoded = query.substr(value_start, end - value_start);
    if (encoded.empty()) return false;
    char* parsed_end = nullptr;
    const long parsed = std::strtol(encoded.c_str(), &parsed_end, 10);
    if (*parsed_end != '\0' || parsed < 0 || parsed > 1000000) return false;
    value = static_cast<int>(parsed);
    return true;
}

json_t* project_json_object(const syn_sig_ra::ProjectRecord& project) {
    json_t* root = json_object();
    json_object_set_new(
        root, "project_id", json_string(project.project_id.c_str())
    );
    json_object_set_new(
        root, "display_name", json_string(project.display_name.c_str())
    );
    json_object_set_new(
        root, "created_at", json_string(project.created_at.c_str())
    );
    return root;
}

std::string usage_json(const syn_sig_ra::UsageSummary& usage) {
    json_t* root = json_object();
    json_object_set_new(root, "requests_last_minute",
                        json_integer(usage.requests_last_minute));
    json_object_set_new(root, "active_jobs", json_integer(usage.active_jobs));
    json_object_set_new(root, "jobs_this_month",
                        json_integer(usage.jobs_this_month));
    json_object_set_new(root, "packages_this_month",
                        json_integer(usage.packages_this_month));
    json_object_set_new(root, "package_bytes_this_month",
                        json_integer(usage.package_bytes_this_month));
    json_object_set_new(root, "queued_jobs", json_integer(usage.queued_jobs));
    json_object_set_new(root, "running_jobs", json_integer(usage.running_jobs));
    json_object_set_new(root, "failed_jobs_this_month",
                        json_integer(usage.failed_jobs_this_month));
    json_object_set_new(root, "quota_rejections_this_month",
                        json_integer(usage.quota_rejections_this_month));
    json_object_set_new(root, "worker_last_seen_at",
                        json_string(usage.worker_last_seen_at.c_str()));
    json_object_set_new(root, "worker_last_status",
                        json_string(usage.worker_last_status.c_str()));
    json_t* limits = json_object();
    json_object_set_new(limits, "requests_per_minute",
                        json_integer(usage.request_limit_per_minute));
    json_object_set_new(limits, "concurrent_jobs",
                        json_integer(usage.concurrent_job_limit));
    json_object_set_new(limits, "jobs_per_month",
                        json_integer(usage.monthly_job_limit));
    json_object_set_new(root, "limits", limits);
    const std::string output = json_dump_line(root);
    json_decref(root);
    return output;
}

const char kUiHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SynSigRa SaaS</title>
  <link rel="stylesheet" href="/syn_sig_ra/ui/style.css">
</head>
<body>
  <main class="shell">
    <section class="hero">
      <div>
        <p class="eyebrow">SynSigRa private beta</p>
        <h1>Challenge package generator</h1>
        <p class="lede">Generate deterministic synthetic biosignal QA packages from curated packs, then download the manifest and archive.</p>
      </div>
      <div class="status-card">
        <div class="label">Service</div>
        <div id="health-status" class="status">checking…</div>
      </div>
    </section>

    <section class="panel">
      <h2>API key</h2>
      <p class="muted">Paste a beta API key. It is kept only in this browser tab session.</p>
      <div class="row">
        <input id="api-key" type="password" autocomplete="off" placeholder="Bearer API key">
        <button id="save-key">Use key</button>
        <button id="clear-key" class="secondary">Clear</button>
      </div>
      <p id="key-status" class="muted"></p>
    </section>

    <section class="grid">
      <div class="panel">
        <div class="panel-heading">
          <h2>Packs</h2>
          <button id="refresh-packs" class="secondary">Refresh</button>
        </div>
        <div id="packs" class="cards"></div>
      </div>

      <div class="panel">
        <div class="panel-heading">
          <h2>Create job</h2>
        </div>
        <label for="project-select">Project</label>
        <select id="project-select"></select>
        <div class="row">
          <input id="project-name" type="text" maxlength="100" placeholder="New project name">
          <button id="create-project" class="secondary">Create project</button>
        </div>
        <label for="pack-select">Pack</label>
        <select id="pack-select"></select>
        <button id="create-job" class="primary">Create challenge job</button>
        <pre id="create-output" class="output"></pre>
      </div>
    </section>

    <section class="panel">
      <div class="panel-heading">
        <h2>Jobs</h2>
        <button id="refresh-jobs" class="secondary">Refresh</button>
      </div>
      <div id="jobs" class="jobs"></div>
    </section>
    <section class="panel">
      <div class="panel-heading">
        <h2>Usage</h2>
        <button id="refresh-usage" class="secondary">Refresh</button>
      </div>
      <div id="usage" class="muted">Paste an API key to inspect usage.</div>
    </section>
  </main>
  <script src="/syn_sig_ra/ui/app.js"></script>
</body>
</html>
)HTML";

const char kUiCss[] = R"CSS(:root {
  color-scheme: light;
  --bg: #f6f7fb;
  --panel: #ffffff;
  --text: #172033;
  --muted: #667085;
  --border: #d9deea;
  --primary: #2258e8;
  --primary-dark: #1644bb;
  --danger: #b42318;
  --ok: #067647;
  --warn: #b54708;
}

* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font: 15px/1.5 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}

.shell {
  max-width: 1180px;
  margin: 0 auto;
  padding: 32px 20px 64px;
}

.hero {
  display: grid;
  grid-template-columns: 1fr minmax(220px, 280px);
  gap: 24px;
  align-items: stretch;
  margin-bottom: 24px;
}

.eyebrow {
  margin: 0 0 8px;
  color: var(--primary);
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: .08em;
  font-size: 12px;
}

h1, h2, h3, p { margin-top: 0; }
h1 { font-size: clamp(32px, 5vw, 56px); line-height: 1; margin-bottom: 16px; }
h2 { font-size: 20px; margin-bottom: 14px; }
h3 { font-size: 16px; margin-bottom: 8px; }
.lede { max-width: 720px; color: var(--muted); font-size: 18px; }
.muted { color: var(--muted); }

.panel, .status-card {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 18px;
  padding: 20px;
  box-shadow: 0 8px 24px rgba(16, 24, 40, .06);
}

.status-card {
  display: flex;
  flex-direction: column;
  justify-content: center;
}

.label {
  color: var(--muted);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: .08em;
}

.status {
  margin-top: 8px;
  font-weight: 700;
  font-size: 20px;
}

.grid {
  display: grid;
  grid-template-columns: minmax(0, 1.3fr) minmax(320px, .7fr);
  gap: 20px;
  margin: 20px 0;
}

.row, .panel-heading {
  display: flex;
  gap: 10px;
  align-items: center;
}

.panel-heading {
  justify-content: space-between;
  margin-bottom: 12px;
}

input, select, button {
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 11px 12px;
  font: inherit;
}

input, select {
  background: #fff;
  min-width: 0;
  width: 100%;
}

button {
  cursor: pointer;
  background: var(--primary);
  border-color: var(--primary);
  color: #fff;
  font-weight: 700;
  white-space: nowrap;
}

button:hover { background: var(--primary-dark); }
button.secondary {
  background: #fff;
  border-color: var(--border);
  color: var(--text);
}
button.secondary:hover { background: #eef2ff; }
button.primary { width: 100%; margin-top: 12px; }
button.danger {
  background: #fff;
  border-color: #fda29b;
  color: var(--danger);
}
button.danger:hover { background: #fee4e2; }
button:disabled { opacity: .55; cursor: not-allowed; }

.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
  gap: 12px;
}

.card, .job {
  border: 1px solid var(--border);
  border-radius: 14px;
  padding: 14px;
  background: #fff;
}

.fingerprint {
  display: block;
  overflow-wrap: anywhere;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 12px;
  color: var(--muted);
}

.jobs {
  display: grid;
  gap: 12px;
}

.job-header {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  align-items: flex-start;
}

.badge {
  border-radius: 999px;
  padding: 4px 9px;
  font-size: 12px;
  font-weight: 700;
  background: #eef2ff;
  color: var(--primary);
}

.badge.succeeded { background: #dcfae6; color: var(--ok); }
.badge.failed { background: #fee4e2; color: var(--danger); }
.badge.running { background: #fef0c7; color: var(--warn); }

.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 12px;
}

.output {
  min-height: 44px;
  max-height: 220px;
  overflow: auto;
  background: #101828;
  color: #f2f4f7;
  border-radius: 12px;
  padding: 12px;
  white-space: pre-wrap;
}

.error { color: var(--danger); }
.ok { color: var(--ok); }

@media (max-width: 820px) {
  .hero, .grid { grid-template-columns: 1fr; }
  .row { align-items: stretch; flex-direction: column; }
}
)CSS";

const char kUiJs[] = R"JS((() => {
  const base = "/syn_sig_ra";
  const state = {
    apiKey: sessionStorage.getItem("syn_sig_ra_api_key") || "",
    packs: [],
    projects: [],
    jobs: [],
    jobsFingerprint: "",
    jobsLoaded: false,
    jobPollInFlight: false
  };

  const $ = (id) => document.getElementById(id);

  function setText(id, text, className) {
    const node = $(id);
    node.textContent = text;
    node.className = className || "";
  }

  function headers(json) {
    const h = {};
    if (json) h["Content-Type"] = "application/json";
    if (state.apiKey) h.Authorization = `Bearer ${state.apiKey}`;
    return h;
  }

  async function api(path, options = {}) {
    const response = await fetch(base + path, {
      ...options,
      headers: { ...headers(options.json), ...(options.headers || {}) },
      body: options.json ? JSON.stringify(options.json) : options.body
    });
    const text = await response.text();
    let body = null;
    try { body = text ? JSON.parse(text) : null; } catch (_) {}
    if (!response.ok) {
      const message = body && body.error ? `${body.error.code}: ${body.error.message}` : text || response.statusText;
      throw new Error(message);
    }
    return body;
  }

  async function checkHealth() {
    try {
      const body = await api("/healthz");
      setText("health-status", `${body.status} (${body.build.version})`, "status ok");
    } catch (error) {
      setText("health-status", error.message, "status error");
    }
  }

  function renderKeyState() {
    $("api-key").value = state.apiKey;
    setText(
      "key-status",
      state.apiKey ? "API key loaded for this tab session." : "No API key loaded.",
      state.apiKey ? "muted ok" : "muted"
    );
  }

  async function loadPacks() {
    $("packs").textContent = "Loading packs…";
    try {
      const body = await api("/v1/packs");
      state.packs = body.packs || [];
      const select = $("pack-select");
      select.innerHTML = "";
      state.packs.forEach((pack) => {
        const option = document.createElement("option");
        option.value = pack.pack_id;
        option.textContent = `${pack.display_name || pack.pack_id} (${pack.pack_id})`;
        select.appendChild(option);
      });
      $("packs").innerHTML = state.packs.map((pack) => `
        <article class="card">
          <h3>${escapeHtml(pack.display_name || pack.pack_id)}</h3>
          <p class="muted">Version ${escapeHtml(pack.version || "")} · ${escapeHtml(pack.scenario_count || 0)} scenarios</p>
          <p class="muted">${escapeHtml(pack.description || "")}</p>
          <p class="muted">Targets: ${escapeHtml((pack.targets || []).join(", ") || "n/a")}</p>
          <details>
            <summary>Scenarios</summary>
            <ul>
              ${(pack.scenarios || []).map((scenario) => `
                <li>${escapeHtml(scenario.scenario_id)} <span class="muted">(${escapeHtml((scenario.targets || []).join(", "))})</span></li>
              `).join("")}
            </ul>
          </details>
          <span class="fingerprint">${escapeHtml(pack.pack_fingerprint || "")}</span>
        </article>
      `).join("") || "<p class=\"muted\">No packs configured.</p>";
    } catch (error) {
      $("packs").innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
    }
  }

  async function loadProjects() {
    const select = $("project-select");
    select.innerHTML = "";
    if (!state.apiKey) return;
    try {
      const body = await api("/v1/projects");
      state.projects = body.projects || [];
      $("create-project").disabled = !["owner", "admin"].includes(body.role);
      state.projects.forEach((project) => {
        const option = document.createElement("option");
        option.value = project.project_id;
        option.textContent = project.display_name;
        select.appendChild(option);
      });
    } catch (error) {
      $("create-output").textContent = error.message;
    }
  }

  async function loadUsage() {
    if (!state.apiKey) {
      $("usage").textContent = "Paste an API key to inspect usage.";
      return;
    }
    try {
      const usage = await api("/v1/usage");
      $("usage").innerHTML = `
        <p>Requests/minute: ${escapeHtml(usage.requests_last_minute)} / ${escapeHtml(usage.limits.requests_per_minute)}</p>
        <p>Active jobs: ${escapeHtml(usage.active_jobs)} / ${escapeHtml(usage.limits.concurrent_jobs)}</p>
        <p>Jobs this month: ${escapeHtml(usage.jobs_this_month)} / ${escapeHtml(usage.limits.jobs_per_month)}</p>
        <p>Packages this month: ${escapeHtml(usage.packages_this_month)} · ${escapeHtml(usage.package_bytes_this_month)} bytes</p>
      `;
    } catch (error) {
      $("usage").textContent = error.message;
    }
  }

  async function createProject() {
    const displayName = $("project-name").value.trim();
    if (!displayName) return;
    $("create-project").disabled = true;
    try {
      const created = await api("/v1/projects", {
        method: "POST",
        json: { display_name: displayName }
      });
      $("project-name").value = "";
      await loadProjects();
      $("project-select").value = created.project_id;
    } catch (error) {
      $("create-output").textContent = error.message;
    }
  }

  async function createJob() {
    if (!state.apiKey) {
      $("create-output").textContent = "Paste an API key first.";
      return;
    }
    const packId = $("pack-select").value;
    const projectId = $("project-select").value;
    $("create-job").disabled = true;
    $("create-output").textContent = "Creating job…";
    try {
      const body = await api("/v1/jobs", { method: "POST", json: { project_id: projectId, pack_id: packId } });
      $("create-output").textContent = JSON.stringify(body, null, 2);
      await loadJobs({ force: true });
      await loadUsage();
    } catch (error) {
      $("create-output").textContent = error.message;
    } finally {
      $("create-job").disabled = false;
    }
  }

  function jobsFingerprint(jobs) {
    return JSON.stringify(jobs.map((job) => ({
      job_id: job.job_id,
      project_id: job.project_id,
      status: job.status,
      package_id: job.package_id || "",
      package_fingerprint: job.package_fingerprint || "",
      completed_at: job.completed_at || "",
      error: job.error || null
    })));
  }

  async function loadJobs(options = {}) {
    const container = $("jobs");
    if (!state.apiKey) {
      state.jobs = [];
      state.jobsFingerprint = "";
      state.jobsLoaded = false;
      container.innerHTML = "<p class=\"muted\">Paste an API key to list jobs.</p>";
      return;
    }
    if (state.jobPollInFlight) return;
    state.jobPollInFlight = true;
    if (!options.silent && !state.jobsLoaded) {
      container.textContent = "Loading jobs…";
    }
    try {
      const body = await api("/v1/jobs");
      const jobs = body.jobs || [];
      const fingerprint = jobsFingerprint(jobs);
      if (options.force || fingerprint !== state.jobsFingerprint) {
        state.jobs = jobs;
        state.jobsFingerprint = fingerprint;
        state.jobsLoaded = true;
        renderJobs();
      }
    } catch (error) {
      if (!options.silent) {
        container.innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
      }
    } finally {
      state.jobPollInFlight = false;
    }
  }

  function renderJobs() {
    const container = $("jobs");
    if (!state.jobs.length) {
      container.innerHTML = "<p class=\"muted\">No jobs yet.</p>";
      return;
    }
    container.innerHTML = state.jobs.map((job) => {
      const artifactActions = job.status === "succeeded" && job.package_id ? `
        <button class="secondary" data-download="${escapeHtml(job.package_id)}" data-file="manifest.json">Manifest</button>
        <button class="secondary" data-download="${escapeHtml(job.package_id)}" data-file="package.zip">Package ZIP</button>
      ` : "";
      const artifactExpired = job.artifact_status === "expired"
        ? `<p class="muted">Artifacts expired; reproducibility metadata remains.</p>`
        : "";
      const deleteAction = job.status === "running" ? "" : `
        <button class="danger" data-delete-job="${escapeHtml(job.job_id)}">Delete</button>
      `;
      const cancelAction = job.status === "queued" ? `
        <button class="secondary" data-job-action="cancel" data-job-id="${escapeHtml(job.job_id)}">Cancel</button>
      ` : "";
      const retryAction = ["failed", "cancelled"].includes(job.status) ? `
        <button class="secondary" data-job-action="retry" data-job-id="${escapeHtml(job.job_id)}">Retry</button>
      ` : "";
      const error = job.error ? `<p class="error">${escapeHtml(job.error.code)}: ${escapeHtml(job.error.message)}</p>` : "";
      return `
        <article class="job">
          <div class="job-header">
            <div>
              <h3>${escapeHtml(job.pack_id || "(unknown pack)")}</h3>
              <span class="fingerprint">${escapeHtml(job.job_id)}</span>
            </div>
            <span class="badge ${escapeHtml(job.status)}">${escapeHtml(job.status)}</span>
          </div>
          <p class="muted">Created: ${escapeHtml(job.created_at || "")}</p>
          ${job.package_id ? `<p class="muted">Package: <span class="fingerprint">${escapeHtml(job.package_id)}</span></p>` : ""}
          ${job.package_fingerprint ? `<span class="fingerprint">${escapeHtml(job.package_fingerprint)}</span>` : ""}
          ${artifactExpired}
          ${error}
          <div class="actions">
            ${artifactActions}
            ${cancelAction}
            ${retryAction}
            ${deleteAction}
          </div>
        </article>
      `;
    }).join("");
  }

  async function deleteJob(jobId) {
    if (!confirm("Delete this job from your job list? Download links for its package will stop working.")) {
      return;
    }
    try {
      await api(`/v1/jobs/${encodeURIComponent(jobId)}`, { method: "DELETE" });
      state.jobs = state.jobs.filter((job) => job.job_id !== jobId);
      state.jobsFingerprint = jobsFingerprint(state.jobs);
      state.jobsLoaded = true;
      renderJobs();
    } catch (error) {
      alert(error.message);
    }
  }

  async function runJobAction(jobId, action) {
    try {
      await api(`/v1/jobs/${encodeURIComponent(jobId)}/${action}`, {
        method: "POST"
      });
      await loadJobs({ force: true });
    } catch (error) {
      alert(error.message);
    }
  }

  async function downloadArtifact(packageId, file) {
    try {
      const response = await fetch(`${base}/v1/artifacts/${encodeURIComponent(packageId)}/${file}`, {
        headers: headers(false)
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
      }
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = `${packageId}-${file}`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
    } catch (error) {
      alert(error.message);
    }
  }

  function escapeHtml(value) {
    return String(value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  $("save-key").addEventListener("click", async () => {
    state.apiKey = $("api-key").value.trim();
    if (state.apiKey) sessionStorage.setItem("syn_sig_ra_api_key", state.apiKey);
    else sessionStorage.removeItem("syn_sig_ra_api_key");
    renderKeyState();
    await loadProjects();
    await loadUsage();
    await loadJobs({ force: true });
  });

  $("clear-key").addEventListener("click", async () => {
    state.apiKey = "";
    sessionStorage.removeItem("syn_sig_ra_api_key");
    renderKeyState();
    await loadProjects();
    await loadUsage();
    await loadJobs({ force: true });
  });

  $("refresh-packs").addEventListener("click", loadPacks);
  $("refresh-jobs").addEventListener("click", () => loadJobs({ force: true }));
  $("refresh-usage").addEventListener("click", loadUsage);
  $("create-job").addEventListener("click", createJob);
  $("create-project").addEventListener("click", createProject);
  $("jobs").addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const deleteJobId = target.getAttribute("data-delete-job");
    if (deleteJobId) deleteJob(deleteJobId);
    const jobAction = target.getAttribute("data-job-action");
    const actionJobId = target.getAttribute("data-job-id");
    if (jobAction && actionJobId) runJobAction(actionJobId, jobAction);
    const packageId = target.getAttribute("data-download");
    const file = target.getAttribute("data-file");
    if (packageId && file) downloadArtifact(packageId, file);
  });

  renderKeyState();
  checkHealth();
  loadPacks();
  loadProjects();
  loadUsage();
  loadJobs({ force: true });
  setInterval(() => loadJobs({ silent: true }), 5000);
})();
)JS";

}  // namespace

namespace syn_sig_ra {

RouteResponse route_request(const std::string& method, const std::string& uri) {
    return route_request(method, uri, "/syn_sig_ra", "", nullptr, "");
}

RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path
) {
    return route_request(
        method,
        uri,
        public_base_path,
        "",
        nullptr,
        ""
    );
}

bool route_requires_authentication(
    const std::string& uri,
    const std::string& public_base_path
) {
    return path_at_or_below(uri, public_base_path + "/v1/jobs") ||
           path_at_or_below(uri, public_base_path + "/v1/artifacts") ||
           path_at_or_below(uri, public_base_path + "/v1/projects") ||
           path_at_or_below(uri, public_base_path + "/v1/usage") ||
           path_at_or_below(uri, public_base_path + "/v1/metrics");
}

RouteResponse route_request(
    const std::string& method,
    const std::string& uri,
    const std::string& public_base_path,
    const std::string& authorization_header,
    MetadataStore* metadata_store,
    const std::string& pack_root,
    const std::string& content_type,
    const std::string& request_body,
    const std::string& data_root,
    const std::string& query_string,
    const std::string& signal_synth_cli
) {
    if (!owns_uri(uri, public_base_path)) {
        RouteResponse response;
        response.disposition = RouteDisposition::declined;
        response.status = 0;
        return response;
    }

    const std::string ready_path = public_base_path + "/readyz";
    if (uri == ready_path) {
        if (method != "GET") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Readiness only accepts GET.\"}}\n");
        }
        const bool database_ok = metadata_store != nullptr;
        const bool generator_ok =
            !signal_synth_cli.empty() && access(signal_synth_cli.c_str(), X_OK) == 0;
        std::vector<PackSummary> packs;
        std::string pack_error;
        const bool packs_ok = PackCatalog(pack_root).list(packs, pack_error) &&
            !packs.empty();
        struct statvfs disk;
        const bool storage_ok = !data_root.empty() &&
            statvfs(data_root.c_str(), &disk) == 0 &&
            access(data_root.c_str(), W_OK) == 0;
        const bool ready = database_ok && generator_ok && packs_ok && storage_ok;
        json_t* body = json_object();
        json_object_set_new(body, "status", json_string(ready ? "ready" : "not_ready"));
        json_object_set_new(body, "database", json_boolean(database_ok));
        json_object_set_new(body, "generator", json_boolean(generator_ok));
        json_object_set_new(body, "pack_catalog", json_boolean(packs_ok));
        json_object_set_new(body, "artifact_store", json_boolean(storage_ok));
        if (storage_ok) {
            json_object_set_new(body, "disk_free_bytes", json_integer(
                static_cast<json_int_t>(disk.f_bavail) * disk.f_frsize));
        }
        const std::string encoded = json_dump_line(body);
        json_decref(body);
        return json_response(ready ? 200 : 503, encoded);
    }

    ApiKeyIdentity authenticated_identity;
    bool authenticated = false;
    if (route_requires_authentication(uri, public_base_path)) {
        if (metadata_store == nullptr) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Authentication storage is unavailable.\"}}\n"
            );
        }
        const AuthenticationResult authentication = authenticate_bearer(
            authorization_header,
            *metadata_store
        );
        if (authentication.status == AuthenticationStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Authentication storage is unavailable.\"}}\n"
            );
            response.internal_error = authentication.internal_error;
            return response;
        }
        if (authentication.status != AuthenticationStatus::authenticated) {
            RouteResponse response = json_response(
                401,
                "{\"error\":{\"code\":\"unauthorized\","
                "\"message\":\"A valid Bearer API key is required.\"}}\n"
            );
            response.www_authenticate = "Bearer realm=\"syn_sig_ra\"";
            return response;
        }
        authenticated_identity = authentication.identity;
        authenticated = true;
        UsageSummary usage;
        std::string quota_error;
        const QuotaStatus quota = metadata_store->check_request_quota(
            authenticated_identity, usage, quota_error
        );
        if (quota == QuotaStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Quota storage is unavailable.\"}}\n"
            );
            response.internal_error = quota_error;
            return response;
        }
        if (quota == QuotaStatus::rate_limited) {
            return json_response(
                429,
                "{\"error\":{\"code\":\"request_rate_limit\","
                "\"message\":\"Per-key request rate limit exceeded.\"}}\n"
            );
        }
    }

    const std::string usage_path = public_base_path + "/v1/usage";
    if (uri == usage_path) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Usage only accepts GET.\"}}\n"
            );
        }
        UsageSummary usage;
        std::string error;
        if (!metadata_store->usage_summary(
                authenticated_identity, usage, error)) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Usage storage is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        return json_response(200, usage_json(usage));
    }

    const std::string metrics_path = public_base_path + "/v1/metrics";
    if (uri == metrics_path) {
        if (method != "GET") {
            return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Metrics only accepts GET.\"}}\n");
        }
        if (authenticated_identity.role != "owner" &&
            authenticated_identity.role != "admin") {
            return json_response(403, "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Owner or admin role is required.\"}}\n");
        }
        UsageSummary usage;
        std::string error;
        if (!metadata_store->usage_summary(authenticated_identity, usage, error)) {
            RouteResponse response = json_response(503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Metrics storage is unavailable.\"}}\n");
            response.internal_error = error;
            return response;
        }
        return json_response(200, usage_json(usage));
    }

    const std::string projects_path = public_base_path + "/v1/projects";
    if (uri == projects_path) {
        if (method == "GET") {
            std::vector<ProjectRecord> projects;
            std::string error;
            if (!metadata_store->list_projects(
                    authenticated_identity, projects, error)) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Project storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            json_t* root = json_object();
            json_t* array = json_array();
            for (std::vector<ProjectRecord>::const_iterator it = projects.begin();
                 it != projects.end(); ++it) {
                json_array_append_new(array, project_json_object(*it));
            }
            json_object_set_new(root, "projects", array);
            json_object_set_new(
                root, "role", json_string(authenticated_identity.role.c_str())
            );
            const std::string body = json_dump_line(root);
            json_decref(root);
            return json_response(200, body);
        }
        if (method != "POST") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Projects only accept GET or POST.\"}}\n"
            );
        }
        if (authenticated_identity.role != "owner" &&
            authenticated_identity.role != "admin") {
            return json_response(
                403,
                "{\"error\":{\"code\":\"forbidden\","
                "\"message\":\"Owner or admin role is required.\"}}\n"
            );
        }
        if (!is_json_content_type(content_type)) {
            return json_response(
                415,
                "{\"error\":{\"code\":\"unsupported_media_type\","
                "\"message\":\"Content-Type must be application/json.\"}}\n"
            );
        }
        json_error_t parse_error;
        json_t* root = json_loadb(
            request_body.data(), request_body.size(),
            JSON_REJECT_DUPLICATES, &parse_error
        );
        json_t* name = root == nullptr ? nullptr :
            json_object_get(root, "display_name");
        if (!json_is_object(root) || json_object_size(root) != 1 ||
            !json_is_string(name)) {
            if (root != nullptr) json_decref(root);
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_project_request\","
                "\"message\":\"display_name is required.\"}}\n"
            );
        }
        const std::string display_name = json_string_value(name);
        json_decref(root);
        ProjectRecord project;
        std::string error;
        if (!metadata_store->create_project(
                authenticated_identity, display_name, project, error)) {
            RouteResponse response = json_response(
                400,
                "{\"error\":{\"code\":\"invalid_project_request\","
                "\"message\":\"Project could not be created.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        json_t* created = project_json_object(project);
        const std::string body = json_dump_line(created);
        json_decref(created);
        return json_response(201, body);
    }

    const std::string jobs_path = public_base_path + "/v1/jobs";
    if (path_at_or_below(uri, jobs_path)) {
        if (!authenticated || metadata_store == nullptr) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Job storage is unavailable.\"}}\n"
            );
        }
        if (uri == jobs_path) {
            if (method == "GET") {
                int limit = 25;
                int offset = 0;
                if (!query_integer(query_string, "limit", 25, limit) ||
                    !query_integer(query_string, "offset", 0, offset) ||
                    limit < 1 || limit > 100) {
                    return json_response(
                        400,
                        "{\"error\":{\"code\":\"invalid_pagination\","
                        "\"message\":\"limit must be 1-100 and offset non-negative.\"}}\n"
                    );
                }
                std::vector<JobRecord> jobs;
                std::string error;
                if (!metadata_store->list_jobs(
                        authenticated_identity,
                        limit,
                        offset,
                        jobs,
                        error
                    )) {
                    RouteResponse response = json_response(
                        503,
                        "{\"error\":{\"code\":\"metadata_unavailable\","
                        "\"message\":\"Job storage is unavailable.\"}}\n"
                    );
                    response.internal_error = error;
                    return response;
                }
                return json_response(
                    200,
                    job_list_json(jobs, public_base_path, limit, offset)
                );
            }
            if (method != "POST") {
                return json_response(
                    405,
                    "{\"error\":{\"code\":\"method_not_allowed\","
                    "\"message\":\"Job collection only accepts GET or POST.\"}}\n"
                );
            }
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403,
                    "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot create jobs.\"}}\n"
                );
            }
            if (!is_json_content_type(content_type)) {
                return json_response(
                    415,
                    "{\"error\":{\"code\":\"unsupported_media_type\","
                    "\"message\":\"Content-Type must be application/json.\"}}\n"
                );
            }
            JobRequest job_request;
            std::string error;
            const JobRequestStatus request_status = parse_job_request(
                request_body,
                job_request,
                error
            );
            if (request_status != JobRequestStatus::valid) {
                const char* code =
                    request_status == JobRequestStatus::unsupported_field
                    ? "unsupported_field"
                    : "invalid_job_request";
                return json_response(
                    400,
                    std::string("{\"error\":{\"code\":\"") + code +
                    "\",\"message\":\"The job request is invalid.\"}}\n"
                );
            }
            ProjectRecord selected_project;
            const RecordLookupStatus project_status =
                metadata_store->find_project(
                    job_request.project_id,
                    authenticated_identity,
                    selected_project,
                    error
                );
            if (project_status == RecordLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"project_not_found\","
                    "\"message\":\"The requested project does not exist.\"}}\n"
                );
            }
            if (project_status == RecordLookupStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Project storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            const PackCatalog catalog(pack_root);
            PackSummary pack;
            const PackLookupStatus pack_status = catalog.find(
                job_request.pack_id,
                pack,
                error
            );
            if (pack_status == PackLookupStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"pack_not_found\","
                    "\"message\":\"The requested pack does not exist.\"}}\n"
                );
            }
            if (pack_status != PackLookupStatus::found) {
                RouteResponse response = json_response(
                    500,
                    "{\"error\":{\"code\":\"pack_catalog_invalid\","
                    "\"message\":\"The configured pack catalog is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            UsageSummary usage;
            const QuotaStatus job_quota = metadata_store->check_job_quota(
                authenticated_identity, usage, error
            );
            if (job_quota == QuotaStatus::concurrent_limit ||
                job_quota == QuotaStatus::monthly_limit) {
                const char* code = job_quota == QuotaStatus::concurrent_limit
                    ? "concurrent_job_limit"
                    : "monthly_job_limit";
                return json_response(
                    429,
                    std::string("{\"error\":{\"code\":\"") + code +
                    "\",\"message\":\"Job quota exceeded.\"}}\n"
                );
            }
            if (job_quota == QuotaStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Quota storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            std::string job_id;
            if (!metadata_store->create_job(
                    authenticated_identity,
                    job_request.project_id,
                    job_request.canonical_json,
                    job_request.pack_id,
                    pack_root + "/" + job_request.pack_id + ".json",
                    pack.pack_fingerprint,
                    job_id,
                    error
                )) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Job storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            json_t* created = json_object();
            json_object_set_new(
                created,
                "job_id",
                json_string(job_id.c_str())
            );
            json_object_set_new(created, "status", json_string("queued"));
            const std::string body = json_dump_line(created);
            json_decref(created);
            return json_response(202, body);
        }

        const std::string job_resource = uri.substr(jobs_path.size() + 1);
        const std::string::size_type action_separator =
            job_resource.find('/');
        const std::string job_id = job_resource.substr(0, action_separator);
        const std::string action = action_separator == std::string::npos
            ? std::string()
            : job_resource.substr(action_separator + 1);
        if (!is_valid_pack_id(job_id) ||
            job_id.compare(0, 4, "job_") != 0) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_job_id\","
                "\"message\":\"The job ID is invalid.\"}}\n"
                );
        }
        if (!action.empty()) {
            if (method != "POST" || (action != "cancel" && action != "retry")) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"route_not_found\","
                    "\"message\":\"The requested job action does not exist.\"}}\n"
                );
            }
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403,
                    "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot modify jobs.\"}}\n"
                );
            }
            std::string error;
            std::string new_job_id;
            const JobLifecycleStatus lifecycle = action == "cancel"
                ? metadata_store->cancel_job(
                    job_id, authenticated_identity, error)
                : metadata_store->retry_job(
                    job_id, authenticated_identity, new_job_id, error);
            if (lifecycle == JobLifecycleStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"job_not_found\","
                    "\"message\":\"The requested job does not exist.\"}}\n"
                );
            }
            if (lifecycle == JobLifecycleStatus::invalid_state) {
                return json_response(
                    409,
                    std::string("{\"error\":{\"code\":\"job_") + action +
                    "_invalid_state\",\"message\":\"The job cannot be " +
                    action + "ed in its current state.\"}}\n"
                );
            }
            if (lifecycle == JobLifecycleStatus::storage_error) {
                RouteResponse response = json_response(
                    503,
                    "{\"error\":{\"code\":\"metadata_unavailable\","
                    "\"message\":\"Job storage is unavailable.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            if (action == "retry") {
                return json_response(
                    202,
                    std::string("{\"job_id\":\"") + new_job_id +
                    "\",\"retry_of\":\"" + job_id +
                    "\",\"status\":\"queued\"}\n"
                );
            }
            return json_response(
                200,
                std::string("{\"job_id\":\"") + job_id +
                "\",\"status\":\"cancelled\"}\n"
            );
        }
        if (method == "DELETE") {
            if (authenticated_identity.role == "viewer") {
                return json_response(
                    403,
                    "{\"error\":{\"code\":\"forbidden\","
                    "\"message\":\"Viewer role cannot delete jobs.\"}}\n"
                );
            }
            std::string error;
            const JobDeleteStatus delete_status = metadata_store->delete_job(
                job_id,
                authenticated_identity,
                error
            );
            if (delete_status == JobDeleteStatus::deleted) {
                return json_response(
                    200,
                    std::string("{\"job_id\":\"") + job_id +
                    "\",\"status\":\"deleted\"}\n"
                );
            }
            if (delete_status == JobDeleteStatus::not_found) {
                return json_response(
                    404,
                    "{\"error\":{\"code\":\"job_not_found\","
                    "\"message\":\"The requested job does not exist.\"}}\n"
                );
            }
            if (delete_status == JobDeleteStatus::running) {
                return json_response(
                    409,
                    "{\"error\":{\"code\":\"job_running\","
                    "\"message\":\"A running job cannot be deleted.\"}}\n"
                );
            }
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Job storage is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Job status only accepts GET or DELETE.\"}}\n"
            );
        }
        JobRecord job;
        std::string error;
        const RecordLookupStatus lookup = metadata_store->find_job(
            job_id,
            authenticated_identity,
            job,
            error
        );
        if (lookup == RecordLookupStatus::not_found) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"job_not_found\","
                "\"message\":\"The requested job does not exist.\"}}\n"
            );
        }
        if (lookup == RecordLookupStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Job storage is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        return json_response(200, job_json(job, public_base_path));
    }

    const std::string artifacts_path =
        public_base_path + "/v1/artifacts";
    if (path_at_or_below(uri, artifacts_path)) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Artifact endpoints only accept GET.\"}}\n"
            );
        }
        const std::string relative =
            uri.size() > artifacts_path.size()
            ? uri.substr(artifacts_path.size() + 1)
            : std::string();
        const std::string::size_type separator = relative.find('/');
        if (separator == std::string::npos ||
            relative.find('/', separator + 1) != std::string::npos) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_artifact_path\","
                "\"message\":\"The artifact path is invalid.\"}}\n"
            );
        }
        const std::string package_id = relative.substr(0, separator);
        const std::string filename = relative.substr(separator + 1);
        if (!is_valid_pack_id(package_id) ||
            package_id.compare(0, 4, "pkg_") != 0 ||
            (filename != "manifest.json" && filename != "package.zip")) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_artifact_path\","
                "\"message\":\"The artifact path is invalid.\"}}\n"
            );
        }
        PackageRecord package;
        std::string error;
        const RecordLookupStatus lookup = metadata_store->find_package(
            package_id,
            authenticated_identity,
            package,
            error
        );
        if (lookup == RecordLookupStatus::not_found) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"artifact_not_found\","
                "\"message\":\"The requested artifact does not exist.\"}}\n"
            );
        }
        if (lookup == RecordLookupStatus::storage_error) {
            RouteResponse response = json_response(
                503,
                "{\"error\":{\"code\":\"metadata_unavailable\","
                "\"message\":\"Artifact metadata is unavailable.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        const std::string expected_storage =
            data_root + "/packages/" + package_id;
        if (data_root.empty() ||
            package.artifact_storage_key != expected_storage) {
            RouteResponse response = json_response(
                500,
                "{\"error\":{\"code\":\"artifact_storage_invalid\","
                "\"message\":\"Artifact storage is unavailable.\"}}\n"
            );
            response.internal_error =
                "artifact storage key does not match configured data root";
            return response;
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = filename == "manifest.json"
            ? "application/json"
            : "application/zip";
        response.file_path = expected_storage + "/" + filename;
        response.content_disposition =
            std::string("attachment; filename=\"") + filename + "\"";
        return response;
    }

    if (uri == public_base_path ||
        uri == public_base_path + "/" ||
        uri == public_base_path + "/ui" ||
        uri == public_base_path + "/ui/") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"The web UI only accepts GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        response.content_type = "text/html; charset=utf-8";
        response.body = kUiHtml;
        return response;
    }

    if (uri == public_base_path + "/ui/style.css" ||
        uri == public_base_path + "/ui/app.js") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Static UI assets only accept GET.\"}}\n"
            );
        }
        RouteResponse response;
        response.disposition = RouteDisposition::handled;
        response.status = 200;
        if (uri == public_base_path + "/ui/style.css") {
            response.content_type = "text/css; charset=utf-8";
            response.body = kUiCss;
        } else {
            response.content_type = "application/javascript; charset=utf-8";
            response.body = kUiJs;
        }
        return response;
    }

    if (uri == public_base_path + "/healthz") {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"The health endpoint only accepts GET.\"}}\n"
            );
        }

        return json_response(
            200,
            "{\"service\":\"signal_synth_saas\",\"status\":\"ok\","
            "\"build\":{\"version\":\"" SYN_SIG_RA_VERSION
            "\",\"git_commit\":\"" SYN_SIG_RA_GIT_COMMIT "\"}}\n"
        );
    }

    const std::string packs_path = public_base_path + "/v1/packs";
    if (path_at_or_below(uri, packs_path)) {
        if (method != "GET") {
            return json_response(
                405,
                "{\"error\":{\"code\":\"method_not_allowed\","
                "\"message\":\"Pack catalog endpoints only accept GET.\"}}\n"
            );
        }
        if (pack_root.empty()) {
            return json_response(
                503,
                "{\"error\":{\"code\":\"pack_catalog_unavailable\","
                "\"message\":\"The pack catalog is unavailable.\"}}\n"
            );
        }

        const PackCatalog catalog(pack_root);
        if (uri == packs_path) {
            std::vector<PackSummary> packs;
            std::string error;
            if (!catalog.list(packs, error)) {
                RouteResponse response = json_response(
                    500,
                    "{\"error\":{\"code\":\"pack_catalog_invalid\","
                    "\"message\":\"The configured pack catalog is invalid.\"}}\n"
                );
                response.internal_error = error;
                return response;
            }
            std::string body("{\"packs\":[");
            for (std::size_t index = 0; index < packs.size(); ++index) {
                if (index != 0) {
                    body += ',';
                }
                body += pack_summary_json(packs[index]);
            }
            body += "]}\n";
            return json_response(200, body);
        }

        const std::string pack_id = uri.substr(packs_path.size() + 1);
        PackSummary pack;
        std::string error;
        const PackLookupStatus status = catalog.find(pack_id, pack, error);
        if (status == PackLookupStatus::invalid_id) {
            return json_response(
                400,
                "{\"error\":{\"code\":\"invalid_pack_id\","
                "\"message\":\"The pack ID is invalid.\"}}\n"
            );
        }
        if (status == PackLookupStatus::not_found) {
            return json_response(
                404,
                "{\"error\":{\"code\":\"pack_not_found\","
                "\"message\":\"The requested pack does not exist.\"}}\n"
            );
        }
        if (status == PackLookupStatus::catalog_error) {
            RouteResponse response = json_response(
                500,
                "{\"error\":{\"code\":\"pack_catalog_invalid\","
                "\"message\":\"The configured pack catalog is invalid.\"}}\n"
            );
            response.internal_error = error;
            return response;
        }
        return json_response(200, pack_summary_json(pack) + "\n");
    }

    return json_response(
        404,
        "{\"error\":{\"code\":\"route_not_found\","
        "\"message\":\"No API route matches the requested path.\"}}\n"
    );
}

}  // namespace syn_sig_ra
