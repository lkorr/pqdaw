// Generates tests/tuning/fixtures/mos_*.json by running the ORIGINAL mos-explorer JavaScript under Node (T2.3).
//
//   node tools/fixtures/gen_mos_fixtures.mjs <path to music-website/apps/web/public/mos-explorer>
//
// The math is never re-implemented here. The named top-level declarations are cut out of index.html and
// mos-calc.js verbatim (by brace matching), evaluated in a vm context, and called. mos-data.js is evaluated whole.
// Only UI side effects (rendering, panel updates, audio) are replaced by no-op stubs so that functions like
// updateHeldNotes and handleModeButton can run headless. The driver below only chooses inputs and records outputs.
//
// Outputs are numbers rounded to 1e-9 (inputs are written at full precision) to keep the files small; the C++
// comparison tolerance is 1e-6 cents. Non-finite numbers are written as the strings "Infinity" / "-Infinity" / "NaN".

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const srcDir = process.argv[2];
if (!srcDir) {
  console.error('usage: node gen_mos_fixtures.mjs <mos-explorer directory>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.resolve(here, '../../tests/tuning/fixtures');
fs.mkdirSync(outDir, { recursive: true });

const indexSrc = fs.readFileSync(path.join(srcDir, 'index.html'), 'utf8');
const calcSrc = fs.readFileSync(path.join(srcDir, 'mos-calc.js'), 'utf8');
const dataSrc = fs.readFileSync(path.join(srcDir, 'mos-data.js'), 'utf8');

// ---- Extraction ------------------------------------------------------------------------------------------------

// Returns the source of the top-level declaration `function NAME(` or `const NAME =` (starting at column 0),
// up to its matching closing brace/bracket (plus a trailing ';' for const). Skips strings, template literals and
// comments while matching. The result is compiled on its own to catch a mis-scan.
function extract(src, name) {
  const re = new RegExp(`^(function ${name}\\(|const ${name} = )`, 'm');
  const m = re.exec(src);
  if (!m) throw new Error(`declaration not found: ${name}`);
  let i = m.index;
  // Find the first opening brace/bracket of the body (for functions, after the parameter list).
  let depth = 0;
  let started = false;
  const stack = [];
  for (; i < src.length; i++) {
    const c = src[i];
    const n2 = src[i + 1];
    if (c === '/' && n2 === '/') { i = src.indexOf('\n', i); continue; }
    if (c === '/' && n2 === '*') { i = src.indexOf('*/', i) + 1; continue; }
    if (c === "'" || c === '"') {
      for (i++; src[i] !== c; i++) if (src[i] === '\\') i++;
      continue;
    }
    if (c === '`') {
      // Template literal; ${ } nesting is tracked on the stack.
      stack.push('`');
      for (i++; i < src.length; i++) {
        if (src[i] === '\\') { i++; continue; }
        if (src[i] === '`') { stack.pop(); break; }
        if (src[i] === '$' && src[i + 1] === '{') {
          // Scan the expression with a simple nested brace count (expressions here hold no strings with braces).
          let d = 0;
          for (i++; i < src.length; i++) {
            if (src[i] === '{') d++;
            else if (src[i] === '}') { d--; if (d === 0) break; }
          }
        }
      }
      continue;
    }
    if (c === '(' && !started && m[1].startsWith('function')) { depth++; continue; }
    if (c === ')' && !started && m[1].startsWith('function')) { depth--; continue; }
    if ((c === '{' || c === '[') && depth === 0 && !started) { started = true; depth = 1; continue; }
    if (started) {
      if (c === '{' || c === '[' || c === '(') depth++;
      else if (c === '}' || c === ']' || c === ')') {
        depth--;
        if (depth === 0) {
          let end = i + 1;
          if (src[end] === ';') end++;
          const text = src.slice(m.index, end);
          new vm.Script(text);  // throws on a mis-scan
          return text;
        }
      }
    }
  }
  throw new Error(`unterminated declaration: ${name}`);
}

const INDEX_DECLS = [
  'PALETTE', 'S',
  'gcd', 'findParentMOS', 'mosDisplayLabel', 'mosDisplayName', 'getChildMOSes', 'isDescendantMOS',
  'buildMOSRegions', 'computeHardness', 'edoStepRatio', 'buildEDOGenerators', 'chainSafeGen', 'buildMOSScale',
  'buildGenSequenceScale', 'buildCurrentScale', 'getGeneratorChain', 'isBrightGenerator', 'computeUDP',
  'gmnGenerator', 'computeGMN', 'gmnLabel', 'formatModmosAlterations', 'computeStepPattern', 'transferModeRotation',
  'rotateByGenerator', 'rotateMOSScale', 'applyMODMOS', 'hasModmos', 'resetModmos', 'hasGenSequence',
  'resetGenSequence', 'initGenSequence', 'updateGenSequenceEntry', 'getEqualizedEnd',
  'getPeriodRepetitions', 'getStretchedOctave', 'stretchCents',
  'getTemperamentsForMOS', 'getTemperamentsAtCursor', 'getModeName',
  'getRotationDegreeMap', 'handleModeButton', 'buildFullScale', 'findNearestDegrees', 'updateHeldNotes',
  'findAdjacentRegion',
];
const CALC_DECLS = [
  'gcd', 'buildMOSScale', 'computeHardness', 'rotateMOSScale', 'findParentMOS', 'getAncestorChain',
  'getMOSRegionBounds', 'generatorForHardness',
];

// UI/audio side effects stubbed out. retuneOsc records what the voice would be tuned to; centsToFreq is the
// identity so the recorded value is the stretched cents.
const STUBS = `
var __retunes = [];
function updatePanel() {}
function render() {}
function refreshLingeringChord() {}
function renderChordHighlights() {}
function centsToFreq(c) { return c; }
function retuneOsc(osc, freq, now) { __retunes.push(freq); osc.freq = freq; }
const SYNTH = { ctx: { currentTime: 0 }, _activeKeys: {}, _glows: {}, _heldScaleSize: null };
`;

const indexCtx = vm.createContext({ Math, Number, Object, Array, Set, Infinity, NaN, isFinite, parseInt, console });
vm.runInContext(dataSrc + '\n' + STUBS + '\n' + INDEX_DECLS.map(d => extract(indexSrc, d)).join('\n\n') +
  '\nglobalThis.__api = { S, SYNTH, TEMPERAMENTS, TEMP_COLORS, MOS_NAMES, MULTI_PERIOD_MOS_NAMES, MODE_NAMES, PALETTE,' +
  INDEX_DECLS.filter(d => d !== 'S' && d !== 'PALETTE').join(', ') + ', getRetunes: () => __retunes.splice(0) };',
  indexCtx);
const J = indexCtx.__api;

const calcCtx = vm.createContext({ Math, Number, Object, Array, Set, Infinity, NaN, isFinite, console });
vm.runInContext(CALC_DECLS.map(d => extract(calcSrc, d)).join('\n\n') +
  `\nglobalThis.__api = { ${CALC_DECLS.join(', ')} };`, calcCtx);
const C = calcCtx.__api;

// ---- Helpers ---------------------------------------------------------------------------------------------------

let seed = 0x9e3779b9;
function rand() {  // mulberry32, deterministic
  seed = (seed + 0x6d2b79f5) | 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const randInt = (lo, hi) => lo + Math.floor(rand() * (hi - lo + 1));  // inclusive
const pick = arr => arr[Math.floor(rand() * arr.length)];

// Output number: rounded to 1e-9, non-finite as a string.
function o(x) {
  if (x === null || x === undefined) return null;
  if (!Number.isFinite(x)) return String(x);
  return +x.toFixed(9);
}
const oa = arr => arr.map(o);
// Copy plain data out of the vm context (arrays from another realm are fine for JSON).
function write(name, obj) {
  const text = JSON.stringify(obj);
  fs.writeFileSync(path.join(outDir, name), text + '\n');
  console.log(`${name}: ${(text.length / 1024).toFixed(1)} KiB`);
}

// [period, viewLimit] pairs exactly as the app's period selector offers them.
const PERIODS = [
  [1200, 1200], [600, 1200], [400, 1200], [300, 1200], [240, 1200], [200, 1200],
  [171.42857142857142, 1200], [1800, 3600], [2400, 2400], [4800, 4800],
];

function setPeriod(P, limit) {
  J.S.period = P;
  J.S.viewLimit = limit;
}
function plainRegion(r) {
  return r ? { perPeriodSize: r.perPeriodSize, lowCents: r.lowCents, highCents: r.highCents } : null;
}

// Region tables per period (maxSize = maxDepth = 40, enough for every size used in the sweeps).
const regionCache = new Map();
function regionsFor(P, limit) {
  const k = `${P}|${limit}`;
  if (!regionCache.has(k)) {
    setPeriod(P, limit);
    regionCache.set(k, J.buildMOSRegions(40, 40, P).regions);
  }
  return regionCache.get(k);
}
// The region of per-period size n containing g (as the app's S.selected would be), or null.
function regionContaining(P, limit, n, g) {
  return regionsFor(P, limit).find(r => r.perPeriodSize === n && g >= r.lowCents && g <= r.highCents) || null;
}

// Generator choices for (n, P): random interior, exact EDO steps (degenerate / equalized edges), region edges
// with tiny offsets, and generators beyond one period.
function chooseGenerator(n, P) {
  const kind = rand();
  if (kind < 0.45) return rand() * P;
  if (kind < 0.7) {
    const N = randInt(2, 41);
    return (randInt(1, N - 1) / N) * P;
  }
  if (kind < 0.85) {
    const N = randInt(2, n);
    const edge = (randInt(1, N - 1) / N) * P;
    return edge + pick([-1e-9, 1e-9, -1e-5, 1e-5, -0.004, 0.004, -0.6, 0.6]);
  }
  if (kind < 0.95) return P + rand() * P;  // raw generator above the period (multi-period use)
  return -rand() * P;                       // negative generator
}

// ---- 1. Scales: buildMOSScale, chainSafeGen, computeHardness, chain, brightness ----------------------------------

const scaleCases = [];
for (let c = 0; c < 1300; c++) {
  const [P, limit] = c < 900 ? PERIODS[0] : pick(PERIODS);
  setPeriod(P, limit);
  const n = c % 10 === 9 ? randInt(2, 60) : randInt(2, 24);
  const g = chooseGenerator(n, P);
  const sel = rand() < 0.7 ? regionContaining(P, limit, n, g) : null;
  J.S.selected = sel;
  const sc = J.buildMOSScale(g, n, P);
  const hr = J.computeHardness(g, n, P);
  scaleCases.push({
    g, n, P, sel: plainRegion(sel),
    safeGen: o(J.chainSafeGen(g, n, P)),
    pitches: oa(sc.pitches), steps: oa(sc.steps), L: o(sc.L), s: o(sc.s),
    hardness: { L: o(hr.L), s: o(hr.s), ratio: o(hr.ratio), str: hr.str },
    chain: J.getGeneratorChain(g, n, P),
    bright: J.isBrightGenerator(g, n, P),
  });
}
J.S.selected = null;
write('mos_scale.json', { cases: scaleCases });

// ---- 2. Modes: rotation, UDP, GMN, step pattern, mode names, rotate by generator --------------------------------

const modeCases = [];
for (let c = 0; c < 2100; c++) {
  const [P, limit] = c < 1400 ? PERIODS[0] : pick(PERIODS);
  setPeriod(P, limit);
  const n = randInt(2, c % 8 === 7 ? 40 : 16);
  const g = rand() < 0.85 ? (rand() < 0.8 ? rand() * P : (randInt(1, 30) / 31) * P) : chooseGenerator(n, P);
  const rot = randInt(0, n - 1);
  const sel = rand() < 0.8 ? regionContaining(P, limit, n, g) : null;
  J.S.selected = sel;
  const rotated = J.rotateMOSScale(J.buildMOSScale(g, n, P), rot, P);
  modeCases.push({
    g, n, P, rot, sel: plainRegion(sel), key: sel ? sel.perPeriodKey : null,
    pitches: oa(rotated.pitches), L: o(rotated.L), s: o(rotated.s),
    udp: J.computeUDP(g, n, rot, P),
    gmn: J.computeGMN(g, n, rot, P),
    pattern: J.computeStepPattern(g, n, rot, P),
    degreeMap: J.getRotationDegreeMap(g, n, rot, P),
    genUp: J.rotateByGenerator(g, n, rot, 1, P),
    genDown: J.rotateByGenerator(g, n, rot, -1, P),
    // getModeName reads S.period; it is only meaningful with a selected region (the app passes sel.perPeriodKey).
    modeName: sel ? J.getModeName(sel.perPeriodKey, g, rot) : null,
  });
}
J.S.selected = null;
write('mos_modes.json', { cases: modeCases });

// ---- 3. MODMOS: applyMODMOS, labels, and the alteration remap of handleModeButton -------------------------------

const modmosCases = [];
for (let c = 0; c < 500; c++) {
  const [P, limit] = c < 300 ? PERIODS[0] : pick(PERIODS);
  setPeriod(P, limit);
  const n = randInt(3, 14);
  const regs = regionsFor(P, limit).filter(r => r.perPeriodSize === n);
  if (!regs.length) continue;
  const sel = pick(regs);
  const g = sel.lowCents + (0.02 + 0.96 * rand()) * (sel.highCents - sel.lowCents);
  const rot = randInt(0, n - 1);
  // Alterations: mostly ±1 chroma, sometimes fractional or ±2, sometimes shorter than n or containing tiny values.
  const len = rand() < 0.8 ? n : randInt(0, n);
  const modmos = [];
  for (let i = 0; i < len; i++) {
    const r = rand();
    modmos.push(r < 0.6 ? 0 : r < 0.8 ? pick([1, -1]) : r < 0.9 ? pick([2, -2, 0.5, -1.5, 1.25]) : pick([0.004, -0.009, 1.005, 0.333]));
  }
  J.S.selected = sel;
  J.S.generatorCents = g;
  const rotated = J.rotateMOSScale(J.buildMOSScale(g, n, P), rot, P);
  const mm = J.applyMODMOS(rotated, modmos, P);
  const action = pick(['step-up', 'step-down', 'gen-up', 'gen-down']);
  J.S.modeRotation = rot;
  J.S.modmos = modmos.slice();
  J.handleModeButton(action);
  modmosCases.push({
    g, n, P, rot, modmos, sel: plainRegion(sel),
    pitches: oa(mm.pitches), steps: oa(mm.steps), L: o(mm.L), s: o(mm.s),
    alts: mm.alts ? oa(mm.alts) : null, origDegs: mm.origDegs || null,
    label: J.formatModmosAlterations(modmos),
    hasModmos: J.hasModmos(),
    action, newRot: J.S.modeRotation, newModmos: oa(J.S.modmos),
  });
}
J.S.selected = null; J.S.generatorCents = null; J.S.modeRotation = 0; J.S.modmos = [];
write('mos_modmos.json', { cases: modmosCases });

// ---- 4. Generator sequences -------------------------------------------------------------------------------------

const genSeqCases = [];
for (let c = 0; c < 300; c++) {
  const [P, limit] = c < 200 ? PERIODS[0] : pick(PERIODS);
  setPeriod(P, limit);
  const n = randInt(2, 20);
  const k = randInt(1, 5);
  const base = rand() * P;
  const seq = [];
  for (let i = 0; i < k; i++) seq.push(rand() < 0.7 ? base + (rand() - 0.5) * 40 : rand() * P * 1.5);
  const sc = J.buildGenSequenceScale(seq, n, P);
  // updateGenSequenceEntry mutates S.genSequence: record the sequence after one edit.
  J.S.genSequence = seq.slice();
  const idx = randInt(0, k - 1);
  const newCents = seq[idx] + (rand() - 0.5) * 300;
  J.updateGenSequenceEntry(idx, newCents);
  genSeqCases.push({
    seq, n, P, pitches: oa(sc.pitches), steps: oa(sc.steps), L: o(sc.L), s: o(sc.s),
    editIdx: idx, editCents: newCents, edited: oa(J.S.genSequence),
  });
}
J.resetGenSequence();
write('mos_genseq.json', { cases: genSeqCases });

// ---- 5. Regions: buildMOSRegions, equalized end, adjacent region ------------------------------------------------

function regionOut(r) {
  return {
    key: r.key, nL: r.nL, nS: r.nS, size: r.size, perPeriodKey: r.perPeriodKey, perPeriodSize: r.perPeriodSize,
    lowFrac: r.lowFrac, highFrac: r.highFrac, lowCents: r.lowCents, highCents: r.highCents,
    collapsedColor: r.collapsedColor, equalizedColor: r.equalizedColor, depth: r.depth, parent: r.parent,
    isMirror: !!r.isMirror,
  };
}
const regionConfigs = [
  [1200, 1200, 18, 18], [1200, 1200, 24, 24], [1200, 1200, 12, 4], [1200, 1200, 1, 1], [1200, 1200, 2, 2],
  [600, 1200, 8, 8], [400, 1200, 8, 8], [300, 1200, 8, 8], [171.42857142857142, 1200, 8, 8],
  [1800, 3600, 18, 18], [2400, 2400, 18, 18], [4800, 4800, 12, 12],
];
const regionSets = [];
for (const [P, limit, maxSize, maxDepth] of regionConfigs) {
  setPeriod(P, limit);
  const { regions, colorMap } = J.buildMOSRegions(maxSize, maxDepth, P);
  const eqEnds = regions.map(r => J.getEqualizedEnd(r));
  // findAdjacentRegion reads S.regions, S.minSize, S.maxSize.
  J.S.regions = regions;
  J.S.minSize = 2;
  J.S.maxSize = maxSize;
  const adjacent = [];
  for (let q = 0; q < 40 && regions.length; q++) {
    const sel = pick(regions);
    const cents = pick([sel.lowCents, sel.highCents]) + pick([-0.6, -0.2, 0, 0.2, 0.6, 3]);
    const adj = J.findAdjacentRegion(sel, cents);
    adjacent.push({ sel: regions.indexOf(sel), cents, result: adj ? regions.indexOf(adj) : -1 });
  }
  regionSets.push({
    P, viewLimit: limit, maxSize, maxDepth, regions: regions.map(regionOut), eqEnds,
    colorMap: Object.entries(colorMap).map(([k, v]) => [+k, v]), adjacent,
  });
}
J.S.regions = []; J.S.minSize = 2; J.S.maxSize = 18;
write('mos_regions.json', { sets: regionSets });

// ---- 6. Mode transfer across region changes ---------------------------------------------------------------------

const transferCases = [];
for (let c = 0; c < 600; c++) {
  const [P, limit] = c < 400 ? PERIODS[0] : pick(PERIODS);
  setPeriod(P, limit);
  const regs = regionsFor(P, limit).filter(r => r.perPeriodSize <= 24);
  const from = pick(regs);
  // Mostly a neighbour sharing a boundary (a generator glide), else a parent/child or any region.
  let to;
  const shared = regs.filter(r => r !== from && (Math.abs(r.lowCents - from.highCents) < 1e-9 || Math.abs(r.highCents - from.lowCents) < 1e-9));
  if (shared.length && rand() < 0.7) to = pick(shared);
  else to = pick(regs);
  const oldG = from.lowCents + (0.01 + 0.98 * rand()) * (from.highCents - from.lowCents);
  const newG = rand() < 0.5 ? oldG : to.lowCents + (0.01 + 0.98 * rand()) * (to.highCents - to.lowCents);
  const oldN = from.perPeriodSize, newN = to.perPeriodSize;
  const oldRot = rand() < 0.9 ? randInt(0, oldN - 1) : pick([-1, oldN, 0.5]);
  J.S.selected = rand() < 0.5 ? to : null;
  transferCases.push({
    P, oldG, oldN, oldRot, newG, newN, sel: plainRegion(J.S.selected),
    newRot: J.transferModeRotation(oldG, oldN, oldRot, newG, newN, P),
  });
}
J.S.selected = null;
write('mos_transfer.json', { cases: transferCases });

// ---- 7. Held notes across generator moves (updateHeldNotes) -----------------------------------------------------

const heldScenarios = [];
for (let c = 0; c < 150; c++) {
  const [P, limit] = c < 100 ? PERIODS[0] : pick(PERIODS.filter(p => p[0] <= 1200));
  setPeriod(P, limit);
  J.S.octaveStretch = rand() < 0.3 ? pick([-12, 3.5, 10]) : 0;
  const regs = regionsFor(P, limit).filter(r => r.perPeriodSize <= 14);
  let sel = pick(regs);
  let g = sel.lowCents + (0.05 + 0.9 * rand()) * (sel.highCents - sel.lowCents);
  J.S.selected = sel;
  J.S.generatorCents = g;
  J.S.modeRotation = randInt(0, sel.perPeriodSize - 1);
  J.S.modmos = rand() < 0.2 ? Array.from({ length: sel.perPeriodSize }, () => pick([0, 0, 1, -1])) : [];
  J.resetGenSequence();
  J.SYNTH._activeKeys = {};
  J.SYNTH._heldScaleSize = null;
  // Press 1..5 notes. startNote's state: degree, octave, currentCents unset; the next updateHeldNotes with
  // _heldScaleSize === null takes the reassignment path (exactly as in the app after the first press).
  const reps = J.getPeriodRepetitions();
  const fullSize = sel.perPeriodSize * reps;
  const voices = [];
  const nv = randInt(1, 5);
  for (let v = 0; v < nv; v++) {
    const deg = randInt(0, fullSize - 1), oct = randInt(-1, 2);
    J.SYNTH._activeKeys['k' + v] = { degreeInScale: deg, noteOctave: oct, osc: {} };
    voices.push({ deg, oct });
  }
  const steps = [];
  const nsteps = randInt(2, 6);
  for (let s = 0; s < nsteps; s++) {
    // Move: small drag inside the region, or jump to a neighbouring/other region (scale size may change).
    if (s > 0) {
      if (rand() < 0.4) g = sel.lowCents + (0.02 + 0.96 * rand()) * (sel.highCents - sel.lowCents);
      else {
        sel = pick(regs);
        g = sel.lowCents + (0.02 + 0.96 * rand()) * (sel.highCents - sel.lowCents);
        J.S.modeRotation = J.S.modeRotation % sel.perPeriodSize;
        if (J.S.modmos.length) J.S.modmos = J.S.modmos.slice(0, sel.perPeriodSize);
      }
      J.S.selected = sel;
      J.S.generatorCents = g;
    }
    J.updateHeldNotes();
    const full = J.buildFullScale();
    steps.push({
      g, sel: plainRegion(sel), n: sel.perPeriodSize, rot: J.S.modeRotation, modmos: J.S.modmos.slice(),
      fullPitches: oa(full.scale.pitches), fullSize: full.size, octave: full.octave,
      voices: Object.keys(J.SYNTH._activeKeys).map(k => {
        const e = J.SYNTH._activeKeys[k];
        return { deg: e.degreeInScale, oct: e.noteOctave, cents: o(e.currentCents), tuned: o(e.osc.freq) };
      }),
      heldScaleSize: J.SYNTH._heldScaleSize,
    });
  }
  heldScenarios.push({ P, viewLimit: limit, stretch: J.S.octaveStretch, voices, steps });
}
J.getRetunes();
J.S.selected = null; J.S.generatorCents = null; J.S.modeRotation = 0; J.S.modmos = []; J.S.octaveStretch = 0;
J.SYNTH._activeKeys = {};
write('mos_held.json', { scenarios: heldScenarios });

// ---- 8. Small functions: tree navigation, names, GMN generator, EDO generators, stretch, hardness search ---------

const misc = {};
misc.parents = [];
for (let a = 0; a <= 25; a++) for (let b = 0; b <= 25; b++) misc.parents.push([a, b, C.findParentMOS(a, b), J.findParentMOS(a, b)]);
misc.children = [];
for (let a = 1; a <= 20; a++) for (let b = 1; b <= 20; b++) misc.children.push([a, b, J.getChildMOSes(`${a}L_${b}s`)]);
misc.descendants = [];
for (let q = 0; q < 600; q++) {
  const a = randInt(1, 14), b = randInt(1, 14), x = randInt(1, 14), y = randInt(1, 14);
  misc.descendants.push([a, b, x, y, J.isDescendantMOS(`${a}L_${b}s`, `${x}L_${y}s`)]);
}
misc.ancestors = [];
for (let a = 1; a <= 16; a++) for (let b = 1; b <= 16; b++) misc.ancestors.push([a, b, C.getAncestorChain(a, b)]);
misc.labels = Object.keys(J.MOS_NAMES).concat(['13L_2s', '2L_2s', '7L_7s']).map(k => [k, J.mosDisplayLabel(k)]);
misc.displayNames = [];
for (const [P, limit] of PERIODS) {
  setPeriod(P, limit);
  for (const k of ['5L_2s', '2L_2s', '3L_3s', '7L_7s', '10L_4s', '4L_6s', '1L_1s', '99L_1s', '6L_6s'])
    misc.displayNames.push([P, limit, k, J.mosDisplayName(k) ?? null]);
}
misc.gmnGenerator = [];
for (const [P] of PERIODS) for (let q = 0; q < 60; q++) {
  const g = q < 10 ? (randInt(1, 9) / 10) * P : (rand() * 2.5 - 0.5) * P;
  misc.gmnGenerator.push([g, P, o(J.gmnGenerator(g, P))]);
}
misc.gmnLabels = [1, 2, 7, 12, 0].map(n => [n, J.gmnLabel(n)]);
misc.edoGenerators = [];
for (const [maxEDO, P] of [[12, 1200], [24, 1200], [10, 600], [8, 171.42857142857142], [5, 2400]]) {
  J.S.edoFilter = null;
  misc.edoGenerators.push({ maxEDO, P, gens: J.buildEDOGenerators(maxEDO, P).map(e => [e.step, e.edo, o(e.cents)]) });
}
misc.edoStepRatio = [];
setPeriod(1200, 1200);
J.S.edos = J.buildEDOGenerators(53, 1200);
for (let q = 0; q < 200; q++) {
  const N = randInt(2, 60), m = randInt(1, N - 1);
  const g = rand() < 0.8 ? (m / N) * 1200 + pick([0, 0, 0.0004, -0.0009, 0.002]) : rand() * 1200;
  const n = randInt(2, 20);
  misc.edoStepRatio.push([g, n, J.edoStepRatio(g, n)]);
}
J.S.edos = [];
misc.stretch = [];
for (const st of [0, -12, 3.5, 7.25, 100]) {
  J.S.octaveStretch = st;
  for (const c of [0, 100, 701.955, 1200, -350, 2400.5]) misc.stretch.push([st, c, o(J.stretchCents(c)), o(J.getStretchedOctave())]);
}
J.S.octaveStretch = 0;
misc.hardnessSearch = [];
for (let q = 0; q < 250; q++) {
  const P = q < 200 ? 1200 : pick([600, 2400]);
  let a = randInt(1, 12), b = randInt(1, 12);
  if (q % 25 === 0) { a = 5; b = 2; }
  const target = pick([1, 1.5, 2, 3, 4, 5 / 3, 1.25, 10, 0.5]) + (q % 3 === 0 ? rand() : 0);
  const r = C.generatorForHardness(a, b, target, P);
  const bounds = C.getMOSRegionBounds(a, b, P);
  misc.hardnessSearch.push({
    nL: a, nS: b, target, P,
    result: r ? { generator: o(r.generator), L: o(r.L), s: o(r.s), ratio: o(r.ratio) } : null,
    bounds: bounds ? { lowCents: o(bounds.lowCents), highCents: o(bounds.highCents), lowFrac: bounds.lowFrac,
      highFrac: bounds.highFrac, mirror: bounds.mirror } : null,
  });
}
// The CLI's hardness (it differs from the app's: no chain-safe nudge, 'toFixed(4)' string, s = 0 when equal).
misc.calcHardness = [];
for (let q = 0; q < 200; q++) {
  const n = randInt(2, 20), P = pick([1200, 600, 2400]);
  const g = q % 4 === 0 ? (randInt(1, 30) / 31) * P : rand() * P;
  const h = C.computeHardness(g, n, P);
  misc.calcHardness.push([g, n, P, o(h.L), o(h.s), o(h.ratio), h.str]);
}
misc.modeNames = [];
setPeriod(1200, 1200);
for (const key of Object.keys(J.MODE_NAMES).concat(['7L_5s', '1L_1s'])) {
  const [nL, nS] = key.replace('s', '').split('L_').map(Number);
  const regs = regionsFor(1200, 1200).filter(r => r.key === key);
  for (const r of regs) for (let rot = 0; rot < nL + nS; rot++) {
    const g = r.lowCents + (0.1 + 0.8 * rand()) * (r.highCents - r.lowCents);
    misc.modeNames.push([key, g, rot, J.getModeName(key, g, rot)]);
  }
}
misc.temperamentQueries = [];
for (const [P, limit] of PERIODS) {
  setPeriod(P, limit);
  const regs = regionsFor(P, limit).filter(r => r.perPeriodSize <= 22);
  for (let q = 0; q < 25; q++) {
    const r = pick(regs);
    const cursor = r.lowCents + rand() * (r.highCents - r.lowCents);
    const forMos = J.getTemperamentsForMOS(r.perPeriodKey, r.lowCents, r.highCents).map(t => [t._idx, o(t.effectiveRange[0]), o(t.effectiveRange[1])]);
    const atCursor = J.getTemperamentsAtCursor(r.perPeriodKey, cursor, r.lowCents, r.highCents).map(t => [t._idx, o(t.effectiveRange[0]), o(t.effectiveRange[1])]);
    // Without a region the mos-tag match decides.
    const byTag = J.getTemperamentsForMOS(r.perPeriodKey).map(t => [t._idx, o(t.effectiveRange[0]), o(t.effectiveRange[1])]);
    misc.temperamentQueries.push({ P, key: r.perPeriodKey, low: r.lowCents, high: r.highCents, cursor, forMos, atCursor, byTag });
  }
}
setPeriod(1200, 1200);
write('mos_misc.json', misc);

// ---- 9. Data tables ---------------------------------------------------------------------------------------------

write('mos_data.json', {
  temperaments: J.TEMPERAMENTS.map(t => ({
    name: t.name, mos: Array.isArray(t.mos) ? t.mos : [t.mos], genRange: t.genRange, period: t.period ?? null,
    commas: t.commas ?? [], commaNames: t.commaNames ?? [], wikiUrl: t.wikiUrl ?? null,
    description: t.description ?? null, disabled: !!t.disabled, keys: Object.keys(t),
  })),
  tempColors: J.TEMP_COLORS, mosNames: J.MOS_NAMES, multiPeriodMosNames: J.MULTI_PERIOD_MOS_NAMES,
  modeNames: J.MODE_NAMES, palette: J.PALETTE,
});

const total = scaleCases.length + modeCases.length + modmosCases.length + genSeqCases.length + transferCases.length;
console.log(`(generator, size, period, mode) cases: ${total}`);
