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
#include <algorithm>

namespace JSC::AOT {

std::span<const NativeTest> siteCacheFillTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
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
    dictionary->structure()->flattenDictionaryStructure(program.vm(), dictionary);
    dictionary->convertToDictionary(program.vm());
    AOT_TEST_CHECK(holdsOwnValueOf(fixture.readAndFill(dictionary), dictionary));
    AOT_TEST_CHECK(dictionary->structure()->isDictionary() && !fixture.hasRow(dictionary) && fixture.isInHashedTable(dictionary));
}

static void testDictionaryNeverFlattenedGetsRow(LoadedProgram& program)
{
    SiteOfSharedName fixture(program);
    JSObject* object = createObjectWithSharedName(fixture);
    object->convertToDictionary(program.vm());
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    object->getPropertySlot(program.realm(), fixture.shared, slot);
    PropertyAnswer first = classifyRead({ program.realm(), object, object->structure(), fixture.shared.impl(), slot });
    // Nothing of a cacheable dictionary moves when it is made flat, so the look that makes it flat is answered.
    AOT_TEST_CHECK(first.kind == PropertyAnswerKind::Value && !first.didFlatten && !object->structure()->isDictionary());
    AOT_TEST_CHECK(holdsOwnValueOf(fixture.readAndFill(object), object));
    AOT_TEST_CHECK(fixture.hasRow(object));
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

static void testFailedFillsAreAskedForMoreRarely(LoadedProgram& program)
{
    SiteOfSharedName fixture(program);
    auto missesLeft = [&] {
        return fixture.caches.siteCaches().stateOf(fixture.site).missesLeft;
    };
    JSObject* unfiled = constructEmptyObject(program.realm());
    unfiled->putDirect(program.vm(), Identifier::fromString(program.vm(), "other"_s), jsNumber(1));
    unfiled->putDirect(program.vm(), fixture.shared, jsNumber(8));
    AOT_TEST_CHECK(!fixture.isInHashedTable(unfiled) && !fixture.hasRow(unfiled));

    // The stubs count the misses of all empty sites together. A site for which nothing was found gets a slot that fits no object, so that it counts its own.
    for (unsigned failures = 0; failures < 3; ++failures) {
        unsigned refillCount = fixture.caches.siteCaches().refillCountOf(fixture.site);
        fixture.caches.fillSiteCacheFromTables(fixture.site, unfiled, fixture.number, SlotAccess::Read);
        AOT_TEST_CHECK(!fixture.site->get().isEmpty() && !fixture.site->get().structureID && missesLeft() == SiteCacheState::missesBeforeRefill(refillCount));
        AOT_TEST_CHECK(fixture.caches.siteCaches().refillCountOf(fixture.site) == refillCount + 1);
    }

    JSObject* filed = createObjectWithSharedName(fixture);
    AOT_TEST_CHECK(filed->structure() != unfiled->structure() && holdsOwnValueOf(fixture.readAndFill(filed), filed));
    for (unsigned failures = 0; failures < SiteCacheState::maxRefillCount + 2; ++failures) {
        unsigned refillCount = fixture.caches.siteCaches().refillCountOf(fixture.site);
        fixture.caches.fillSiteCacheFromTables(fixture.site, unfiled, fixture.number, SlotAccess::Read);
        AOT_TEST_CHECK(holdsOwnValueOf(fixture.site->get(), filed));
        AOT_TEST_CHECK(missesLeft() == SiteCacheState::missesBeforeRefill(refillCount));
        AOT_TEST_CHECK(fixture.caches.siteCaches().refillCountOf(fixture.site) == std::min(refillCount + 1, SiteCacheState::maxRefillCount));
    }
    AOT_TEST_CHECK(missesLeft() == SiteCacheState::missesBeforeRefill(SiteCacheState::maxRefillCount));
}

static constexpr ASCIILiteral programWithClassText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Holder" module=m0 constructor=f1
function f0 name="main" kind=Module results=[{type=anything}] {
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
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="shared" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=7
    Return v1
}
function f3 name="size" kind=Getter flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=3
    Return v1
}
function f4 name="callShared" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    v2 = Call v1, v0 calleeKey="shared"
    Return v2
}
function f5 name="readSize" parameters=[{type=anything}] results=[{type=anything}] {
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
        // Hot: outside fast code a call of a method is one call of a stub and has no site cache to fill.
        { "heat=EverythingIsHot"_s }, statementsUsingMembersOfClass, { "--aotCountsPath=/dev/null"_s });
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<siteCacheFillTests, programText, body>;

std::span<const NativeTest> siteCacheFillTests()
{
    static constexpr NativeTest tests[] = {
        { "site-cache-fill/structure-of-many-objects-gets-row"_s, withProgram<testStructureOfManyObjectsGetsRow> },
        { "site-cache-fill/dictionary-gets-no-row"_s, withProgram<testDictionaryGetsNoRow> },
        { "site-cache-fill/dictionary-never-flattened-gets-row"_s, withProgram<testDictionaryNeverFlattenedGetsRow> },
        { "site-cache-fill/global-proxy-follows-global-object"_s, withProgram<testGlobalProxyFollowsGlobalObject> },
        { "site-cache-fill/global-proxy-of-another-realm-is-not-followed"_s, withProgram<testGlobalProxyOfAnotherRealmIsNotFollowed> },
        { "site-cache-fill/failed-fills-are-asked-for-more-rarely"_s, withProgram<testFailedFillsAreAskedForMoreRarely> },
        { "site-cache-fill/members-of-class-fill-sites"_s, testMembersOfClassFillSites },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
