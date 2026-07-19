(function (global) {
  'use strict';

  const COLORS = [
    '#65dcff', '#8b94ff', '#d281ff', '#4ed6a5', '#ffc866', '#ff7f9f',
    '#70a7ff', '#b7e36e', '#ff9f68', '#72e0d1', '#c7a7ff', '#f2d56b',
    '#52bbff', '#e980c0', '#88d77a', '#ff8d78'
  ];

  function readUint64(view, offset) {
    const low = view.getUint32(offset, true);
    const high = view.getUint32(offset + 4, true);
    const value = low + high * 0x100000000;
    if (!Number.isSafeInteger(value)) throw new Error('Signal index exceeds browser precision.');
    return value;
  }

  function decodeSignalWindow(buffer) {
    const view = new DataView(buffer);
    if (view.byteLength < 80) throw new Error('Signal response is truncated.');
    const magic = String.fromCharCode.apply(null, new Uint8Array(buffer, 0, 8));
    if (magic !== 'SYNSIGV1' || view.getUint16(8, true) !== 1 ||
        view.getUint16(10, true) !== 80 || view.getUint32(12, true) !== 1) {
      throw new Error('Unsupported signal response format.');
    }
    const channelCount = view.getUint32(16, true);
    const bucketCount = view.getUint32(20, true);
    const payloadOffset = view.getUint32(72, true);
    const expectedOffset = 80 + channelCount * 4;
    const expectedSize = expectedOffset + channelCount * bucketCount * 4;
    if (!channelCount || !bucketCount || payloadOffset !== expectedOffset ||
        expectedSize !== view.byteLength) {
      throw new Error('Signal response layout is invalid.');
    }
    const channels = [];
    for (let output = 0; output < channelCount; output += 1) {
      const index = view.getUint32(80 + output * 4, true);
      const minimum = new Int16Array(bucketCount);
      const maximum = new Int16Array(bucketCount);
      let cursor = payloadOffset + output * bucketCount * 4;
      for (let bucket = 0; bucket < bucketCount; bucket += 1) {
        minimum[bucket] = view.getInt16(cursor, true);
        maximum[bucket] = view.getInt16(cursor + 2, true);
        cursor += 4;
      }
      channels.push({ index, minimum, maximum });
    }
    return {
      requestedStart: readUint64(view, 24),
      requestedCount: readUint64(view, 32),
      dataStart: readUint64(view, 40),
      samplesPerBucket: readUint64(view, 48),
      sourceSampleCount: readUint64(view, 56),
      sampleRateHz: view.getFloat64(64, true),
      bucketCount,
      channels,
      byteLength: view.byteLength
    };
  }

  class HttpSignalDataSource {
    constructor(options) {
      this.apiBase = String(options.apiBase || '').replace(/\/$/, '');
      this.fetch = options.fetchImpl || global.fetch.bind(global);
      this.describePath = options.describePath || ((sourceId) =>
        `/v1/jobs/${encodeURIComponent(sourceId)}/viewer`);
      this.windowPath = options.windowPath || ((sourceId) =>
        `/v1/jobs/${encodeURIComponent(sourceId)}/viewer/window`);
      this.overlayPath = options.overlayPath || ((sourceId) =>
        `/v1/jobs/${encodeURIComponent(sourceId)}/viewer/overlays`);
    }

    async request(path, options) {
      const response = await this.fetch(this.apiBase + path, {
        credentials: 'same-origin',
        cache: 'no-store',
        ...options
      });
      if (!response.ok) {
        let message = response.statusText || `HTTP ${response.status}`;
        try {
          const body = await response.json();
          if (body && body.error && body.error.message) message = body.error.message;
        } catch (_) {}
        const error = new Error(message);
        error.status = response.status;
        throw error;
      }
      return response;
    }

    async describe(jobId, signal) {
      const response = await this.request(this.describePath(jobId), { signal });
      return response.json();
    }

    async readWindow(jobId, request, signal) {
      const query = new URLSearchParams({
        case_id: request.caseId,
        start_sample: String(Math.trunc(request.startSample)),
        sample_count: String(Math.trunc(request.sampleCount)),
        points: String(Math.trunc(request.points)),
        channels: request.channels.join(',')
      });
      const response = await this.request(
        `${this.windowPath(jobId)}?${query}`, { signal }
      );
      return decodeSignalWindow(await response.arrayBuffer());
    }

    async readOverlays(jobId, request, signal) {
      const query = new URLSearchParams({
        case_id: request.caseId,
        start_sample: String(Math.trunc(request.startSample)),
        sample_count: String(Math.trunc(request.sampleCount)),
        max_items: String(Math.trunc(request.maxItems || 4000))
      });
      const response = await this.request(
        `${this.overlayPath(jobId)}?${query}`, { signal }
      );
      return response.json();
    }
  }

  class BoundedLruCache {
    constructor(maxBytes) {
      this.maxBytes = Math.max(1024, Number(maxBytes) || 32 * 1024 * 1024);
      this.entries = new Map();
      this.bytes = 0;
    }

    get(key) {
      const entry = this.entries.get(key);
      if (!entry) return null;
      this.entries.delete(key);
      this.entries.set(key, entry);
      return entry.value;
    }

    set(key, value, byteLength) {
      const bytes = Math.max(0, Number(byteLength) || 0);
      const previous = this.entries.get(key);
      if (previous) {
        this.bytes -= previous.bytes;
        this.entries.delete(key);
      }
      if (bytes > this.maxBytes) return false;
      this.entries.set(key, { value, bytes });
      this.bytes += bytes;
      while (this.bytes > this.maxBytes && this.entries.size) {
        const oldestKey = this.entries.keys().next().value;
        const oldest = this.entries.get(oldestKey);
        this.entries.delete(oldestKey);
        this.bytes -= oldest.bytes;
      }
      return true;
    }

    deleteWhere(predicate) {
      Array.from(this.entries.entries()).forEach(([key, entry]) => {
        if (!predicate(key, entry.value)) return;
        this.entries.delete(key);
        this.bytes -= entry.bytes;
      });
    }

    values() {
      return Array.from(this.entries.values()).map((entry) => entry.value);
    }

    summary() {
      return { bytes: this.bytes, maxBytes: this.maxBytes, entries: this.entries.size };
    }
  }

  class SignalWindowCache {
    constructor(maxBytes) {
      this.cache = new BoundedLruCache(maxBytes);
    }

    sourceKey(sourceId, caseId, channels) {
      return `${sourceId}|${caseId}|${channels.join(',')}`;
    }

    add(sourceId, caseId, windowData) {
      const channels = windowData.channels.map((channel) => channel.index);
      const key = `${this.sourceKey(sourceId, caseId, channels)}|${windowData.requestedStart}|${windowData.requestedCount}|${windowData.samplesPerBucket}`;
      this.cache.set(key, { sourceId, caseId, channels, windowData }, windowData.byteLength);
    }

    find(sourceId, caseId, channels, startSample, sampleCount, maximumBucketSize) {
      const endSample = startSample + sampleCount;
      let best = null;
      let bestKey = '';
      this.cache.entries.forEach((entry, key) => {
        const candidate = entry.value;
        const windowData = candidate.windowData;
        const windowEnd = windowData.requestedStart + windowData.requestedCount;
        if (candidate.sourceId !== sourceId || candidate.caseId !== caseId ||
            candidate.channels.length !== channels.length ||
            !candidate.channels.every((value, index) => value === channels[index]) ||
            windowData.requestedStart > startSample || windowEnd < endSample ||
            windowData.samplesPerBucket > maximumBucketSize) return;
        if (!best || windowData.samplesPerBucket > best.samplesPerBucket ||
            (windowData.samplesPerBucket === best.samplesPerBucket &&
             windowData.requestedCount < best.requestedCount)) {
          best = windowData;
          bestKey = key;
        }
      });
      if (bestKey) this.cache.get(bestKey);
      return best;
    }

    summary() {
      return this.cache.summary();
    }
  }

  function niceDuration(seconds, precision) {
    if (!Number.isFinite(seconds)) return '—';
    const digits = precision === undefined
      ? (Math.abs(seconds) < 10 ? 3 : Math.abs(seconds) < 100 ? 2 : 1)
      : precision;
    if (Math.abs(seconds) < 60) return `${seconds.toFixed(digits)} s`;
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const remaining = seconds % 60;
    return hours
      ? `${hours}h ${String(minutes).padStart(2, '0')}m ${remaining.toFixed(1)}s`
      : `${minutes}m ${remaining.toFixed(1)}s`;
  }

  class SignalCanvasRenderer {
    constructor(canvas, options) {
      this.canvas = canvas;
      this.context = canvas.getContext('2d', { alpha: false });
      this.layout = 'stacked';
      this.amplitudeScale = 1;
      this.channelSpacing = 1;
      this.caseMetadata = null;
      this.window = null;
      this.overlayWindow = null;
      this.localEvents = [];
      this.enabledOverlayKinds = new Set();
      this.viewportStart = 0;
      this.viewportCount = 1;
      this.onResize = options && options.onResize;
      this.resizeQueued = false;
      this.resizeObserver = 'ResizeObserver' in global
        ? new ResizeObserver(() => this.queueResize())
        : null;
      if (this.resizeObserver) this.resizeObserver.observe(canvas);
      else global.addEventListener('resize', () => this.queueResize());
      this.resize();
    }

    queueResize() {
      if (this.resizeQueued) return;
      this.resizeQueued = true;
      global.requestAnimationFrame(() => {
        this.resizeQueued = false;
        const changed = this.resize();
        if (changed && this.onResize) this.onResize();
      });
    }

    resize() {
      const rect = this.canvas.getBoundingClientRect();
      const ratio = Math.min(global.devicePixelRatio || 1, 2.5);
      const width = Math.max(320, Math.floor(rect.width * ratio));
      const height = Math.max(320, Math.floor(rect.height * ratio));
      const changed = this.canvas.width !== width || this.canvas.height !== height;
      if (changed) {
        this.canvas.width = width;
        this.canvas.height = height;
        this.draw();
      }
      return changed;
    }

    targetPoints() {
      return Math.max(256, Math.min(4096, Math.ceil(this.canvas.width * 1.15)));
    }

    setCase(metadata) {
      this.caseMetadata = metadata;
      this.window = null;
      this.overlayWindow = null;
      this.localEvents = [];
      this.viewportStart = 0;
      this.viewportCount = metadata ? metadata.sample_count : 1;
      this.draw();
    }

    setWindow(windowData) {
      this.window = windowData;
      this.draw();
    }

    setOverlays(overlayWindow) {
      this.overlayWindow = overlayWindow;
      this.draw();
    }

    setLocalEvents(events) {
      this.localEvents = Array.isArray(events) ? events : [];
      this.draw();
    }

    setEnabledOverlayKinds(kinds) {
      this.enabledOverlayKinds = new Set(kinds || []);
      this.draw();
    }

    setViewport(startSample, sampleCount) {
      if (!Number.isFinite(startSample) || !Number.isFinite(sampleCount) || sampleCount <= 0) return;
      this.viewportStart = startSample;
      this.viewportCount = sampleCount;
      this.draw();
    }

    setLayout(layout) {
      this.layout = layout === 'overlay' ? 'overlay' : 'stacked';
      this.draw();
    }

    setAmplitudeScale(scale) {
      this.amplitudeScale = Math.max(0.125, Math.min(32, scale));
      this.draw();
    }

    setChannelSpacing(scale) {
      this.channelSpacing = Math.max(4 / 9, Math.min(9 / 4, scale));
      this.draw();
    }

    plotRect() {
      const ratio = Math.min(global.devicePixelRatio || 1, 2.5);
      const width = this.canvas.width / ratio;
      const height = this.canvas.height / ratio;
      return {
        left: 92,
        top: 18,
        width: Math.max(100, width - 132),
        height: Math.max(100, height - 58)
      };
    }

    sampleAtClientX(clientX) {
      const bounds = this.canvas.getBoundingClientRect();
      const plot = this.plotRect();
      const x = (clientX - bounds.left) / bounds.width * (this.canvas.width /
        Math.min(global.devicePixelRatio || 1, 2.5));
      const ratio = Math.max(0, Math.min(1, (x - plot.left) / plot.width));
      return Math.max(0, Math.min(
        this.caseMetadata ? this.caseMetadata.sample_count - 1 : Number.MAX_SAFE_INTEGER,
        Math.round(this.viewportStart + ratio * this.viewportCount)
      ));
    }

    inspectSample(sample) {
      if (!this.window || !this.caseMetadata) return null;
      const result = {
        sample,
        timeSeconds: sample / this.caseMetadata.sample_rate_hz,
        channels: [],
        annotations: []
      };
      const bucket = Math.floor(
        (sample - this.window.dataStart) / this.window.samplesPerBucket);
      const metadata = new Map(
        this.caseMetadata.channels.map((channel) => [channel.index, channel]));
      if (bucket >= 0 && bucket < this.window.bucketCount) {
        this.window.channels.forEach((values) => {
          const channel = metadata.get(values.index);
          if (!channel) return;
          result.channels.push({
            name: channel.name,
            unit: channel.unit,
            minimum: this.physical(channel, values.minimum[bucket]),
            maximum: this.physical(channel, values.maximum[bucket]),
            exact: this.window.samplesPerBucket === 1
          });
        });
      }
      const tolerance = Math.max(1, Math.ceil(this.viewportCount / 180));
      const overlays = this.overlayWindow && Array.isArray(this.overlayWindow.items)
        ? this.overlayWindow.items : [];
      overlays.forEach((item) => {
        if (!this.enabledOverlayKinds.has(item.kind)) return;
        const hit = item.interval
          ? sample >= item.start_sample && sample < item.end_sample
          : sample >= item.start_sample - tolerance && sample <= item.end_sample + tolerance;
        if (hit) result.annotations.push(item);
      });
      this.localEvents.forEach((item) => {
        if (Math.abs(sample - item.sample) <= tolerance) {
          result.annotations.push({ ...item, kind: 'local_detection', source: 'local file' });
        }
      });
      return result;
    }

    physical(channel, digital) {
      return (digital - channel.adc_zero) / channel.gain;
    }

    visibleBucketRange(values) {
      const bucketSize = this.window.samplesPerBucket;
      const first = Math.max(
        0,
        Math.floor((this.viewportStart - this.window.dataStart) / bucketSize) - 1
      );
      const past = Math.min(
        values.minimum.length,
        Math.ceil(
          (this.viewportStart + this.viewportCount - this.window.dataStart) /
          bucketSize
        ) + 1
      );
      return { first, past: Math.max(first, past) };
    }

    channelRange(channel, values) {
      let maximum = 0;
      const visible = this.visibleBucketRange(values);
      for (let bucket = visible.first; bucket < visible.past; bucket += 1) {
        maximum = Math.max(
          maximum,
          Math.abs(this.physical(channel, values.minimum[bucket])),
          Math.abs(this.physical(channel, values.maximum[bucket]))
        );
      }
      return maximum > 0 ? maximum : 1 / channel.gain;
    }

    drawGrid(ctx, plot, windowData) {
      ctx.save();
      ctx.strokeStyle = 'rgba(173, 193, 224, 0.105)';
      ctx.fillStyle = '#8291aa';
      ctx.lineWidth = 1;
      ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      const divisions = Math.max(4, Math.min(10, Math.floor(plot.width / 110)));
      for (let division = 0; division <= divisions; division += 1) {
        const ratio = division / divisions;
        const x = plot.left + plot.width * ratio;
        ctx.beginPath();
        ctx.moveTo(x, plot.top);
        ctx.lineTo(x, plot.top + plot.height);
        ctx.stroke();
        const sample = this.viewportStart + this.viewportCount * ratio;
        ctx.fillText(niceDuration(sample / windowData.sampleRateHz), x, plot.top + plot.height + 8);
      }
      ctx.restore();
    }

    drawChannel(ctx, values, channel, color, plot, centerY, halfHeight, overlay) {
      const windowData = this.window;
      const range = this.channelRange(channel, values);
      const visible = this.visibleBucketRange(values);
      const pixelsPerUnit = halfHeight * 0.82 / range * this.amplitudeScale;
      const xFor = (bucket) => {
        const sample = windowData.dataStart +
          (bucket + 0.5) * windowData.samplesPerBucket;
        return plot.left +
          (sample - this.viewportStart) /
          this.viewportCount * plot.width;
      };
      const yFor = (digital) => centerY -
        this.physical(channel, digital) * pixelsPerUnit;

      ctx.save();
      ctx.beginPath();
      ctx.rect(plot.left, plot.top, plot.width, plot.height);
      ctx.clip();
      ctx.strokeStyle = overlay ? 'rgba(170, 187, 213, 0.08)' : 'rgba(170, 187, 213, 0.13)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(plot.left, centerY);
      ctx.lineTo(plot.left + plot.width, centerY);
      ctx.stroke();

      ctx.strokeStyle = color;
      ctx.globalAlpha = overlay ? 0.84 : 0.96;
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      if (windowData.samplesPerBucket === 1) {
        ctx.lineWidth = 1.3;
        ctx.beginPath();
        for (let bucket = visible.first; bucket < visible.past; bucket += 1) {
          const value = (values.minimum[bucket] + values.maximum[bucket]) / 2;
          const x = xFor(bucket);
          const y = yFor(value);
          if (bucket === visible.first) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        ctx.stroke();
      } else {
        const upperY = (bucket) => yFor(values.maximum[bucket]);
        const lowerY = (bucket) => yFor(values.minimum[bucket]);
        ctx.fillStyle = color;
        ctx.globalAlpha = overlay ? 0.08 : 0.12;
        ctx.beginPath();
        for (let bucket = visible.first; bucket < visible.past; bucket += 1) {
          const x = xFor(bucket);
          const y = upperY(bucket);
          if (bucket === visible.first) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        for (let bucket = visible.past - 1; bucket >= visible.first; bucket -= 1) {
          ctx.lineTo(xFor(bucket), lowerY(bucket));
        }
        ctx.closePath();
        ctx.fill();

        ctx.strokeStyle = color;
        ctx.globalAlpha = overlay ? 0.68 : 0.9;
        ctx.lineWidth = 1.15;
        [upperY, lowerY].forEach((edgeY) => {
          ctx.beginPath();
          for (let bucket = visible.first; bucket < visible.past; bucket += 1) {
            const x = xFor(bucket);
            const y = edgeY(bucket);
            if (bucket === visible.first) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
          }
          ctx.stroke();
        });
      }
      ctx.restore();
      return range;
    }

    xForSample(sample, plot) {
      return plot.left + (sample - this.viewportStart) /
        this.viewportCount * plot.width;
    }

    visibleOverlayItems(interval) {
      if (!this.overlayWindow || !Array.isArray(this.overlayWindow.items)) return [];
      return this.overlayWindow.items.filter((item) =>
        Boolean(item.interval) === interval &&
        this.enabledOverlayKinds.has(item.kind) &&
        item.end_sample >= this.viewportStart &&
        item.start_sample <= this.viewportStart + this.viewportCount
      );
    }

    drawIntervalOverlays(ctx, plot) {
      const styles = {
        artifact: { fill: 'rgba(255, 183, 77, 0.13)', stroke: '#ffbf69' },
        episode: { fill: 'rgba(177, 132, 255, 0.12)', stroke: '#b991ff' },
        missing_pulse: { fill: 'rgba(255, 109, 145, 0.10)', stroke: '#ff759a' }
      };
      ctx.save();
      ctx.beginPath();
      ctx.rect(plot.left, plot.top, plot.width, plot.height);
      ctx.clip();
      this.visibleOverlayItems(true).forEach((item, index) => {
        const style = styles[item.kind] || styles.artifact;
        const left = this.xForSample(
          Math.max(this.viewportStart, item.start_sample), plot);
        const right = this.xForSample(
          Math.min(this.viewportStart + this.viewportCount, item.end_sample), plot);
        ctx.fillStyle = style.fill;
        ctx.fillRect(left, plot.top, Math.max(2, right - left), plot.height);
        ctx.strokeStyle = style.stroke;
        ctx.globalAlpha = 0.72;
        ctx.setLineDash(item.kind === 'episode' ? [8, 5] : [3, 4]);
        ctx.strokeRect(left, plot.top + 1, Math.max(2, right - left), plot.height - 2);
        if (right - left > 54 && index < 20) {
          ctx.setLineDash([]);
          ctx.globalAlpha = 0.9;
          ctx.fillStyle = style.stroke;
          ctx.font = '10px ui-sans-serif, system-ui, sans-serif';
          ctx.textAlign = 'left';
          ctx.textBaseline = 'top';
          ctx.fillText(item.label, left + 5, plot.top + 5);
        }
      });
      ctx.restore();
    }

    drawMarkerShape(ctx, kind, x, y, color, local) {
      ctx.save();
      ctx.strokeStyle = color;
      ctx.fillStyle = local ? '#060e1a' : color;
      ctx.lineWidth = local ? 2 : 1.4;
      ctx.beginPath();
      if (kind === 'r_peak') {
        ctx.moveTo(x, y - 6); ctx.lineTo(x - 5, y + 3); ctx.lineTo(x + 5, y + 3); ctx.closePath();
      } else if (kind === 'ppg_onset') {
        ctx.moveTo(x, y - 5); ctx.lineTo(x - 5, y); ctx.lineTo(x, y + 5); ctx.lineTo(x + 5, y); ctx.closePath();
      } else if (kind === 'ppg_peak') {
        ctx.arc(x, y, 4.5, 0, Math.PI * 2);
      } else if (kind === 'low_perfusion') {
        ctx.moveTo(x - 5, y - 5); ctx.lineTo(x + 5, y + 5);
        ctx.moveTo(x + 5, y - 5); ctx.lineTo(x - 5, y + 5);
      } else {
        ctx.rect(x - 4.5, y - 4.5, 9, 9);
      }
      ctx.fill();
      ctx.stroke();
      ctx.restore();
    }

    drawEventOverlays(ctx, plot, visibleChannels) {
      const styles = {
        r_peak: '#4ee4ff',
        beat_class: '#d4a4ff',
        ecg_fiducial: '#ff9fd5',
        ppg_onset: '#64e6ad',
        ppg_peak: '#ffd16f',
        low_perfusion: '#ff9e62'
      };
      const items = this.visibleOverlayItems(false).map((item) => ({ ...item, local: false }));
      this.localEvents.forEach((item) => {
        if (item.sample >= this.viewportStart &&
            item.sample <= this.viewportStart + this.viewportCount) {
          items.push({
            ...item,
            kind: item.target === 'ppg_systolic_peak' ? 'ppg_peak' :
              item.target === 'ppg_pulse_onset' ? 'ppg_onset' :
              item.target === 'ecg_beat_classification' ? 'beat_class' : 'r_peak',
            start_sample: item.sample,
            end_sample: item.sample,
            source: 'local file',
            local: true,
            count: 1
          });
        }
      });
      ctx.save();
      ctx.beginPath();
      ctx.rect(plot.left, plot.top, plot.width, plot.height);
      ctx.clip();
      items.forEach((item, index) => {
        const color = item.local ? '#ff6f91' : (styles[item.kind] || '#d5deef');
        const sample = item.count > 1
          ? (item.start_sample + item.end_sample) / 2
          : item.start_sample;
        const x = this.xForSample(sample, plot);
        const laneIndex = this.layout === 'stacked' && item.kind === 'ecg_fiducial'
          ? visibleChannels.findIndex((entry) => entry.channel.name === item.channel)
          : -1;
        const laneHeight = laneIndex >= 0 ? plot.height / visibleChannels.length : plot.height;
        const lineTop = laneIndex >= 0 ? plot.top + laneIndex * laneHeight : plot.top + 14;
        const lineBottom = laneIndex >= 0 ? lineTop + laneHeight : plot.top + plot.height;
        const markerY = laneIndex >= 0 ? lineTop + laneHeight / 2 : plot.top + 11;
        ctx.strokeStyle = color;
        ctx.globalAlpha = item.local ? 0.92 : 0.56;
        ctx.lineWidth = item.local ? 1.5 : 1;
        ctx.setLineDash(item.local ? [6, 4] :
          item.source === 'construction' ? [2, 4] : []);
        ctx.beginPath();
        ctx.moveTo(x, lineTop);
        ctx.lineTo(x, lineBottom);
        ctx.stroke();
        ctx.setLineDash([]);
        this.drawMarkerShape(ctx, item.kind, x, markerY, color, item.local);
        const meaningfulClass = item.kind === 'beat_class' && item.label &&
          item.label !== 'normal';
        if ((item.count > 1 || item.local || meaningfulClass) && index < 120) {
          const label = item.count > 1 ? `${item.label} ×${item.count}` :
            (item.label || (item.local ? 'local' : item.kind));
          ctx.fillStyle = color;
          ctx.globalAlpha = 0.96;
          ctx.font = '10px ui-sans-serif, system-ui, sans-serif';
          ctx.textAlign = 'left';
          ctx.textBaseline = 'top';
          ctx.fillText(label, x + 6, markerY + 7 + (index % 3) * 12);
        }
      });
      ctx.restore();
    }

    draw() {
      const ctx = this.context;
      if (!ctx) return;
      const ratio = Math.min(global.devicePixelRatio || 1, 2.5);
      const width = this.canvas.width / ratio;
      const height = this.canvas.height / ratio;
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      ctx.fillStyle = '#060e1a';
      ctx.fillRect(0, 0, width, height);
      if (!this.window || !this.caseMetadata) return;

      const plot = this.plotRect();
      this.drawGrid(ctx, plot, this.window);
      this.drawIntervalOverlays(ctx, plot);
      const metadataByIndex = new Map(this.caseMetadata.channels.map((channel) => [channel.index, channel]));
      const visible = this.window.channels
        .map((values) => ({ values, channel: metadataByIndex.get(values.index) }))
        .filter((item) => item.channel);
      if (!visible.length) return;

      ctx.font = '11px ui-sans-serif, system-ui, sans-serif';
      ctx.textBaseline = 'middle';
      if (this.layout === 'overlay') {
        const center = plot.top + plot.height / 2;
        visible.forEach((item, index) => {
          const color = COLORS[item.channel.index % COLORS.length];
          this.drawChannel(ctx, item.values, item.channel, color, plot, center, plot.height * 0.47, true);
          const legendX = plot.left + 8 + (index % 4) * Math.max(110, plot.width / 4);
          const legendY = plot.top + 12 + Math.floor(index / 4) * 18;
          ctx.fillStyle = color;
          ctx.fillRect(legendX, legendY - 4, 9, 9);
          ctx.fillStyle = '#c7d2e5';
          ctx.fillText(`${item.channel.name} (${item.channel.unit})`, legendX + 14, legendY);
        });
      } else {
        const laneHeight = plot.height / visible.length;
        visible.forEach((item, index) => {
          const center = plot.top + laneHeight * (index + 0.5);
          const color = COLORS[item.channel.index % COLORS.length];
          const range = this.drawChannel(
            ctx, item.values, item.channel, color, plot, center,
            laneHeight / this.channelSpacing * 0.46, false
          );
          ctx.fillStyle = color;
          ctx.textAlign = 'right';
          ctx.fillText(item.channel.name, plot.left - 10, center - 7);
          ctx.fillStyle = '#718096';
          ctx.font = '9px ui-monospace, SFMono-Regular, Menlo, monospace';
          ctx.fillText(`±${range.toPrecision(3)} ${item.channel.unit}`, plot.left - 10, center + 8);
          ctx.font = '11px ui-sans-serif, system-ui, sans-serif';
        });
      }
      this.drawEventOverlays(ctx, plot, visible);
    }
  }

  global.SynsigraSignalViewer = Object.freeze({
    COLORS,
    HttpSignalDataSource,
    BoundedLruCache,
    SignalWindowCache,
    SignalCanvasRenderer,
    decodeSignalWindow,
    niceDuration
  });
})(window);
