/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTPropertyCaches.h"
#include "AOTShellProgramTestSupport.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertySlot.h"

namespace JSC::AOT {

std::span<const NativeTest> siteCacheFillTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="shared"
    Return v1
}
)IR"_s;

namespace {

struct SiteOfSharedName {
    explicit SiteOfSharedName(LoadedProgram& program)
        : program(program)
        , caches(*program.state().propertyCaches)
        , shared(Identifier::fromString(program.vm(), "shared"_s))
        , deferGC(program.vm())
    {
        std::optional<uint32_t> identifierNumber = caches.nameOf(shared.impl()).identifierNumber;
        AOT_TEST_CHECK(identifierNumber && RowDisplacementTable::hasColumn(*identifierNumber));
        AOT_TEST_CHECK(caches.siteCaches().size());
        number = *identifierNumber;
        site = caches.siteCaches().at(0);
    }

    // The answer is filed as a read by key files it, in the hashed table alone. A site is written at its second miss.
    const Slot& readAndFill(JSObject* base)
    {
        JSGlobalObject* realm = program.realm();
        Structure* structure = base->structure();
        PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
        base->getPropertySlot(realm, shared, slot);
        caches.cache({ structure, { shared.impl(), std::nullopt }, SlotAccess::Read }, classifyRead({ realm, base, structure, shared.impl(), slot }));
        AOT_TEST_CHECK(isInHashedTable(base));
        for (unsigned miss = 0; miss < 2; ++miss)
            caches.fillSiteCacheFromTables(site, base, number, SlotAccess::Read);
        return site->get();
    }

    bool isInHashedTable(JSObject* base) { return caches.tables().hashedTable().find(base->structure()->id(), shared.impl(), SlotAccess::Read); }
    bool hasRow(JSObject* base) { return caches.tables().front().hasRow(base->structure()->id()); }

    LoadedProgram& program;
    PropertyCaches& caches;
    Identifier shared;
    DeferGC deferGC;
    uint32_t number { 0 };
    SiteCache site { nullptr };
};

}

static JSObject* createObjectWithSharedName(SiteOfSharedName& fixture)
{
    JSObject* object = constructEmptyObject(fixture.program.realm());
    object->putDirect(fixture.program.vm(), fixture.shared, jsNumber(7));
    return object;
}

static bool holdsOwnValueOf(const Slot& slot, JSObject* object)
{
    return slot.structureID == object->structure()->id() && slot.kind == Slot::Kind::Value && slot.has(Slot::isOwn) && !slot.has(Slot::dependsOnChain);
}

static void testStructureOfManyObjectsGetsRow(LoadedProgram& program)
{
    SiteOfSharedName fixture(program);
    JSObject* object = createObjectWithSharedName(fixture);
    AOT_TEST_CHECK(holdsOwnValueOf(fixture.readAndFill(object), object));
    AOT_TEST_CHECK(fixture.hasRow(object) && !fixture.isInHashedTable(object));
}

static void testDictionaryGetsNoRow(LoadedProgram& program)
{
    SiteOfSharedName fixture(program);
    JSObject* dictionary = createObjectWithSharedName(fixture);
    dictionary->convertToDictionary(program.vm());
    AOT_TEST_CHECK(holdsOwnValueOf(fixture.readAndFill(dictionary), dictionary));
    AOT_TEST_CHECK(!fixture.hasRow(dictionary) && fixture.isInHashedTable(dictionary));
}

static void testGlobalProxyFollowsGlobalObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    SiteOfSharedName fixture(program);
    JSGlobalObject* realm = program.realm();
    JSObject* proxy = realm->globalThis();
    AOT_TEST_CHECK(proxy->type() == GlobalProxyType);

    realm->putDirect(vm, fixture.shared, jsNumber(7));
    const Slot& held = fixture.readAndFill(proxy);
    AOT_TEST_CHECK(held.structureID == proxy->structure()->id() && held.kind == Slot::Kind::Value && held.holder == realm && held.has(Slot::dependsOnChain));
    AOT_TEST_CHECK(!fixture.hasRow(proxy) && fixture.isInHashedTable(proxy));

    AOT_TEST_CHECK(JSCell::deleteProperty(realm, realm, fixture.shared));
    AOT_TEST_CHECK(fixture.site->get().isEmpty() && !fixture.isInHashedTable(proxy));

    const Slot& absent = fixture.readAndFill(proxy);
    AOT_TEST_CHECK(absent.structureID == proxy->structure()->id() && absent.kind == Slot::Kind::Absent && absent.has(Slot::dependsOnChain));

    realm->putDirect(vm, fixture.shared, jsNumber(8));
    AOT_TEST_CHECK(fixture.site->get().isEmpty() && !fixture.isInHashedTable(proxy));
}

