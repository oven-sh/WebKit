// Generate cases with the fuzzer's generator, attach the reference result and V8's, write JSONL.
// usage: node refgen.mjs <seedFrom> <seedTo> <count> <profile> <out.jsonl>
import { refExecEncoded } from "./refmatch.mjs";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
const [from, to, count, profile, outPath] = [+process.argv[2], +process.argv[3], +process.argv[4], process.argv[5], process.argv[6]];
const unesc = (s) => s.replace(/\\u([0-9a-f]{4})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
const seen = new Set(); const out = fs.createWriteStream(outPath); let n = 0;
for (let seed = from; seed <= to; seed++) {
  const txt = execFileSync(process.execPath, ["regex-fuzz.js", String(seed), String(count), profile], { maxBuffer: 1 << 28 }).toString();
  for (const line of txt.split("\n")) {
    const tab = line.split("\t"); if (tab.length < 2) continue;
    let o; try { o = JSON.parse(tab[1]); } catch { continue; }
    if (!o.p || !o.r || o.r.x || o.r.fatal) continue;
    const p = unesc(o.p), s = unesc(o.s), f = o.f.replace("d", "");
    for (const subj of [s, s.slice(1), s + "x"]) {
      const key = p + "" + f + "" + subj; if (seen.has(key)) continue; seen.add(key);
      const ref = refExecEncoded(p, f, subj, 0);
      if (ref === "UNSUPPORTED" || ref === "STEPS") continue;
      let v8; try { const m = new RegExp(p, f).exec(subj); v8 = m && [m.index, ...[...m].map(v => v === undefined ? "~U" : v)]; } catch { continue; }
      out.write(JSON.stringify({ p, f, s: subj, ref, v8 }) + "\n"); n++;
    }
  }
}
out.end(); console.error("cases:", n);
