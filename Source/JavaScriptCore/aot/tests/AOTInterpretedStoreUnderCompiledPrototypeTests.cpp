/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Empty"
layout L1 name="Holder" fields=[{key="held" declared={type=anything} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="makeEmpty" results=[{type=anything}] {
    v0 = NewObject layout=L0 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f2 name="makeHolder" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L1 fields=[0]
    v2 = FinishObject v1
    Return v2
}
)IR"_s;

// `add` runs in the interpreter, which remembers a store that adds a property for as long as the Structures of the prototype chain stay the same.
static constexpr ASCIILiteral statements = R"JS(
globalThis.publish = (makeEmpty, makeHolder) => {
    const add = (target, value) => { target.held = value; return target; };
    const measure = (what, prototype) => {
        const fresh = () => Object.create(prototype);
        const warmUp = () => { for (let turn = 0; turn < 300; turn++) add(fresh(), turn); };
        const show = () => { const made = add(fresh(), 1); return Object.keys(made) + "/" + made.held; };
        let line = what + ":";
        warmUp();
        line += " " + show();
        Object.defineProperty(prototype, "held", { value: "loose", writable: true, configurable: true });
        line += " " + show();
        delete prototype.held;
        warmUp();
        Object.defineProperty(prototype, "held", { get() { return "got"; }, configurable: true });
        line += " " + show();
        delete prototype.held;
        warmUp();
        Object.defineProperty(prototype, "held", { value: "fixed", writable: false, configurable: true });
        line += " " + show();
        delete prototype.held;
        warmUp();
        line += " " + show();
        print(line);
    };
    measure("compiled prototype", makeEmpty());
    measure("ordinary prototype", {});
    const wide = makeEmpty();
    for (let i = 0; i < 200; i++) wide["name" + i] = i;
    measure("compiled prototype with many names", wide);

    const freeze = (what, prototype) => {
        const fresh = () => Object.create(prototype);
        for (let turn = 0; turn < 300; turn++) add(fresh(), turn);
        let made = add(fresh(), 1);
        let line = what + ": " + Object.keys(made) + "/" + made.held;
        Object.freeze(prototype);
        made = add(fresh(), 1);
        print(line + " " + Object.keys(made) + "/" + made.held);
    };
    freeze("compiled prototype is frozen", makeHolder("loose"));
    freeze("ordinary prototype is frozen", { held: "loose" });
};
)JS"_s;

static void testStoreSeesWhatThePrototypeGets()
{
    checkOutputOfProgramInShell(programText,
        "compiled prototype: held/1 held/1 /got /fixed held/1\n"
        "ordinary prototype: held/1 held/1 /got /fixed held/1\n"
        "compiled prototype with many names: held/1 held/1 /got /fixed held/1\n"
        "compiled prototype is frozen: held/1 /loose\n"
        "ordinary prototype is frozen: held/1 /loose\n"_s,
        { }, statements);
}

std::span<const NativeTest> interpretedStoreUnderCompiledPrototypeTests()
{
    static constexpr NativeTest tests[] = {
        { "interpreted-store-under-compiled-prototype/store-sees-what-the-prototype-gets"_s, testStoreSeesWhatThePrototypeGets },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
