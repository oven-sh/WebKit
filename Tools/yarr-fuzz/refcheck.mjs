// Compare the reference matcher against an engine's exec on fuzzer-generated cases.
// usage: node refcheck.mjs <seedFrom> <seedTo> <count> <profile>   (engine = this node)
//        prints disagreements; summary at end.
import { refExecEncoded } from "./refmatch.mjs";
import { execFileSync } from "node:child_process";
const [from, to, count, profile] = [+(process.argv[2]||1), +(process.argv[3]||3), +(process.argv[4]||200), process.argv[5]||"small"];
const unesc = (s) => s.replace(/\\u([0-9a-f]{4})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
let total = 0, supported = 0, agree = 0, steps = 0, disagree = 0;
const seen = new Set();
for (let seed = from; seed <= to; seed++) {
  const out = execFileSync(process.execPath, ["regex-fuzz.js", String(seed), String(count), profile], { maxBuffer: 1 << 28 }).toString();
  for (const line of out.split("\n")) {
    const tab = line.split("\t"); if (tab.length < 2) continue;
    let o; try { o = JSON.parse(tab[1]); } catch { continue; }
    if (!o.p || !o.r || o.r.x || o.r.fatal) continue;
    const key = o.p + "/" + o.f + "/" + o.s; if (seen.has(key)) continue; seen.add(key);
    total++;
    const p = unesc(o.p), s = unesc(o.s), f = o.f.replace("d", "");
    const ref = refExecEncoded(p, f, s, 0);
    if (ref === "UNSUPPORTED") continue;
    supported++;
    if (ref === "STEPS") { steps++; continue; }
    // engine result: o.r.e is [index, m0, ...] with strings escaped via safeStr, or null; recompute directly here instead
    let eng; try { const re = new RegExp(p, f); const m = re.exec(s); eng = m && [m.index, ...[...m].map(v => v === undefined ? "~U" : v)]; } catch { continue; }
    if (JSON.stringify(eng) === JSON.stringify(ref)) agree++;
    else { disagree++; if (disagree <= 25) console.log("DIFF /" + p + "/" + f, JSON.stringify(s).slice(0, 60), "\n   engine:", JSON.stringify(eng), "\n   ref:   ", JSON.stringify(ref)); }
  }
}
console.log({ total, supported, agree, steps, disagree });
