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
      this.caseMetadata = null;
      this.window = null;
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
      this.draw();
    }

    setWindow(windowData) {
      this.window = windowData;
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

    physical(channel, digital) {
      return (digital - channel.adc_zero) / channel.gain;
    }

    channelRange(channel, values) {
      let maximum = 0;
      for (let bucket = 0; bucket < values.minimum.length; bucket += 1) {
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
        const sample = windowData.requestedStart + windowData.requestedCount * ratio;
        ctx.fillText(niceDuration(sample / windowData.sampleRateHz), x, plot.top + plot.height + 8);
      }
      ctx.restore();
    }

    drawChannel(ctx, values, channel, color, plot, centerY, halfHeight, overlay) {
      const windowData = this.window;
      const range = this.channelRange(channel, values);
      const pixelsPerUnit = halfHeight * 0.82 / range * this.amplitudeScale;
      const xFor = (bucket) => {
        const sample = windowData.dataStart +
          (bucket + 0.5) * windowData.samplesPerBucket;
        return plot.left +
          (sample - windowData.requestedStart) /
          windowData.requestedCount * plot.width;
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
      if (windowData.samplesPerBucket === 1) {
        ctx.lineWidth = 1.3;
        ctx.beginPath();
        for (let bucket = 0; bucket < windowData.bucketCount; bucket += 1) {
          const value = (values.minimum[bucket] + values.maximum[bucket]) / 2;
          const x = xFor(bucket);
          const y = Math.max(centerY - halfHeight, Math.min(centerY + halfHeight, yFor(value)));
          if (bucket === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        ctx.stroke();
      } else {
        ctx.lineWidth = Math.max(1, Math.min(2, plot.width / windowData.bucketCount * 0.7));
        ctx.beginPath();
        for (let bucket = 0; bucket < windowData.bucketCount; bucket += 1) {
          const x = xFor(bucket);
          const low = Math.max(centerY - halfHeight, Math.min(centerY + halfHeight, yFor(values.maximum[bucket])));
          const high = Math.max(centerY - halfHeight, Math.min(centerY + halfHeight, yFor(values.minimum[bucket])));
          ctx.moveTo(x, low);
          ctx.lineTo(x, high);
        }
        ctx.stroke();
      }
      ctx.restore();
      return range;
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

      const plot = { left: 92, top: 18, width: Math.max(100, width - 132), height: Math.max(100, height - 58) };
      this.drawGrid(ctx, plot, this.window);
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
            ctx, item.values, item.channel, color, plot, center, laneHeight * 0.46, false
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
    }
  }

  global.SynsigraSignalViewer = Object.freeze({
    COLORS,
    HttpSignalDataSource,
    SignalCanvasRenderer,
    decodeSignalWindow,
    niceDuration
  });
})(window);
