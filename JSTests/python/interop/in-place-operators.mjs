import m from "./in-place-operators.py";
function show(label, f) { try { print(label, "=>", String(f())); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
show("each of them", () => { let a = m.Acc(); const same = a; a += 1; a -= 2; a *= 3; a /= 4; a %= 5; a **= 6; a <<= 7; a >>= 8; a &= 9; a |= 10; a ^= 11; return [a, a === same].join(" "); });
show("and not these", () => { let a = m.Acc(); const same = a; a = a + 1; a = a - 2; a = a * 3; a = a / 4; a = a % 5; a = a ** 6; a = a << 7; a = a >> 8; a = a & 9; a = a | 10; a = a ^ 11; return [a, a === same, same].join(" "); });
show("nor these", () => { let a = m.Acc(); const same = a; a++; ++a; a--; --a; return [a, a === same, same].join(" "); });
show("property", () => { const o = { a: m.Acc() }; const same = o.a; o.a += 1; o.a *= 2; return [o.a, o.a === same].join(" "); });
show("element", () => { const o = [m.Acc()]; const same = o[0]; o[0] += 1; o[0] |= 2; const k = "x", p = { x: m.Acc() }; p[k] -= 3; return [o[0], o[0] === same, p.x].join(" "); });
show("closure", () => { let a = m.Acc(); const same = a; (() => { a += 1; a %= 2; })(); return [a, a === same].join(" "); });
globalThis.g = m.Acc();
show("global", () => { const same = g; g += 1; g **= 2; return [g, g === same].join(" "); });
show("private field and static", () => { class K { #a = m.Acc(); static s = m.Acc(); go() { this.#a += 1; K.s <<= 2; return [this.#a, K.s].join(" "); } } return new K().go(); });
show("attribute of something of Python's", () => { const h = m.Acc(); h.inner = m.Acc(); const same = h.inner; h.inner += 5; return [h.inner, h.inner === same].join(" "); });
show("only __add__", () => { let a = m.OnlyPlain(1); const same = a; a += 2; return [a, a === same, same].join(" "); });
show("number on the left", () => { let n = 5; n += m.OnlyPlain(1); return n; });
show("declines", () => { let a = m.Declines(); a += 1; return a; });
show("replaces", () => { let a = m.Replaces(); a += 1; return a; });
show("with a string", () => { let a = m.Acc(); a += "s"; return a; });
show("value of the expression", () => { let a = m.Acc(); const v = (a += 1); return v === a; });
show("set", () => { let s = m.new_set(); const same = s; s |= m.set3; s -= m.set1; return [s, s === same, same].join(" "); });
show("set, not in place", () => { let s = m.new_set(); const same = s; s = s | m.set3; return [s, s === same, same].join(" "); });
show("dict", () => { let d = m.new_dict(); const same = d; d |= m.dict_b; return [d, d === same].join(" "); });

// The same code, until it is compiled and after, with numbers before and after.
function plus(x, y) { x += y; return x; }
function plain(x, y) { x = x + y; return x; }
function times(x, y) { x *= y; return x; }
function shift(x, y) { x <<= y; return x; }
function power(x, y) { x **= y; return x; }
function inlined(x, y) { return plus(x, y); }
noInline(plus); noInline(plain); noInline(times); noInline(shift); noInline(power); noInline(inlined);
show("numbers first", () => { let t = 0; for (let i = 0; i < 100000; i++) t += plus(i, 1) + plain(i, 1) + times(i, 2) + shift(i & 7, 1) + power(2, i & 3); return t; });
show("then Python", () => { const a = m.Acc(); return [plus(a, 1) === a, plain(a, 1) === a, times(a, 2) === a, shift(a, 3) === a, power(a, 4) === a, a].join(" "); });
show("Python hot", () => { const a = m.Acc(); let same = 0, other = 0; for (let i = 0; i < 20000; i++) { if (plus(a, 0) === a) same++; if (plain(a, 0) !== a) other++; if (inlined(a, 0) === a) same++; a.log.length = 0; } return [same, other].join(" "); });
show("and numbers still", () => [plus(1, 2), plain(1, 2), times(3, 4), shift(1, 4), power(2, 5), plus("a", "b")].join(" "));
