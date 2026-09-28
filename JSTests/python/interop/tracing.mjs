// What is told of Python's frames when there are frames of JavaScript's among them, and how deep in Python's calls a thread is counted to be.
import * as m from "./tracing.py";
const show = (label, f) => { try { const r = f(); print(label, "=>", String(r)); } catch (e) { print(label, "!!", String(e)); } finally { m.stop(); } };
const traced = f => { m.start(); try { f(); } catch (e) { } return m.stop().map(e => `${e[0]} ${e[1]} ${e[2]}${e[3] === undefined ? "" : " " + e[3]}`).join("; "); };
const profiled = f => { m.start_profile(); try { f(); } catch (e) { } return m.stop().map(e => `${e[0]} ${e[1]}${e[2] === undefined ? "" : " " + e[2]}`).join("; "); };

show("called from JavaScript", () => traced(() => m.leaf(1)));
show("Python, JavaScript, Python", () => traced(() => m.calls_back(x => m.leaf(x) * 2, 1)));
show("thrown by JavaScript, caught by Python", () => traced(() => m.catches(() => { throw new TypeError("js"); })));
show("thrown by JavaScript, through Python", () => traced(() => m.lets_through(() => { throw new RangeError("js"); })));
show("raised by Python, through JavaScript, caught by Python", () => traced(() => m.catches(() => m.raises())));
show("raised by Python, caught by JavaScript", () => traced(() => m.calls_back(() => { try { m.raises(); } catch (e) { return 5; } }, 0)));
show("a generator gone through by JavaScript", () => traced(() => { for (const x of m.gen()) { } }));
show("a generator left early by JavaScript", () => traced(() => { for (const x of m.gen()) break; }));
show("f_back passes over JavaScript", () => m.calls_back(() => m.backs(), 0));
show("profile", () => profiled(() => m.calls_back(function named(x) { return m.leaf(x); }, 1)));
show("profile, thrown", () => profiled(() => m.catches(function thrower() { throw new Error("e"); })));

// The count of frames
const base = m.depth();
show("depth from JavaScript", () => m.depth() - base);
show("depth by way of JavaScript", () => m.calls_back(() => m.depth(), 0) - base);
show("depth after an exception through both", () => { try { m.lets_through(() => m.lets_through(() => { throw new Error("x"); })); } catch (e) { } return m.depth() - base; });
show("depth after Python's through both", () => { try { m.lets_through(() => m.lets_through(() => m.raises())); } catch (e) { } return m.depth() - base; });
show("depth after a generator is left", () => { for (const x of m.gen()) break; const g = m.gen(); g.next(); return m.depth() - base; });
show("both ways down", () => { let n = 0; const down = k => { n++; return m.recurse_with(down, k + 1); }; try { down(0); } catch (e) { return [e.name, n > 400, m.depth() - base]; } });
show("hot, then traced", () => { let t = 0; for (let i = 0; i < 100000; i++) t += m.leaf(i); return [t, traced(() => m.leaf(1))]; });
show("traced, then hot", () => { let t = 0; for (let i = 0; i < 100000; i++) t += m.leaf(i); return [t, m.depth() - base]; });
