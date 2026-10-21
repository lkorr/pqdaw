// Generates src/tuning/src/interval_names_tables.cpp from the ORIGINAL JI composition trainer data (T2.5).
//
//   node tools/fixtures/gen_ji_names_cpp.mjs "<path to ji composition trainer>"
//
// Two tables:
//   1. INTERVAL_NAMES from data/intervalNames.js, evaluated as-is in a vm context (so duplicate keys, if any, resolve
//      exactly as in the browser), in Object.entries order. Used by the port of getIntervalName (js/utils.js).
//   2. The interval gallery in "interval names.txt" (a tab-separated copy of the Xenharmonic wiki's "Gallery of just
//      intervals"): ratio, cents, colour-notation abbreviation and name, FJS name, common names. The text is
//      irregular, so the parsing rules are explicit (see parseGallery) and every anomaly is reported on stderr.
//
// The C++ tables are constant data (no dynamic initialisation). Numbers and strings are copied verbatim; the only
// normalisation is splitting the colour column into abbreviation and name (see splitColour) and trimming whitespace.
// The listed cents are not copied: they are derivable from the ratio, and six of them are typos (KNOWN_CENTS_TYPOS);
// any other disagreement between a ratio and its cents fails the generator.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const srcDir = process.argv[2];
if (!srcDir) {
  console.error('usage: node gen_ji_names_cpp.mjs <ji composition trainer directory>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const outFile = path.resolve(here, '../../src/tuning/src/interval_names_tables.cpp');

// ---- 1. INTERVAL_NAMES -----------------------------------------------------------------------------------------

const namesSrc = fs.readFileSync(path.join(srcDir, 'data/intervalNames.js'), 'utf8');
const ctx = vm.createContext({});
vm.runInContext(namesSrc + '\nglobalThis.__n = INTERVAL_NAMES;', ctx);
const trainer = [];
for (const [key, name] of Object.entries(ctx.__n)) {
  const m = /^(\d+)\/(\d+)$/.exec(key);
  if (!m || String(+m[1]) !== m[1] || String(+m[2]) !== m[2]) throw new Error(`non-canonical INTERVAL_NAMES key ${key}`);
  if (typeof name !== 'string') throw new Error(`non-string name for ${key}`);
  trainer.push({ num: +m[1], den: +m[2], name });
}

// ---- 2. Gallery ------------------------------------------------------------------------------------------------

function gcd(a, b) { while (b) [a, b] = [b, a % b]; return a; }

// The colour column holds an abbreviation ("zg2") and a name ("zogu 2nd"), usually "abbr, name" but sometimes
// "name, abbr", and sometimes only an abbreviation. Abbreviations contain no space; names do. Parts are classified
// on that rule and each group is re-joined with ", " in its original order.
function splitColour(col) {
  const parts = col.split(', ').map((s) => s.trim()).filter((s) => s.length);
  const abbr = parts.filter((p) => !/\s/.test(p));
  const name = parts.filter((p) => /\s/.test(p));
  return { abbr: abbr.join(', '), name: name.join(', ') };
}

// Rows whose listed cents disagree with their ratio by more than the listed precision: transposed or mistyped digits
// in the source (checked by hand: each ratio matches its colour/FJS names, the cents value does not).
const KNOWN_CENTS_TYPOS = new Set(['81/70', '72/49', '35/18', '19/4', '18/1', '25/1']);

function parseGallery(text) {
  const rows = [];
  const anomalies = [];
  for (const raw of text.split(/\r?\n/)) {
    const f = raw.split('\t').map((s) => s.trim());
    if (!/^\d+\/\d+$/.test(f[0] ?? '')) continue;  // headings and notes
    const [ns, ds] = f[0].split('/');
    const num = +ns, den = +ds;
    if (gcd(num, den) !== 1) anomalies.push(`unreduced ratio ${f[0]}`);
    if (!Number.isSafeInteger(num) || !Number.isSafeInteger(den)) throw new Error(`ratio too large: ${f[0]}`);
    // The cents column is not copied (it is derivable, and has typos); it is used to validate the ratio column.
    const cents = Number(f[1]);
    if (!Number.isFinite(cents)) throw new Error(`bad cents in row: ${raw}`);
    const computed = 1200 * Math.log2(num / den);
    if (Math.abs(computed - cents) >= 6e-4) {
      if (!KNOWN_CENTS_TYPOS.has(f[0])) throw new Error(`cents ${f[1]} disagree with ratio ${f[0]} (${computed})`);
      anomalies.push(`${f[0]}: listed cents ${f[1]} is a typo (ratio gives ${computed.toFixed(6)})`);
    }
    let abbr, name, fjs, common;
    if (f.length === 5) {
      // ratio, cents, "abbr, name", FJS, common names (intervals up to an octave)
      ({ abbr, name } = splitColour(f[2]));
      fjs = f[3];
      common = f[4];
    } else if (f.length === 6) {
      // ratio, cents, abbr, name, FJS, common names (intervals above an octave)
      abbr = f[2];
      name = f[3];
      fjs = f[4];
      common = f[5];
    } else {
      throw new Error(`unexpected column count ${f.length}: ${raw}`);
    }
    if (!abbr) anomalies.push(`${f[0]}: no colour abbreviation`);
    if (!name) anomalies.push(`${f[0]}: no colour name`);
    rows.push({ num, den, abbr, name, fjs, common });
  }
  return { rows, anomalies };
}

const galleryText = fs.readFileSync(path.join(srcDir, 'interval names.txt'), 'utf8');
const { rows, anomalies } = parseGallery(galleryText);
// Sort by ratio value (exact: cross-multiplication in BigInt) for binary search; report duplicates and keep the first.
rows.sort((a, b) => {
  const l = BigInt(a.num) * BigInt(b.den), r = BigInt(b.num) * BigInt(a.den);
  return l < r ? -1 : l > r ? 1 : 0;
});
const gallery = [];
for (const r of rows) {
  const prev = gallery[gallery.length - 1];
  if (prev && prev.num === r.num && prev.den === r.den) {
    anomalies.push(`duplicate ratio ${r.num}/${r.den}: kept the first row`);
    continue;
  }
  gallery.push(r);
}
for (const a of anomalies) console.error('note:', a);

// ---- C++ output ------------------------------------------------------------------------------------------------

function str(s) {
  let out = '"';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (ch === '"') out += '\\"';
    else if (ch === '\\') out += '\\\\';
    else if (c < 0x20) out += '\\' + c.toString(8).padStart(3, '0');
    else out += ch;
  }
  return out + '"';
}

const L = [];
L.push('// GENERATED by tools/fixtures/gen_ji_names_cpp.mjs from the JI composition trainer\'s data/intervalNames.js and');
L.push('// "interval names.txt". Do not edit by hand: regenerate instead.');
L.push('');
L.push('#include "pq/tuning/interval_names.h"');
L.push('');
L.push('namespace pq::tuning {');
L.push('');
L.push('namespace {');
L.push('');
L.push(`constexpr TrainerIntervalName kTrainerNames[${trainer.length}] = {`);
for (const t of trainer) L.push(`    {${t.num}, ${t.den}, ${str(t.name)}},`);
L.push('};');
L.push('');
L.push(`constexpr JiIntervalInfo kGallery[${gallery.length}] = {`);
for (const g of gallery)
  L.push(`    {${g.num}, ${g.den}, ${str(g.abbr)}, ${str(g.name)}, ${str(g.fjs)}, ${str(g.common)}},`);
L.push('};');
L.push('');
L.push('}  // namespace');
L.push('');
L.push('std::span<const TrainerIntervalName> trainerIntervalNames() noexcept { return kTrainerNames; }');
L.push('');
L.push('std::span<const JiIntervalInfo> jiIntervalGallery() noexcept { return kGallery; }');
L.push('');
L.push('}  // namespace pq::tuning');
L.push('');
fs.writeFileSync(outFile, L.join('\n'));
console.log(`wrote ${outFile}: ${trainer.length} trainer names, ${gallery.length} gallery rows`);
