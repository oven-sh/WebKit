import { counter as aliasCounter, Api as HttpApi } from "../bun-tdz-error-names-the-import.js";
import { shouldBe, shouldThrow } from "../resources/assert.js";

function tdz(name) {
    return `ReferenceError: Cannot access '${name}' before initialization.`;
}

class Store {
    api = new HttpApi;
}

shouldThrow(() => HttpApi, tdz("HttpApi"));
shouldThrow(() => typeof (aliasCounter), tdz("aliasCounter"));
shouldThrow(() => { ++aliasCounter; }, tdz("aliasCounter"));
shouldThrow(() => { aliasCounter += 1; }, tdz("aliasCounter"));

// The field initializers of a class run in a frame that stack traces hide.
shouldThrow(() => new Store, tdz("HttpApi"));
shouldThrow(() => { class Registry { static shared = new HttpApi; } }, tdz("HttpApi"));

// So do the parameter defaults of an async function that awaits. The second one is called by a reaction job, with no
// JavaScript caller.
shouldBe(await (async (first = aliasCounter) => { await null; })().then(() => "fulfilled", String), tdz("aliasCounter"));
shouldBe(await Promise.resolve().then(async (first = new HttpApi) => { await null; }).then(() => "fulfilled", String), tdz("HttpApi"));

export const checked = 8;
