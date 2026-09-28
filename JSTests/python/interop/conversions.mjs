import m from "./conversions.py";
function show(label, f) { try { print(label, "=>", String(f())); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
show("Number()", () => [Number(m.Num()), Number(m.Idx()), Number(m.Big()), Number(m.OnlyInt()), Number(m.Plain()), Number(m.MyInt(5)), Number(m.MyFloat(2.5)), Number(m.MyStr("12"))]);
show("unary plus", () => [+m.Num(), +m.Idx(), +m.MyInt(5)]);
show("unary plus of a big one", () => +m.Big());
show("String() and templates", () => [String(m.Num()), `${m.Num()}`, String(m.Plain()), `${m.MyInt(5)}`, String(m.t), `${m.z}`]);
show("Math", () => [Math.floor(m.Num()), Math.max(m.Idx(), 1), Math.round(m.MyFloat(2.5))]);
show("as an index and a key", () => [["a", "b", "c", "d"][m.Idx()], ({ plain: 1 })[m.Plain()], "abcd".charAt(m.Idx()), [1, 2, 3, 4, 5].slice(m.Idx())]);
show("isNaN, parseInt, BigInt, >>>", () => [isNaN(m.Num()), isNaN(m.Plain()), parseInt(m.MyStr("42")), BigInt(m.Idx()), m.Idx() >>> 0]);
show("new Date, new Array, repeat", () => [new Date(m.Idx()).getTime(), "ab".repeat(m.Idx()), new Array(m.MyInt(2)).length]);
show("bad", () => Number(m.Bad()));
show("JSON.stringify", () => JSON.stringify([m.MyInt(5), m.MyFloat(2.5), m.MyStr("s"), m.t]));
