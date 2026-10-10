//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(condition, what) {
    if (!condition)
        throw new Error(what);
}
function applies(f, pattern) {
    let remarks = aotRemarks(f.name);
    check(!remarks || remarks.some(remark => remark === pattern || remark.startsWith(pattern + ":")), pattern + " does not apply to " + f.name + ": " + (remarks || []).join(" "));
}

class KeptInEnvironment { }
class KeptInCapture { }
class KeptInFrame { }

function makeWithEnvironment() {
    let kept = new KeptInEnvironment;
    return { read() { return kept; }, replace(value) { kept = value; } };
}
function makeWithCapture() {
    const kept = new KeptInCapture;
    return () => kept;
}
async function suspends(promise) {
    const kept = new KeptInFrame;
    await promise;
    return kept;
}
noInline(makeWithEnvironment);
noInline(makeWithCapture);
noInline(suspends);

globalThis.holders = [makeWithEnvironment(), makeWithCapture()];
suspends(new Promise(resolve => { globalThis.resolveLater = resolve; }));
applies(makeWithEnvironment, "allocation-escapes:environment");
applies(makeWithCapture, "captures-by-value");

const snapshot = JSON.parse(generateHeapSnapshotForGCDebugging());
const classOf = new Map;
const labelOf = new Map;
for (let i = 0; i < snapshot.nodes.length; i += 7) {
    classOf.set(snapshot.nodes[i], snapshot.nodeClassNames[snapshot.nodes[i + 2]]);
    labelOf.set(snapshot.nodes[i], snapshot.labels[snapshot.nodes[i + 4]]);
}
const targets = new Map;
const holders = new Map;
for (let i = 0; i < snapshot.edges.length; i += 4) {
    let [from, to] = [snapshot.edges[i], snapshot.edges[i + 1]];
    if (!targets.has(from))
        targets.set(from, []);
    targets.get(from).push(to);
    if (!holders.has(to))
        holders.set(to, []);
    holders.get(to).push(classOf.get(from));
}
const reached = new Set;
const worklist = [...classOf].filter(([node, className]) => className === "<root>" || className === "global").map(([node]) => node);
for (let i = 0; i < snapshot.roots.length; i += 3)
    worklist.push(snapshot.roots[i]);
while (worklist.length) {
    let node = worklist.pop();
    if (reached.has(node))
        continue;
    reached.add(node);
    for (let target of targets.get(node) || [])
        worklist.push(target);
}

for (let [name, ...expectedHolders] of [["KeptInEnvironment", "JSLexicalEnvironment"], ["KeptInCapture", "Function", "JSLexicalEnvironment"], ["KeptInFrame", "JSLexicalEnvironment"]]) {
    let nodes = [...classOf].filter(([node, className]) => className === name).map(([node]) => node);
    check(nodes.length === 1, nodes.length + " cells of class " + name);
    check((holders.get(nodes[0]) || []).some(holder => expectedHolders.includes(holder)), name + " is held by " + (holders.get(nodes[0]) || []).join());
    check(reached.has(nodes[0]), "no path from a root to " + name);
}

const labels = [...labelOf.values()].filter(label => label && label.split("#")[0] === "makeWithCapture");
check(labels.length === 1, labels.length + " functions are labeled makeWithCapture");
check(isAOTCompiled(makeWithCapture) ? /^makeWithCapture#\d+$/.test(labels[0]) : labels[0] === "makeWithCapture", "the label is " + labels[0]);
