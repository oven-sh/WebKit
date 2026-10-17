// jsc side: read cases JSONL (arguments[0]), compare exec against ref; print JSON lines for mismatches + summary.
const lines = readFile(arguments[0]).split("\n");
let n = 0, bad = 0, badV8Agrees = 0;
for (const line of lines) {
  if (!line) continue;
  const c = JSON.parse(line); n++;
  let got;
  try { const m = new RegExp(c.p, c.f).exec(c.s); got = m && [m.index, ...Array.from(m, v => v === undefined ? "~U" : v)]; } catch (e) { got = "ERR:" + String(e).slice(0, 60); }
  if (JSON.stringify(got) !== JSON.stringify(c.ref)) {
    bad++;
    const v8ok = JSON.stringify(c.v8) === JSON.stringify(c.ref);
    if (v8ok) badV8Agrees++;
    print(JSON.stringify({ p: c.p, f: c.f, s: c.s, jsc: got, ref: c.ref, v8: c.v8, v8AgreesWithRef: v8ok }));
  }
}
print(JSON.stringify({ summary: true, cases: n, mismatches: bad, mismatchesWhereV8AgreesWithRef: badV8Agrees }));
