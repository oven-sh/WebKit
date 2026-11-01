let seed = 4242; const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const A = 'abcdefghijklmnopqrstuvwxyz';
const word = (n) => { let s = ''; for (let i = 0; i < n; i++) s += A[rnd(26)]; return s; };
const out = [];
const bases = ['fre', 'sh-SU', 'en-uk', 'und-Armn-SU', 'ja-Latn-hepburn-heploc', 'cel-gaulish', 'und', 'en'];
for (let i = 0; i < 20000; i++) {
  let t = bases[rnd(bases.length)];
  const nv = rnd(20); const vs = new Set(); for (let j = 0; j < nv; j++) vs.add(word(5 + rnd(4))); for (const v of vs) t += '-' + v;
  if (rnd(2)) { t += '-t-' + ['iw', 'sh-su', 'en-uk'][rnd(3)]; const nf = rnd(26); const ks = new Set(); for (let j = 0; j < nf; j++) { const k = A[rnd(26)] + rnd(10); if (!ks.has(k)) { ks.add(k); t += '-' + k + '-' + word(3 + rnd(6)); } } }
  if (rnd(2)) { t += '-u'; const nk = 1 + rnd(40); const ks = new Set(['rg', 'sd']); if (rnd(2)) t += '-rg-no23'; if (rnd(2)) t += '-sd-cn11'; for (let j = 0; j < nk; j++) { const k = A[rnd(26)] + A[rnd(26)]; if (!ks.has(k)) { ks.add(k); } } const sorted = [...ks].filter(k => k !== 'rg' && k !== 'sd'); for (const k of sorted) t += '-' + k + '-' + word(3 + rnd(6)); }
  if (rnd(3) === 0) { t += '-x'; const n = 1 + rnd(20); for (let j = 0; j < n; j++) t += '-' + word(1 + rnd(8)); }
  out.push(t);
}
require('fs').writeFileSync('/tmp/jsc-alias-probe/long2.txt', out.join('\n') + '\n');
const lens = out.map(s => s.length).sort((a, b) => a - b); console.log('n', out.length, 'min', lens[0], 'median', lens[lens.length >> 1], 'max', lens[lens.length - 1], '>157:', lens.filter(l => l > 157).length);
