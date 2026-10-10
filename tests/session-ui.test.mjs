import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import vm from 'node:vm';

// Run the actual UI source unchanged. No browser/CDP/application is touched.
const source = await fs.readFile(new URL('../session-ui.js', import.meta.url), 'utf8');
const A = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
const B = 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb';
class Element {
  constructor(tag = 'div') {
    this.tagName = tag.toUpperCase(); this.children = []; this.dataset = {};
    this.attributes = new Map(); this.listeners = new Map(); this.textContent = '';
    this.className = ''; this.hidden = false; this.isConnected = false;
    const styles = new Map();
    this.style = { display: '',
      setProperty: (key, value, priority = '') => styles.set(key, { value, priority }),
      getPropertyValue: key => styles.get(key)?.value || '',
      getPropertyPriority: key => styles.get(key)?.priority || '',
      removeProperty: key => styles.delete(key),
    };
    Object.defineProperty(this.style, 'cssText', { set: value => {
      const display = value.match(/(?:^|;)display:([^;]+)/)?.[1];
      if (display) this.style.display = display;
    } });
    const classes = new Set();
    this.classList = {
      contains: key => classes.has(key) || this.className.split(/\s+/).includes(key),
      toggle: (key, force) => { const on = force ?? !classes.has(key); on ? classes.add(key) : classes.delete(key); return on; },
    };
  }
  append(...nodes) { this.children.push(...nodes); for (const node of nodes) { node.parentElement = this; node.isConnected = true; } }
  setAttribute(key, value) { this.attributes.set(key, String(value)); }
  getAttribute(key) { return this.attributes.get(key) ?? null; }
  addEventListener(key, fn) { this.listeners.set(key, fn); }
  removeEventListener(key) { this.listeners.delete(key); }
  getBoundingClientRect() { return { left: 100, top: 300, right: 700, bottom: 420, width: 600, height: 120 }; }
  closest() { return null; }
  remove() { this.isConnected = false; }
  attachShadow() { return this.shadowRoot = new Element('shadow-root'); }
  set innerHTML(html) {
    this.html = html;
    for (const match of html.matchAll(/<([a-z]+)\b[^>]*\bclass="([^"]+)"([^>]*)>/g)) {
      const node = new Element(match[1]); node.className = match[2];
      node.hidden = /\bhidden\b/.test(match[3]); this.append(node);
    }
  }
  querySelectorAll(selector) {
    const result = [];
    const visit = node => { for (const child of node.children) {
      if (selector.startsWith('.') && child.classList.contains(selector.slice(1))) result.push(child);
      visit(child);
    } }; visit(this); return result;
  }
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
}

