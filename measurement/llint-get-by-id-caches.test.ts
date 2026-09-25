import { expect, test } from "bun:test";
import { bunEnv, bunExe } from "harness";
import { join } from "node:path";

// oven-sh/WebKit#PRNUMBER. The interpreter (LLInt) of JavaScriptCore now caches a property read that finds no
// property, reads the length of a string without a call, makes its cache of a value of the prototype chain more
// than once for each read, and sends a function to the Baseline JIT early when one of its reads or writes keeps
// missing the cache. Each of the four has an option. None of them may change what a read returns.
const fixture = join(import.meta.dir, "llint-get-by-id-caches-fixture.js");

const allOff = {
  BUN_JSC_missCountForLLIntTierUp: "0",
  BUN_JSC_useLLIntUnsetCaching: "0",
  BUN_JSC_useLLIntStringLengthCaching: "0",
  BUN_JSC_useLLIntPrototypeCacheRearming: "0",
};

const configurations: [string, Record<string, string>][] = [
  ["the defaults", {}],
  ["the interpreter only", { BUN_JSC_useJIT: "0" }],
  ["the interpreter only, with the four options off", { BUN_JSC_useJIT: "0", ...allOff }],
  ["the interpreter only, with a collection at every chance", { BUN_JSC_useJIT: "0", BUN_JSC_collectContinuously: "1" }],
  [
    // Every function goes to the Baseline JIT at its second miss, and from there to the DFG.
    "tier-up at the second miss",
    {
      BUN_JSC_useConcurrentJIT: "0",
      BUN_JSC_missCountForLLIntTierUp: "2",
      BUN_JSC_thresholdForJITSoon: "10",
      BUN_JSC_thresholdForOptimizeAfterWarmUp: "100",
    },
  ],
  ["only the tier-up", { ...allOff, BUN_JSC_missCountForLLIntTierUp: "12" }],
  ["only the cache of no property", { ...allOff, BUN_JSC_useJIT: "0", BUN_JSC_useLLIntUnsetCaching: "1" }],
  ["only the length of a string", { ...allOff, BUN_JSC_useJIT: "0", BUN_JSC_useLLIntStringLengthCaching: "1" }],
  ["only the countdown that starts again", { ...allOff, BUN_JSC_useJIT: "0", BUN_JSC_useLLIntPrototypeCacheRearming: "1" }],
];

for (const [name, env] of configurations) {
  test.concurrent(`property reads return the same with ${name}`, async () => {
    await using proc = Bun.spawn({
      cmd: [bunExe(), fixture],
      env: { ...bunEnv, ...env },
      stdout: "pipe",
      stderr: "pipe",
    });
    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
    expect(stderr).toBe("");
    expect(stdout).toBe("ok\n");
    expect(exitCode).toBe(0);
  });
}
