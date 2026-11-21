// Runs every JSTests/structs/*.js with the given jsc binary; a test passes when jsc exits 0.
//
//   node run.mjs <jsc> [jsc option...]
//
// The options are passed to jsc before the test, after --useStructs=true. CI runs the suite three times: with the
// default options, with low JIT thresholds so that the hot loops of the *-perf tests reach the DFG and FTL tiers
// early, and with --useJIT=false so that the same tests also run in the interpreter alone. A test can print lines
// that start with "# " to report something (the number of DFG compiles of a hot function, say); they are echoed.
import { readdirSync, readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const [jsc, ...options] = process.argv.slice(2);
const dir = dirname(fileURLToPath(import.meta.url));
let failed = 0;
console.log(`jsc options: --useStructs=true ${options.join(" ")}`);
for (const f of readdirSync(dir).filter(f => f !== "run.mjs" && (f.endsWith(".js") || f.endsWith(".mjs"))).sort()) {
  // A test can ask for jsc options of its own with a line "// jsc-options: --option ..." (--can-block-is-false, say).
  const own = (readFileSync(join(dir, f), "utf8").match(/^\/\/ jsc-options: (.*)$/m)?.[1] ?? "").split(/\s+/).filter(Boolean);
  const r = spawnSync(jsc, ["--useStructs=true", ...options, ...own, join(dir, f)], { encoding: "utf8", timeout: 120000 });
  const ok = r.status === 0;
  console.log(`${ok ? "PASS" : "FAIL"} ${f}`);
  for (const line of (r.stdout ?? "").split("\n"))
    if (line.startsWith("# "))
      console.log(`  ${line}`);
  if (!ok) {
    failed++;
    console.log(`status=${r.status} signal=${r.signal} error=${r.error}`);
    console.log(r.stdout + r.stderr);
  }
}
process.exit(failed ? 1 : 0);
