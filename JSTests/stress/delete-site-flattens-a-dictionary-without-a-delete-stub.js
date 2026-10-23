//@ runNoCJIT

// The global object gets no delete stub: its deleteProperty answers from the symbol table. A delete site still has to
// make it flat when it is a dictionary, as it does for an object that gets a stub. If it stays a dictionary, each delete
// that removes a property copies the property table.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": bad value: " + actual + " expected: " + expected);
}

function deleteMissing(object) { return delete object.notThere; }
noInline(deleteMissing);

let global = $vm.createGlobalObject();
shouldBe(describe(global).includes(", Dictionary"), true, "a new global object is a dictionary");

for (let i = 0; i < testLoopCount; ++i)
    shouldBe(deleteMissing(global), true, "delete");

shouldBe(describe(global).includes(", Has been dictionary"), true, "the site made it flat");
