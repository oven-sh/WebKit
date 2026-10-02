//@ skip if $architecture != "arm64"
//@ requireOptions("-m", "--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

let n = 0;
export const state = { name: "state of an instance" };
export function count() { return ++n; }
export const closure = () => () => count() + state.name.length;

if (!globalThis.isFurther) {
    globalThis.isFurther = true;
    const dropped = [];
    const kept = [];
    const closures = [];
    for (let i = 0; i < 40; i++) {
        const ns = await importInNewLoader("./aot-loader-collection.js");
        ns.count();
        if (i % 4)
            dropped.push(new WeakRef(ns.state));
        else {
            kept.push(new WeakRef(ns.state));
            closures.push(ns.closure());
        }
    }
    setTimeout(() => {
        fullGC();
        fullGC();
        const alive = refs => refs.filter(ref => ref.deref()).length;
        if (alive(dropped) > 5)
            throw new Error(`${alive(dropped)} of ${dropped.length} instances that were let go of are still there`);
        if (alive(kept) !== kept.length)
            throw new Error(`only ${alive(kept)} of ${kept.length} instances with a live closure are still there`);
        for (const f of closures) {
            if (f() !== 22 || f() !== 23)
                throw new Error("a closure lost its instance");
        }
    }, 0);
}
