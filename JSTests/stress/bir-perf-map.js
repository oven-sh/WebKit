//@ skip if !$isFTLPlatform
//@ skip if $hostOS != "linux"
//@ requireOptions("--useDollarVM=1", "--writeCModulePerfMap=1")

// With Options::writeCModulePerfMap, every C function that gets machine code gets a line in /tmp/perf-<pid>.map, the
// file perf reads to name code that is in no image: "<start> <size> C:<name>", in hex.
load("./resources/bir-assembler.js", "caller relative");
// The file is what Linux's perf reads; nothing writes one anywhere else.
if ($vm.cModuleHost()[1] !== 0) {
    print("perf map skipped: not Linux");
    quit();
}

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });

// static int helper(int x) { return x * 3; }   (noinline)     int first(int x) { return helper(x) + 1; }     int second(int x) { return x; }
// int third(int x) { return inlined(x); }   static int inlined(int x) { return x + 5; }   has no code of its own until something needs it
const names = { helper: "perf map helper", first: "perfMapFirst", second: "perf_map_second", inlined: "perfMapInlined", third: "perfMapThird" };
const intToInt = { ret: T.i32, params: [T.i32] };
const exported = $vm.cModule(assemble({
    sigs: [intToInt],
    funcs: [
        { name: names.helper, sig: 0, noinline: true, blocks: [[["ConstI32", s(3)], ["Mul", 0, 1], ["Ret", 2]]] },
        { name: names.first, sig: 0, exported: true, blocks: [[["Call", 0, 1, 0], ["ConstI32", s(1)], ["Add", 1, 2], ["Ret", 3]]] },
        { name: names.second, sig: 0, exported: true, blocks: [[["Ret", 0]]] },
        { name: names.inlined, sig: 0, blocks: [[["ConstI32", s(5)], ["Add", 0, 1], ["Ret", 2]]] },
        { name: names.third, sig: 0, exported: true, blocks: [[["Call", 3, 1, 0], ["Ret", 1]]] },
    ],
    exports: [names.first, names.second, names.third].map((name, i) => ({ name, func: [1, 2, 4][i], ret: FFI.i32, args: [FFI.i32] })),
}));
eq(exported[names.first](4), 13, "first");
eq(exported[names.second](4), 4, "second");
eq(exported[names.third](4), 9, "third");

const path = `/tmp/perf-${$vm.getpid()}.map`;
const lines = readFile(path).split("\n").filter(line => line.length);
const ranges = new Map;
for (const line of lines) {
    const match = /^([0-9a-f]+) ([0-9a-f]+) C:(.+)$/.exec(line);
    if (!match)
        throw new Error(`not a line of a perf map: "${line}"`);
    const [, start, size, name] = match;
    if (ranges.has(name))
        throw new Error(`${name} is there twice`);
    ranges.set(name, { start: parseInt(start, 16), size: parseInt(size, 16) });
}
// Every function with code is there once, under its own name, and no two of them share a byte.
for (const name of Object.values(names)) {
    if (!ranges.has(name))
        throw new Error(`no line for "${name}" in:\n${lines.join("\n")}`);
    if (!(ranges.get(name).size > 0))
        throw new Error(`"${name}" has no size`);
}
eq(ranges.size, Object.keys(names).length, "lines");
for (const name of [names.first, names.second, names.third])
    eq(ranges.get(name).start, Number(exported[name].ptr), `where ${name} starts`);
const sorted = [...ranges.values()].sort((a, b) => a.start - b.start);
for (let i = 1; i < sorted.length; i++) {
    if (sorted[i - 1].start + sorted[i - 1].size > sorted[i].start)
        throw new Error("two functions overlap");
}

// A second module's functions are added to the same file.
$vm.cModule(assemble({ sigs: [intToInt], funcs: [{ name: "perfMapLater", sig: 0, exported: true, blocks: [[["Ret", 0]]] }], exports: [{ name: "perfMapLater", func: 0, ret: FFI.i32, args: [FFI.i32] }] }));
const later = readFile(path).split("\n").filter(line => line.length);
eq(later.length, lines.length + 1, "lines after a second module");
if (!later[later.length - 1].endsWith(" C:perfMapLater"))
    throw new Error(`the last line is "${later[later.length - 1]}"`);

// Nothing of it is left in /tmp.
writeFile(path, "");
print("perf map ok");
