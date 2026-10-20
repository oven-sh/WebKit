function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

// Records what the promise of an import() does.
globalThis.track = (name, promise) => {
    states[name] = "pending";
    promise.then(
        () => { states[name] = "fulfilled"; settled.push(name); },
        error => { states[name] = `rejected (${error.message})`; settled.push(name); });
};

// The bodies that ran and the state of each import, after the jobs that were queued ran.
function snapshot() {
    drainMicrotasks();
    return `${log.join(" ")} | ${Object.keys(states).map(name => `${name}=${states[name]}`).join(" ")}`;
}

// Imports root.js of a graph. "again" is one more import of a module that a body imported while that module was
// on the stack. When a module waits for `gate`, the result has the snapshot before the gate opened too.
function evaluate(graph, again) {
    globalThis.log = [];
    globalThis.states = {};
    globalThis.settled = [];
    globalThis.gate = new Promise(resolve => { globalThis.openGate = resolve; });
    track("root", import(`./${graph}/root.js`));
    const beforeGate = snapshot();
    openGate();
    const afterGate = snapshot();
    track("again", import(`./${graph}/${again}.js`));
    const result = snapshot();
    return beforeGate === afterGate ? result : `${beforeGate} -> ${result}`;
}
