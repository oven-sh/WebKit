#!/usr/bin/env node
// Minimal test262 runner for the RegExp-related subset. Usage:
//   node run262.mjs <label> <jsc> [jsc flags...]   -> writes out262/<label>.json {file: "PASS"|"FAIL: ..."}
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "node:fs";
import { execFile } from "node:child_process";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import { cpus } from "node:os";
import { execFileSync } from "node:child_process";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..", "JSTests", "test262");
const [label, jsc, ...flags] = process.argv.slice(2);
const PAR = Number(process.env.PAR || 12);
const dirs = ["test/built-ins/RegExp", "test/annexB/built-ins/RegExp", "test/language/literals/regexp", "test/built-ins/String/prototype/match", "test/built-ins/String/prototype/matchAll", "test/built-ins/String/prototype/replace", "test/built-ins/String/prototype/replaceAll", "test/built-ins/String/prototype/split", "test/built-ins/String/prototype/search", "test/built-ins/RegExpStringIteratorPrototype"];
const files = execFileSync("find", [...dirs.map((d) => join(root, d)), "-name", "*.js", "-not", "-name", "*_FIXTURE.js"], { encoding: "utf8", maxBuffer: 1 << 26 }).trim().split("\n").sort();
const harnessCache = {};
const harness = (n) => harnessCache[n] ??= readFileSync(join(root, "harness", n), "utf8");
const scratch = join(tmpdir(), "yarr-run262-" + label + "-" + process.pid);
mkdirSync(scratch, { recursive: true });

function meta(src) {
    const m = /\/\*---([\s\S]*?)---\*\//.exec(src);
    const y = m ? m[1] : "";
    const includes = [...(y.match(/includes:\s*\[([^\]]*)\]/)?.[1]?.split(",").map((s) => s.trim()).filter(Boolean) || [])];
    const incBlock = /includes:\s*\n((?:\s+-\s*.*\n)+)/.exec(y);
    if (incBlock) for (const l of incBlock[1].split("\n")) { const t = l.replace(/^\s*-\s*/, "").trim(); if (t) includes.push(t); }
    const flags = (y.match(/flags:\s*\[([^\]]*)\]/)?.[1] || "").split(",").map((s) => s.trim());
    const negative = /negative:/.test(y) ? { phase: (y.match(/phase:\s*(\w+)/) || [])[1], type: (y.match(/type:\s*(\w+)/) || [])[1] } : null;
    const features = (y.match(/features:\s*\[([^\]]*)\]/)?.[1] || "").split(",").map((s) => s.trim());
    return { includes, flags, negative, features };
}

const results = {};
let idx = 0, active = 0, done = 0;
await new Promise((resolve) => {
    const pump = () => {
        while (active < PAR && idx < files.length) {
            const file = files[idx++];
            const rel = file.slice(root.length + 1);
            const src = readFileSync(file, "utf8");
            const m = meta(src);
            if (m.flags.includes("module") || m.flags.includes("async") || m.features.includes("cross-realm")) { results[rel] = "SKIP"; done++; continue; }
            let body = "";
            if (!m.flags.includes("raw")) {
                body += harness("assert.js") + "\n" + harness("sta.js") + "\n";
                if (m.includes.includes("doneprintHandle.js") || m.flags.includes("async")) body += harness("doneprintHandle.js") + "\n";
                for (const inc of m.includes) body += harness(inc) + "\n";
            }
            const strict = m.flags.includes("onlyStrict");
            const testPath = join(scratch, rel.replace(/[\/]/g, "_"));
            writeFileSync(testPath, (strict ? '"use strict";\n' : "") + body + src);
            active++;
            execFile(jsc, [...flags, testPath], { timeout: 120000, env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0" }, maxBuffer: 1 << 24 }, (err, stdout, stderr) => {
                active--; done++;
                let out;
                if (m.negative) {
                    // A negative test passes only on an ordinary error exit naming the expected error,
                    // never on a signal, a timeout kill, or a different error type.
                    if (!err)
                        out = "FAIL: expected " + m.negative.type + " but succeeded";
                    else if (err.signal || err.killed)
                        out = "FAIL: expected " + m.negative.type + " but got " + (err.signal || "killed");
                    else if (!(stderr + stdout).includes(m.negative.type))
                        out = "FAIL: expected " + m.negative.type + " but got: " + (stderr + stdout).trim().split("\n").slice(-1)[0].slice(0, 200);
                    else
                        out = "PASS";
                } else if (err) {
                    out = "FAIL: " + (err.signal || err.code) + " " + (stderr + stdout).trim().split("\n").slice(-3).join(" | ").slice(0, 400);
                } else out = "PASS";
                results[rel] = out;
                if (done === files.length) resolve(); else pump();
            });
        }
        if (done === files.length) resolve();
    };
    pump();
});
mkdirSync(join(here, "out262"), { recursive: true });
writeFileSync(join(here, "out262", label + ".json"), JSON.stringify(results, null, 1));
const fails = Object.entries(results).filter(([k, v]) => v.startsWith("FAIL"));
console.log(`${label}: ${files.length} files, ${fails.length} fail, ${Object.values(results).filter((v) => v === "SKIP").length} skip`);
for (const [k, v] of fails.slice(0, 40)) console.log("  " + k + " :: " + v.slice(0, 200));
