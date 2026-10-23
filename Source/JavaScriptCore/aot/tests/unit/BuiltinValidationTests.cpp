/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRealmTestSupport.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTChildProcess.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "Options.h"
#include <algorithm>
#include <span>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> builtinValidationTests();

namespace {

struct ReportedChange {
    ASCIILiteral change;
    NamedBuiltin builtin;
    ASCIILiteral mismatch;
};

} // anonymous namespace

static String mismatchOf(BuiltinID id, ASCIILiteral mismatch)
{
    return makeString(BuiltinTable::singleton().textOf(id), ": "_s, mismatch);
}

static void checkMismatches(JSGlobalObject* realm, ASCIILiteral change, const Vector<String>& expected)
{
    Vector<String> mismatches = BuiltinTable::singleton().mismatches(realm);
    bool isAsExpected = mismatches.size() == expected.size() && std::ranges::all_of(expected, [&](const String& mismatch) {
        return mismatches.contains(mismatch);
    });
    if (isAsExpected)
        return;
    for (const String& mismatch : expected)
        dataLogLn("expected: ", mismatch);
    for (const String& mismatch : mismatches)
        dataLogLn("reported: ", mismatch);
    failNativeTest(__FILE__, __LINE__, change.characters());
}

static void checkChange(ASCIILiteral change, const Vector<String>& expected)
{
    Realms realms;
    realms.evaluate(realms.plain(), change);
    checkMismatches(realms.plain(), change, expected);
}

static void checkChanges(std::span<const ReportedChange> changes)
{
    for (const ReportedChange& reported : changes)
        checkChange(reported.change, { mismatchOf(reported.builtin, reported.mismatch) });
}

static void testFreshRealmsAgree()
{
    Realms realms;
    checkMismatches(realms.plain(), "a realm that nothing has changed"_s, { });
    checkMismatches(realms.locked(), "a realm whose always-locked built-ins are immutable"_s, { });
    checkMismatches(realms.plain(), "a realm that was compared before"_s, { });
}

static void testEntriesAreNamedByPath()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    AOT_TEST_CHECK(table.textOf(Builtins::ArrayPrototypePush) == "Array.prototype.push"_s);
    AOT_TEST_CHECK(table.textOf(Builtins::MathPI) == "Math.PI"_s);
    AOT_TEST_CHECK(table.textOf(Builtins::ObjectPrototype) == "Object.prototype"_s);
}

static void testDeletedGlobalIsReported()
{
    static constexpr ReportedChange changes[] = {
        { "delete globalThis.Map"_s, Builtins::Map, "is missing"_s },
        { "delete globalThis.SharedArrayBuffer"_s, Builtins::SharedArrayBuffer, "is missing"_s },
    };
    checkChanges(changes);
}

static void testChangedMethodsAreReported()
{
    static constexpr ReportedChange changes[] = {
        { "delete Array.prototype.push"_s, Builtins::ArrayPrototypePush, "is missing"_s },
        { "Array.prototype.push = 1"_s, Builtins::ArrayPrototypePush, "is not an object"_s },
        { "Object.defineProperty(Array.prototype, 'push', { get() { return 1 } })"_s, Builtins::ArrayPrototypePush, "is an accessor"_s },
        { "Array.prototype.push = function () { }"_s, Builtins::ArrayPrototypePush, "has another kind of callee than the table says"_s },
        { "Object.defineProperty(Array.prototype.push, 'length', { value: 7 })"_s, Builtins::ArrayPrototypePush, "has another length than the table says"_s },
    };
    checkChanges(changes);

    const BuiltinTable& table = BuiltinTable::singleton();
    checkChange("Array.prototype.push = Array.prototype.pop"_s, {
        mismatchOf(Builtins::ArrayPrototypePush, "has another native function than the table says"_s),
        makeString(table.textOf(Builtins::ArrayPrototypePush), ": is the same object as "_s, table.textOf(Builtins::ArrayPrototypePop), ", which the table does not say"_s),
    });
}

static void testChangedAccessorsAreReported()
{
    static constexpr ReportedChange changes[] = {
        { "delete ArrayBuffer.prototype.byteLength"_s, Builtins::ArrayBufferPrototypeByteLength, "is missing"_s },
        { "Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { get() { return 0 } })"_s, Builtins::ArrayBufferPrototypeByteLength, "has another kind of callee than the table says"_s },
        { "Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { get: undefined })"_s, Builtins::ArrayBufferPrototypeByteLength, "has no getter"_s },
        { "Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { value: 1 })"_s, Builtins::ArrayBufferPrototypeByteLength, "is not an accessor"_s },
        { "Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { set(value) { } })"_s, Builtins::ArrayBufferPrototypeByteLength, "has a setter, and the table says it has none"_s },
    };
    checkChanges(changes);
}

