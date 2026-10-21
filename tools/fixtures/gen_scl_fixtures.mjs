// Writes tests/tuning/fixtures/scl/*.scl: Scala scale files authored for pqdaw (T2.4).
//
//   node tools/fixtures/gen_scl_fixtures.mjs
//
// Every pitch set here is computed from its mathematical definition or is a plain list of published ratios (a fact,
// not a copied file); all descriptions and comments are our own text. The files deliberately cover the .scl format's
// edge cases (see tests/tuning/fixtures/scl/SOURCES.txt), so the exact bytes matter: line ends, tabs, trailing
// spaces, a Latin-1 byte and a missing final newline are written explicitly. .gitattributes keeps the bytes as-is.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.resolve(here, '../../tests/tuning/fixtures/scl');
fs.mkdirSync(outDir, { recursive: true });

const log2 = Math.log2;
const cents = (r) => 1200 * log2(r);
const fix = (x, d) => x.toFixed(d);

// lines: array of strings (no line ends). eol: '\r\n' or '\n'. finalNewline: write eol after the last line.
// latin1: encode as ISO-8859-1 (one byte per char) instead of UTF-8.
function write(name, lines, { eol = '\r\n', finalNewline = true, latin1 = false } = {}) {
  let text = lines.join(eol);
  if (finalNewline) text += eol;
  const buf = latin1 ? Buffer.from(text, 'latin1') : Buffer.from(text, 'utf8');
  fs.writeFileSync(path.join(outDir, name), buf);
  return name;
}

const header = (name, desc, count) => [`! ${name}`, '!', desc, ` ${count}`, '!'];
const written = [];

// 1. 12-EDO in cents, CRLF, the canonical layout.
{
  const p = [];
  for (let k = 1; k <= 11; k++) p.push(` ${fix(100 * k, 5)}`);
  p.push(' 2/1');
  written.push(write('edo12.scl', [...header('edo12.scl', '12 equal divisions of the octave', 12), ...p]));
}

// 2. 19-EDO, LF-only line ends.
{
  const p = [];
  for (let k = 1; k <= 18; k++) p.push(` ${fix((1200 * k) / 19, 6)}`);
  p.push(' 2/1');
  written.push(write('edo19_lf.scl', [...header('edo19_lf.scl', '19-EDO, LF line ends', 19), ...p], { eol: '\n' }));
}

// 3. 31-EDO with tab characters and whitespace-only comment lines.
{
  const p = [];
  for (let k = 1; k <= 30; k++) p.push(`\t${fix((1200 * k) / 31, 5)}\t`);
  p.push('\t2/1');
  written.push(write('edo31_tabs.scl', ['! edo31_tabs.scl', '!\t ', '31-EDO written with tabs', '\t31', '!  ', ...p]));
}

// 4. Harmonics 16..32 over 16, written unreduced (18/16, 20/16, … 32/16).
{
  const p = [];
  for (let h = 17; h <= 32; h++) p.push(` ${h}/16`);
  written.push(write('harmonics16_32.scl', [...header('harmonics16_32.scl', 'Harmonic series segment 16 to 32, unreduced', 16), ...p]));
}

// 5. Harmonics 2..16 and subharmonics 1/2..1/16, ending on 1/1 (an equave of 1/1).
{
  const q = [];  // 15 harmonics, 14 subharmonics, then 1/1 as the formal octave: 30 pitches
  for (let h = 2; h <= 16; h++) q.push(` ${h}/1`);
  for (let h = 2; h <= 15; h++) q.push(` 1/${h}`);
  q.push(' 1/1');
  written.push(write('harm_subharm16.scl', [...header('harm_subharm16.scl', 'Harmonics 2-16 and subharmonics 2-15, formal octave 1/1', q.length), ...q]));
}

// 6. A descending scale whose formal octave is below 1/1.
written.push(write('descending.scl', [...header('descending.scl', 'Descending scale with formal octave 1/2', 3), ' 8/9', ' 4/5', ' 1/2']));

// 7. Pythagorean 12: a chain of pure fifths from Eb to G#, octave-reduced (3-limit).
{
  const ratios = ['2187/2048', '9/8', '32/27', '81/64', '4/3', '729/512', '3/2', '6561/4096', '27/16', '16/9', '243/128', '2/1'];
  written.push(write('pythagorean12.scl', [...header('pythagorean12.scl', 'Pythagorean chain of 3/2 fifths, 12 notes', 12), ...ratios.map((r) => ` ${r}`)]));
}

// 8. Ptolemy's intense diatonic (5-limit major scale), equave written as a bare integer "2".
written.push(write('ptolemy_diatonic.scl', [...header('ptolemy_diatonic.scl', "Ptolemy's intense diatonic (5-limit major)", 7), ' 9/8', ' 5/4', ' 4/3', ' 3/2', ' 5/3', ' 15/8', ' 2']));

