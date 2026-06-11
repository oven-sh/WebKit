export const meta = {
  name: 'thread-fuzz-setup',
  description: 'Setup-only slice of thread-fuzz: build Fuzzilli (or the fallback grammar fuzzer) + JSCThreads profile + REPRL-enabled jsc in its OWN build dir, smoke the rig. NO campaigns — those wait for a stable GIL-off tree.',
  whenToUse: 'Run alongside engine bring-up: paths are disjoint (/root/fuzzilli, Tools/threads/fuzz, WebKitBuild/Fuzz, docs/threads/FUZZ.md). Campaigns launch later via thread-fuzz.',
  phases: [{ title: 'Setup', detail: 'Solo agent: toolchain, fuzzer, profile, REPRL jsc (own build dir), 10-min smoke' }],
}

const RESULT = { type: 'object', required: ['summary', 'files'], properties: { summary: { type: 'string' }, files: { type: 'array', items: { type: 'string' } }, risks: { type: 'array', items: { type: 'string' } } } }

phase('Setup')
const setup = await agent(`Repo: /root/WebKit (branch jarred/threads). Shared-memory Thread API behind --useJSThreads
(GIL-off bring-up is IN PROGRESS on this tree — another agent owns Source/ and WebKitBuild/Debug|Release|TSan).
YOUR OWNED PATHS ONLY: /root/fuzzilli/**, Tools/threads/fuzz/**, docs/threads/FUZZ.md, WebKitBuild/Fuzz/** (your
own build dir — NEVER build into WebKitBuild/Debug, Release, or TSan). nice -n 10 every build command — the box
is shared with an active bring-up loop. No git in /root/WebKit; cloning external repos outside the repo is fine.
1. Get Fuzzilli running: check for swift; if absent try to install a Swift toolchain; clone google/fuzzilli to
   /root/fuzzilli and build (nice -n 10). If Swift is genuinely unobtainable, FALLBACK: a generative fuzzer in
   JS/Python under Tools/threads/fuzz/ composing random programs from a grammar of thread ops (spawn/join/asyncJoin,
   Lock/Condition, ThreadLocal, Atomics.* on properties, shared-object property add/delete/read/write storms,
   array resize races, dictionary flips, proxies/getters on shared objects) — say loudly which path you took.
2. Fuzzilli path: configure a REPRL-enabled jsc build in WebKitBuild/Fuzz (JSC has Fuzzilli support; ASAN on;
   cmake -B WebKitBuild/Fuzz so nothing touches the other build dirs; nice -n 10 ninja). Write a JSCThreads profile
   extending the JSC profile: register Thread/Lock/Condition/ThreadLocal builtins + custom CodeGenerators for the
   op classes above; default flags --useJSThreads=1 plus rotating stress flags.
3. Smoke: a SHORT (10-minute, timeout-bounded) run; confirm coverage feedback works and the corpus grows.
4. Document exact campaign commands in docs/threads/FUZZ.md so thread-fuzz can run them later. NO long campaigns now.`,
  { label: 'fuzz-setup', phase: 'Setup', schema: RESULT })
if (!setup) throw new Error('fuzz setup failed')
log(`Fuzz rig ready: ${String(setup.summary).slice(0, 160)}`)
return { ready: true, files: setup.files }
