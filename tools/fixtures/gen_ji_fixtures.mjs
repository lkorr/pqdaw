// Generates tests/tuning/fixtures/ji_bfs.json and ji_names.json by running the ORIGINAL JI composition trainer
// JavaScript under Node (T2.5).
//
//   node tools/fixtures/gen_ji_fixtures.mjs "<path to ji composition trainer>"
//
// The trainer code is never re-implemented here. The needed top-level functions are cut out verbatim (by brace
// matching) from js/game-pianoroll.js (jiRatioToEdoSteps, largestPrimeFactor, prBuildStepToRatioMap),
// js/utils.js (getIntervalName) and data/chordTypes.js (gcd); data/intervalNames.js and data/keyMappings.js are
// evaluated whole. The globals those functions read (prEdo, prLabelPrimeLimit, allMappings) are set per case; the
// keymap is merged inside the vm exactly as js/ui.js does (allMappings = { ...primeMappings, ...reciprocalMappings }),
// so JS property order applies. The driver below only chooses inputs and records outputs.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const srcDir = process.argv[2];
if (!srcDir) {
  console.error('usage: node gen_ji_fixtures.mjs <ji composition trainer directory>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.resolve(here, '../../tests/tuning/fixtures');
fs.mkdirSync(outDir, { recursive: true });
const read = (rel) => fs.readFileSync(path.join(srcDir, rel), 'utf8');

// Source of the top-level `function NAME(` declaration (column 0) through its matching closing brace. Skips strings,
// template literals and comments while matching; the result is compiled on its own to catch a mis-scan.
function extractFunction(src, name) {
  const m = new RegExp(`^function ${name}\\(`, 'm').exec(src);
  if (!m) throw new Error(`function not found: ${name}`);
  let depth = 0;
  let started = false;
  for (let i = src.indexOf('{', src.indexOf(')', m.index)); i < src.length; i++) {
    const c = src[i];
    const n2 = src[i + 1];
    if (c === '/' && n2 === '/') { i = src.indexOf('\n', i); continue; }
    if (c === '/' && n2 === '*') { i = src.indexOf('*/', i) + 1; continue; }
    if (c === "'" || c === '"' || c === '`') {
      for (i++; src[i] !== c; i++) if (src[i] === '\\') i++;
      continue;
    }
    if (c === '{') { depth++; started = true; }
    else if (c === '}') {
      depth--;
      if (started && depth === 0) {
        const text = src.slice(m.index, i + 1);
        new vm.Script(text);
        return text;
      }
    }
  }
  throw new Error(`unterminated function: ${name}`);
}

const pianoroll = read('js/game-pianoroll.js');
const utils = read('js/utils.js');
const chordTypes = read('data/chordTypes.js');

const code = [
  read('data/intervalNames.js'),
  read('data/keyMappings.js'),
  extractFunction(chordTypes, 'gcd'),
  extractFunction(utils, 'getIntervalName'),
  extractFunction(pianoroll, 'jiRatioToEdoSteps'),
  extractFunction(pianoroll, 'largestPrimeFactor'),
  extractFunction(pianoroll, 'prBuildStepToRatioMap'),
  // The globals the extracted functions read (declared with `let` in game-pianoroll.js / state.js).
  'var prEdo = 12; var prLabelPrimeLimit = 13; var allMappings = {};',
  // Merge as js/ui.js updateAllMappings does; returns the iteration order the BFS will see.
  `function __setMappings(primeList, recipList) {
     const primeMappings = {}; for (const [k, n, d] of primeList) primeMappings[k] = { num: n, denom: d };
     const reciprocalMappings = {}; for (const [k, n, d] of recipList) reciprocalMappings[k] = { num: n, denom: d };
     allMappings = { ...primeMappings, ...reciprocalMappings };
     const order = []; for (const key in allMappings) order.push([key, allMappings[key].num, allMappings[key].denom]);
     return order;
   }`,
  `globalThis.__api = { __setMappings, prBuildStepToRatioMap, getIntervalName, jiRatioToEdoSteps,
     DEFAULT_PRIME_MAPPINGS, DEFAULT_RECIPROCAL_MAPPINGS, INTERVAL_NAMES,
     setEdo: (e) => { prEdo = e; }, setLimit: (l) => { prLabelPrimeLimit = l; } };`,
].join('\n\n');
const ctx = vm.createContext({ Math, Object, Number, Set, Map, Array, String, console });
vm.runInContext(code, ctx);
const J = ctx.__api;

function write(name, obj) {
  const text = JSON.stringify(obj);
  fs.writeFileSync(path.join(outDir, name), text + '\n');
  console.log(`${name}: ${(text.length / 1024).toFixed(1)} KiB`);
}
const entriesOf = (o) => Object.entries(o).map(([k, v]) => [k, v.num, v.denom]);

// ---- Keymaps ---------------------------------------------------------------------------------------------------

const KEYMAPS = {
  // data/keyMappings.js defaults (SPEC §6.2's keymap).
  default: [entriesOf(J.DEFAULT_PRIME_MAPPINGS), entriesOf(J.DEFAULT_RECIPROCAL_MAPPINGS)],
  // midi_config.ini preset "qwerrtty": 2/1 3/2 5/4 7/4 11/8 13/8 17/16 on 1–7, reciprocals on q–u.
  qwerrtty: [
    [['1', 2, 1], ['2', 3, 2], ['3', 5, 4], ['4', 7, 4], ['5', 11, 8], ['6', 13, 8], ['7', 17, 16]],
    [['q', 1, 2], ['w', 2, 3], ['e', 4, 5], ['r', 4, 7], ['t', 8, 11], ['y', 8, 13], ['u', 16, 17]],
  ],
  // Property-order and dedupe edge cases: array-index keys out of insertion order, a non-index digit key ("01"),
  // a key redefined by the reciprocal map, a 0-step comma and step collisions.
  ordering: [
    [['a', 3, 2], ['5', 5, 4], ['b', 9, 8], ['10', 7, 6], ['1', 6, 5], ['k', 81, 80], ['c', 10, 9]],
    [['5', 4, 5], ['z', 2, 3], ['01', 6, 7], ['m', 16, 15], ['x', 1, 2], ['2', 2, 1]],
  ],
  // Mid-sized ratios (septimal/undecimal commas and neutral intervals) with only a few keys.
  sparse: [[['g', 7, 6], ['h', 11, 9], ['j', 27, 25]], [['n', 6, 7], ['v', 9, 11]]],
};

const EDOS = [1, 2, 3, 5, 7, 10, 12, 15, 16, 17, 19, 22, 24, 26, 31, 34, 41, 53, 72, 99];
const LIMITS = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31];

const bfsCases = [];
for (const [mapName, [primeList, recipList]] of Object.entries(KEYMAPS)) {
  const order = J.__setMappings(primeList, recipList);
  const limits = mapName === 'default' || mapName === 'qwerrtty' ? LIMITS : [3, 7, 13, 31];
  for (const edo of EDOS) {
    for (const limit of limits) {
      J.__setMappings(primeList, recipList);
      J.setEdo(edo);
      J.setLimit(limit);
      const result = J.prBuildStepToRatioMap();
      const labels = Object.entries(result).map(([s, r]) => [Number(s), r]).sort((a, b) => a[0] - b[0]);
      bfsCases.push({ keymap: mapName, edo, limit, labels });
    }
  }
  KEYMAPS[mapName].push(order);
}
// Steps per key (jiRatioToEdoSteps) for the moves, a cheap extra check of the rounding.
const stepCases = [];
for (const edo of EDOS)
  for (const [n, d] of [[3, 2], [5, 4], [7, 4], [11, 8], [13, 8], [29, 16], [1, 2], [16, 29], [81, 80], [9, 8], [2, 1]]) {
    J.setEdo(edo);
    stepCases.push([edo, n, d, J.jiRatioToEdoSteps(n, d)]);
  }

write('ji_bfs.json', {
  keymaps: Object.fromEntries(
    Object.entries(KEYMAPS).map(([k, [p, r, order]]) => [k, { prime: p, reciprocal: r, order }])),
  cases: bfsCases,
  steps: stepCases,
});

// ---- Interval names --------------------------------------------------------------------------------------------

const nameCases = [];
const seen = new Set();
function addName(n, d) {
  const k = `${n}/${d}`;
  if (seen.has(k)) return;
  seen.add(k);
  nameCases.push([n, d, J.getIntervalName(n, d)]);
}
for (let n = 1; n <= 40; n++) for (let d = 1; d <= 40; d++) addName(n, d);
for (const key of Object.keys(J.INTERVAL_NAMES)) {
  const [n, d] = key.split('/').map(Number);
  addName(n, d);
  addName(d, n);
  addName(2 * n, 2 * d);  // unreduced: no name in the JS
}
write('ji_names.json', { cases: nameCases });