// 9. 5-limit 12-tone just scale (Zarlino-style) with note names after the values and comments between pitch lines.
{
  const rows = [['16/15', 'Db'], ['9/8', 'D'], ['6/5', 'Eb'], ['5/4', 'E'], ['4/3', 'F'], ['45/32', 'F#'], ['3/2', 'G'], ['8/5', 'Ab'], ['5/3', 'A'], ['9/5', 'Bb'], ['15/8', 'B'], ['2/1', 'C']];
  const p = [];
  rows.forEach(([r, n], i) => {
    if (i === 6) p.push('! upper tetrachord');
    p.push(` ${r}   ${n}`);
  });
  written.push(write('just5_12.scl', [...header('just5_12.scl', '5-limit just chromatic scale on C', 12), '! lower tetrachord', ...p]));
}

// 10. Quarter-comma meantone, computed: fifth = 5^(1/4); E and G# are the pure 5/4 and 25/16.
const meantone = (() => {
  const f = cents(5) / 4;  // 696.578428...
  const fifths = { 1: 7, 2: 2, 3: -3, 5: -1, 6: 6, 7: 1, 9: 3, 10: -2, 11: 5 };  // degree → fifths from C
  const out = {};
  for (const [deg, k] of Object.entries(fifths)) {
    const c = k * f;
    out[deg] = c - 1200 * Math.floor(c / 1200);
  }
  return out;
})();
{
  const p = [];
  for (let d = 1; d <= 11; d++) {
    if (d === 4) p.push(' 5/4');
    else if (d === 8) p.push(' 25/16');
    else p.push(` ${fix(meantone[d], 5)}`);
  }
  p.push(' 2/1');
  written.push(write('meantone_qc.scl', [...header('meantone_qc.scl', 'Quarter-comma meantone, Eb to G#', 12), ...p]));
}

// 11. Bohlen-Pierce equal-tempered: 13 equal divisions of the tritave 3/1.
{
  const p = [];
  for (let k = 1; k <= 12; k++) p.push(` ${fix((cents(3) * k) / 13, 5)}`);
  p.push(' 3/1');
  written.push(write('bp_equal.scl', [...header('bp_equal.scl', 'Bohlen-Pierce: 13 equal steps of 3/1', 13), ...p]));
}

// 12. Bohlen-Pierce just scale (published 7-limit odd-only ratios), tritave equave.
written.push(write('bp_just.scl', [...header('bp_just.scl', 'Bohlen-Pierce just tuning, odd harmonics 3 5 7', 13),
  ...['27/25', '25/21', '9/7', '7/5', '75/49', '5/3', '9/5', '49/25', '15/7', '7/3', '63/25', '25/9', '3/1'].map((r) => ` ${r}`)]));

// 13. An alpha-like non-octave scale: 3/2 split into 9 equal steps, 18 steps (equave ≈ 1404 cents), cents only.
{
  const step = cents(1.5) / 9;
  const p = [];
  for (let k = 1; k <= 18; k++) p.push(` ${fix(step * k, 5)}`);
  written.push(write('fifth_by_nine.scl', [...header('fifth_by_nine.scl', 'Pure fifth divided in nine, two fifths repeat', 18), ...p]));
}

// 14. Ptolemy's diatonic tetrachord, repeating at 4/3; trailing spaces after the description.
written.push(write('tetrachord.scl', [...header('tetrachord.scl', 'Diatonic tetrachord 16/15 9/8 10/9, repeats at 4/3      ', 3), ' 16/15', ' 6/5', ' 4/3']));

// 15. Latin-1 description byte (é = 0xE9), 12-EDO cents.
{
  const p = [];
  for (let k = 1; k <= 11; k++) p.push(` ${fix(100 * k, 1)}`);
  p.push(' 2/1');
  written.push(write('latin1.scl', [...header('latin1.scl', 'Gamme tempérée à douze demi-tons égaux', 12), ...p], { latin1: true }));
}

// 16. Negative cents: a 5-note scale written relative to a centre, below and above 1/1.
written.push(write('negative.scl', [...header('negative.scl', 'Pitches below and above the unison', 5), '-240.0', '-120.0', ' 120.0', ' 240.0', ' 1200.0']));

// 17. Cents only, no leading spaces, stretched octave (1202.5 cents), 10 equal steps of it.
{
  const p = [];
  for (let k = 1; k <= 10; k++) p.push(fix((1202.5 * k) / 10, 3));
  written.push(write('stretched10.scl', ['! stretched10.scl', '!', '10 equal steps of a 1202.5 cent octave', '10', '!', ...p]));
}

// 18. Pythagorean commas: ratios whose parts exceed int64 (3^40/2^63, 2^65/3^41, 3^53/2^84), plus one that fits.
const big = (() => {
  const p3 = (k) => 3n ** BigInt(k);
  const p2 = (k) => 2n ** BigInt(k);
  return [
    `${p3(12)}/${p2(19)}`,  // Pythagorean comma, fits
    `${p3(40)}/${p2(63)}`,  // > int64 numerator and denominator
    `${p2(65)}/${p3(41)}`,
    `${p3(53)}/${p2(84)}`,  // Mercator's comma
    '2/1',
  ];
})();
written.push(write('big_ratios.scl', [...header('big_ratios.scl', 'Powers of three over powers of two, beyond 64-bit', big.length), ...big.map((r) => ` ${r}`)]));

