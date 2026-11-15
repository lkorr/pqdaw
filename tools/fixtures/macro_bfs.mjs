// Generates tests/views/fixtures/macro_bfs.json (T8.1): the fewest-keypress labels of macro mode's presets, computed
// by the ORIGINAL JI composition trainer JavaScript under Node.
//
//   node tools/fixtures/macro_bfs.mjs "<path to ji composition trainer>"
//
// The trainer's functions are cut out verbatim from js/game-pianoroll.js (jiRatioToEdoSteps, largestPrimeFactor,
// prBuildStepToRatioMap) and data/chordTypes.js (gcd), as tools/fixtures/gen_ji_fixtures.mjs does; nothing of the
// search is re-implemented here. Per preset (assets/macro/*.json) the trainer's `allMappings` is built from the
// preset's interval keys in file order (a JS object: its property order is what the search iterates), then the map is
// built for a few EDOs at the preset's prime limit. The test (tests/views/test_macro_mode.cpp) compares
// MacroMode::keypressMap() with `labels` and checks that the key sequence it shows multiplies to that ratio.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const srcDir = process.argv[2];
if (!srcDir) {
  console.error('usage: node macro_bfs.mjs <ji composition trainer directory>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const presetDir = path.resolve(here, '../../assets/macro');
const outFile = path.resolve(here, '../../tests/views/fixtures/macro_bfs.json');
const read = (rel) => fs.readFileSync(path.join(srcDir, rel), 'utf8');

// Source of the top-level `function NAME(` declaration (column 0) through its matching closing brace.
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
const code = [
  extractFunction(read('data/chordTypes.js'), 'gcd'),
  extractFunction(pianoroll, 'jiRatioToEdoSteps'),
  extractFunction(pianoroll, 'largestPrimeFactor'),
  extractFunction(pianoroll, 'prBuildStepToRatioMap'),
  'var prEdo = 12; var prLabelPrimeLimit = 13; var allMappings = {};',
  `function __setMappings(list) {
     allMappings = {};
     for (const [k, n, d] of list) allMappings[k] = { num: n, denom: d };
     const order = []; for (const key in allMappings) order.push([key, allMappings[key].num, allMappings[key].denom]);
     return order;
   }`,
  `globalThis.__api = { __setMappings, prBuildStepToRatioMap,
     set(edo, limit) { prEdo = edo; prLabelPrimeLimit = limit; } };`,
].join('\n');
const sandbox = { Math, console };
vm.createContext(sandbox);
vm.runInContext(code, sandbox);
const api = sandbox.__api;

const edos = [12, 19, 22, 31, 37, 53];
const cases = [];
for (const file of fs.readdirSync(presetDir).filter((f) => f.endsWith('.json')).sort()) {
  const preset = JSON.parse(fs.readFileSync(path.join(presetDir, file), 'utf8'));
  const list = preset.intervals.map((e) => {
    const [n, d] = e.ratio.split('/').map(Number);
    return [e.key, n, d === undefined ? 1 : d];
  });
  const order = api.__setMappings(list);
  const limit = preset.primeLimit === undefined ? 13 : preset.primeLimit;
  for (const edo of edos) {
    api.set(edo, limit);
    const labels = api.prBuildStepToRatioMap();
    cases.push({ preset: path.basename(file, '.json'), edo, limit, order, labels: { ...labels } });
  }
}
fs.writeFileSync(outFile, JSON.stringify({
  source: 'ji composition trainer js/game-pianoroll.js prBuildStepToRatioMap (run by tools/fixtures/macro_bfs.mjs)',
  cases,
}) + '\n');
console.log(`wrote ${outFile}: ${cases.length} cases`);
