// Generates tests/engine/fixtures/mode_transfer.json by running the ORIGINAL mos-explorer JavaScript under Node
// (T2.6-fix2, M3 review M7): what the explorer plays when the generator of a 5L2s mode moves across 720 cents
// (3\5, where the 7-note chain changes order).
//
//   node tools/fixtures/gen_mode_transfer_fixture.mjs <path to music-website/apps/web/public/mos-explorer>
//
// For every rotation r of 5L2s defined at 700 cents (region hint 5L2s, S.selected), and generators stepping through
// 720 cents (exactly 720 included), the explorer carries the mode with transferModeRotation(700, 7, r, g, 7) — what
// it does when a generator change moves into another region of the same size — and plays rotateMOSScale(
// buildMOSScale(g, 7)) at that rotation. Those per-degree pitches are the fixture. The math is not re-implemented:
// the declarations are cut out of index.html verbatim (as gen_mos_fixtures.mjs does) and called in a vm context.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const srcDir = process.argv[2];
if (!srcDir) {
  console.error('usage: node gen_mode_transfer_fixture.mjs <mos-explorer directory>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.resolve(here, '../../tests/engine/fixtures');
fs.mkdirSync(outDir, { recursive: true });
const indexSrc = fs.readFileSync(path.join(srcDir, 'index.html'), 'utf8');

// The top-level declaration `function NAME(` or `const NAME =` up to its matching brace (strings, template literals
// and comments skipped), compiled on its own to catch a mis-scan. (The same scanner as gen_mos_fixtures.mjs.)
function extract(src, name) {
  const re = new RegExp(`^(function ${name}\\(|const ${name} = )`, 'm');
  const m = re.exec(src);
  if (!m) throw new Error(`declaration not found: ${name}`);
  let depth = 0;
  let started = false;
  for (let i = m.index; i < src.length; i++) {
    const c = src[i];
    const n2 = src[i + 1];
    if (c === '/' && n2 === '/') { i = src.indexOf('\n', i); continue; }
    if (c === '/' && n2 === '*') { i = src.indexOf('*/', i) + 1; continue; }
    if (c === "'" || c === '"') {
      for (i++; src[i] !== c; i++) if (src[i] === '\\') i++;
      continue;
    }
    if (c === '`') {
      for (i++; i < src.length; i++) {
        if (src[i] === '\\') { i++; continue; }
        if (src[i] === '`') break;
        if (src[i] === '$' && src[i + 1] === '{') {
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
          new vm.Script(text);
          return text;
        }
      }
    }
  }
  throw new Error(`unterminated declaration: ${name}`);
}

const DECLS = ['S', 'gcd', 'findParentMOS', 'buildMOSRegions', 'chainSafeGen', 'buildMOSScale', 'getGeneratorChain',
               'transferModeRotation', 'rotateMOSScale'];
const ctx = vm.createContext({ Math, Number, Object, Array, Set, Infinity, NaN, isFinite, parseInt, console });
vm.runInContext('const PALETTE = [];\n' + DECLS.map(d => extract(indexSrc, d)).join('\n\n') +
  `\nglobalThis.__api = { ${DECLS.join(', ')} };`, ctx);
const J = ctx.__api;

const P = 1200;
const n = 7;
const def = 700;
J.S.period = P;
J.S.viewLimit = P;
const region = J.buildMOSRegions(n, n, P).regions.find(r => r.perPeriodSize === n && def >= r.lowCents && def <= r.highCents);
if (!region) throw new Error('no 5L2s region at 700 cents');
J.S.selected = region;  // the hint chainSafeGen reads
const gens = [710, 715, 718, 719, 719.9, 719.99, 719.999, 720, 720.001, 720.01, 720.1, 721, 722.5, 725, 730, 740];
const cases = [];
for (let r = 0; r < n; r++) {
  for (const g of gens) {
    const rot = J.transferModeRotation(def, n, r, g, n, P);
    const sc = J.rotateMOSScale(J.buildMOSScale(g, n, P), rot, P);
    cases.push({ rotation: r, generator: g, carried: rot, pitches: sc.pitches.map(x => +x.toFixed(9)) });
  }
}
const out = {
  source: 'mos-explorer index.html: transferModeRotation, rotateMOSScale(buildMOSScale(g, 7, 1200)) with S.selected',
  n, period: P, definitionGenerator: def,
  region: { perPeriodSize: region.perPeriodSize, lowCents: region.lowCents, highCents: region.highCents },
  cases,
};
const text = JSON.stringify(out);
fs.writeFileSync(path.join(outDir, 'mode_transfer.json'), text + '\n');
console.log(`mode_transfer.json: ${cases.length} cases, ${(text.length / 1024).toFixed(1)} KiB`);