// 19. Equal temperament of 10/9 as exact powers (10/9)^k, k = 1..14: parts up to 10^14.
{
  const p = [];
  for (let k = 1; k <= 14; k++) p.push(` ${10n ** BigInt(k)}/${9n ** BigInt(k)}`);
  written.push(write('powers_10_9.scl', [...header('powers_10_9.scl', 'Powers of 10/9 up to the 14th', 14), ...p]));
}

// 20. 7-EDO, no final newline, and a commented-out alternative after the pitches.
{
  const p = [];
  for (let k = 1; k <= 6; k++) p.push(fix((1200 * k) / 7, 5));
  p.push('2/1');
  const tail = ['!', '! alternative just version:', '! 8/7', '! 21/16', '! 3/2'];
  written.push(write('edo7_nofinalnl.scl', ['! edo7_nofinalnl.scl', '!', '7-EDO, file ends without a newline', '7', '!', ...p, ...tail], { finalNewline: false }));
}

// 21. Cents with a trailing period ("80."), 15-EDO.
{
  const p = [];
  for (let k = 1; k <= 14; k++) p.push(`${80 * k}.`);
  p.push('2/1');
  written.push(write('edo15_trailing_dot.scl', [...header('edo15_trailing_dot.scl', '15-EDO with cents written as 80.', 15), ...p]));
}

// 22. Cents followed by a unit word: "100.000 cents".
{
  const p = [];
  for (const c of [200, 400, 500, 700, 900, 1100]) p.push(` ${fix(c, 3)} cents`);
  p.push(' 2/1');
  written.push(write('cents_units.scl', [...header('cents_units.scl', 'Major scale in 12-EDO cents with units', 7), ...p]));
}

// 23. Partch's 43-tone just scale (the published ratio list), each value followed by "! n" annotations.
const partch = ['81/80', '33/32', '21/20', '16/15', '12/11', '11/10', '10/9', '9/8', '8/7', '7/6', '32/27', '6/5', '11/9',
  '5/4', '14/11', '9/7', '21/16', '4/3', '27/20', '11/8', '7/5', '10/7', '16/11', '40/27', '3/2', '32/21', '14/9', '11/7',
  '8/5', '18/11', '5/3', '27/16', '12/7', '7/4', '16/9', '9/5', '20/11', '11/6', '15/8', '40/21', '64/33', '160/81', '2/1'];
written.push(write('partch43.scl', [...header('partch43.scl', "Partch's 43-tone monophonic scale (ratios)", 43), ...partch.map((r, i) => `${r}   ! ${i + 1}`)]));

// 24. Unreduced ratios with "!" annotations after values and comment lines between pitches (Pythagorean 7).
written.push(write('unreduced_annotated.scl', ['! unreduced_annotated.scl', '!', 'Pythagorean diatonic written with unreduced ratios', '7', '!',
  '! degree 1 is 1/1', '18/16 ! whole tone (9/8)', '162/128 ! ditone (81/64)', '! the fourth', '8/6 ! 4/3',
  '6/4 ! fifth', '54/32 ! 27/16', '486/256 ! 243/128', '4/2 ! octave']));

// 25. Comment between description and count, count with a trailing space, bare integers and pitches outside the
//     octave and out of order: the hexany of {1, 3, 5, 7} as unnormalised pair products over 1·3.
written.push(write('hexany_raw.scl', ['! hexany_raw.scl', '! ', 'Hexany 1.3.5.7 as raw pair products over 3', '! six products of two factors', '6 ', '! ',
  '5/3 ', '7/3 ', '5 ', '7 ', '35/3 ', '2/1']));

// 26. 53-EDO in cents, each line annotated with its step number.
{
  const p = [];
  for (let k = 1; k <= 52; k++) p.push(` ${fix((1200 * k) / 53, 6)}  ! ${k}\\53`);
  p.push(' 2/1    ! 53\\53');
  written.push(write('edo53_annotated.scl', [...header('edo53_annotated.scl', '53-EDO with step labels', 53), ...p]));
}

// 27. An empty description line.
written.push(write('empty_description.scl', ['! empty_description.scl', '!', '', ' 2', '!', ' 3/2', ' 2/1']));

// 28. 22-EDO in cents, both description and pitches with leading tabs/spaces mixed.
{
  const p = [];
  for (let k = 1; k <= 21; k++) p.push(`${k % 2 ? ' ' : '\t'}${fix((1200 * k) / 22, 5)}`);
  p.push(' 2/1');
  written.push(write('edo22.scl', [...header('edo22.scl', '22 equal divisions of the octave', 22), ...p]));
}

console.log(`wrote ${written.length} files to ${outDir}`);
