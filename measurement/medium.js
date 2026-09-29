// Many functions, each called a few hundred times, with polymorphic property reads.
const shapes = [];
for (let i = 0; i < 12; ++i) { const o = {}; for (let j = 0; j < i; ++j) o["p" + j] = j; o.tag = i; o.v = i * 2; shapes.push(o); }
const fns = [];
for (let i = 0; i < 400; ++i)
    fns.push(new Function("o", "s", `let r = o.tag + o.v + s.length + (o.missing${i % 7} === undefined ? 1 : 0); return r + ${i};`));
let total = 0;
for (let round = 0; round < 300; ++round) {
    for (let i = 0; i < fns.length; ++i)
        total += fns[i](shapes[(i + round) % shapes.length], "str" + round);
}
print(total);
