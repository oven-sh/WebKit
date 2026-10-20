import defer * as a from "./a.js";
log.push("b1");
track("a", import("./a.js"));
drainMicrotasks();
globalThis.readDeferred = () => {
    try {
        return `value=${a.value}`;
    } catch (error) {
        return `throws (${error.message})`;
    }
};
log.push(`b2(a=${states.a})`);
