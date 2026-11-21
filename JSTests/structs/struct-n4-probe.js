function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct Probe { seen; constructor() { this.seen = Object.keys(this).join(); } }
struct Probe2 extends Probe { extra; constructor() { super(); } }
assert(new Probe2().seen === "seen,extra", "subclass fields visible in the parent constructor");
