const { test } = require('node:test');
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const vm = require('node:vm');

const generator = process.env.HERMES_PLACEMENT_SCRIPT_GENERATOR;
assert.ok(generator, 'Set HERMES_PLACEMENT_SCRIPT_GENERATOR to the compiled script.cc');

function script(mode = 'gamescope', output = 'Virtual-1') {
  return execFileSync(generator, [mode, output], { encoding: 'utf8' });
}

function desktop(existing, mode = 'gamescope', outputExists = true) {
  const physical = { name: 'HDMI-A-1' };
  const target = { name: 'Virtual-1' };
  const moves = [];
  let onAdded;
  const workspace = {
    screens: outputExists ? [physical, target] : [physical],
    windowList: () => existing,
    windowAdded: { connect: fn => { onAdded = fn; } },
    sendClientToScreen: (window, screen) => {
      moves.push(window);
      window.output = screen;
    },
  };
  vm.runInNewContext(script(mode), { workspace });
  return { moves, target, add: window => onAdded(window) };
}

function window(pid, resourceClass = 'gamescope', fullScreen = true) {
  return { pid, resourceClass, fullScreen, normalWindow: true, output: { name: 'HDMI-A-1' } };
}

test('moves only launched Gamescope PIDs, including windows already created', () => {
  const local = window(900);
  const streamed = window(42);
  const { moves, target, add } = desktop([local, streamed]);
  assert.deepEqual(moves, [streamed]);
  assert.equal(streamed.output, target);
  assert.equal(streamed.fullScreen, true);
  assert.equal(local.output.name, 'HDMI-A-1');
  const futureLocal = window(901);
  add(futureLocal);
  assert.equal(futureLocal.output.name, 'HDMI-A-1');
  const detached = window(43, 'gamescope', false);
  add(detached);
  assert.deepEqual(moves, [streamed, detached]);
  assert.equal(detached.fullScreen, false);
  add(streamed);
  assert.equal(moves.length, 2, 'do not move or toggle a window already on the target');
});

test('no launched PID means no class-wide Gamescope fallback', () => {
  for (const mode of ['no-pids', 'invalid-pids']) {
    const windows = [window(0), window(1), window(42), window(900)];
    const { moves, add } = desktop(windows, mode);
    add(window(42));
    assert.equal(moves.length, 0);
  }
});

test('Steam can reuse an existing process and keeps its fullscreen state', () => {
  const steam = window(900, 'steam', false);
  const unrelated = window(42);
  const { moves, add } = desktop([steam, unrelated], 'steam');
  assert.deepEqual(moves, [steam]);
  assert.equal(steam.fullScreen, false);
  const bigPicture = window(901, 'steam', true);
  add(bigPicture);
  assert.deepEqual(moves, [steam, bigPicture]);
  assert.equal(bigPicture.fullScreen, true);
});

test('Big Picture that moves itself back and goes fullscreen is placed again', () => {
  // Recorded on KDE Plasma 6.8 with a 2560x1440 primary monitor lit: Steam
  // adds "Steam Big Picture Mode" on the primary monitor, the script moves it
  // to Virtual-1, Steam moves it straight back and makes it fullscreen there.
  const handlers = [];
  let fullScreen = false;
  const bigPicture = {
    pid: 4538,
    resourceClass: 'steam',
    normalWindow: true,
    output: { name: 'HDMI-A-1' },
    fullScreenChanged: { connect: fn => handlers.push(fn) },
    get fullScreen() { return fullScreen; },
    set fullScreen(value) {
      if (value !== fullScreen) {
        fullScreen = value;
        handlers.forEach(fn => fn());
      }
    },
  };
  const { moves, target, add } = desktop([], 'steam');
  add(bigPicture);
  assert.deepEqual(moves, [bigPicture]);
  assert.equal(bigPicture.output, target);
  assert.equal(bigPicture.fullScreen, false, 'a window that was not fullscreen stays that way');

  bigPicture.output = { name: 'HDMI-A-1' };  // Steam moves it back...
  bigPicture.fullScreen = true;              // ...and goes fullscreen there.
  assert.deepEqual(moves, [bigPicture, bigPicture]);
  assert.equal(bigPicture.output, target);
  assert.equal(bigPicture.fullScreen, true);

  bigPicture.fullScreen = false;  // Leaving fullscreen on the target is left alone.
  bigPicture.fullScreen = true;
  assert.equal(moves.length, 2, 'already on the target: no further move');
});

test('missing outputs and non-normal windows are left alone', () => {
  const hidden = window(42);
  hidden.normalWindow = false;
  assert.equal(desktop([hidden]).moves.length, 0);
  assert.equal(desktop([window(42)], 'gamescope', false).moves.length, 0);
});

test('rejects an output name that could inject JavaScript', () => {
  assert.equal(script('gamescope', 'Virtual-1";throw Error("injected")//'), '');
});
