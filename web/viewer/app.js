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
    empty: document.querySelector('#viewer-empty')
  };
  const state = {
    jobs: [],
    jobId: '',
    description: null,
    caseMetadata: null,
    selectedChannels: [],
    startSample: 0,
    spanSamples: 0,
    amplitudeScale: 1,
    layout: 'stacked',
    requestController: null,
    requestSerial: 0,
    requestTimer: null,
    dragging: null
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
    renderer.setAmplitudeScale(1);
    renderer.setCase(caseMetadata);
    elements.heading.textContent = caseMetadata.case_id;
    elements.sourceSummary.className = 'source-summary';
    elements.sourceSummary.innerHTML = `<strong>${escapeHtml(caseMetadata.case_id)}</strong>
      ${caseMetadata.channels.length} channels · ${caseMetadata.sample_rate_hz.toLocaleString()} Hz ·
      ${library.niceDuration(caseMetadata.sample_count / caseMetadata.sample_rate_hz)}`;
    elements.empty.hidden = true;
    renderChannels();
    updatePositionControls();
    updateLocation();
    scheduleWindow(true);
  }

  async function loadDescription() {
    state.jobId = elements.job.value;
    state.description = null;
    state.caseMetadata = null;
    elements.case.disabled = true;
    elements.case.innerHTML = '<option>Loading signal cases…</option>';
    elements.channelFieldset.disabled = true;
    elements.empty.hidden = false;
    elements.empty.querySelector('strong').textContent = 'Loading signal source';
    elements.empty.querySelector('span').textContent = 'Preparing case and channel metadata…';
    setStatus('Loading metadata', 'loading');
    try {
      const description = await dataSource.describe(state.jobId);
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
      elements.case.innerHTML = '<option>Viewer unavailable</option>';
      elements.sourceSummary.className = 'source-summary error';
      elements.sourceSummary.innerHTML = error.status === 401
        ? `Sign in to view generated data. <a href="${apiBase}/account">Open account</a>`
        : `${escapeHtml(error.message)} Generate the pack again if it predates the signal viewer.`;
      elements.empty.querySelector('strong').textContent = 'Signal viewer unavailable';
      elements.empty.querySelector('span').textContent = error.message;
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
        elements.empty.querySelector('strong').textContent = 'No completed package yet';
        elements.empty.querySelector('span').textContent = 'Choose a pack and generate a job to inspect its signals.';
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
      setStatus(error.message, 'error');
    }
  }

  async function loadWindow() {
    window.clearTimeout(state.requestTimer);
    if (!state.caseMetadata || !state.selectedChannels.length) {
      renderer.setWindow(null);
      elements.empty.hidden = false;
      elements.empty.querySelector('strong').textContent = 'Select at least one channel';
      elements.empty.querySelector('span').textContent = 'The API only returns channels selected for display.';
      setStatus('No channels selected', '');
      return;
    }
    if (state.requestController) state.requestController.abort();
    state.requestController = new AbortController();
    const serial = ++state.requestSerial;
    setStatus('Loading viewport', 'loading');
    try {
      const data = await dataSource.readWindow(state.jobId, {
        caseId: state.caseMetadata.case_id,
        startSample: state.startSample,
        sampleCount: state.spanSamples,
        points: renderer.targetPoints(),
        channels: state.selectedChannels
      }, state.requestController.signal);
      if (serial !== state.requestSerial) return;
      renderer.setWindow(data);
      elements.empty.hidden = true;
      const mode = data.samplesPerBucket === 1
        ? 'raw samples'
        : `${data.samplesPerBucket.toLocaleString()} samples/bucket`;
      elements.detail.textContent = `${data.bucketCount.toLocaleString()} buckets · ${data.channels.length} channels · ${mode} · ${formatBytes(data.byteLength)} transferred`;
      setStatus('Viewport ready', 'ok');
    } catch (error) {
      if (error.name === 'AbortError') return;
      if (serial !== state.requestSerial) return;
      elements.empty.hidden = false;
      elements.empty.querySelector('strong').textContent = 'Could not load viewport';
      elements.empty.querySelector('span').textContent = error.message;
      setStatus(error.message, 'error');
    }
  }

  function scheduleWindow(immediate) {
    window.clearTimeout(state.requestTimer);
    state.requestTimer = window.setTimeout(loadWindow, immediate ? 0 : 70);
  }

  function setTimeWindow(span, anchorRatio) {
    if (!state.caseMetadata) return;
    const oldSpan = state.spanSamples;
    const ratio = anchorRatio === undefined ? 0.5 : anchorRatio;
    const anchor = state.startSample + oldSpan * ratio;
    state.spanSamples = Math.max(16, Math.min(state.caseMetadata.sample_count, Math.round(span)));
    state.startSample = clampStart(anchor - state.spanSamples * ratio);
    updatePositionControls();
    scheduleWindow(true);
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

  elements.job.addEventListener('change', loadDescription);
  elements.case.addEventListener('change', () => applyCase(selectedCase(), false));
  elements.channelList.addEventListener('change', () => {
    state.selectedChannels = Array.from(
      elements.channelList.querySelectorAll('input:checked')
    ).map((input) => Number(input.value));
    scheduleWindow(true);
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
  }
  document.querySelector('#layout-stacked').addEventListener('click', () => setLayout('stacked'));
  document.querySelector('#layout-overlay').addEventListener('click', () => setLayout('overlay'));
  document.querySelector('#time-in').addEventListener('click', () => setTimeWindow(state.spanSamples / 2));
  document.querySelector('#time-out').addEventListener('click', () => setTimeWindow(state.spanSamples * 2));
  document.querySelector('#amplitude-in').addEventListener('click', () => setAmplitude(state.amplitudeScale * 2));
  document.querySelector('#amplitude-out').addEventListener('click', () => setAmplitude(state.amplitudeScale / 2));
  document.querySelector('#amplitude-fit').addEventListener('click', () => setAmplitude(1));

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
    state.dragging = { x: event.clientX, start: state.startSample };
    elements.canvas.classList.add('dragging');
    elements.canvas.setPointerCapture(event.pointerId);
  });
  elements.canvas.addEventListener('pointermove', (event) => {
    if (!state.dragging) return;
    const width = elements.canvas.getBoundingClientRect().width;
    state.startSample = clampStart(
      state.dragging.start - (event.clientX - state.dragging.x) / width * state.spanSamples
    );
    updatePositionControls();
    scheduleWindow(false);
  });
  function endDrag() {
    if (!state.dragging) return;
    state.dragging = null;
    elements.canvas.classList.remove('dragging');
    scheduleWindow(true);
  }
  elements.canvas.addEventListener('pointerup', endDrag);
  elements.canvas.addEventListener('pointercancel', endDrag);
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

  loadJobs();
})();
