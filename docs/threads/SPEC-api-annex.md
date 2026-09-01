# SPEC-api-annex — FROZEN NORMATIVE ANNEX (rev 12)

Normative annex of `SPEC-api.md`: the §8 per-file test manifests, moved verbatim for
the size cap. MUST be consumed with the spec; read-only.

---

## §T. Test corpus file manifests (from spec §8 r12)

api/:thread-basic.js (I2,I4,I5);thread-exc.js (I3);thread-ctor-errors.js (4.1);thread-id-bounds.js (I17);thread-lifecycle.js (I20);thread-restrict.js (I14);lock-basic.js (I6 small-N main-thr,I7,I8);lock-async-hold.js (I12,I23,release contract,barging);condition-basic.js (I9,2-wtr handover);condition-async-wait.js (I12);threadlocal-basic.js (I13);blocking-gate.js (I18).

atomics/:ta-path-unchanged.js (I1);property-load-store.js;property-rmw.js (I15 single-thr edges+tiered >=1e4 Atomics.add(o,"x",1) loop,default JIT);property-cas-samevaluezero.js (NaN/-0);property-wait-notify.js (I10,I24 quantum-wakeup half);property-wait-termination.js (I24;--watchdog);property-wtr-isolation.js (I11);property-waitasync-timeout.js (I22);ta-wait-thread-gate.js (I21);property-errors.js (4.5 error cases).

races/ (GI;amplifier+TSAN when present,G15):counter-lock.js (I6 at scale,N=8 M=1e5,>=2 parked wtrs);counter-atomics.js (I15 at scale);transition-vs-read.js+transition-vs-write.js (I16);wait-notify-storm.js (I10 under contention);join-storm.js (I4).

## §T2. Test-corpus conventions (FROZEN NORMATIVE; relocated from spec §8 at r14)

Conventions:every test starts //@ requireOptions("--useJSThreads=1") (G16) except ta-path-unchanged.js (both ways);blocking-gate.js also needs --can-block-is-false (G34;I18;runner appends it);self-checking,failure=throw;join/await every spawned thr (4.6.3);no preemptive-GIL reliance (5.2);race tests bound blocking ops;headers list API-I<n>;CI greps API-I1..I24.
