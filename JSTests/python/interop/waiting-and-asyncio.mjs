import m from "./waiting-and-asyncio.py";
const out = [];
async function show(label, f) { try { out.push(label + " => " + JSON.stringify(await f())); } catch (e) { out.push(label + " !! " + String(e)); } }
await show("which loop", () => m.asks());
for (const kind of ["C", "Python"]) {
    await show(kind + ": a coroutine awaits one that is pending", () => m.awaits(m.pending(kind)));
    await show(kind + ": a coroutine awaits one that is done", () => m.awaits(m.done(kind, "result")));
    for (const how of ["result", "exception", "cancelled"])
        await show(kind + ": JavaScript awaits one, " + how, () => m.done(kind, how));
}
await show("JavaScript awaits one that is settled later", () => { const f = m.pending("C"); Promise.resolve().then(() => f.set_result("later")); return f; });
await show("the one promise", () => { const f = m.done("C", "result"); return f.then(() => {}) instanceof Promise; });
await show("what is no coroutine", () => m.Generator());
await show("with a loop running", () => m.run_in_a_loop());
print(out.join("\n"));
