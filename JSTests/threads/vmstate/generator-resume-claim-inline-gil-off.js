//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-ungil §N.5 (eighth landing round). GIL off, a generator resume claims
// the generator with one CAS on its state word and publishes completion with
// another; both used to be host calls from the builtin on every next() (2.5 M
// per run of a generator-heavy program). The DFG and FTL now inline them
// (GeneratorClaimResume / GeneratorPublishResume, the token read from the
// thread's VMLite). (1) The protocol still holds with the inline forms: four
// threads race next() on shared generators; every value a generator yields is
// delivered to exactly one thread, the losers of a claim see the "executing"
// TypeError, and no result is torn (cve/mc-prim-generator-resume-claim.js is
// the two-thread probe of the same invariant). (2) A single-threaded hot
// resume loop stops reaching the host claim function once optimized (before:
// 300000 host claims for 300000 resumes GIL off; after: only the calls made
// before tier-up, a few thousand).
load("../harness.js", "caller relative");

function* counter(n) { for (let i = 0; i < n; ++i) yield i; }

// (1) Shared generators, four resumers each.
const GENERATORS = 40, N = 400;
const gens = []; for (let g = 0; g < GENERATORS; ++g) gens.push(counter(N));
function resumer() {
    const seen = []; let typeErrors = 0, torn = null;
    for (let g = 0; g < gens.length && !torn; ++g) {
        const it = gens[g];
        for (;;) {
            let r;
            try { r = it.next(); } catch (e) { if (e instanceof TypeError) { ++typeErrors; continue; } torn = "threw " + e; break; }
            if (typeof r !== "object" || r === null || typeof r.done !== "boolean") { torn = "result " + String(r); break; }
            if (r.done) break;
            if (typeof r.value !== "number") { torn = "value " + String(r.value); break; }
            seen.push(g * N + r.value);
        }
    }
    return { seen, typeErrors, torn };
}
const threads = []; for (let i = 0; i < 3; ++i) threads.push(new Thread(resumer));
const mine = resumer();
const all = threads.map(t => t.join()); all.push(mine);
const delivered = new Uint8Array(GENERATORS * N);
let total = 0, errors = 0;
for (const r of all) {
    if (r.torn) throw new Error("torn resume: " + r.torn);
    errors += r.typeErrors;
    for (const v of r.seen) { if (delivered[v]) throw new Error("value " + v + " delivered twice"); delivered[v] = 1; ++total; }
}
if (total !== GENERATORS * N) throw new Error("delivered " + total + " of " + GENERATORS * N + " values");
if (typeof AMPLIFY_VERBOSE !== "undefined") print("delivered " + total + " values once each; claim losers (TypeError): " + errors);

// (2) Host claims after tier-up.
function drain(n) { let s = 0; for (const v of counter(n)) s += v; return s; }
noInline(drain);
for (let i = 0; i < 300; ++i) drain(100);
const before = $vm.jsThreadsCounter("generatorClaimResume");
const RESUMES = 300000;
const s = drain(RESUMES);
const hostClaims = $vm.jsThreadsCounter("generatorClaimResume") - before;
if (s !== (RESUMES * (RESUMES - 1)) / 2) throw new Error("sum " + s);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("host claim calls for " + RESUMES + " resumes: " + hostClaims);
if (!$vm.useThreadGIL() && hostClaims > RESUMES / 10) throw new Error(hostClaims + " of " + RESUMES + " generator resumes still called the host claim GIL off");
print("PASS");
