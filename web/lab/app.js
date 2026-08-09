(function () {
  'use strict';

  const base = document.body.dataset.apiBase || '/syn_sig_ra';
  const viewer = window.SynsigraSignalViewer;
  const $ = (id) => document.getElementById(id);
  const clone = (value) => JSON.parse(JSON.stringify(value));
  const state = {
    schema: null,
    templates: [],
    scenario: null,
    targets: [],
    preflight: null,
    preflightKey: '',
    validationSerial: 0,
    validationTimer: null,
    preview: null,
    lastAppliedKey: '',
    savedScenarioId: '',
    rendering: false,
    description: null,
    caseMetadata: null,
    channels: [],
    startSample: 0,
    spanSamples: 0,
    amplitude: 1,
    requestController: null,
    requestSerial: 0
  };
  const cache = new viewer.SignalWindowCache(24 * 1024 * 1024);
  const renderer = new viewer.SignalCanvasRenderer($('signal-canvas'), {
    onResize: () => scheduleWindow()
  });
  const dataSource = new viewer.HttpSignalDataSource({
    apiBase: base,
    describePath: (id) => `/v1/lab/previews/${encodeURIComponent(id)}/viewer`,
    windowPath: (id) => `/v1/lab/previews/${encodeURIComponent(id)}/viewer/window`,
    overlayPath: (id) => `/v1/lab/previews/${encodeURIComponent(id)}/viewer/overlays`
  });

  const targetLabels = {
    r_peak: 'ECG R peaks', rr_interval: 'RR intervals', hrv: 'HRV metrics',
    ecg_beat_classification: 'Beat classes', rhythm_episode: 'Rhythm episodes',
    rhythm_burden: 'Rhythm burden', signal_quality: 'Signal quality',
    ecg_delineation: 'ECG delineation', qtc: 'QTc',
    morphology_assertions: 'ECG morphology', ppg_systolic_peak: 'PPG peaks',
    ppg_pulse_onset: 'PPG onsets', ecg_ppg_alignment: 'ECG–PPG timing',
    ppg_optical: 'Optical PPG', prv: 'PRV', respiratory_rate: 'Respiratory rate'
  };
  const artifactLabels = {
    ecg_baseline_wander: 'ECG baseline wander', ecg_powerline: 'Power-line interference',
    ecg_emg_noise: 'Muscle / EMG noise', ecg_dropout: 'ECG dropout',
    ecg_saturation: 'ECG saturation', ppg_dropout: 'PPG dropout',
    ecg_lead_reversal: 'ECG lead reversal', ecg_lead_swap: 'ECG lead swap',
    ecg_electrode_misplacement: 'Electrode misplacement', ecg_gain_mismatch: 'ECG gain mismatch',
    ecg_offset_drift: 'ECG offset drift', ecg_clock_drift: 'ECG clock drift',
    ecg_dropped_samples: 'Dropped ECG samples', ecg_quantization: 'ECG quantization',
    ecg_adc_clipping: 'ECG ADC clipping', ppg_motion_periodic: 'Periodic PPG motion',
    ppg_motion_burst: 'PPG motion burst', ppg_motion_broadband: 'Broadband PPG motion',
    ppg_ambient_light: 'PPG ambient light', ppg_sensor_saturation: 'PPG sensor saturation'
  };
  const rhythmDefaults = { afib: 120, psvt: 180, svarr: 150, vt: 160, vf: 0, asystole: 0 };

  function escapeHtml(value) {
    return String(value == null ? '' : value).replace(/[&<>'"]/g, (char) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;'
    })[char]);
  }

  async function api(path, options = {}) {
    const headers = { Accept: 'application/json', ...(options.headers || {}) };
    const init = { credentials: 'same-origin', cache: 'no-store', ...options, headers };
    if (Object.prototype.hasOwnProperty.call(options, 'json')) {
      init.body = JSON.stringify(options.json);
      init.headers['Content-Type'] = 'application/json';
      delete init.json;
    }
    const response = await fetch(base + path, init);
    let body = null;
    try { body = await response.json(); } catch (_) {}
    if (!response.ok) {
      const error = new Error(body && body.error && body.error.message
        ? body.error.message : (response.statusText || `HTTP ${response.status}`));
      error.status = response.status;
      error.body = body;
      throw error;
    }
    return body;
  }

  function toast(message, kind) {
    const node = $('toast');
    node.textContent = message;
    node.className = `toast ${kind || ''}`;
    node.hidden = false;
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => { node.hidden = true; }, 5000);
  }

  function scenarioKey() {
    return JSON.stringify({ scenario: state.scenario, targets: [...state.targets].sort() });
  }

  function getPath(path) {
    return path.replace(/^\$\.?/, '').split('.').filter(Boolean)
      .reduce((value, key) => value == null ? undefined : value[key], state.scenario);
  }

  function setPath(path, value) {
    const parts = path.replace(/^\$\.?/, '').split('.').filter(Boolean);
    let cursor = state.scenario;
    parts.forEach((part, index) => {
      if (index === parts.length - 1) cursor[part] = value;
      else {
        if (!cursor[part] || typeof cursor[part] !== 'object') cursor[part] = {};
        cursor = cursor[part];
      }
    });
  }

  function donor(templateId) {
    const template = state.templates.find((item) => item.template_id === templateId);
    return template ? template.scenario : null;
  }

  function inputNumber(id, fallback = 0) {
    const value = Number($(id).value);
    return Number.isFinite(value) ? value : fallback;
  }

  function markEdited() {
    state.savedScenarioId = '';
    syncAdvancedJson();
    updateFreshness();
    clearTimeout(state.validationTimer);
    state.validationTimer = setTimeout(validateScenario, 350);
  }

  function updateFreshness() {
    const fresh = Boolean(state.preview && state.lastAppliedKey === scenarioKey());
    ['save-draft', 'continue-pack', 'build-pack'].forEach((id) => {
      $(id).disabled = !fresh || state.rendering;
    });
    $('save-guidance').textContent = fresh
      ? 'This saves the exact canonical scenario used by the visible preview.'
      : state.preview
        ? 'Settings changed. Apply again before saving so the waveform and case stay identical.'
        : 'Render a valid preview first. Saving never uses unrendered edits.';
    const validationCurrent = state.preflightKey === scenarioKey();
    if (state.preview && !fresh &&
        (!validationCurrent || (state.preflight && state.preflight.success))) {
      $('validation-status').className = 'status-pill dirty';
      $('validation-status').textContent = 'Changes not applied';
    }
  }

  function syncAdvancedJson() {
    $('scenario-json').value = JSON.stringify(state.scenario || {}, null, 2);
  }

  function setControl(id, value) {
    const node = $(id);
    if (!node) return;
    if (node.type === 'checkbox') node.checked = Boolean(value);
    else node.value = value == null ? '' : String(value);
  }

  function currentEnvelope() {
    return (((state.scenario || {}).randomization || {}).envelopes || [])
      .find((item) => item.parameter === 'ecg.heart_rate_bpm');
  }

  function syncControls() {
    if (!state.scenario) return;
    setControl('case-name', state.scenario.name);
    setControl('duration', state.scenario.duration_seconds);
    setControl('sample-rate', state.scenario.sample_rate_hz);
    setControl('case-seed', state.scenario.seed);
    setControl('heart-rate', getPath('$.ecg.heart_rate_bpm'));
    setControl('rr-variation', getPath('$.ecg.rr_variability_seconds'));
    const envelope = currentEnvelope();
    setControl('vary-heart-rate', Boolean(envelope));
    $('heart-rate-range').hidden = !envelope;
    setControl('heart-rate-min', envelope ? envelope.minimum : Math.max(10, Number(getPath('$.ecg.heart_rate_bpm') || 70) - 10));
    setControl('heart-rate-max', envelope ? envelope.maximum : Math.min(400, Number(getPath('$.ecg.heart_rate_bpm') || 70) + 10));

    const hrvEnabled = Boolean(getPath('$.hrv.enabled'));
    setControl('hrv-enabled', hrvEnabled); $('hrv-settings').hidden = !hrvEnabled;
    setControl('hrv-mean', getPath('$.hrv.target_mean_hr_bpm'));
    setControl('hrv-sdnn', Number(getPath('$.hrv.target_sdnn_seconds') || 0) * 1000);
    setControl('hrv-vlf', Number(getPath('$.hrv.vlf_power_fraction') || 0) * 100);
    setControl('hrv-lfhf', getPath('$.hrv.lf_hf_ratio'));
    setControl('hrv-lf', getPath('$.hrv.lf_center_hz'));
    setControl('hrv-hf', getPath('$.hrv.hf_center_hz'));
    renderAdvancedHrv();

    const conditions = getPath('$.ecg.conditions') || [];
    const ectopy = conditions.find((item) => item.code === 'PAC' || item.code === 'PVC');
    setControl('ectopy-type', ectopy ? ectopy.code : '');
    setControl('ectopy-every', getPath('$.ecg.ectopic_every_n_beats') || 5);
    setControl('ectopy-severity', Math.round(Number(ectopy ? ectopy.severity : .8) * 100));
    $('ectopy-every').disabled = !ectopy;
    $('ectopy-severity').disabled = !ectopy;
    $('ectopy-severity-output').textContent = `${$('ectopy-severity').value}%`;
    const episode = (getPath('$.ecg.rhythm_episodes') || [])[0];
    setControl('rhythm-enabled', Boolean(episode));
    $('rhythm-settings').hidden = !episode;
    setControl('rhythm-type', episode ? episode.type : 'afib');
    setControl('rhythm-rate', episode ? episode.rate_bpm : rhythmDefaults.afib);
    setControl('rhythm-start', episode ? episode.start_seconds : Math.max(1, Math.round(Number(state.scenario.duration_seconds) / 3)));
    setControl('rhythm-duration', episode ? episode.duration_seconds : Math.max(2, Math.round(Number(state.scenario.duration_seconds) / 3)));
    setControl('rhythm-transition', episode ? episode.transition_seconds : .25);
    $('rhythm-rate').disabled = Boolean(episode && (episode.type === 'vf' || episode.type === 'asystole'));
    renderConditions();
    renderNoise();

    const ppgEnabled = Boolean(getPath('$.ppg.enabled'));
    setControl('ppg-enabled', ppgEnabled); $('ppg-settings').hidden = !ppgEnabled;
    setControl('ppg-delay', getPath('$.ppg.pulse_delay_ms'));
    setControl('ppg-amplitude', getPath('$.ppg.amplitude_au'));
    setControl('ppg-rise', getPath('$.ppg.rise_time_ms'));
    setControl('ppg-decay', getPath('$.ppg.decay_time_ms'));
    setControl('ppg-missing', getPath('$.ppg.missing_pulse_every_n_beats') || 0);
    setControl('ppg-jitter', getPath('$.ppg.pulse_delay_jitter_ms') || 0);
    renderTargets();
    syncAdvancedJson();
  }

  function renderAdvancedHrv() {
    const fields = [
      ['VLF center', 'vlf_center_hz', 'Hz', .0001], ['VLF bandwidth', 'vlf_bandwidth_hz', 'Hz', .0001],
      ['LF bandwidth', 'lf_bandwidth_hz', 'Hz', .001], ['HF bandwidth', 'hf_bandwidth_hz', 'Hz', .001],
      ['Respiration frequency', 'respiratory_frequency_hz', 'Hz', .005],
      ['Respiratory RR amplitude', 'respiratory_amplitude_seconds', 'seconds', .001],
      ['Minimum RR', 'minimum_rr_seconds', 'seconds', .01], ['Maximum RR', 'maximum_rr_seconds', 'seconds', .01]
    ];
    $('hrv-advanced').innerHTML = fields.map(([label, key, unit, step]) => `<label>${escapeHtml(label)} <span class="unit">${escapeHtml(unit)}</span><input type="number" step="${step}" data-hrv-field="${key}" value="${escapeHtml(getPath(`$.hrv.${key}`) == null ? '' : getPath(`$.hrv.${key}`))}" data-path="$.hrv.${key}"></label>`).join('');
  }

  function renderConditions() {
    const metadata = new Map(((state.schema && state.schema.conditions) || []).map((item) => [item.code, item]));
    const conditions = (getPath('$.ecg.conditions') || []).filter((item) => item.code !== 'PAC' && item.code !== 'PVC');
    $('condition-list').innerHTML = conditions.length ? conditions.map((item) => {
      const detail = metadata.get(item.code) || {};
      return `<div class="condition-pill"><span><strong>${escapeHtml(detail.name || item.code)}</strong><small>${escapeHtml(item.code)} · intensity ${escapeHtml(Math.round(Number(item.severity || 1) * 100))}%</small></span><button type="button" class="remove" data-remove-condition="${escapeHtml(item.code)}">Remove</button></div>`;
    }).join('') : '<p class="empty-list">No additional morphology or rhythm condition.</p>';
  }

  function renderNoise() {
    const artifacts = Array.isArray(state.scenario.artifacts) ? state.scenario.artifacts : [];
    $('noise-list').innerHTML = artifacts.length ? artifacts.map((item, index) => `<article class="ingredient">
      <div class="ingredient-head"><strong>${escapeHtml(artifactLabels[item.type] || item.type)}</strong><button class="remove" type="button" data-remove-noise="${index}">Remove</button></div>
      <div class="ingredient-grid">
        <label>Strength <span class="unit">${Math.round(Number(item.severity || 0) * 100)}%</span><input type="range" min="0" max="100" step="1" value="${escapeHtml(Math.round(Number(item.severity || 0) * 100))}" data-noise-index="${index}" data-noise-field="severity"></label>
        <label>Channels<select data-noise-index="${index}" data-noise-field="channels"><option value="all_ecg" ${String((item.channels || [])[0]) === 'all_ecg' ? 'selected' : ''}>All ECG leads</option><option value="II" ${String((item.channels || [])[0]) === 'II' ? 'selected' : ''}>Lead II only</option><option value="all_ppg" ${String((item.channels || [])[0]) === 'all_ppg' ? 'selected' : ''}>All PPG channels</option></select></label>
        <label>Starts at <span class="unit">seconds</span><input type="number" min="0" max="${escapeHtml(state.scenario.duration_seconds)}" step=".1" value="${escapeHtml(item.start_seconds)}" data-noise-index="${index}" data-noise-field="start_seconds"></label>
        <label>Lasts <span class="unit">seconds</span><input type="number" min=".01" max="${escapeHtml(state.scenario.duration_seconds)}" step=".1" value="${escapeHtml(item.duration_seconds)}" data-noise-index="${index}" data-noise-field="duration_seconds"></label>
      </div>
    </article>`).join('') : '<p class="empty-list">No noise yet — the case is clean.</p>';
  }

  function requirementSatisfied(requirement) {
    if (requirement === 'ppg.enabled') return Boolean(getPath('$.ppg.enabled'));
    if (requirement === 'ppg.optical.enabled') return Boolean(getPath('$.ppg.optical.enabled'));
    if (requirement === 'hrv.enabled') return Boolean(getPath('$.hrv.enabled'));
    if (requirement === 'duration_seconds>=300') return Number(state.scenario.duration_seconds) >= 300;
    if (requirement.includes('artifacts.length')) return (state.scenario.artifacts || []).length > 0;
    if (requirement.includes('rhythm_episodes')) return (getPath('$.ecg.rhythm_episodes') || []).length > 0;
    if (requirement === 'ecg.conditions') return (getPath('$.ecg.conditions') || []).length > 0;
    return true;
  }

  function renderTargets() {
    const targets = (state.schema && state.schema.targets) || [];
    const checked = new Set(state.targets);
    const preferred = targets.filter((target) => checked.has(target.name) || (target.requires || []).every(requirementSatisfied));
    const advanced = targets.filter((target) => !preferred.includes(target));
    const option = (target) => `<label class="target-option" title="${escapeHtml((target.requires || []).length ? `Requires ${target.requires.join(', ')}` : 'No extra case requirement')}"><input type="checkbox" data-target="${escapeHtml(target.name)}" ${checked.has(target.name) ? 'checked' : ''}><strong>${escapeHtml(targetLabels[target.name] || target.name)}</strong></label>`;
    $('target-list').innerHTML = preferred.map(option).join('') + (advanced.length ? `<details style="grid-column:1/-1"><summary>More outputs (${advanced.length})</summary><div class="target-list" style="margin-top:8px">${advanced.map(option).join('')}</div></details>` : '');
  }

  function loadTemplate(templateId) {
    const template = state.templates.find((item) => item.template_id === templateId);
    if (!template) return;
    state.scenario = clone(template.scenario);
    const suffix = Date.now().toString(36).slice(-6);
    state.scenario.scenario_id = `lab_${template.template_id}_${suffix}`.slice(0, 96);
    state.scenario.name = `${template.name} case`;
    state.scenario.description = `Built interactively from the ${template.name} core recipe in Synsigra Lab.`;
    state.targets = [...template.targets];
    state.preview = null;
    state.lastAppliedKey = '';
    state.savedScenarioId = '';
    $('template-description').textContent = template.description;
    syncControls();
    updateFreshness();
    scheduleValidation();
  }

  function scheduleValidation() {
    clearTimeout(state.validationTimer);
    state.validationTimer = setTimeout(validateScenario, 50);
  }

  async function validateScenario() {
    if (!state.scenario || !state.targets.length) {
      state.preflight = null;
      $('validation-status').className = 'status-pill invalid';
      $('validation-status').textContent = 'Choose an output';
      $('render-preview').disabled = true;
      return;
    }
    const serial = ++state.validationSerial;
    const key = scenarioKey();
    if (!(state.preview && state.lastAppliedKey !== key)) {
      $('validation-status').className = 'status-pill checking';
      $('validation-status').textContent = 'Checking…';
    }
    try {
      const result = await api('/v1/authoring/preview', { method: 'POST', json: { scenario: state.scenario, targets: state.targets } });
      if (serial !== state.validationSerial) return;
      state.preflight = result;
      state.preflightKey = key;
      const valid = result.success === true;
      $('validation-status').className = `status-pill ${valid ? 'valid' : 'invalid'}`;
      $('validation-status').textContent = valid ? 'Ready to render' : 'Needs attention';
      $('render-preview').disabled = !valid || state.rendering;
      const messages = result.messages || [];
      $('validation-panel').innerHTML = valid
        ? `<strong>Core preflight passed.</strong> ${escapeHtml((result.summary || {}).total_sample_count || 0)} samples · ${escapeHtml(state.targets.map((name) => targetLabels[name] || name).join(', '))}`
        : `<strong>Fix this case before rendering.</strong>${messages.length ? `<ul>${messages.map((item) => `<li><button data-error-path="${escapeHtml(item.path || '$')}">${escapeHtml(item.path || '$')}</button> — ${escapeHtml(item.message || item.code)}</li>`).join('')}</ul>` : ''}`;
    } catch (error) {
      if (serial !== state.validationSerial) return;
      state.preflight = null;
      state.preflightKey = key;
      $('validation-status').className = 'status-pill invalid';
      $('validation-status').textContent = 'Needs attention';
      $('render-preview').disabled = true;
      const errors = error.body && error.body.validation_errors;
      $('validation-panel').innerHTML = errors && errors.length
        ? `<strong>Fix ${errors.length} field${errors.length === 1 ? '' : 's'} before rendering.</strong><ul>${errors.map((item) => `<li><button data-error-path="${escapeHtml(item.path || '$')}">${escapeHtml(item.path || '$')}</button> — ${escapeHtml(item.message)}</li>`).join('')}</ul>`
        : `<strong>${escapeHtml(error.message)}</strong>`;
    }
    updateFreshness();
  }

  function focusPath(path) {
    document.querySelectorAll('.builder-section.is-invalid').forEach((node) => node.classList.remove('is-invalid'));
    const exact = document.querySelector(`[data-path="${CSS.escape(path)}"]`);
    const candidate = exact || [...document.querySelectorAll('[data-path]')].find((node) => path.startsWith(node.dataset.path));
    if (candidate) {
      const section = candidate.closest('.builder-section');
      if (section) section.classList.add('is-invalid');
      candidate.scrollIntoView({ behavior: 'smooth', block: 'center' });
      setTimeout(() => candidate.focus(), 300);
    } else {
      $('advanced-json').open = true;
      $('scenario-json').focus();
    }
  }

  function previewResponseScenario(value) {
    if (value && typeof value === 'object') return clone(value);
    if (typeof value === 'string') return JSON.parse(value);
    throw new Error('Preview omitted its canonical scenario.');
  }

  async function renderPreview() {
    const key = scenarioKey();
    if (!state.preflight || !state.preflight.success || state.preflightKey !== key) {
      await validateScenario();
      if (!state.preflight || !state.preflight.success || state.preflightKey !== scenarioKey()) return;
    }
    state.rendering = true;
    $('render-preview').disabled = true;
    $('render-preview').textContent = 'Rendering…';
    $('request-status').textContent = 'Generating bounded waveform…';
    const previous = state.preview && state.preview.preview_id;
    try {
      const result = await api('/v1/lab/previews', { method: 'POST', json: { scenario: state.scenario, targets: state.targets } });
      state.preview = result;
      state.scenario = previewResponseScenario(result.canonical_scenario);
      state.lastAppliedKey = scenarioKey();
      syncControls();
      await loadPreviewSource();
      if (previous && previous !== result.preview_id) {
        api(`/v1/lab/previews/${encodeURIComponent(previous)}`, { method: 'DELETE' }).catch(() => {});
      }
      $('preview-heading').textContent = state.scenario.name || 'Rendered Lab case';
      $('preview-summary').textContent = `${Number(result.duration_seconds).toFixed(2).replace(/\.00$/, '')} s · ${Number(result.sample_rate_hz).toLocaleString()} Hz · short-lived preview`;
      $('provenance-card').hidden = false;
      $('provenance-text').textContent = `${result.document_fingerprint} · generator ${result.generator.version} · expires ${new Date(result.expires_at).toLocaleTimeString()}`;
      $('resolved-scenario').textContent = JSON.stringify(result.resolved_scenario, null, 2);
      toast('Preview rendered. The visible waveform and saved case now share the same canonical scenario.');
    } catch (error) {
      toast(error.message, 'error');
      $('request-status').textContent = error.message;
    } finally {
      state.rendering = false;
      $('render-preview').textContent = 'Apply & render';
      $('render-preview').disabled = !(state.preflight && state.preflight.success && state.preflightKey === scenarioKey());
      updateFreshness();
    }
  }

  function defaultChannels(metadata) {
    const wanted = metadata.channels.filter((channel) => channel.name === 'II' || /ppg|accel/i.test(channel.name)).map((channel) => channel.index);
    return wanted.length ? wanted : metadata.channels.slice(0, 1).map((channel) => channel.index);
  }

  async function loadPreviewSource() {
    if (state.requestController) state.requestController.abort();
    state.description = await dataSource.describe(state.preview.preview_id);
    state.caseMetadata = state.description.cases[0];
    if (!state.caseMetadata) throw new Error('Preview contains no viewable channels.');
    state.channels = defaultChannels(state.caseMetadata);
    state.startSample = 0;
    state.spanSamples = Math.min(state.caseMetadata.sample_count, Math.max(16, Math.round(state.caseMetadata.sample_rate_hz * 10)));
    state.amplitude = 1;
    renderer.setCase(state.caseMetadata);
    renderer.setAmplitudeScale(1);
    renderer.setChannelSpacing(1);
    renderer.setLayout('stacked');
    renderChannelChips();
    updatePosition();
    $('canvas-empty').hidden = true;
    await requestWindow(true);
  }

  function renderChannelChips() {
    if (!state.caseMetadata) { $('channel-list').innerHTML = ''; return; }
    $('channel-list').innerHTML = state.caseMetadata.channels.map((channel) => {
      const active = state.channels.includes(channel.index);
      const color = viewer.COLORS[channel.index % viewer.COLORS.length];
      return `<button type="button" class="channel-chip ${active ? 'active' : ''}" style="--channel:${color}" data-channel="${channel.index}">${escapeHtml(channel.name)}</button>`;
    }).join('');
  }

  function maximumStart() {
    return state.caseMetadata ? Math.max(0, state.caseMetadata.sample_count - state.spanSamples) : 0;
  }

  function updatePosition() {
    if (!state.caseMetadata) return;
    state.startSample = Math.max(0, Math.min(maximumStart(), Math.round(state.startSample)));
    $('position-slider').max = String(maximumStart());
    $('position-slider').value = String(state.startSample);
    $('position-slider').step = String(Math.max(1, Math.floor(state.spanSamples / 500)));
    $('position-slider').disabled = maximumStart() === 0;
    renderer.setViewport(state.startSample, state.spanSamples);
    const rate = state.caseMetadata.sample_rate_hz;
    $('viewport-label').textContent = `${viewer.niceDuration(state.startSample / rate)} — ${viewer.niceDuration((state.startSample + state.spanSamples) / rate)} of ${viewer.niceDuration(state.caseMetadata.sample_count / rate)}`;
  }

  function scheduleWindow() {
    clearTimeout(scheduleWindow.timer);
    scheduleWindow.timer = setTimeout(() => requestWindow(false), 80);
  }

  async function requestWindow(force) {
    if (!state.preview || !state.caseMetadata || !state.channels.length) return;
    const points = Math.min(4096, Math.max(800, Math.round($('signal-canvas').clientWidth * 1.7)));
    const bucket = Math.max(1, Math.ceil(state.spanSamples / points));
    if (!force) {
      const cached = cache.find(state.preview.preview_id, state.caseMetadata.case_id, state.channels, state.startSample, state.spanSamples, bucket);
      if (cached) {
        renderer.setWindow(cached);
        $('request-status').textContent = 'Cached viewport';
        return;
      }
    }
    if (state.requestController) state.requestController.abort();
    state.requestController = new AbortController();
    const serial = ++state.requestSerial;
    const padding = Math.round(state.spanSamples * .35);
    const start = Math.max(0, state.startSample - padding);
    const end = Math.min(state.caseMetadata.sample_count, state.startSample + state.spanSamples + padding);
    $('request-status').textContent = 'Loading visible samples…';
    try {
      const windowData = await dataSource.readWindow(state.preview.preview_id, {
        caseId: state.caseMetadata.case_id, startSample: start, sampleCount: Math.max(1, end - start),
        points: Math.min(16384, Math.round(points * 1.7)), channels: state.channels
      }, state.requestController.signal);
      if (serial !== state.requestSerial) return;
      cache.add(state.preview.preview_id, state.caseMetadata.case_id, windowData);
      renderer.setWindow(windowData);
      $('request-status').textContent = `${windowData.bucketCount.toLocaleString()} display points`;
    } catch (error) {
      if (error.name !== 'AbortError') $('request-status').textContent = error.message;
    }
  }

  function zoomTime(factor) {
    if (!state.caseMetadata) return;
    const center = state.startSample + state.spanSamples / 2;
    state.spanSamples = Math.max(16, Math.min(state.caseMetadata.sample_count, Math.round(state.spanSamples * factor)));
    state.startSample = center - state.spanSamples / 2;
    updatePosition(); scheduleWindow();
  }

  async function saveExactScenario() {
    if (!state.preview || state.lastAppliedKey !== scenarioKey()) throw new Error('Apply current changes before saving.');
    if (state.savedScenarioId) return state.savedScenarioId;
    const saved = await api('/v1/scenarios', { method: 'POST', json: {
      name: state.scenario.name || 'Synsigra Lab case', scenario: state.scenario, target_intent: state.targets
    }});
    state.savedScenarioId = saved.scenario_id;
    toast('Scenario draft saved.');
    return saved.scenario_id;
  }

  async function saveAction(destination) {
    try {
      const id = await saveExactScenario();
      if (destination === 'pack') window.location.href = `${base}/custom-packs?scenario_id=${encodeURIComponent(id)}`;
    } catch (error) { toast(error.message, 'error'); }
  }

  async function buildOneCasePack() {
    try {
      const id = await saveExactScenario();
      const pack = await api('/v1/custom-packs', { method: 'POST', json: {
        name: `${state.scenario.name || 'Lab case'} test`,
        description: 'One-case custom challenge created from a successfully rendered Synsigra Lab preview.',
        targets: state.targets, scenario_ids: [id]
      }});
      window.location.href = `${base}/generate?pack_id=${encodeURIComponent(pack.pack_id)}`;
    } catch (error) { toast(error.message, 'error'); }
  }

  function bindHumanControls() {
    const simple = {
      'case-name': ['$.name', String], duration: ['$.duration_seconds', Number],
      'sample-rate': ['$.sample_rate_hz', Number], 'case-seed': ['$.seed', Number],
      'heart-rate': ['$.ecg.heart_rate_bpm', Number], 'rr-variation': ['$.ecg.rr_variability_seconds', Number],
      'hrv-mean': ['$.hrv.target_mean_hr_bpm', Number], 'hrv-lfhf': ['$.hrv.lf_hf_ratio', Number],
      'hrv-lf': ['$.hrv.lf_center_hz', Number], 'hrv-hf': ['$.hrv.hf_center_hz', Number],
      'ppg-delay': ['$.ppg.pulse_delay_ms', Number], 'ppg-amplitude': ['$.ppg.amplitude_au', Number],
      'ppg-rise': ['$.ppg.rise_time_ms', Number], 'ppg-decay': ['$.ppg.decay_time_ms', Number],
      'ppg-missing': ['$.ppg.missing_pulse_every_n_beats', Number], 'ppg-jitter': ['$.ppg.pulse_delay_jitter_ms', Number]
    };
    Object.entries(simple).forEach(([id, [path, cast]]) => $(id).addEventListener('change', () => {
      setPath(path, cast($(id).value));
      if (id === 'case-name') state.scenario.description = `Interactive Synsigra Lab case: ${$(id).value}.`;
      if (id === 'case-seed' && getPath('$.hrv.enabled')) setPath('$.hrv.seed', Number($(id).value));
      if (id === 'heart-rate' && getPath('$.hrv.enabled')) setPath('$.hrv.target_mean_hr_bpm', Number($(id).value));
      if (id === 'rr-variation' && getPath('$.hrv.enabled')) setPath('$.hrv.target_sdnn_seconds', Number($(id).value));
      markEdited();
    }));
    $('vary-heart-rate').addEventListener('change', () => {
      if (!state.scenario.randomization) state.scenario.randomization = { enabled: false, seed: Number(state.scenario.seed) + 1, envelopes: [] };
      state.scenario.randomization.envelopes = (state.scenario.randomization.envelopes || []).filter((item) => item.parameter !== 'ecg.heart_rate_bpm');
      if ($('vary-heart-rate').checked) {
        state.scenario.randomization.enabled = true;
        state.scenario.randomization.envelopes.push({ parameter: 'ecg.heart_rate_bpm', minimum: inputNumber('heart-rate-min'), maximum: inputNumber('heart-rate-max') });
      } else if (!state.scenario.randomization.envelopes.length) state.scenario.randomization.enabled = false;
      syncControls(); markEdited();
    });
    ['heart-rate-min', 'heart-rate-max'].forEach((id) => $(id).addEventListener('change', () => {
      const envelope = currentEnvelope(); if (!envelope) return;
      envelope.minimum = inputNumber('heart-rate-min'); envelope.maximum = inputNumber('heart-rate-max'); markEdited();
    }));
    $('hrv-enabled').addEventListener('change', () => {
      if ($('hrv-enabled').checked) {
        const source = donor('ecg_hrv_benchmark');
        if (!state.scenario.hrv) state.scenario.hrv = clone(source.hrv);
        state.scenario.hrv.enabled = true;
        state.scenario.hrv.target_mean_hr_bpm = Number(getPath('$.ecg.heart_rate_bpm'));
        state.scenario.hrv.target_sdnn_seconds = Math.max(.001, Number(getPath('$.ecg.rr_variability_seconds')) || .05);
        state.scenario.hrv.seed = Number(state.scenario.seed);
        setPath('$.ecg.rr_variability_seconds', state.scenario.hrv.target_sdnn_seconds);
        state.scenario.duration_seconds = Math.max(300, Number(state.scenario.duration_seconds));
      } else if (state.scenario.hrv) state.scenario.hrv.enabled = false;
      syncControls(); markEdited();
    });
    $('hrv-sdnn').addEventListener('change', () => {
      const seconds = inputNumber('hrv-sdnn') / 1000;
      setPath('$.hrv.target_sdnn_seconds', seconds);
      setPath('$.ecg.rr_variability_seconds', seconds);
      markEdited();
    });
    $('hrv-vlf').addEventListener('change', () => { setPath('$.hrv.vlf_power_fraction', inputNumber('hrv-vlf') / 100); markEdited(); });
    $('hrv-advanced').addEventListener('change', (event) => {
      if (!event.target.dataset.hrvField) return;
      setPath(`$.hrv.${event.target.dataset.hrvField}`, Number(event.target.value)); markEdited();
    });
    $('ppg-enabled').addEventListener('change', () => {
      if ($('ppg-enabled').checked) {
        const source = donor('ecg_ppg_peak');
        if (!state.scenario.ppg) state.scenario.ppg = clone(source.ppg);
        state.scenario.ppg.enabled = true;
      } else state.scenario.ppg.enabled = false;
      syncControls(); markEdited();
    });
  }

  function bindIngredients() {
    $('ectopy-type').addEventListener('change', () => {
      const code = $('ectopy-type').value;
      if (code) setPath('$.ecg.rhythm_episodes', []);
      let conditions = (getPath('$.ecg.conditions') || []).filter((item) => item.code !== 'PAC' && item.code !== 'PVC'
        && (!code || (item.code !== 'PSVT' && item.code !== 'SVARR' && item.code !== 'SR')));
      if (code) {
        conditions = conditions.filter((item) => item.code !== 'NORM');
        conditions.push({ code, severity: inputNumber('ectopy-severity', 80) / 100 });
        setPath('$.ecg.ectopic_every_n_beats', Math.max(2, inputNumber('ectopy-every', 5)));
      } else {
        setPath('$.ecg.ectopic_every_n_beats', 0);
        if (!conditions.length) conditions.push({ code: 'NORM', severity: 1 });
      }
      setPath('$.ecg.conditions', conditions); syncControls(); markEdited();
    });
    $('rhythm-enabled').addEventListener('change', () => {
      if ($('rhythm-enabled').checked) {
        const duration = Number(state.scenario.duration_seconds);
        const episodeDuration = Math.max(2, Math.min(duration * .4, duration - Math.min(1, duration * .1)));
        const type = 'afib';
        setPath('$.ecg.rhythm_episodes', [{
          type,
          start_seconds: Math.max(0, (duration - episodeDuration) / 2),
          duration_seconds: episodeDuration,
          transition_seconds: Math.min(.25, episodeDuration / 4),
          rate_bpm: rhythmDefaults[type],
          seed: Number(state.scenario.seed) + 41
        }]);
        setPath('$.ecg.ectopic_every_n_beats', 0);
        setPath('$.ecg.rr_variability_seconds', 0);
        setPath('$.ecg.conditions', [{ code: 'SR', severity: 1 }]);
      } else {
        setPath('$.ecg.rhythm_episodes', []);
        setPath('$.ecg.conditions', [{ code: 'NORM', severity: 1 }]);
      }
      syncControls(); markEdited();
    });
    ['rhythm-type', 'rhythm-rate', 'rhythm-start', 'rhythm-duration', 'rhythm-transition'].forEach((id) => $(id).addEventListener('change', () => {
      const episode = (getPath('$.ecg.rhythm_episodes') || [])[0];
      if (!episode) return;
      if (id === 'rhythm-type') {
        episode.type = $('rhythm-type').value;
        episode.rate_bpm = rhythmDefaults[episode.type];
        setPath('$.ecg.conditions', episode.type === 'psvt' ? [{ code: 'PSVT', severity: 1 }]
          : episode.type === 'svarr' ? [{ code: 'SVARR', severity: 1 }] : [{ code: 'SR', severity: 1 }]);
      } else if (id === 'rhythm-rate') episode.rate_bpm = inputNumber(id);
      else if (id === 'rhythm-start') episode.start_seconds = inputNumber(id);
      else if (id === 'rhythm-duration') episode.duration_seconds = inputNumber(id);
      else episode.transition_seconds = inputNumber(id);
      syncControls(); markEdited();
    }));
    $('ectopy-every').addEventListener('change', () => { setPath('$.ecg.ectopic_every_n_beats', inputNumber('ectopy-every')); markEdited(); });
    $('ectopy-severity').addEventListener('input', () => { $('ectopy-severity-output').textContent = `${$('ectopy-severity').value}%`; });
    $('ectopy-severity').addEventListener('change', () => {
      const code = $('ectopy-type').value;
      const item = (getPath('$.ecg.conditions') || []).find((condition) => condition.code === code);
      if (item) item.severity = inputNumber('ectopy-severity') / 100;
      renderConditions(); markEdited();
    });
    $('add-condition').addEventListener('click', () => {
      const code = $('other-condition').value; if (!code) return;
      const conditions = getPath('$.ecg.conditions') || [];
      if (!conditions.some((item) => item.code === code)) conditions.push({ code, severity: 1 });
      setPath('$.ecg.conditions', conditions); $('other-condition').value = ''; renderConditions(); markEdited();
    });
    $('condition-list').addEventListener('click', (event) => {
      const button = event.target.closest('[data-remove-condition]'); if (!button) return;
      let conditions = (getPath('$.ecg.conditions') || []).filter((item) => item.code !== button.dataset.removeCondition);
      if (!conditions.length) conditions = [{ code: 'NORM', severity: 1 }];
      setPath('$.ecg.conditions', conditions); renderConditions(); markEdited();
    });
    $('add-noise').addEventListener('click', () => {
      const type = $('noise-type').value; if (!type) return;
      const meta = (state.schema.artifacts || []).find((item) => item.type === type) || {};
      if (!Array.isArray(state.scenario.artifacts)) state.scenario.artifacts = [];
      state.scenario.artifacts.push({ type, start_seconds: 0, duration_seconds: Number(state.scenario.duration_seconds), severity: .4, seed: Number(state.scenario.seed) + state.scenario.artifacts.length + 101, channels: [meta.channel_family === 'ppg' ? 'all_ppg' : 'all_ecg'] });
      $('noise-type').value = ''; renderNoise(); renderTargets(); markEdited();
    });
    $('noise-list').addEventListener('click', (event) => {
      const button = event.target.closest('[data-remove-noise]'); if (!button) return;
      state.scenario.artifacts.splice(Number(button.dataset.removeNoise), 1); renderNoise(); renderTargets(); markEdited();
    });
    $('noise-list').addEventListener('change', (event) => {
      const index = Number(event.target.dataset.noiseIndex); const field = event.target.dataset.noiseField;
      if (!Number.isInteger(index) || !field || !state.scenario.artifacts[index]) return;
      state.scenario.artifacts[index][field] = field === 'channels' ? [event.target.value] : field === 'severity' ? Number(event.target.value) / 100 : Number(event.target.value);
      renderNoise(); markEdited();
    });
  }

  function bindViewer() {
    $('channel-list').addEventListener('click', (event) => {
      const button = event.target.closest('[data-channel]'); if (!button) return;
      const index = Number(button.dataset.channel);
      if (state.channels.includes(index)) {
        if (state.channels.length === 1) return;
        state.channels = state.channels.filter((value) => value !== index);
      } else state.channels.push(index);
      state.channels.sort((a, b) => a - b); renderChannelChips(); requestWindow(true);
    });
    $('position-slider').addEventListener('input', () => { state.startSample = Number($('position-slider').value); updatePosition(); scheduleWindow(); });
    $('time-in').addEventListener('click', () => zoomTime(.5)); $('time-out').addEventListener('click', () => zoomTime(2));
    $('amplitude-in').addEventListener('click', () => { state.amplitude = Math.min(16, state.amplitude * 1.5); renderer.setAmplitudeScale(state.amplitude); });
    $('amplitude-out').addEventListener('click', () => { state.amplitude = Math.max(.125, state.amplitude / 1.5); renderer.setAmplitudeScale(state.amplitude); });
  }

  function bindPage() {
    $('case-template').addEventListener('change', () => loadTemplate($('case-template').value));
    $('target-list').addEventListener('change', (event) => {
      if (!event.target.dataset.target) return;
      state.targets = [...document.querySelectorAll('[data-target]:checked')].map((input) => input.dataset.target);
      markEdited();
    });
    $('apply-json').addEventListener('click', () => {
      try { state.scenario = JSON.parse($('scenario-json').value); syncControls(); markEdited(); toast('Advanced JSON applied to the case.'); }
      catch (error) { toast(`Invalid JSON: ${error.message}`, 'error'); }
    });
    $('format-json').addEventListener('click', () => {
      try { $('scenario-json').value = JSON.stringify(JSON.parse($('scenario-json').value), null, 2); }
      catch (error) { toast(`Invalid JSON: ${error.message}`, 'error'); }
    });
    $('validation-panel').addEventListener('click', (event) => { const button = event.target.closest('[data-error-path]'); if (button) focusPath(button.dataset.errorPath); });
    $('render-preview').addEventListener('click', renderPreview);
    $('save-draft').addEventListener('click', () => saveAction('stay'));
    $('continue-pack').addEventListener('click', () => saveAction('pack'));
    $('build-pack').addEventListener('click', buildOneCasePack);
    bindHumanControls(); bindIngredients(); bindViewer();
  }

  async function initialize() {
    bindPage();
    try {
      const [schema, catalog] = await Promise.all([api('/v1/authoring/schema'), api('/v1/authoring/templates')]);
      state.schema = schema; state.templates = catalog.templates || [];
      $('case-template').innerHTML = state.templates.map((item) => `<option value="${escapeHtml(item.template_id)}">${escapeHtml(item.name)} · ${escapeHtml(item.difficulty)}</option>`).join('');
      $('case-template').disabled = false;
      $('other-condition').innerHTML += (schema.conditions || []).filter((item) => item.code !== 'NORM' && item.code !== 'PAC' && item.code !== 'PVC').map((item) => `<option value="${escapeHtml(item.code)}">${escapeHtml(item.name)} · ${escapeHtml(item.category)}</option>`).join('');
      $('noise-type').innerHTML += (schema.artifacts || []).map((item) => `<option value="${escapeHtml(item.type)}">${escapeHtml(artifactLabels[item.type] || item.type)} · ${escapeHtml(item.channel_family.toUpperCase())}</option>`).join('');
      loadTemplate(state.templates.some((item) => item.template_id === 'ecg_rpeak_clean') ? 'ecg_rpeak_clean' : state.templates[0].template_id);
      $('case-template').value = state.templates.some((item) => item.template_id === 'ecg_rpeak_clean') ? 'ecg_rpeak_clean' : state.templates[0].template_id;
    } catch (error) {
      if (error.status === 401) {
        $('validation-panel').innerHTML = `<strong>Sign in to use Synsigra Lab.</strong> <a href="${base}/account?next=lab">Open account</a>`;
        $('validation-status').textContent = 'Sign in required';
      } else $('validation-panel').innerHTML = `<strong>${escapeHtml(error.message)}</strong>`;
    }
  }

  initialize();
})();
