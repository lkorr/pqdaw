// Generates tests/views/fixtures/roll_layout.json by running the ORIGINAL mos-explorer piano roll (mos-roll.js) under
// Node (T3.12): ROLL.getRows() + ROLL.layoutRows() for 50 (generator, size, height) cases, each in the proportional
// and the equidistant layout.
//
//   node tools/fixtures/gen_roll_layout_fixtures.mjs <path to music-website/apps/web/public/mos-explorer>
//
// Nothing is re-implemented here: the ROLL object and its ROLL_* constants are cut out of mos-roll.js verbatim, and
// the scale functions it calls (buildFullScale and what that needs) out of index.html, with the same brace-matching
// cutter as gen_mos_fixtures.mjs, and evaluated together in one vm context. The driver only sets S (the explorer's
// state: the selected region, generator, mode, MODMOS), the canvas height and the layout flag, then records every
// row's pitch, y, top and bottom. The C++ test (tests/views/test_piano_roll.cpp) builds the same MOS with
// pq::views::roll::RowModel at the same px per cent and compares the geometry relative to degree 0 (±0.5 px).

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const srcDir = process.argv[2];
if (!srcDir) {
  console.error('usage: node gen_roll_layout_fixtures.mjs <mos-explorer directory>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.resolve(here, '../../tests/views/fixtures');
fs.mkdirSync(outDir, { recursive: true });

const indexSrc = fs.readFileSync(path.join(srcDir, 'index.html'), 'utf8');
const rollSrc = fs.readFileSync(path.join(srcDir, 'mos-roll.js'), 'utf8');

// ---- Extraction (the cutter of gen_mos_fixtures.mjs) -----------------------------------------------------------

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
  'PALETTE', 'S', 'gcd', 'findParentMOS', 'buildMOSRegions', 'chainSafeGen', 'buildMOSScale', 'buildGenSequenceScale',
  'buildCurrentScale', 'rotateMOSScale', 'applyMODMOS', 'hasModmos', 'hasGenSequence', 'getPeriodRepetitions',
  'buildFullScale',
];
// The ROLL_* constants are one-line `const ROLL_X = value;` declarations at the top of mos-roll.js: taken verbatim.
const rollConsts = [...rollSrc.matchAll(/^const ROLL_[A-Z_]+ = [^;\n]+;/gm)].map(m => m[0]);
if (rollConsts.length < 10) throw new Error('ROLL_* constants not found');

const ctx = vm.createContext({ Math, Number, Object, Array, Set, Map, Infinity, NaN, isFinite, parseInt, console });
vm.runInContext(rollConsts.join('\n') + '\n' + INDEX_DECLS.map(d => extract(indexSrc, d)).join('\n\n') + '\n' +
  extract(rollSrc, 'ROLL') + '\nglobalThis.__api = { S, ROLL, buildMOSRegions, getPeriodRepetitions };', ctx);
const J = ctx.__api;
const { S, ROLL } = J;

// ---- Cases -----------------------------------------------------------------------------------------------------

let seed = 0x2545f491;
function rand() {  // mulberry32, deterministic
  seed = (seed + 0x6d2b79f5) | 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const randInt = (lo, hi) => lo + Math.floor(rand() * (hi - lo + 1));
const o = x => +x.toFixed(6);

const regionsByPeriod = new Map();
function regionsFor(P, limit) {
  const k = `${P}|${limit}`;
  S.period = P;
  S.viewLimit = limit;
  if (!regionsByPeriod.has(k)) regionsByPeriod.set(k, J.buildMOSRegions(24, 24, P).regions);
  return regionsByPeriod.get(k);
}

// Periods with the view limit that sets the periods per equave (1200 weighted 3×).
const PERIODS = [[1200, 1200], [1200, 1200], [1200, 1200], [600, 1200], [400, 1200], [1800, 3600]];
// Named cases first: 5L 2s at 18\31, near collapse (s → 0 next to 3\5), at the edge of collapse, near equalized;
// 2L 5s.
const named = [
  { n: 7, gen: 1200 * 18 / 31 },
  { n: 7, gen: 718.5 },
  { n: 7, gen: 719.97 },
  { n: 7, gen: 686.0 },
  { n: 7, gen: 670.0 },
];

const cases = [];
for (let i = 0; i < 50; i++) {
  const [P, limit] = i < named.length ? [1200, 1200] : PERIODS[randInt(0, PERIODS.length - 1)];
  const regions = regionsFor(P, limit);
  let region, gen;
  if (i < named.length) {
    gen = named[i].gen;
    region = regions.find(r => r.perPeriodSize === named[i].n && gen >= r.lowCents && gen <= r.highCents);
  } else {
    const pool = regions.filter(r => r.perPeriodSize >= 2 && r.perPeriodSize <= 24);
    region = pool[randInt(0, pool.length - 1)];
    const span = region.highCents - region.lowCents;
    const kind = rand();
    // Interior, or within 0.2 % of the span of either end (collapsed / equalized).
    const f = kind < 0.6 ? 0.05 + 0.9 * rand() : kind < 0.8 ? 0.002 * rand() : 1 - 0.002 * rand();
    gen = region.lowCents + f * span;
  }
  if (!region) throw new Error(`case ${i}: no region`);
  const n = region.perPeriodSize;
  const rotation = i % 3 === 0 ? 0 : randInt(0, n - 1);
  const modmos = (i % 7 === 3 && n >= 4) ? Array.from({ length: n }, (_, j) => (j === 1 ? 1 : j === n - 2 ? -1 : 0)) : [];
  const height = randInt(180, 1100);
  for (const equidistant of [false, true]) {
    S.period = P;
    S.viewLimit = limit;
    S.octaveStretch = 0;
    S.selected = region;
    S.generatorCents = gen;
    S.modeRotation = rotation;
    S.modmos = modmos.slice();
    S.genSequence = null;
    ROLL.equidistant = equidistant;
    ROLL.clip.notes = [];
    ROLL._rowsKey = '';
    ROLL._rows = null;
    ROLL.view = { scrollX: 0, pxPerSec: 100, octLow: -1, octHigh: 2, centreCents: 600, spanCents: 3600, _init: false };
    ROLL._cssW = 800;
    ROLL._cssH = height;
    const model = ROLL.getRows();
    const { laneH, pxPerCent } = ROLL.layoutRows(model);
    cases.push({
      generator: gen, n, period: P, periodsPerEquave: J.getPeriodRepetitions(), rotation, modmos,
      region: { perPeriodSize: region.perPeriodSize, lowCents: region.lowCents, highCents: region.highCents },
      height, equidistant, pxPerCent, laneH: o(laneH), size: model.size, equave: model.equave,
      // [absolute degree, true cents, y, top, bottom] for degree −1 … two equaves up (the rest of the octLow …
      // octHigh model only repeats this geometry)
      rows: model.rows.filter(r => r.octave * model.size + r.degree >= -1 && r.octave * model.size + r.degree <= 2 * model.size).map(r => [r.octave * model.size + r.degree, o(r.trueCents), o(r.y), o(r.top), o(r.bottom)]),
    });
  }
}
const text = JSON.stringify({ source: 'mos-roll.js ROLL.getRows + ROLL.layoutRows', cases });
fs.writeFileSync(path.join(outDir, 'roll_layout.json'), text + '\n');
console.log(`roll_layout.json: ${cases.length} layouts, ${(text.length / 1024).toFixed(1)} KiB`);
