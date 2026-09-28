// What a context variable of Python's has is kept where JavaScript keeps the like, and follows what is being done from one language to the other and across `await`.
//
// What comes first is AsyncLocalStorage as an embedder has it, over the same place, so as to see that neither gets in the other's way.
import * as m from "./contexts.py";

const get = () => $vm.asyncContext();
const set = frame => { $vm.setAsyncContext(frame); };
class Frame { constructor(storage, value, prev, masked) { this.storage = storage; this.value = value; this.prev = prev; this.masked = masked; } }
let frameMutations = 0;
const isMasked = (frame, storage) => frame !== undefined && frame.masked !== undefined && frame.masked.includes(storage);
function unmask(masked, storage) {
    if (masked === undefined) return undefined;
    const rest = masked.filter(s => s !== storage);
    return rest.length === 0 ? undefined : rest.length === masked.length ? masked : rest;
}
function find(frame, storage) { for (let f = frame; f !== undefined; f = f.prev) if (f.storage === storage) return f; }
const push = (head, storage, value) => new Frame(storage, value, head, head === undefined ? undefined : unmask(head.masked, storage));
function copyUntil(from, stop, tail) {
    if (from === stop) return tail === undefined || tail.masked === from.masked ? tail : new Frame(tail.storage, tail.value, tail.prev, from.masked);
    const copied = [];
    for (let f = from; f !== stop; f = f.prev) copied.push(f);
    for (let i = copied.length - 1; i >= 0; i--) tail = new Frame(copied[i].storage, copied[i].value, tail, copied[i].masked);
    return tail;
}
function without(frame, storage) { const found = find(frame, storage); return found === undefined ? frame : copyUntil(frame, found, found.prev); }
class AsyncLocalStorage {
    static snapshot() { const context = get(); return (fn, ...args) => { const prev = get(); set(context); try { return fn(...args); } finally { set(prev); } }; }
    enterWith(store) { set(push(without(get(), this), this, store)); }
    run(store, callback, ...args) {
        const prior = get(), bound = find(prior, this);
        const beforeValue = bound !== undefined && !isMasked(prior, this) ? bound.value : undefined;
        if (Object.is(beforeValue, store)) return callback(...args);
        const mutations = frameMutations;
        const frame = push(bound === undefined ? prior : copyUntil(prior, bound, bound.prev), this, store);
        set(frame);
        try { return callback(...args); } finally {
            const head = get();
            if (mutations === frameMutations && (head === frame || (head !== undefined && head.prev === frame.prev && head.storage === this && head.masked === frame.masked)))
                set(prior === undefined ? undefined : new Frame(prior.storage, prior.value, prior.prev, prior.masked));
            else
                set(push(without(head, this), this, beforeValue));
        }
    }
    disable() { const top = get(); if (top !== undefined && !isMasked(top, this) && find(top, this) !== undefined) { top.masked = [...(top.masked ?? []), this]; frameMutations++; } }
    getStore() { const start = get(); if (start === undefined || isMasked(start, this)) return undefined; return find(start, this)?.value; }
}
// As the embedder checks it.
function chain() {
    const out = [];
    for (let f = get(), n = 0; f !== undefined; f = f.prev, n++) {
        if (!(f instanceof Frame || Object.getPrototypeOf(f) === null) || typeof f.storage !== "object" && typeof f.storage !== "function" || !(f.masked === undefined || Array.isArray(f.masked)) || n > 100) return "not a chain";
        out.push(f.storage instanceof AsyncLocalStorage ? "js" : "py");
    }
    return out.join(",");
}

const tick = () => Promise.resolve();
const results = [];
const show = async (label, f) => {
    // Each begins with nothing.
    set(undefined);
    try { print(label, "=>", JSON.stringify(await f())); } catch (e) { print(label, "!!", String(e)); }
};

await show("nothing has been set", () => [m.get(), chain(), m.size()]);
await show("set, and seen after await", async () => { m.put("a"); await tick(); const a = m.get(); await tick(); return [a, m.get(), chain()]; });
await show("what was left by the last is not seen", () => [m.get(), chain()]);
await show("two at once, each in a copy", async () => {
    const flow = async x => { m.put(x); await tick(); const a = m.get(); await tick(); return [a, m.get()]; };
    const both = await Promise.all([m.in_copy(flow, "one"), m.in_copy(flow, "two")]);
    return [both, m.get()];
});
await show("two at once, in no context of their own", async () => {
    // As with AsyncLocalStorage.enterWith(): what is set before the first await is set for what called it too.
    const flow = async x => { const before = m.get(); m.put(x); await tick(); return [before, m.get()]; };
    const one = flow("one"), between = m.get(), two = flow("two");
    return [await one, between, await two, m.get()];
});
await show("then() of a function of Python's", async () => { m.put("when it was put off"); const p = tick().then(m.get_whatever); m.put("afterwards"); return [await p, m.get()]; });
await show("a token, across await", async () => { m.put("first"); const t = m.put("second"); await tick(); const a = m.get(); m.reset(t); await tick(); return [a, m.get()]; });
await show("a token is used once", async () => { const t = m.put("x"); m.reset(t); try { m.reset(t); } catch (e) { return String(e).replace(/ at 0x[0-9a-f]+/g, ""); } });