function boot({ selected = null, percent = 34, usedTokens = 68000, contextWindow = 200000, hasMeter = true } = {}) {
  const body = new Element('body'), root = new Element('html');
  const scope = new Element(), composer = new Element(), field = new Element('textarea'), marker = new Element(), meter = new Element('span');
  marker.setAttribute('data-above-composer-conversation-id', A);
  composer.closest = selector => selector === '[data-app-shell-active-page="true"]' ? scope : null;
  let meters = hasMeter ? [meter] : [], ring = new Element('circle');
  meter.querySelector = selector => selector === 'circle[stroke-dasharray][stroke-dashoffset]' ? ring : null;
  composer.querySelectorAll = selector => selector === 'textarea,[contenteditable="true"]' ? [field]
    : selector.includes('[role="img"]') ? meters : [];
  scope.querySelectorAll = selector => selector === '[data-above-composer-conversation-id]' ? [marker] : [];
  const setUsage = usage => {
    meter.setAttribute('role', 'img'); meter.setAttribute('aria-label', 'Context usage ' + Math.round(usage.percent) + '%');
    if (ring) { ring.setAttribute('stroke-dasharray', '100'); ring.setAttribute('stroke-dashoffset', String(100 - usage.percent)); }
    meter.__reactFiber$fixture = { memoizedProps: {}, return: { memoizedProps: { contextUsage: usage } } };
  };
  setUsage({ percent, usedTokens, contextWindow });
  const document = new Element('document'); document.body = body; document.documentElement = root; document.hidden = false;
  document.createElement = tag => new Element(tag);
  document.querySelectorAll = selector => selector.includes('.composer-surface-chrome') ? [composer] : [];
  const window = new Element('window'); window.innerWidth = 1400;
  const requests = [], timers = new Map(), mutationObservers = [], resizeObservers = [];
  const storage = new Map(selected ? [['codex-session-only-metrics-v1', JSON.stringify(selected)]] : []);
  window.__codexSessionMonitorRequest = payload => requests.push(JSON.parse(payload));
  let clock = 1000000, sequence = 0;
  const observerClass = list => class {
    constructor(callback) { this.callback = callback; this.disconnected = false; list.push(this); }
    observe(target, options = {}) { this.target = target; this.options = options; this.disconnected = false; }
    disconnect() { this.disconnected = true; }
  };
  const context = vm.createContext({
    window, document, innerWidth: 1400,
    localStorage: { getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value) },
    getComputedStyle: () => ({ paddingBottom: '6', colorScheme: 'light' }), Date: { now: () => clock },
    setTimeout: (fn, delay) => { const id = ++sequence; timers.set(id, { fn, delay, due: clock + delay }); return id; },
    clearTimeout: id => timers.delete(id),
    MutationObserver: observerClass(mutationObservers), ResizeObserver: observerClass(resizeObservers),
  });
  vm.runInContext(source, context, { filename: 'session-ui.js' });
  const api = window.__CODEX_SESSION_ONLY_MONITOR__, host = body.children[0], shadow = host.shadowRoot;
  const rows = shadow.querySelector('.rows').children;
  const keys = ['totalTokens', 'lastTurnTokens', 'cacheHitPercent', 'compactionCount', 'status', 'contextPercent', 'lastTtftMs', 'averageOutputTps'];
  const runNext = () => {
    assert.equal(timers.size, 1); const [id, timer] = [...timers][0]; timers.delete(id); clock = timer.due; timer.fn();
  };
  return {
    api, host, shadow, requests, timers, mutationObservers, resizeObservers, window, document, marker, meter, storage,
    runNext, nextDelay: () => [...timers.values()][0]?.delay,
    row: key => rows[keys.indexOf(key)], value: key => rows[keys.indexOf(key)].children[2],
    update: value => api.update({ threadId: A, totalTokens: 413000, averageOutputTps: 13.2, status: 'completed', ...value }),
    setNative: usage => { setUsage(usage); mutationObservers[0].callback([]); },
    setMeters: value => { meters = value; mutationObservers[0].callback([]); },
    setRing: value => { ring = value; },
    refreshNative: () => mutationObservers[0].callback([]),
    identityChange: () => mutationObservers[1].callback([]),
    emit: (surface, event) => surface.listeners.get(event)?.({}),
  };
}

let passed = 0, failed = 0;
function test(name, run) {
  try { run(); passed++; console.log('PASS ' + name); }
  catch (error) { failed++; console.error('FAIL ' + name + ': ' + error.message); }
}

test('complete eight metrics and original default selection', () => {
  const ui = boot(); assert.equal(ui.api.health().metrics, 8);
  assert.equal(ui.shadow.querySelectorAll('.row').length, 8);
  assert.deepEqual(ui.shadow.querySelector('.rows').children.map(row => row.children[0].checked), [true, false, false, false, false, true, false, false]);
  assert(ui.shadow.querySelector('.rows').children.every(row => row.children[0].type === 'checkbox'));
});

test('previous selected preferences are preserved and checkbox changes persist', () => {
  const ui = boot({ selected: ['cacheHitPercent', 'status', 'invalid'] });
  ui.update({ cacheHitPercent: 55.5, status: 'running' });
  assert.match(ui.shadow.querySelector('.summary').textContent, /Cache 55\.5%.*Running/);
  assert.doesNotMatch(ui.shadow.querySelector('.summary').textContent, /Total|Context/);
  assert.equal(ui.shadow.querySelector('.activity').hidden, true);
  const checkbox = ui.row('averageOutputTps').children[0]; checkbox.checked = true; checkbox.listeners.get('change')();
  assert.deepEqual(JSON.parse(ui.storage.get('codex-session-only-metrics-v1')), ['cacheHitPercent', 'status', 'averageOutputTps']);
});

