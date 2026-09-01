# TP2 ARM B (snapshot fix, spin retained)

Change vs ARM A: keep the widened window (spin) and the detector, but return
the VERIFIED snapshot instead of the reload. The detector still logs when the
reload differs (proves the window is still being hit) — but the returned
string is the verified one, so no corruption can result from this mechanism.

Edit in KeyAtomStringCacheInlines.h: `return reloaded;` -> `return verified;`
(everything else identical to armA-instrumentation.patch).
