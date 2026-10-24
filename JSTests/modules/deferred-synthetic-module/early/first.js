import { peek, viaBarrel } from "./main.js";
(globalThis.earlyLog ??= []).push("first:" + peek() + ":" + viaBarrel());
// Often enough for the optimizing tiers: each read still finds that the binding has no value yet.
for (let i = 0; i < 1e4; ++i) {
    if (peek() !== "undefined|undefined|" || viaBarrel() !== "undefined")
        throw new Error("bad value before the module was evaluated");
}
