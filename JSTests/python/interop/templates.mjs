import { greeting, tag, make, with_javascript_values } from "./templates.py";
const show = (label, f) => { try { print(label, "=>", String(f())); } catch (e) { print(label, "!!", String(e)); } };
const list = (strings, ...values) => JSON.stringify([[...strings], values]);

show("what it is", () => [typeof greeting, String(greeting)]);
show("its parts", () => [JSON.stringify([...greeting.strings]), JSON.stringify([...greeting.values]), greeting.strings.length]);
// What a template has is what a tag function is given.
show("given to a tag function", () => list(greeting.strings, ...greeting.values));
show("String.raw", () => String.raw({ raw: greeting.strings }, ...greeting.values));
show("gone through", () => [...greeting].map(part => typeof part === "string" ? JSON.stringify(part) : `${part.expression}=${part.value}!${part.conversion}:${part.format_spec}`));
show("an interpolation", () => { const [i] = greeting.interpolations; return [i.value, i.expression, i.conversion, JSON.stringify(i.format_spec)]; });
// And a function of Python's can be one.
show("a function of Python's as a tag", () => tag`a${1}b${"two"}c`);
show("made with a value of JavaScript's", () => { const o = { toString() { return "object"; } }; return [make(o).values[0] === o, make(undefined).values[0], make(null).values[0]]; });
show("values of JavaScript's in Python", () => { const t = with_javascript_values(); return [t.values[0] instanceof Map, t.values[1], t.values[2] === Math.PI, t.interpolations[2].format_spec]; });
show("+", () => [(greeting + make(1)).strings.length, String(make(1) + make(2))]);
show("+ with a string", () => greeting + "x");
