function libOuter(seed) { let s = seed; function inc() { return ++s; } class B { static make() { return () => s; } } function* g() { while (true) yield inc(); } return { inc, B, g, arrow: (x) => x + s }; }
function LibCtor(a) { this.a = a; this.f = function () { return a; }; }
var libTag = (s) => s; function libSite() { return libTag`q${1}`; }
globalThis.libLoads = (globalThis.libLoads | 0) + 1;
