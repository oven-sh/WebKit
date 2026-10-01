import { shouldBe } from "./resources/assert.js";
// Imported in an order that is neither the exporter's, nor code point order.
import { Z, aa, _, b, \u03c0, A, $, z0, a, \u{1D4D0}, B, \u00e4 } from "./import-slots/names.js";
import { "string name" as spaced, "" as empty, "dashed-name" as dashed } from "./import-slots/names.js";
import theDefault, { default as alsoDefault } from "./import-slots/names.js";

function all() { return [a, A, aa, b, B, Z, z0, _, $, \u00e4, \u03c0, \u{1D4D0}, spaced, dashed, empty, theDefault, alsoDefault].join(); }
const expected = "a,A,aa,b,B,Z,z0,_,$,a-umlaut,pi,astral,string name,dashed-name,empty,default,default";
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(all(), expected);
// Each one alone, from its own function, so each access site links separately.
const readers = [() => a, () => A, () => aa, () => b, () => B, () => Z, () => z0, () => _, () => $, () => \u00e4, () => \u03c0, () => \u{1D4D0}, () => spaced, () => dashed, () => empty, () => theDefault, () => alsoDefault];
shouldBe(readers.map(reader => reader()).join(), expected);
shouldBe(readers.reverse().map(reader => reader()).reverse().join(), expected);