for (const [percent, level] of [[69.9, 'normal'], [70, 'warning'], [84.9, 'warning'], [85, 'high'], [100, 'high']]) {
  test('native ' + percent + '% has ' + level + ' color', () => {
    const ui = boot({ percent }); ui.update({ contextPercent: 10 });
    assert.equal(ui.api.health().contextLevel, level);
    assert(ui.shadow.querySelectorAll('.dot').every(dot => dot.dataset.level === level));
    assert.equal(ui.api.health().contextSource, 'codex-native');
  });
}

test('backend 100% cannot override native 34% and native tokens', () => {
  const ui = boot(); ui.update({ contextPercent: 100, contextUsed: 999999999, contextWindow: 1 });
  assert.equal(ui.api.health().contextLevel, 'normal'); assert.equal(ui.api.health().contextPercent, 34);
  assert.equal(ui.value('contextPercent').textContent, '68K tokens · 34.0%');
  assert.match(ui.value('contextPercent').title, /tokens used: 68,000.*window: 200,000/);
  assert.doesNotMatch(ui.value('contextPercent').textContent, /999|100\.0/);
});

test('missing native meter stays gray and unavailable despite backend context', () => {
  const ui = boot({ hasMeter: false }); ui.update({ contextPercent: 100, contextUsed: 999999, contextWindow: 1000000 });
  assert.equal(ui.api.health().contextLevel, 'unknown'); assert.equal(ui.api.health().contextSource, 'unavailable');
  assert.equal(ui.value('contextPercent').textContent, '--'); assert.equal(ui.api.health().contextPercent, null);
});

test('removing native meter clears stale native value', () => {
  const ui = boot(); ui.update({ contextPercent: 100 }); ui.setMeters([]);
  assert.equal(ui.api.health().contextLevel, 'unknown'); assert.equal(ui.value('contextPercent').textContent, '--');
});

test('aria percentage fallback remains native and never substitutes backend tokens', () => {
  const ui = boot(); delete ui.meter.__reactFiber$fixture;
  ui.setRing(null);
  ui.meter.setAttribute('aria-label', 'Context usage 72,5%'); ui.refreshNative();
  ui.update({ contextPercent: 100, contextUsed: 1000000 });
  assert.equal(ui.api.health().contextSource, 'codex-native-label'); assert.equal(ui.api.health().contextLevel, 'warning');
  assert.equal(ui.value('contextPercent').textContent, '72.5%'); assert.match(ui.value('contextPercent').title, /tokens used: --/);
});

test('invalid native percent and invalid aria text do not use logs', () => {
  const ui = boot(); ui.meter.__reactFiber$fixture.return.memoizedProps.contextUsage.percent = null;
  ui.setRing(null);
  ui.meter.setAttribute('aria-label', 'Context usage unavailable'); ui.refreshNative(); ui.update({ contextPercent: 90 });
  assert.equal(ui.api.health().contextLevel, 'unknown'); assert.equal(ui.value('contextPercent').textContent, '--');
});

test('current SVG ring chooses matching alternate instead of stale React props', () => {
  const ui = boot();
  const ancestor = ui.meter.__reactFiber$fixture.return;
  ancestor.memoizedProps.contextUsage = { percent: 90, usedTokens: 180000, contextWindow: 200000 };
  ancestor.alternate = { memoizedProps: { contextUsage: { percent: 34, usedTokens: 68000, contextWindow: 200000 } } };
  ui.refreshNative(); ui.update({ contextPercent: 100 });
  assert.equal(ui.api.health().contextSource, 'codex-native'); assert.equal(ui.api.health().contextPercent, 34);
  assert.equal(ui.value('contextPercent').textContent, '68K tokens · 34.0%'); assert.equal(ui.api.health().contextLevel, 'normal');
});

test('stale React props are rejected while the current SVG percent is retained', () => {
  const ui = boot(); ui.meter.__reactFiber$fixture.return.memoizedProps.contextUsage = { percent: 90, usedTokens: 180000, contextWindow: 200000 };
  ui.refreshNative(); ui.update({ contextPercent: 100, contextUsed: 999999999 });
  assert.equal(ui.api.health().contextSource, 'codex-native-label');
  assert.equal(ui.value('contextPercent').textContent, '34.0%'); assert.equal(ui.api.health().contextLevel, 'normal');
  assert.match(ui.value('contextPercent').title, /tokens used: --/);
});