// ---- What JavaScript waits for has a context of its own, as a Task of asyncio's has
await show("a coroutine", async () => { m.put("outside"); const r = await m.sets_and_waits("inside", tick); return [r, m.get()]; });
await show("a coroutine does not see what is set once it has begun", async () => { m.put("before"); const p = m.reads(tick).then(x => x); m.put("after"); return [await p, m.get()]; });
await show("two coroutines at once", async () => [await Promise.all([m.sets_and_waits("one", tick), m.sets_and_waits("two", tick)]), m.get()]);
await show("a token in a coroutine", async () => { m.put("outside"); return [await m.token_across("inside", tick), m.get()]; });
await show("JavaScript that a coroutine calls", async () => await m.calls(() => m.get(), tick));
await show("a coroutine that awaits another is one with it", async () => await m.awaits_inner(tick));
await show("a token of one is no good in another", async () => {
    let token; const first = m.calls(() => { token = m.put("x"); }, tick); await first;
    try { await m.calls(() => m.reset(token), tick); } catch (e) { return String(e).replace(/ at 0x[0-9a-f]+/g, ""); }
});
// It runs in the context of what resumes it, as a generator does in either language. So what it sets before it first waits is set for what asked, as with enterWith().
await show("an asynchronous generator", async () => { m.put("outside"); const seen = []; for await (const x of m.agen(tick)) seen.push([x, m.get()]); return seen; });
await show("an asynchronous generator of JavaScript's does the same", async () => {
    const als = new AsyncLocalStorage; als.enterWith("outside"); const seen = [];
    async function* agen() { als.enterWith("in the generator"); yield als.getStore(); await tick(); yield als.getStore(); }
    for await (const x of agen()) seen.push([x, als.getStore()]);
    return seen;
});

// ---- Context.run()
await show("a new context", () => { m.put("outside"); return [m.in_new(() => { const a = m.get(); m.put("inside"); return [a, m.get()]; }), m.get()]; });
await show("a context is kept", () => { const c = m.snapshot(); c.run(m.put, "kept"); return [m.get(), c.run(m.get), c.get(m.v), c.__len__()]; });
await show("what an async function does after await is not in the context", async () => {
    const c = m.snapshot(); await c.run(async () => { m.put("before await"); await tick(); m.put("after await"); }); return [c.get(m.v), m.get()];
});
await show("entered twice", () => { const c = m.snapshot(); try { c.run(() => c.run(m.get)); } catch (e) { return String(e).replace(/ at 0x[0-9a-f]+/g, ""); } });
await show("JavaScript throws out of it", () => { m.put("outside"); const c = m.snapshot(); try { c.run(() => { m.put("inside"); throw new Error("thrown"); }); } catch (e) { return [String(e), m.get(), c.get(m.v), c.run(m.get)]; } });

// ---- With AsyncLocalStorage
const als = new AsyncLocalStorage, other = new AsyncLocalStorage;
await show("one in the other", () => als.run(1, () => { m.put("x"); return other.run(2, () => { m.put_w("y"); return [als.getStore(), other.getStore(), m.get(), m.get_w(), chain()]; }); }));
await show("run() puts back its own and no more", () => { const inside = als.run(1, () => { m.put("set inside run()"); return chain(); }); return [inside, als.getStore() === undefined, m.get(), chain()]; });
await show("Context.run() puts back its own and no more", () => { m.put("outside"); const inside = m.in_copy(() => { m.put("inside"); als.enterWith("entered inside"); return chain(); }); return [inside, als.getStore(), m.get(), chain()]; });
await show("a new context has what AsyncLocalStorage has", () => als.run(1, () => m.in_new(() => [als.getStore(), m.get(), chain()])));
await show("both, across await", async () => await als.run(1, async () => { m.put("x"); await tick(); const a = [als.getStore(), m.get()]; als.enterWith(2); m.put("y"); await tick(); return [a, als.getStore(), m.get(), chain()]; }));
await show("snapshot() has Python's too", () => { m.put("then"); const snap = AsyncLocalStorage.snapshot(); m.put("now"); return [snap(m.get), m.get()]; });
await show("copy_context() has Python's alone", () => als.run("then", () => { const c = m.snapshot(); return als.run("now", () => c.run(() => als.getStore())); }));
await show("a coroutine, with both", async () => await als.run("store", async () => { m.put("outside"); return [await m.calls(() => [als.getStore(), m.get()], tick), als.getStore(), m.get()]; }));
await show("what is disabled stays so", () => als.run(1, () => { als.disable(); m.put("x"); const a = [als.getStore() === undefined, m.get(), chain()]; m.put("y"); m.in_new(() => a.push(als.getStore() === undefined)); return [...a, als.getStore() === undefined, m.get()]; }));
await show("Python's is not first", () => { m.put("x"); als.enterWith(1); other.enterWith(2); const a = chain(); m.put("y"); return [a, chain(), m.get(), als.getStore(), other.getStore()]; });
await show("taken out from under", () => { const t = m.put("x"); als.enterWith(1); m.reset(t); return [chain(), m.get(), als.getStore(), m.size()]; });
await show("there is one frame however many variables", () => { m.put("x"); m.put_w("y"); m.put("z"); return [chain(), m.size(), m.get(), m.get_w()]; });
await show("hot", async () => { let n = 0; for (let i = 0; i < 20000; i++) { m.put(i); if (i % 1000 === 0) await tick(); n += m.get() === i; } return [n, chain()]; });
