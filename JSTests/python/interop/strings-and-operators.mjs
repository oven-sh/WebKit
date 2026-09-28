// x + "s" and "s" + x, where x is Python's. It is x that is asked, and the string is not: x.__add__("s"), or x.__radd__("s"). If x has nothing to say they are concatenated, as
// any object and a string are, so that "value: " + x says what x is. This holds however the addition is compiled, and JavaScript compiles few additions of strings to additions.
import * as m from "./strings-and-operators.py";
const show = (label, f) => {
    let result;
    try { result = f(); result = typeof result + " " + String(result); } catch (e) { result = "!! " + String(e); }
    const took = m.took();
    print(label, "=>", result, took ? "   [" + took + "]" : "");
};
// What is evaluated is noted where what Python is asked is noted, to see which comes first.
const E = value => { m.log.push("evaluate " + (typeof value === "string" ? value : "path")); return value; };
const path = m.Path("p"), text = m.Text("t"), plain = m.Plain(), declines = m.Declines(), counts = m.Counts(), raises = m.Raises();

for (let round = 0; round < 3; round++) {
    print(["--- in the interpreter", "--- again", "--- after ten thousand times"][round]);
    show("path + s", () => path + "/x");
    show("s + path", () => "/x" + path);
    show("s + path + s", () => "a" + path + "b");
    show("path + s + s", () => path + "a" + "b");
    show("s + path + path", () => "a" + path + path);
    show("s + text + s", () => "a" + text + "b");
    show("s + text + number", () => "a" + text + 1);
    show("s + counts + number", () => "abc" + counts + 1);
    show("s + counts + s + number", () => "abc" + counts + "x" + 1);
    show("number + (s + counts) + number", () => 1 + ("abc" + counts) + 1);
    show("plain + s", () => plain + "!");
    show("s + plain", () => "value: " + plain);
    show("s + plain + s", () => "<" + plain + ">");
    show("declines + s", () => declines + "!");
    show("s + declines + s", () => "<" + declines + ">");
    show("raises + s", () => raises + "!");
    show("s + raises + s", () => "<" + raises + ">");
    show("path + empty", () => path + "");
    show("empty + path", () => "" + path);
    show("plain + empty", () => plain + "");
    show("empty + counts", () => "" + counts);
    show("s += path", () => { let s = "a"; s += path; return s; });
    show("s += path + s", () => { let s = "a"; s += path + "b"; return s; });
    show("path += s", () => { let p = path; p += "a"; return p; });
    show("path += s + number", () => { let p = path; p += "a" + 1 + "b"; return p; });
    show("in place += s", () => { let i = m.InPlace(); i += "a"; i += "b" + 1 + "c"; i += ""; return i; });
    show("template literal", () => `${path} ${plain}`);
    show("String()", () => String(path));
    show("concat()", () => "a".concat(path));
    show("str subclass", () => [m.Str("a") + "b", "b" + m.Str("a"), "<" + m.Str("a") + ">", typeof (m.Str("a") + "b")]);
    show("int subclass", () => [m.Int(5) + "b", "b" + m.Int(5), "<" + m.Int(5) + ">"]);
    show("in order", () => E("a") + E(path) + E("b") + E("c"));
    show("in order, with literals", () => "a" + E(path) + "b" + E("c") + "d" + "e" + E("f"));
    show("in order, assigning", () => { let p = E(path); p += E("a") + "b" + E("c"); return p; });
    for (const name of ["tuple", "dict", "set", "range", "complex", "none", "class", "big"])
        show(name, () => "<" + m.values.get(name) + ">");
    show("tuple + s", () => m.values.get("tuple") + "s");
    show("bytes + s", () => m.values.get("bytes") + "s");
    if (round === 1) {
        // Long enough for all of it to be compiled, by every compiler there is.
        const fs = [x => "a" + x + "b", x => x + "a" + "b", x => x + "", x => "" + x, x => { let s = "a"; s += x + "b"; return s; }, x => { let p = x; p += "a" + 1 + "b"; return p; }];
        for (const f of fs) {
            for (let i = 0; i < 10000; i++) {
                for (const x of [path, text, plain, declines, counts, "s", 1])
                    f(x);
            }
        }
        m.took();
    }
}