static void testGlobalProxyOfAnotherRealmIsNotFollowed(LoadedProgram& program)
{
    SiteOfSharedName fixture(program);
    JSGlobalObject* otherRealm = program.realms().plain();
    JSObject* proxy = otherRealm->globalThis();
    otherRealm->putDirect(program.vm(), fixture.shared, jsNumber(7));
    const Slot& slot = fixture.readAndFill(proxy);
    AOT_TEST_CHECK(slot.structureID == proxy->structure()->id() && slot.kind == Slot::Kind::Uncacheable && !slot.has(Slot::dependsOnChain));
    AOT_TEST_CHECK(!fixture.hasRow(proxy));
}

static constexpr ASCIILiteral programWithClassText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Holder" module=m0 constructor=f1
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = NewClosure function=f2
    DefineMethod v3, v4 layout=L0 key="shared"
    v6 = NewClosure function=f3
    DefineMethod v3, v6 layout=L0 methodKind=Getter index=1 key="size"
    v8 = FinishClass v3 layout=L0
    v9 = NewClosure function=f4
    v10 = NewClosure function=f5
    v11 = Call v1, -, v8, v9, v10
    Return
}
function f1 name="Holder" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="shared" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=7
    Return v1
}
function f3 name="size" kind=Getter flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=3
    Return v1
}
function f4 name="callShared" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="shared"
    v2 = Call v1, v0 calleeKey="shared"
    Return v2
}
function f5 name="readSize" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="size"
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral statementsUsingMembersOfClass = R"JS(
const fills = () => Number(aotOperationCount("operationAOTFillSiteCache"));
const measure = (what, access) => {
    for (let i = 0; i < 100; i++) access();
    const fillsBefore = fills();
    let result;
    for (let i = 0; i < 1000; i++) result = access();
    const fillsMade = fills() - fillsBefore;
    print(what + ": " + result + ", " + (fillsMade ? "asks for fills" : "no fill"));
};
globalThis.publish = (Holder, callShared, readSize) => {
    const object = new Holder();
    measure("method", () => callShared(object));
    measure("getter", () => readSize(object));
    Holder.prototype.shared = () => 8;
    measure("method replaced", () => callShared(object));
    object.shared = () => 9;
    measure("method shadowed", () => callShared(object));
    const behindProxy = Object.create(new Proxy({ shared: () => 10 }, { }));
    measure("method behind a proxy", () => callShared(behindProxy));
};
)JS"_s;

static void testMembersOfClassFillSites()
{
    checkOutputOfProgramInShell(programWithClassText,
        "method: 7, no fill\n"
        "getter: 3, no fill\n"
        "method replaced: 8, no fill\n"
        "method shadowed: 9, no fill\n"
        "method behind a proxy: 10, asks for fills\n"_s,
        { }, statementsUsingMembersOfClass, { "--aotCountsPath=/dev/null"_s });
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<siteCacheFillTests, programText, body>;

std::span<const NativeTest> siteCacheFillTests()
{
    static constexpr NativeTest tests[] = {
        { "site-cache-fill/structure-of-many-objects-gets-row"_s, withProgram<testStructureOfManyObjectsGetsRow> },
        { "site-cache-fill/dictionary-gets-no-row"_s, withProgram<testDictionaryGetsNoRow> },
        { "site-cache-fill/global-proxy-follows-global-object"_s, withProgram<testGlobalProxyFollowsGlobalObject> },
        { "site-cache-fill/global-proxy-of-another-realm-is-not-followed"_s, withProgram<testGlobalProxyOfAnotherRealmIsNotFollowed> },
        { "site-cache-fill/members-of-class-fill-sites"_s, testMembersOfClassFillSites },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
