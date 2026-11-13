import { shouldBe } from "./resources/assert.js";

// A load by name (import(), the load of an entry point) has its registry entry from the call on, ahead of its fetch.
// So whoever removes the module from the registry finds a load that is at its fetch. That load does not put the module
// back under the key afterwards: it joins what is registered there when its fetch settles, or ends on the entry that was removed.

const counted = "./bun-load-by-name-owns-its-entry/counted.js";
const remove = () => $vm.removeModuleRegistryEntry(counted);
globalThis.evaluations = 0;

// Removed while the load is at its fetch, and nothing asks for the key until the load has ended.
{
    const inFlight = import(counted);
    shouldBe(remove(), true);
    const first = await inFlight;
    shouldBe(first.instance, 1);
    shouldBe(remove(), false);
    const second = await import(counted);
    shouldBe(second.instance, 2);
    shouldBe(await import(counted), second);
}

// Removed while the load is at its fetch, and the key is asked for again before the fetch has settled.
{
    shouldBe(remove(), true);
    const inFlight = import(counted);
    shouldBe(remove(), true);
    const later = import(counted);
    shouldBe(await inFlight, await later);
    shouldBe((await later).instance, 3);
    shouldBe(await import(counted), await later);
}

// The loads that are at the fetch together share the entry, and the module.
{
    shouldBe(remove(), true);
    const inFlight = [import(counted), import(counted), import(counted)];
    shouldBe(remove(), true);
    const namespaces = await Promise.all(inFlight);
    shouldBe(namespaces[0] === namespaces[1] && namespaces[1] === namespaces[2], true);
    shouldBe(namespaces[0].instance, 4);
    shouldBe(remove(), false);
}

// A load that joined a loaded module, which is removed before the next step of the load, gets the module that it joined.
{
    const loaded = await import(counted);
    shouldBe(loaded.instance, 5);
    const joining = import(counted);
    shouldBe(remove(), true);
    shouldBe(await joining, loaded);
    shouldBe(remove(), false);
    shouldBe(globalThis.evaluations, 5);
}

// The same, where the key is asked for again before that step.
{
    const loaded = await import(counted);
    shouldBe(loaded.instance, 6);
    const joining = import(counted);
    shouldBe(remove(), true);
    const later = import(counted);
    shouldBe(await joining, loaded);
    shouldBe((await later).instance, 7);
    shouldBe(await import(counted), await later);
    shouldBe(globalThis.evaluations, 7);
}

// A fetch that fails leaves no entry behind.
{
    const missing = "./bun-load-by-name-owns-its-entry/missing.js";
    const inFlight = [import(missing), import(missing)];
    const failures = await Promise.all(inFlight.map(load => load.then(() => "loaded", error => String(error).includes("Could not open file"))));
    shouldBe(failures.join(), "true,true");
    shouldBe($vm.removeModuleRegistryEntry(missing), false);
}
