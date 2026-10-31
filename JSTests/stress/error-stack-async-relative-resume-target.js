//@ requireOptions("--useAsyncStackTrace=1")

async function leaf() {
    await Promise.resolve();
    return new Error().stack;
}
async function parent() {
    const first = await leaf();
    const second = await leaf();
    return [first, second];
}
let traces;
parent().then(value => traces = value);
drainMicrotasks();
if (!traces || traces.length !== 2)
    throw new Error("Async stacks were not captured");
for (let i = 0; i < traces.length; ++i) {
    const trace = traces[i];
    const parentFrame = trace.split("\n").find(line => /\bparent(?:\s|@|\()/.test(line));
    const position = /:(\d+):(\d+)\)?$/.exec(parentFrame);
    if (!position || +position[1] !== 8 + i)
        throw new Error(JSON.stringify({ expectedLine: 8 + i, trace }));
}
