// A generator is a generator to either language, whichever it is written in. How it is told to stop before it has finished is up to the language that it is written in, whichever asks.
import * as m from "./closing-generators.py";
const show = (label, f) => { try { print(label, "=>", JSON.stringify(f())); } catch (e) { print(label, "!!", String(e)); } };
show("break", () => { for (const x of m.gen()) break; return m.take(); });
show("return()", () => { const g = m.gen(); g.next(); const r = g.return(5); return [r, m.take()]; });
show("return() before it begins", () => { const g = m.gen(); const r = g.return(5); return [r, m.take(), g.next()]; });
show("destructuring", () => { const [a] = m.gen(); return [a, m.take()]; });
show("throw()", () => { const g = m.gen(); g.next(); try { g.throw(new Error("e")); } catch (e) { return [String(e), m.take()]; } });
show("ignores", () => { const g = m.ignores(); g.next(); return g.return(5); });
show("returns a value", () => { const g = m.returns_value(); g.next(); return g.return(5); });
show("typeof", () => { const g = m.gen(); return [typeof g.next, typeof g.return, typeof g.throw, typeof g.close, typeof g.send]; });
show("a transaction that is left early", () => { for (const x of m.in_transaction()) break; return m.take(); });
show("a transaction that is gone through", () => { for (const x of m.in_transaction()) { } return m.take(); });
show("a transaction that JavaScript throws out of", () => { try { for (const x of m.in_transaction()) throw new Error("e"); } catch (e) { } return m.take(); });

// One that is written in JavaScript
const events = [];
function* js() { try { yield 1; yield 2; } catch (e) { events.push("caught " + e); throw e; } finally { events.push("finally"); } }
const took = () => events.splice(0);
show("closed by Python", () => { const g = js(); g.next(); return [m.close(g) === undefined, took(), g.next()]; });
show("closed by Python before it begins", () => { const g = js(); return [m.close(g) === undefined, took(), g.next()]; });
show("left early by Python", () => [m.first_of(js()), took()]);
show("gone through by Python", () => [m.go_through(js()), took()]);
show("thrown into by Python", () => { const g = js(); g.next(); try { m.throw(g, new RangeError("r")); } catch (e) { return [String(e), took()]; } });
function* stubborn() { try { yield 1; } finally { yield 2; } }
show("one that yields on being told to stop", () => { const g = stubborn(); g.next(); return m.close(g); });
show("what they are to Python", () => [m.kind(js()), m.kind(m.gen())].map(String));

// What one of Python's has, to JavaScript
show("send", () => { const g = m.echo(); return [g.next().value, g.send(5), g.next(6).value, m.send(g, 7)]; });
show("attributes", () => { const g = m.gen(); return [g.__name__, g.gi_running, g.gi_suspended, typeof g.gi_frame, g.gi_code.co_name, String(g).replace(/0x[0-9a-f]+/, "0x"), g[Symbol.iterator]() === g, g instanceof m.gen().constructor]; });
show("still a generator to JavaScript", () => { const g = m.gen(); const P = Object.getPrototypeOf(Object.getPrototypeOf(function* () { }.prototype)); return [Object.prototype.toString.call(g), P.isPrototypeOf(g), [...m.gen()], Array.from(m.gen()), Math.max(...m.gen())]; });
// What one of JavaScript's has, to Python. There is no code object for it, nor a frame object.
show("attributes of JavaScript's", () => { const g = js(); const before = String(m.look(g)); g.next(); const during = String(m.look(g)); g.return(); return [before, before === during, before === String(m.look(g))]; });
took();
m.take();
show("hot", () => { let t = 0; for (let i = 0; i < 20000; i++) for (const x of m.gen()) { t += x; break; } return [t, m.take().length]; });
