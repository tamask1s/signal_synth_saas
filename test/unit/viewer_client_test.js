'use strict';

const assert = require('assert');
global.window = global;
global.fetch = () => Promise.reject(new Error('network must not be used'));
require('../../web/viewer/signal-viewer.js');

const library = global.SynsigraSignalViewer;
assert(library, 'viewer library should be exported');

global.addEventListener = () => {};
const canvasContext = {
  setTransform() {},
  fillRect() {}
};
const canvas = {
  width: 0,
  height: 0,
  getContext: () => canvasContext,
  getBoundingClientRect: () => ({ width: 800, height: 400 })
};
const renderer = new library.SignalCanvasRenderer(canvas);
assert.strictEqual(renderer.channelSpacing, 1, 'stacked channels should fill the viewer by default');
renderer.setChannelSpacing(0.25);
assert.strictEqual(renderer.channelSpacing, 0.5, 'channel spacing should retain a readable lower bound');
renderer.setChannelSpacing(1.5);
assert.strictEqual(renderer.channelSpacing, 1, 'channel spacing must not grow beyond the fitted layout');

const bounded = new library.BoundedLruCache(1024);
assert.strictEqual(bounded.set('first', { id: 1 }, 700), true);
assert.strictEqual(bounded.set('second', { id: 2 }, 500), true);
assert.strictEqual(bounded.get('first'), null, 'least-recently-used payload should be evicted');
assert.strictEqual(bounded.get('second').id, 2);
assert.strictEqual(bounded.summary().bytes, 500);

function signalWindow(start, count, bucket, bytes) {
  return {
    requestedStart: start,
    requestedCount: count,
    samplesPerBucket: bucket,
    byteLength: bytes,
    channels: [{ index: 0, minimum: new Int16Array(1), maximum: new Int16Array(1) }]
  };
}

const signals = new library.SignalWindowCache(4096);
signals.add('job_a', 'case_a', signalWindow(0, 3000, 4, 800));
signals.add('job_a', 'case_a', signalWindow(3000, 3000, 1, 900));
assert.strictEqual(
  signals.find('job_a', 'case_a', [0], 500, 1000, 4).requestedStart,
  0,
  'a covered viewport should be served by the client cache'
);
assert.strictEqual(
  signals.find('job_a', 'case_a', [0], 500, 1000, 1),
  null,
  'a higher-resolution viewport must not reuse coarse cached buckets'
);
assert.strictEqual(
  signals.find('job_a', 'case_a', [0], 3500, 500, 1).requestedStart,
  3000,
  'multiple retained viewport segments should remain independently reusable'
);
assert.strictEqual(
  signals.find('job_b', 'case_a', [0], 500, 1000, 4),
  null,
  'cache entries must stay scoped to their source job'
);

console.log('viewer client cache tests passed');