test('native meter updates repaint immediately without new backend requests', () => {
  const ui = boot(); ui.update({}); const before = ui.requests.length, delay = ui.nextDelay();
  ui.setNative({ percent: 85, usedTokens: 170000, contextWindow: 200000 });
  assert.equal(ui.api.health().contextLevel, 'high'); assert.equal(ui.requests.length, before); assert.equal(ui.nextDelay(), delay);
  assert.equal(ui.value('contextPercent').textContent, '170K tokens · 85.0%');
});

test('unchanged statistics and native context do not rewrite rendered text', () => {
  const ui = boot(); ui.update({}); const summary = ui.shadow.querySelector('.summary');
  let text = summary.textContent, writes = 0;
  Object.defineProperty(summary, 'textContent', { get: () => text, set: value => { text = value; writes++; } });
  ui.update({}); ui.refreshNative();
  assert.equal(writes, 0);
});

test('old asynchronous chat update is rejected after selection changes', () => {
  const ui = boot(); ui.update({}); ui.marker.setAttribute('data-above-composer-conversation-id', B);
  ui.identityChange(); ui.runNext();
  assert.equal(ui.api.health().threadId, B); assert.equal(ui.update({ totalTokens: 999999 }), false);
  assert.equal(ui.update({ threadId: B, totalTokens: 10 }), true); assert.equal(ui.value('totalTokens').textContent, '10');
});

test('refresh cadence remains idle 10s, running 2.5s and hidden 15s', () => {
  const ui = boot(); assert.equal(ui.nextDelay(), 10000);
  ui.update({ status: 'running' }); assert.equal(ui.nextDelay(), 2500);
  ui.update({ status: 'completed' }); assert.equal(ui.nextDelay(), 10000);
  ui.document.hidden = true; ui.emit(ui.document, 'visibilitychange'); const count = ui.requests.length;
  ui.runNext(); assert.equal(ui.nextDelay(), 15000); assert.equal(ui.api.health().visible, false);
  ui.runNext(); assert.equal(ui.requests.length, count);
});

test('observers stay limited to native meter and identities, not streamed chat text', () => {
  const ui = boot(); assert.equal(ui.mutationObservers.length, 2); assert.equal(ui.resizeObservers.length, 1);
  const [native, identity] = ui.mutationObservers;
  assert.equal(native.target, ui.meter); assert.equal(identity.target, ui.document.body);
  assert.deepEqual(Array.from(native.options.attributeFilter), ['aria-label', 'stroke-dashoffset']);
  assert.deepEqual(Array.from(identity.options.attributeFilter), ['data-app-shell-active-page', 'data-above-composer-conversation-id', 'data-conversation-id', 'data-thread-id']);
  for (const observer of ui.mutationObservers) { assert(!observer.options.childList); assert(!observer.options.characterData); }
  assert.equal(ui.window.listeners.has('scroll'), false);
});

test('returning from ordinary Chat requests a new snapshot even when data is unchanged', () => {
  const ui = boot(); ui.update({}); ui.marker.setAttribute('data-above-composer-conversation-id', 'chatgpt:' + B);
  ui.identityChange(); ui.runNext(); assert.equal(ui.api.health().visible, false);
  ui.marker.setAttribute('data-above-composer-conversation-id', A); ui.identityChange(); ui.runNext();
  assert.equal(ui.requests.at(-1).threadId, A); assert.equal(ui.requests.at(-1).needsSnapshot, true);
});

test('removal clears timer, disconnects all observers and restores composer padding', () => {
  const ui = boot(); assert.equal(ui.api.remove(), true); assert.equal(ui.timers.size, 0);
  assert(ui.mutationObservers.every(observer => observer.disconnected)); assert(ui.resizeObservers.every(observer => observer.disconnected));
  assert.equal(ui.window.__CODEX_SESSION_ONLY_MONITOR__, undefined); assert.equal(ui.host.isConnected, false);
  assert.equal(ui.resizeObservers[0].target.style.getPropertyValue('padding-bottom'), '');
});

console.log(`${passed} native-context UI checks passed; ${failed} failed.`);
if (failed) process.exitCode = 1;
