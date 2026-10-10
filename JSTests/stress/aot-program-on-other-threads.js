//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

var sticky = /a+/y;
var made = 0;
class Empty { }
class Base { #secret = 1; get secret() { return this.#secret; } constructor() { made++; } }
class Derived extends Base { field = [1, 2, 3]; method(x = 2) { return this.field.map(v => v * x).join("") + super.secret; } }
function tag(strings, ...values) { return strings.raw.join("|") + values.length; }
function site() { return tag`a${1}b`; }
function* each(n) { for (let i = 0; i < n; i++) yield { i, big: 12345678901234567890n, text: `t${i}` }; }

function work()
{
    let out = "";
    for (let i = 0; i < 3000; i++) {
        const text = "x" + i + "-aab-" + (i % 7) + "Z";
        const last = [...each(3)].at(-1);
        sticky.lastIndex = 0;
        sticky.test("aaa");
        out = [
            /(\d+)-(a+)b/.exec(text)[2], text.replace(/[0-9]/g, "#").length, /z$/i.test(text), text.split(/-/).length,
            text.match(/(?<n>\d)Z/).groups.n, text.search(/aab/), sticky.lastIndex, [...text.matchAll(/a/g)].length,
            new Derived().method(), new Empty() instanceof Empty, last.text, last.big % 97n, site(), { a: 1, b: [i & 1] }.b[0],
        ].join();
    }
    return out + "," + made;
}

function report()
{
    return JSON.stringify([work(), [work, site, each, tag, Derived.prototype.method].map(isAOTCompiled)]);
}

if (globalThis.isAgent)
    $.agent.report(report());
else {
    const count = 6;
    for (let i = 0; i < count; i++)
        $.agent.start(`globalThis.isAgent = true; load("aot-program-on-other-threads.js");`);
    const reports = [];
    while (reports.length < count) {
        const next = $.agent.getReport();
        if (next === null)
            $.agent.sleep(1);
        else
            reports.push(next);
    }
    const expected = JSON.stringify(["aa,12,true,3,3,6,3,2,2461,true,t2,3,a|b1,1,3000", [true, true, true, true, true]]);
    for (const actual of [...reports, report()]) {
        if (actual !== expected)
            throw new Error(`expected ${expected} but got ${actual}`);
    }
}