static void testChangedConstantIsReported()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<double> pi = table.numberOf(Builtins::MathPI);
    AOT_TEST_CHECK(pi);

    Realms realms;
    VM& vm = realms.vm();
    realms.object(realms.plain(), "Math"_s)->putDirect(vm, Identifier::fromString(vm, "PI"_s), jsNumber(3));
    checkMismatches(realms.plain(), "Math.PI is 3"_s, { makeString(table.textOf(Builtins::MathPI), ": is not "_s, *pi) });
}

static void testChangedPrototypeIsReported()
{
    static constexpr ReportedChange changes[] = {
        { "Object.setPrototypeOf(Map.prototype, null)"_s, Builtins::MapPrototype, "inherits from another object than the table says"_s },
        { "Object.setPrototypeOf(Map.prototype, Set.prototype)"_s, Builtins::MapPrototype, "inherits from another object than the table says"_s },
    };
    checkChanges(changes);
}

static void testHiddenRootsAreFound()
{
    Realms realms;
    const BuiltinTable& table = BuiltinTable::singleton();
    unsigned hiddenRootCount = 0;
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        BuiltinID id(index);
        const Builtin& builtin = table.at(id);
        if (builtin.holder || id == Builtins::GlobalThis || builtin.origin != BuiltinOrigin::Engine || builtin.scope != BuiltinScope::Realm)
            continue;
        for (JSGlobalObject* realm : { realms.plain(), realms.locked() }) {
            auto found = findInRealm(realm, BuiltinCellKey::object(id));
            AOT_TEST_CHECK(found.has_value() && (*found)->isObject());
            AOT_TEST_CHECK(getByPath(realm, id) == JSValue(*found));
        }
        ++hiddenRootCount;
    }
    AOT_TEST_CHECK(hiddenRootCount >= 15);
}

static void testUnlistedMembersAreReported()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    AOT_TEST_CHECK(table.at(Builtins::ArrayPrototype).flags.contains(BuiltinFlag::ListsAllMembers));
    AOT_TEST_CHECK(!table.at(Builtins::GlobalThis).flags.contains(BuiltinFlag::ListsAllMembers));
    checkChange("Array.prototype.extra = 1; Array.prototype[Symbol.match] = 1; globalThis.extra = 1"_s, {
        "Array.prototype.extra: is not in the table"_s,
        "Array.prototype[Symbol.match]: is not in the table"_s,
    });
}

static void testAdditionsToObjectPrototypeAreReported()
{
    checkChange("Object.prototype[Symbol.match] = 1"_s, { "Object.prototype[Symbol.match]: is not in the table"_s });
    checkChange("Object.prototype.extra = 1"_s, { "Object.prototype.extra: is not in the table"_s });
}

static void runWithOptionalGlobals(ASCIILiteral testName, void (*body)())
{
    if (Options::useSharedArrayBuffer() && Options::useShadowRealm()) {
        body();
        return;
    }
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), "--useSharedArrayBuffer=1"_s, "--useShadowRealm=1"_s };
    ChildProcessResult result = launcher.run(arguments.span());
    if (result.exitedWith(0))
        return;
    dataLogLn(result.output);
    dataLogLn("The process whose realms have SharedArrayBuffer and ShadowRealm ", result.description());
    failNativeTest(__FILE__, __LINE__, testName.characters());
}

template<void (*body)()>
static void withOptionalGlobals()
{
    for (const NativeTest& test : builtinValidationTests()) {
        if (test.run == &withOptionalGlobals<body>) {
            runWithOptionalGlobals(test.name, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const NativeTest> builtinValidationTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-validation/fresh-realms-agree"_s, withOptionalGlobals<testFreshRealmsAgree> },
        { "builtin-validation/entries-are-named-by-path"_s, testEntriesAreNamedByPath },
        { "builtin-validation/deleted-global-is-reported"_s, withOptionalGlobals<testDeletedGlobalIsReported> },
        { "builtin-validation/changed-methods-are-reported"_s, withOptionalGlobals<testChangedMethodsAreReported> },
        { "builtin-validation/changed-accessors-are-reported"_s, withOptionalGlobals<testChangedAccessorsAreReported> },
        { "builtin-validation/changed-constant-is-reported"_s, withOptionalGlobals<testChangedConstantIsReported> },
        { "builtin-validation/changed-prototype-is-reported"_s, withOptionalGlobals<testChangedPrototypeIsReported> },
        { "builtin-validation/hidden-roots-are-found"_s, withOptionalGlobals<testHiddenRootsAreFound> },
        { "builtin-validation/unlisted-members-are-reported"_s, withOptionalGlobals<testUnlistedMembersAreReported> },
        { "builtin-validation/additions-to-object-prototype-are-reported"_s, withOptionalGlobals<testAdditionsToObjectPrototypeAreReported> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
