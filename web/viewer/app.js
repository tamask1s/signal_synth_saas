(function () {
  'use strict';

  const library = window.SynsigraSignalViewer;
  const apiBase = document.body.dataset.apiBase || '/syn_sig_ra';
  const dataSource = new library.HttpSignalDataSource({ apiBase });
  const elements = {
    job: document.querySelector('#viewer-job'),
    case: document.querySelector('#viewer-case'),
    channelFieldset: document.querySelector('#channel-fieldset'),
    channelList: document.querySelector('#channel-list'),
    sourceSummary: document.querySelector('#source-summary'),
    heading: document.querySelector('#viewer-heading'),
    position: document.querySelector('#viewport-position'),
    detail: document.querySelector('#viewport-detail'),
    status: document.querySelector('#request-status'),
    slider: document.querySelector('#position-slider'),
    canvas: document.querySelector('#signal-canvas'),
    canvasShell: document.querySelector('#canvas-shell'),
    empty: document.querySelector('#viewer-empty'),
    overlayFieldset: document.querySelector('#overlay-fieldset'),
    overlayList: document.querySelector('#overlay-list'),
    detectionFile: document.querySelector('#local-detection-file'),
    detectionStatus: document.querySelector('#local-detection-status'),
    cursor: document.querySelector('#cursor-inspector'),
    spacingValue: document.querySelector('#spacing-value'),
    spacingIn: document.querySelector('#spacing-in'),
    spacingOut: document.querySelector('#spacing-out')
  };
  const signalCache = new library.SignalWindowCache(28 * 1024 * 1024);
  const overlayCache = new library.BoundedLruCache(4 * 1024 * 1024);
  const CHANNEL_SPACING_STEP = 1.5;
  const MIN_CHANNEL_SPACING = 1 / (CHANNEL_SPACING_STEP * CHANNEL_SPACING_STEP);
  const MAX_CHANNEL_SPACING = CHANNEL_SPACING_STEP * CHANNEL_SPACING_STEP;
  const state = {
    jobs: [],
    jobId: '',
    description: null,
    caseMetadata: null,
    selectedChannels: [],
    startSample: 0,
    spanSamples: 0,
    amplitudeScale: 1,
    channelSpacing: 1,
    layout: 'stacked',
    currentWindow: null,
    currentOverlays: null,
    enabledOverlayKinds: new Set(),
    overlaySelectionInitialized: false,
    localEvents: [],
    requestController: null,
    requestSerial: 0,
    metadataSerial: 0,
    requestTimer: null,
    dragging: null
  };

  const OVERLAY_LABELS = {
    artifact: 'Artifacts and signal quality',
    episode: 'Clinical episodes',
    r_peak: 'ECG R peaks',
    beat_class: 'ECG beat classes',
    ecg_fiducial: 'ECG delineation fiducials',
    ppg_onset: 'PPG pulse onsets',
    ppg_peak: 'PPG systolic peaks',
    low_perfusion: 'Low-perfusion pulses',
    missing_pulse: 'Expected missing pulses'
  };

  const renderer = new library.SignalCanvasRenderer(elements.canvas, {
    onResize: () => scheduleWindow(false)
  });

  function escapeHtml(value) {
    return String(value).replace(/[&<>'"]/g, (character) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;'
    })[character]);
  }

  function setStatus(message, kind) {
    elements.status.textContent = message;
    elements.status.className = `request-status ${kind || ''}`;
  }

  function setEmptyState(visible, title, message) {
    elements.empty.classList.toggle('is-visible', Boolean(visible));
    elements.empty.hidden = !visible;
    elements.empty.setAttribute('aria-hidden', String(!visible));
    if (title !== undefined) elements.empty.querySelector('strong').textContent = title;
    if (message !== undefined) elements.empty.querySelector('span').textContent = message;
  }

  function formatBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
    return `${(bytes / 1024 / 1024).toFixed(1)} MiB`;
  }

  function selectedCase() {
    return state.description && state.description.cases.find(
      (item) => item.case_id === elements.case.value
    );
  }

  function maximumStart() {
    return state.caseMetadata
      ? Math.max(0, state.caseMetadata.sample_count - state.spanSamples)
      : 0;
  }

  function clampStart(value) {
    return Math.max(0, Math.min(maximumStart(), Math.round(value)));
  }

  function updateLocation() {
    const url = new URL(window.location.href);
    if (state.jobId) url.searchParams.set('job_id', state.jobId);
    else url.searchParams.delete('job_id');
    if (state.caseMetadata) url.searchParams.set('case_id', state.caseMetadata.case_id);
    else url.searchParams.delete('case_id');
    window.history.replaceState({}, '', url);
  }

  function updatePositionControls() {
    if (!state.caseMetadata) return;
    state.startSample = clampStart(state.startSample);
    elements.slider.max = String(maximumStart());
    elements.slider.value = String(state.startSample);
    elements.slider.step = String(Math.max(1, Math.floor(state.spanSamples / 1000)));
    elements.slider.disabled = maximumStart() <= 0;
    const rate = state.caseMetadata.sample_rate_hz;
    const start = state.startSample / rate;
    const end = (state.startSample + state.spanSamples) / rate;
    const total = state.caseMetadata.sample_count / rate;
    elements.position.textContent = `${library.niceDuration(start)} — ${library.niceDuration(end)} of ${library.niceDuration(total)}`;
    renderer.setViewport(state.startSample, state.spanSamples);
  }

  function renderChannels() {
    const channels = state.caseMetadata ? state.caseMetadata.channels : [];
    elements.channelList.innerHTML = channels.map((channel) => {
      const checked = state.selectedChannels.includes(channel.index) ? ' checked' : '';
      const color = library.COLORS[channel.index % library.COLORS.length];
      return `<label class="channel-option">
        <input type="checkbox" value="${channel.index}"${checked}>
        <span class="channel-color" style="background:${color}"></span>
        <span class="channel-name" title="${escapeHtml(channel.name)}">${escapeHtml(channel.name)}</span>
        <span class="channel-unit">${escapeHtml(channel.unit)}</span>
      </label>`;
    }).join('');
    elements.channelFieldset.disabled = !channels.length;
  }

  function applyCase(caseMetadata, preserveWindow) {
    state.caseMetadata = caseMetadata;
    state.currentWindow = null;
    state.currentOverlays = null;
    state.localEvents = [];
    state.enabledOverlayKinds = new Set();
    state.overlaySelectionInitialized = false;
    state.selectedChannels = caseMetadata.channels.map((channel) => channel.index);
    const initialSpan = Math.min(
      caseMetadata.sample_count,
      Math.max(16, Math.round(caseMetadata.sample_rate_hz * 10))
    );
    if (!preserveWindow || !state.spanSamples) {
      state.startSample = 0;
      state.spanSamples = initialSpan;
    } else {
      state.spanSamples = Math.min(caseMetadata.sample_count, state.spanSamples);
      state.startSample = clampStart(state.startSample);
    }
    state.amplitudeScale = 1;
    state.channelSpacing = 1;
    renderer.setAmplitudeScale(1);
    setChannelSpacing(1);
    renderer.setCase(caseMetadata);
    renderer.setLocalEvents([]);
    renderer.setEnabledOverlayKinds([]);
    elements.overlayList.innerHTML = '<p class="control-hint">Loading available annotations…</p>';
    elements.overlayFieldset.disabled = true;
    elements.detectionFile.value = '';
    elements.detectionStatus.textContent = 'No local detector output loaded.';
    elements.cursor.innerHTML = '<strong>Cursor inspector</strong><span>Click the plot to inspect an exact sample and nearby annotations.</span>';
    elements.heading.textContent = caseMetadata.case_id;
    elements.sourceSummary.className = 'source-summary';
    elements.sourceSummary.innerHTML = `<strong>${escapeHtml(caseMetadata.case_id)}</strong>
      ${caseMetadata.channels.length} channels · ${caseMetadata.sample_rate_hz.toLocaleString()} Hz ·
      ${library.niceDuration(caseMetadata.sample_count / caseMetadata.sample_rate_hz)}`;
    setEmptyState(true, 'Loading viewport', 'Fetching signal samples…');
    renderChannels();
    updatePositionControls();
    updateLocation();
    scheduleWindow(true);
  }

  async function loadDescription() {
    const metadataSerial = ++state.metadataSerial;
    window.clearTimeout(state.requestTimer);
    if (state.requestController) state.requestController.abort();
    state.requestController = null;
    state.requestSerial += 1;
    state.jobId = elements.job.value;
    state.description = null;
    state.caseMetadata = null;
    state.currentWindow = null;
    state.currentOverlays = null;
    state.localEvents = [];
    state.enabledOverlayKinds = new Set();
    state.overlaySelectionInitialized = false;
    renderer.setWindow(null);
    renderer.setOverlays(null);
    renderer.setLocalEvents([]);
    renderer.setEnabledOverlayKinds([]);
    elements.case.disabled = true;
    elements.case.innerHTML = '<option>Loading signal cases…</option>';
    elements.channelFieldset.disabled = true;
    elements.overlayFieldset.disabled = true;
    elements.overlayList.innerHTML = '<p class="control-hint">Loading available annotations…</p>';
    elements.detectionFile.value = '';
    elements.detectionStatus.textContent = 'No local detector output loaded.';
    elements.detectionStatus.className = 'local-status';
    elements.heading.textContent = 'Loading signal…';
    elements.position.textContent = 'Preparing case metadata';
    elements.sourceSummary.className = 'source-summary';
    elements.sourceSummary.textContent = 'Loading the selected generated package…';
    setEmptyState(
      true,
      'Loading signal source',
      'Preparing case and channel metadata…');
    setStatus('Loading metadata', 'loading');
    try {
      const description = await dataSource.describe(state.jobId);
      if (metadataSerial !== state.metadataSerial) return;
      state.description = description;
      elements.case.innerHTML = description.cases.map((item) =>
        `<option value="${escapeHtml(item.case_id)}">${escapeHtml(item.case_id)}</option>`
      ).join('');
      elements.case.disabled = false;
      const requestedCase = new URLSearchParams(window.location.search).get('case_id');
      if (requestedCase && description.cases.some((item) => item.case_id === requestedCase)) {
        elements.case.value = requestedCase;
      }
      applyCase(selectedCase(), false);
      setStatus('Ready', 'ok');
    } catch (error) {
      if (metadataSerial !== state.metadataSerial) return;
      elements.case.innerHTML = '<option>Viewer unavailable</option>';
      elements.heading.textContent = 'Signal viewer unavailable';
      elements.position.textContent = 'Select another completed job or generate a new package.';
      elements.sourceSummary.className = 'source-summary error';
      elements.sourceSummary.innerHTML = error.status === 401
        ? `Sign in to view generated data. <a href="${apiBase}/account">Open account</a>`
        : `${escapeHtml(error.message)} Generate the pack again if it predates the signal viewer.`;
      setEmptyState(true, 'Signal viewer unavailable', error.message);
      setStatus(error.message, 'error');
    }
  }

  async function apiJson(path) {
    const response = await fetch(apiBase + path, {
      credentials: 'same-origin',
      cache: 'no-store',
      headers: { Accept: 'application/json' }
    });
    if (!response.ok) {
      const error = new Error(response.status === 401 ? 'Sign in to continue.' : `Request failed (${response.status}).`);
      error.status = response.status;
      throw error;
    }
    return response.json();
  }

  async function loadJobs() {
    setStatus('Loading jobs', 'loading');
    try {
      const jobs = [];
      let offset = 0;
      for (let page = 0; page < 10; page += 1) {
        const result = await apiJson(`/v1/jobs?limit=100&offset=${offset}`);
        jobs.push(...result.jobs);
        if (result.next_offset === undefined) break;
        offset = result.next_offset;
      }
      state.jobs = jobs.filter((job) =>
        job.status === 'succeeded' && job.package_id && job.artifact_status !== 'expired'
      );
      if (!state.jobs.length) {
        elements.job.innerHTML = '<option>No retained completed jobs</option>';
        elements.sourceSummary.innerHTML = `Generate a package first. <a href="${apiBase}/packs">Choose a pack</a>`;
        setEmptyState(
          true,
          'No completed package yet',
          'Choose a pack and generate a job to inspect its signals.');
        setStatus('No source', '');
        return;
      }
      elements.job.innerHTML = state.jobs.map((job) => {
        const date = job.completed_at ? new Date(job.completed_at).toLocaleString() : 'completed';
        return `<option value="${escapeHtml(job.job_id)}">${escapeHtml(job.pack_id)} · ${escapeHtml(date)}</option>`;
      }).join('');
      elements.job.disabled = false;
      const requestedJob = new URLSearchParams(window.location.search).get('job_id');
      if (requestedJob && state.jobs.some((job) => job.job_id === requestedJob)) {
        elements.job.value = requestedJob;
      }
      await loadDescription();
    } catch (error) {
      elements.job.innerHTML = '<option>Sign in required</option>';
      elements.sourceSummary.className = 'source-summary error';
      elements.sourceSummary.innerHTML = error.status === 401
        ? `Sign in to view your data. <a href="${apiBase}/account">Open account</a>`
        : escapeHtml(error.message);
      setEmptyState(true, 'Signal viewer unavailable', error.message);
      setStatus(error.message, 'error');
    }
  }

  function expectedBucketSize(span, points) {
    const desired = Math.max(1, Math.ceil(span / Math.max(1, points)));
    if (desired < 64) return desired;
    let power = 64;
    while (power < desired) power *= 2;
    return power;
  }

  function overlayCacheKey(jobId, caseId, data) {
    return `${jobId}|${caseId}|${data.requested_start_sample}|${data.requested_sample_count}`;
  }

  function findCachedOverlays(startSample, sampleCount) {
    const end = startSample + sampleCount;
    let match = null;
    let matchKey = '';
    overlayCache.entries.forEach((entry, key) => {
      const candidate = entry.value;
      const candidateEnd = candidate.data.requested_start_sample +
        candidate.data.requested_sample_count;
      if (candidate.jobId === state.jobId &&
          candidate.caseId === state.caseMetadata.case_id &&
          candidate.data.requested_start_sample <= startSample &&
          candidateEnd >= end &&
          (!match || candidate.data.requested_sample_count <
            match.requested_sample_count)) {
        match = candidate.data;
        matchKey = key;
      }
    });
    if (matchKey) overlayCache.get(matchKey);
    return match;
  }

  function cacheSummary() {
    const signal = signalCache.summary();
    const overlays = overlayCache.summary();
    return {
      bytes: signal.bytes + overlays.bytes,
      maxBytes: signal.maxBytes + overlays.maxBytes,
      entries: signal.entries + overlays.entries
    };
  }

  function prefetchedRequest() {
    const total = state.caseMetadata.sample_count;
    const sampleCount = Math.min(total, Math.max(16, state.spanSamples * 3));
    const startSample = Math.max(
      0,
      Math.min(total - sampleCount, state.startSample - state.spanSamples)
    );
    return {
      startSample: Math.round(startSample),
      sampleCount: Math.round(sampleCount),
      points: Math.min(16384, renderer.targetPoints() * 3)
    };
  }

  function renderOverlayControls(availableKinds) {
    const available = Array.isArray(availableKinds) ? availableKinds : [];
    const previous = new Set(state.enabledOverlayKinds);
    if (!state.overlaySelectionInitialized) {
      const preferred = available.includes('r_peak') ? 'r_peak' :
        available.includes('beat_class') ? 'beat_class' : available[0];
      state.enabledOverlayKinds = new Set(preferred ? [preferred] : []);
      state.overlaySelectionInitialized = true;
    } else {
      state.enabledOverlayKinds = new Set(
        available.filter((kind) => previous.has(kind)));
    }
    elements.overlayList.innerHTML = available.length ? available.map((kind) => `
      <label class="overlay-option overlay-${escapeHtml(kind)}">
        <input type="checkbox" value="${escapeHtml(kind)}"${state.enabledOverlayKinds.has(kind) ? ' checked' : ''}>
        <span class="overlay-symbol" aria-hidden="true"></span>
        <span>${escapeHtml(OVERLAY_LABELS[kind] || kind)}</span>
      </label>
    `).join('') : '<p class="control-hint">This case has no compatible ground-truth annotations.</p>';
    elements.overlayFieldset.disabled = !available.length;
    renderer.setEnabledOverlayKinds(Array.from(state.enabledOverlayKinds));
  }

  function applyLoadedData(windowData, overlayData, source) {
    if (windowData) {
      state.currentWindow = windowData;
      renderer.setWindow(windowData);
    }
    if (overlayData) {
      state.currentOverlays = overlayData;
      renderer.setOverlays(overlayData);
      renderOverlayControls(overlayData.available_kinds);
    }
    renderer.setViewport(state.startSample, state.spanSamples);
    if (!state.currentWindow) {
      setEmptyState(true, 'Loading viewport', 'Fetching signal samples…');
      return;
    }
    setEmptyState(false);
    const data = state.currentWindow;
    const mode = data.samplesPerBucket === 1
      ? 'raw samples'
      : `exact min/max envelope · ${data.samplesPerBucket.toLocaleString()} samples/bucket`;
    const cache = cacheSummary();
    const overlayDetail = state.currentOverlays
      ? ` · ${state.currentOverlays.items.length.toLocaleString()} overlay items${state.currentOverlays.aggregated ? ' (clustered)' : ''}`
      : '';
    elements.detail.textContent = `${data.bucketCount.toLocaleString()} buckets · ${data.channels.length} channels · ${mode}${overlayDetail} · client cache ${formatBytes(cache.bytes)} / ${formatBytes(cache.maxBytes)} (${cache.entries} segments)`;
    setStatus(`Viewport ready · ${source}`, 'ok');
  }

  async function loadWindow() {
    window.clearTimeout(state.requestTimer);
    if (!state.caseMetadata || !state.selectedChannels.length) {
      state.currentWindow = null;
      renderer.setWindow(null);
      setEmptyState(
        true,
        'Select at least one channel',
        'The API only returns channels selected for display.');
      setStatus('No channels selected', '');
      return;
    }
    const maximumBucket = expectedBucketSize(
      state.spanSamples, renderer.targetPoints());
    const cachedSignal = signalCache.find(
      state.jobId, state.caseMetadata.case_id, state.selectedChannels,
      state.startSample, state.spanSamples, maximumBucket);
    const cachedOverlays = findCachedOverlays(
      state.startSample, state.spanSamples);
    if (cachedSignal || cachedOverlays) {
      applyLoadedData(cachedSignal, cachedOverlays, 'client cache');
    }
    if (cachedSignal && cachedOverlays) return;
    if (state.requestController) state.requestController.abort();
    const controller = new AbortController();
    state.requestController = controller;
    const serial = ++state.requestSerial;
    setStatus('Loading viewport', 'loading');
    try {
      const request = prefetchedRequest();
      const signalPromise = cachedSignal ? Promise.resolve(cachedSignal) :
        dataSource.readWindow(state.jobId, {
          caseId: state.caseMetadata.case_id,
          startSample: request.startSample,
          sampleCount: request.sampleCount,
          points: request.points,
          channels: state.selectedChannels
        }, controller.signal);
      const overlayPromise = cachedOverlays ? Promise.resolve(cachedOverlays) :
        dataSource.readOverlays(state.jobId, {
          caseId: state.caseMetadata.case_id,
          startSample: request.startSample,
          sampleCount: request.sampleCount,
          maxItems: 6000
        }, controller.signal).catch((error) => {
          if (error.name === 'AbortError') throw error;
          if (error.status === 409) return null;
          throw error;
        });
      const [data, overlays] = await Promise.all([signalPromise, overlayPromise]);
      if (serial !== state.requestSerial) return;
      if (!cachedSignal) signalCache.add(
        state.jobId, state.caseMetadata.case_id, data);
      if (!cachedOverlays && overlays) {
        const bytes = new Blob([JSON.stringify(overlays)]).size * 2;
        overlayCache.set(
          overlayCacheKey(state.jobId, state.caseMetadata.case_id, overlays),
          { jobId: state.jobId, caseId: state.caseMetadata.case_id, data: overlays },
          bytes);
      }
      if (!overlays && !cachedOverlays) renderOverlayControls([]);
      applyLoadedData(data, overlays, 'network + prefetch');
    } catch (error) {
      if (error.name === 'AbortError') return;
      if (serial !== state.requestSerial) return;
      setEmptyState(true, 'Could not load viewport', error.message);
      setStatus(error.message, 'error');
    } finally {
      if (state.requestController === controller) state.requestController = null;
    }
  }

  function scheduleWindow(immediate, forceNetwork) {
    window.clearTimeout(state.requestTimer);
    if (state.requestController) {
      state.requestController.abort();
      state.requestController = null;
      state.requestSerial += 1;
    }
    state.requestTimer = window.setTimeout(
      () => loadWindow(),
      immediate ? 0 : 140
    );
  }

  function setTimeWindow(span, anchorRatio) {
    if (!state.caseMetadata) return;
    const oldSpan = state.spanSamples;
    const ratio = anchorRatio === undefined ? 0.5 : anchorRatio;
    const anchor = state.startSample + oldSpan * ratio;
    state.spanSamples = Math.max(16, Math.min(state.caseMetadata.sample_count, Math.round(span)));
    state.startSample = clampStart(anchor - state.spanSamples * ratio);
    updatePositionControls();
    scheduleWindow(false);
  }

  function panBy(samples) {
    if (!state.caseMetadata) return;
    state.startSample = clampStart(state.startSample + samples);
    updatePositionControls();
    scheduleWindow(false);
  }

  function setAmplitude(scale) {
    state.amplitudeScale = Math.max(0.125, Math.min(32, scale));
    renderer.setAmplitudeScale(state.amplitudeScale);
  }

  function setChannelSpacing(scale) {
    state.channelSpacing = Math.max(MIN_CHANNEL_SPACING, Math.min(MAX_CHANNEL_SPACING, scale));
    const viewportHeight = window.visualViewport &&
      Number.isFinite(window.visualViewport.height)
      ? window.visualViewport.height
      : window.innerHeight;
    const fittedHeight = Math.max(430, Math.round(viewportHeight - 140));
    elements.canvasShell.style.height = `${Math.round(fittedHeight * state.channelSpacing)}px`;
    renderer.setChannelSpacing(state.channelSpacing);
    elements.spacingValue.textContent = `Spacing ${Math.round(state.channelSpacing * 100)}%`;
    elements.spacingIn.disabled = state.layout !== 'stacked' ||
      state.channelSpacing >= MAX_CHANNEL_SPACING - 0.001;
    elements.spacingOut.disabled = state.layout !== 'stacked' ||
      state.channelSpacing <= MIN_CHANNEL_SPACING + 0.001;
  }

  function parseCsv(text) {
    const rows = [];
    let row = [];
    let cell = '';
    let quoted = false;
    for (let index = 0; index < text.length; index += 1) {
      const character = text[index];
      if (quoted) {
        if (character === '"' && text[index + 1] === '"') {
          cell += '"';
          index += 1;
        } else if (character === '"') quoted = false;
        else cell += character;
      } else if (character === '"') quoted = true;
      else if (character === ',') {
        row.push(cell);
        cell = '';
      } else if (character === '\n') {
        row.push(cell.replace(/\r$/, ''));
        if (row.some((value) => value.length)) rows.push(row);
        row = [];
        cell = '';
      } else cell += character;
    }
    if (quoted) throw new Error('CSV contains an unterminated quoted value.');
    row.push(cell.replace(/\r$/, ''));
    if (row.some((value) => value.length)) rows.push(row);
    if (!rows.length) throw new Error('The detection file is empty.');
    const headers = rows[0].map((value) => value.trim());
    if (!headers.includes('time_seconds')) {
      throw new Error('Detection CSV must contain a time_seconds column.');
    }
    return rows.slice(1).map((values) => Object.fromEntries(
      headers.map((header, index) => [header, values[index] === undefined ? '' : values[index].trim()])
    ));
  }

  function normalizeDetectionTarget(value, filename) {
    const target = String(value || '').trim();
    if (target === 'r_peak' || target === 'ppg_systolic_peak' ||
        target === 'ppg_pulse_onset' || target === 'ecg_beat_classification') return target;
    if (target) throw new Error(`Unsupported local detection target: ${target}.`);
    const lower = filename.toLowerCase();
    if (lower.includes('class')) return 'ecg_beat_classification';
    if (lower.includes('ppg') && lower.includes('onset')) return 'ppg_pulse_onset';
    if (lower.includes('ppg')) return 'ppg_systolic_peak';
    return 'r_peak';
  }

  function normalizeLocalEvents(document, filename) {
    const target = normalizeDetectionTarget(document.target, filename);
    const input = Array.isArray(document.events) ? document.events : [];
    if (!input.length) throw new Error('The detection file contains no events.');
    if (input.length > 250000) {
      throw new Error('The local overlay is limited to 250,000 events per file.');
    }
    const rate = state.caseMetadata.sample_rate_hz;
    return input.map((item, index) => {
      const seconds = Number(item.time_seconds);
      const suppliedSample = item.sample_index === '' || item.sample_index === undefined ||
        item.sample_index === null ? null : Number(item.sample_index);
      const confidence = item.confidence === '' || item.confidence === undefined ||
        item.confidence === null ? null : Number(item.confidence);
      if (!Number.isFinite(seconds) || seconds < 0) {
        throw new Error(`Event ${index + 1} has an invalid time_seconds value.`);
      }
      if (suppliedSample !== null && (!Number.isSafeInteger(suppliedSample) || suppliedSample < 0)) {
        throw new Error(`Event ${index + 1} has an invalid sample_index value.`);
      }
      if (confidence !== null && (!Number.isFinite(confidence) || confidence < 0 || confidence > 1)) {
        throw new Error(`Event ${index + 1} has confidence outside 0–1.`);
      }
      const sample = suppliedSample === null ? Math.round(seconds * rate) : suppliedSample;
      if (sample >= state.caseMetadata.sample_count) {
        throw new Error(`Event ${index + 1} lies outside the selected case.`);
      }
      return {
        sample,
        timeSeconds: seconds,
        target,
        channel: String(item.channel || ''),
        label: String(item.label || 'local detection'),
        confidence
      };
    }).sort((left, right) => left.sample - right.sample);
  }

  async function loadLocalDetections(file) {
    if (!file || !state.caseMetadata) return;
    if (file.size > 16 * 1024 * 1024) {
      elements.detectionStatus.textContent = 'Local overlay files are limited to 16 MiB.';
      elements.detectionStatus.className = 'local-status error';
      return;
    }
    try {
      const text = await file.text();
      let document;
      if (text.trimStart().startsWith('{')) {
        document = JSON.parse(text);
        if (!document || typeof document !== 'object' || !Array.isArray(document.events)) {
          throw new Error('Detection JSON must be an object with an events array.');
        }
      } else {
        document = { events: parseCsv(text) };
      }
      state.localEvents = normalizeLocalEvents(document, file.name);
      renderer.setLocalEvents(state.localEvents);
      const target = state.localEvents[0].target;
      elements.detectionStatus.textContent = `${state.localEvents.length.toLocaleString()} ${target} events loaded locally. The file was not uploaded.`;
      elements.detectionStatus.className = 'local-status ok';
    } catch (error) {
      state.localEvents = [];
      renderer.setLocalEvents([]);
      elements.detectionStatus.textContent = error.message;
      elements.detectionStatus.className = 'local-status error';
    }
  }

  function renderCursorInspection(sample) {
    const inspection = renderer.inspectSample(sample);
    if (!inspection) return;
    const channelRows = inspection.channels.map((channel) => {
      const value = channel.exact
        ? `${channel.minimum.toPrecision(6)} ${channel.unit}`
        : `${channel.minimum.toPrecision(5)}…${channel.maximum.toPrecision(5)} ${channel.unit}`;
      return `<dt>${escapeHtml(channel.name)}</dt><dd>${escapeHtml(value)}${channel.exact ? '' : ' <small>min–max bucket</small>'}</dd>`;
    }).join('');
    const annotations = inspection.annotations.length
      ? `<ul>${inspection.annotations.slice(0, 20).map((item) =>
          `<li><strong>${escapeHtml(item.label || item.kind)}</strong> <span>${escapeHtml(item.source || '')}${item.channel ? ` · ${escapeHtml(item.channel)}` : ''}${item.interval ? ` · samples ${Number(item.start_sample).toLocaleString()}–${Number(item.end_sample).toLocaleString()}` : ''}${item.value !== undefined ? ` · value ${Number(item.value).toPrecision(4)}` : ''}${item.count > 1 ? ` · ${Number(item.count).toLocaleString()} clustered events` : ''}</span></li>`
        ).join('')}</ul>`
      : '<p>No enabled annotation at this sample.</p>';
    elements.cursor.innerHTML = `
      <div><strong>Sample ${inspection.sample.toLocaleString()}</strong><span>${library.niceDuration(inspection.timeSeconds, 6)}</span></div>
      <dl>${channelRows}</dl>
      ${annotations}
    `;
  }

  elements.job.addEventListener('change', loadDescription);
  elements.case.addEventListener('change', () => applyCase(selectedCase(), false));
  elements.channelList.addEventListener('change', () => {
    state.selectedChannels = Array.from(
      elements.channelList.querySelectorAll('input:checked')
    ).map((input) => Number(input.value));
    scheduleWindow(true);
  });
  elements.overlayList.addEventListener('change', () => {
    state.enabledOverlayKinds = new Set(Array.from(
      elements.overlayList.querySelectorAll('input:checked')
    ).map((input) => input.value));
    renderer.setEnabledOverlayKinds(Array.from(state.enabledOverlayKinds));
  });
  elements.detectionFile.addEventListener('change', () => {
    loadLocalDetections(elements.detectionFile.files[0]);
  });
  document.querySelector('.file-button').addEventListener('keydown', (event) => {
    if (event.key !== 'Enter' && event.key !== ' ') return;
    event.preventDefault();
    elements.detectionFile.click();
  });
  document.querySelector('#clear-local-detections').addEventListener('click', () => {
    state.localEvents = [];
    elements.detectionFile.value = '';
    elements.detectionStatus.textContent = 'No local detector output loaded.';
    elements.detectionStatus.className = 'local-status';
    renderer.setLocalEvents([]);
  });
  document.querySelector('#select-all-channels').addEventListener('click', () => {
    state.selectedChannels = state.caseMetadata.channels.map((channel) => channel.index);
    renderChannels();
    scheduleWindow(true);
  });
  document.querySelector('#select-no-channels').addEventListener('click', () => {
    state.selectedChannels = [];
    renderChannels();
    scheduleWindow(true);
  });

  function setLayout(layout) {
    state.layout = layout;
    renderer.setLayout(layout);
    ['stacked', 'overlay'].forEach((name) => {
      const button = document.querySelector(`#layout-${name}`);
      const active = name === layout;
      button.classList.toggle('active', active);
      button.setAttribute('aria-pressed', String(active));
    });
    setChannelSpacing(state.channelSpacing);
  }
  document.querySelector('#layout-stacked').addEventListener('click', () => setLayout('stacked'));
  document.querySelector('#layout-overlay').addEventListener('click', () => setLayout('overlay'));
  document.querySelector('#time-in').addEventListener('click', () => setTimeWindow(state.spanSamples / 2));
  document.querySelector('#time-out').addEventListener('click', () => setTimeWindow(state.spanSamples * 2));
  document.querySelector('#amplitude-in').addEventListener('click', () => setAmplitude(state.amplitudeScale * 2));
  document.querySelector('#amplitude-out').addEventListener('click', () => setAmplitude(state.amplitudeScale / 2));
  document.querySelector('#amplitude-fit').addEventListener('click', () => setAmplitude(1));
  elements.spacingIn.addEventListener('click', () =>
    setChannelSpacing(state.channelSpacing * CHANNEL_SPACING_STEP));
  elements.spacingOut.addEventListener('click', () =>
    setChannelSpacing(state.channelSpacing / CHANNEL_SPACING_STEP));

  let displayResizeTimer = null;
  window.addEventListener('resize', () => {
    window.clearTimeout(displayResizeTimer);
    displayResizeTimer = window.setTimeout(() => setChannelSpacing(state.channelSpacing), 100);
  });

  elements.slider.addEventListener('input', () => {
    state.startSample = clampStart(Number(elements.slider.value));
    updatePositionControls();
    scheduleWindow(false);
  });
  elements.slider.addEventListener('change', () => scheduleWindow(true));

  elements.canvas.addEventListener('wheel', (event) => {
    if (!state.caseMetadata || (!event.ctrlKey && !event.metaKey && !event.shiftKey)) return;
    event.preventDefault();
    if (event.ctrlKey || event.metaKey) {
      const rect = elements.canvas.getBoundingClientRect();
      const anchor = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
      setTimeWindow(state.spanSamples * (event.deltaY > 0 ? 2 : 0.5), anchor);
    } else {
      panBy((event.deltaY || event.deltaX) / 600 * state.spanSamples);
    }
  }, { passive: false });

  elements.canvas.addEventListener('pointerdown', (event) => {
    if (!state.caseMetadata) return;
    state.dragging = { x: event.clientX, start: state.startSample, moved: false };
    elements.canvas.classList.add('dragging');
    elements.canvas.setPointerCapture(event.pointerId);
  });
  elements.canvas.addEventListener('pointermove', (event) => {
    if (!state.dragging) return;
    if (Math.abs(event.clientX - state.dragging.x) > 3) state.dragging.moved = true;
    const width = elements.canvas.getBoundingClientRect().width;
    state.startSample = clampStart(
      state.dragging.start - (event.clientX - state.dragging.x) / width * state.spanSamples
    );
    updatePositionControls();
    scheduleWindow(false);
  });
  function endDrag(event) {
    if (!state.dragging) return;
    const wasClick = !state.dragging.moved;
    state.dragging = null;
    elements.canvas.classList.remove('dragging');
    if (wasClick && event && Number.isFinite(event.clientX)) {
      renderCursorInspection(renderer.sampleAtClientX(event.clientX));
    } else scheduleWindow(true);
  }
  elements.canvas.addEventListener('pointerup', endDrag);
  elements.canvas.addEventListener('pointercancel', () => endDrag());
  elements.canvas.addEventListener('keydown', (event) => {
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      panBy(state.spanSamples * (event.key === 'ArrowLeft' ? -0.1 : 0.1));
    } else if (event.key === '+' || event.key === '=') {
      event.preventDefault();
      setTimeWindow(state.spanSamples / 2);
    } else if (event.key === '-') {
      event.preventDefault();
      setTimeWindow(state.spanSamples * 2);
    }
  });

  setChannelSpacing(1);
  loadJobs();
})();
