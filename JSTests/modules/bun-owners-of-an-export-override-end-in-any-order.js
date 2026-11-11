import { shouldBe, shouldThrow } from "./resources/assert.js";
import * as variable from "./bun-owners-of-an-export-override-end-in-any-order/variable.js";
import * as inner from "./bun-owners-of-an-export-override-end-in-any-order/inner.js";
import * as outer from "./bun-owners-of-an-export-override-end-in-any-order/outer.js";
import * as late from "./bun-owners-of-an-export-override-end-in-any-order/late.js";
import * as handedOn from "./bun-owners-of-an-export-override-end-in-any-order/handed-on.js";
import { log } from "./bun-owners-of-an-export-override-end-in-any-order/log.js";

// JSModuleNamespaceObject::layExportOverride() and its fellows: three namespace objects export one binding, and each owner stores through another one.
const first = {}, second = {}, third = {};
const found = (namespace, name, owner) => JSON.stringify(exportFoundByOverride(namespace, name, owner));
const seen = () => [variable.a, inner.a, outer.renamed].join();

// The owner that stored last gives back what it found.
shouldBe(layExportOverride(inner, "a", "first", first), true);
shouldBe(layExportOverride(outer, "renamed", "second", second), true);
shouldBe(seen(), "second,second,second");
shouldBe(found(variable, "a", first), `["real"]`);
shouldBe(found(variable, "a", second), `["first"]`);
shouldBe(endExportOverride(variable, "a", second), "restored");
shouldBe(seen(), "first,first,first");
shouldBe(endExportOverride(variable, "a", first), "restored");
shouldBe(seen(), "real,real,real");
shouldBe(found(variable, "a", first), undefined);

// An owner that ends under a later one hands it what it found, and the binding keeps the later value.
layExportOverride(inner, "a", "first", first);
layExportOverride(outer, "renamed", "second", second);
layExportOverride(variable, "a", "third", third);
shouldBe(endExportOverride(inner, "a", second), "handed over");
shouldBe(found(variable, "a", third), `["first"]`);
shouldBe(endExportOverride(inner, "a", first), "handed over");
shouldBe(found(variable, "a", third), `["real"]`);
shouldBe(seen(), "third,third,third");
shouldBe(endExportOverride(inner, "a", first), "not overridden");
shouldBe(endExportOverride(outer, "renamed", third), "restored");
shouldBe(seen(), "real,real,real");

// An owner that stores again shows its value again, above the owner that stored after it.
layExportOverride(variable, "a", "first", first);
layExportOverride(variable, "a", "second", second);
layExportOverride(variable, "a", "first again", first);
shouldBe(seen(), "first again,first again,first again");
shouldBe(found(variable, "a", second), `["real"]`);
shouldBe(found(variable, "a", first), `["second"]`);
// An owner whose value shows keeps what it found when it stores again.
layExportOverride(variable, "a", "first once more", first);
shouldBe(found(variable, "a", first), `["second"]`);
shouldBe(endExportOverride(variable, "a", second), "handed over");
shouldBe(endExportOverride(variable, "a", first), "restored");
shouldBe(seen(), "real,real,real");

// An owner that is set aside keeps its place: its next value shows from there.
layExportOverride(variable, "a", "first", first);
layExportOverride(variable, "a", "second", second);
shouldBe(endExportOverride(variable, "a", first, true), "handed over");
shouldBe(found(variable, "a", first), undefined);
shouldBe(found(variable, "a", second), `["real"]`);
shouldBe(layExportOverride(variable, "a", "first, back", first), true);
shouldBe(seen(), "second,second,second");
shouldBe(found(variable, "a", second), `["first, back"]`);
shouldBe(endExportOverride(variable, "a", second, true), "restored");
shouldBe(seen(), "first, back,first, back,first, back");
layExportOverride(variable, "a", "second, back", second);
shouldBe(seen(), "second, back,second, back,second, back");
shouldBe(endExportOverride(variable, "a", first), "handed over");
shouldBe(endExportOverride(variable, "a", second), "restored");
shouldBe(seen(), "real,real,real");

// An owner that is alone and is set aside keeps its place too.
layExportOverride(variable, "a", "first", first);
shouldBe(endExportOverride(variable, "a", first, true), "restored");
shouldBe(seen(), "real,real,real");
shouldBe(found(variable, "a", first), undefined);
layExportOverride(variable, "a", "second", second);
shouldBe(layExportOverride(variable, "a", "first, back", first), true);
shouldBe(seen(), "second,second,second");
shouldBe(found(variable, "a", second), `["first, back"]`);
shouldBe(endExportOverride(variable, "a", second), "restored");
shouldBe(seen(), "first, back,first, back,first, back");
shouldBe(endExportOverride(variable, "a", first), "restored");
shouldBe(seen(), "real,real,real");

// What the module itself assigns meanwhile is what the owner that shows gives way to, and no owner's.
layExportOverride(variable, "a", "first", first);
variable.assign("assigned");
layExportOverride(variable, "a", "second", second);
shouldBe(endExportOverride(variable, "a", first), "handed over");
shouldBe(endExportOverride(variable, "a", second), "restored");
shouldBe(seen(), "real,real,real");
variable.assign("assigned");
layExportOverride(variable, "a", "first", first);
shouldBe(endExportOverride(variable, "a", first), "restored");
shouldBe(seen(), "assigned,assigned,assigned");
variable.assign("real");

// Two names of one binding are one override of their owner.
layExportOverride(variable, "one", "first", first);
layExportOverride(variable, "other", "first too", first);
layExportOverride(inner, "other", "second", second);
shouldBe(found(variable, "one", first), `["real"]`);
shouldBe(found(variable, "one", second), `["first too"]`);
shouldBe(endExportOverride(variable, "one", first), "handed over");
shouldBe(endExportOverride(variable, "other", first), "not overridden");
shouldBe(endExportOverride(variable, "one", second), "restored");
shouldBe([variable.one, variable.other].join(), "real,real");

// A binding that had no value yet keeps the value of the owner that ends, and takes the value of its module after that.
shouldBe(JSON.stringify(log), JSON.stringify([true, "[]", "laid", "had no value", "laid"]));
shouldBe(late.value, "real");

// The binding of `export * as` takes the namespace object of its module when that object is first made, which nothing has done here: the owner finds the object.
shouldBe(layExportOverride(handedOn, "all", "first", first), true);
shouldBe(handedOn.all, "first");
shouldBe(endExportOverride(handedOn, "all", first), "restored");
shouldBe(handedOn.all, await import("./bun-owners-of-an-export-override-end-in-any-order/held.js"));
shouldBe(handedOn.all.a, "real");

// An export that no binding stands behind takes no override.
shouldBe(layExportOverride(variable, "absent", "first", first), false);
shouldBe(endExportOverride(variable, "absent", first), "not overridden");
shouldThrow(() => layExportOverride({}, "a", "first", first), "TypeError: An export override takes a namespace object, the name of an export and an owner");

// overrideExportValue(), which the owned overrides stand beside, is not tested here: the shell has no function for it.
