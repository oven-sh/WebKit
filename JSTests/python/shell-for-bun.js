// What the tests use that is the shell's own, for bun-as-jsc.sh.
import { describe, drainMicrotasks, edenGC, fullGC, noInline } from "bun:jsc";
Object.assign(globalThis, { describe, drainMicrotasks, edenGC, fullGC, noInline, print: (...values) => console.log(values.map(String).join(" ")) });
