/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "JSCInlines.h"
#include "JSPropertyNameEnumerator.h"
#include "ObjectConstructor.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> publishedEnumeratorTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Pair" fields=[{key="first" declared={type=anything}}, {key="second" declared={type=anything}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="makePair" results=[{type=anything}] {
  b0():
    v0 = NewObject layout=L0 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f2 name="fill" parameters=[{type=anything}, {type=anything}, {type=string}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: string):
    PropSet v0, v1 key="first"
    PropSet v0, v1 key="second"
    ElemSet v0, v2, v1
    Return
}
)IR"_s;

static Object* createPair(LoadedProgram& program, std::initializer_list<unsigned> fieldsInOrder)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        uint32_t shape = tables.allocations()[number].shape;
        if (!tables.hasLayoutForShape(shape) || !Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), "Pair"_s))
            continue;
        Object* pair = uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
        for (unsigned field : fieldsInOrder) {
            PutPropertySlot slot(pair);
            AOT_TEST_CHECK(pair->methodTable()->put(pair, program.realm(), Identifier::fromString(program.vm(), field ? "second"_s : "first"_s), jsNumber(field), slot));
        }
        AOT_TEST_CHECK(isCompiledObject(pair) && !pair->hasExtraProperties());
        return pair;
    }
    failNativeTest(__FILE__, __LINE__, "no allocation of Pair");
}

static JSCell* publishedFor(LoadedProgram& program, Object* object)
{
    uint32_t structureID = object->structureID().bits();
    const EnumeratorCacheEntry& entry = program.instance().objects.enumerators[indexOfCachedEnumerator(structureID, object->orderWord())];
    return entry.structureID == structureID && entry.orderWord == object->orderWord() ? entry.enumerator : nullptr;
}

static void testEnumeratorIsPublishedByStructureAndOrderWord(LoadedProgram& program)
{
    Object* pair = createPair(program, { 0, 1 });
    Object* samePair = createPair(program, { 0, 1 });
    Object* reversedPair = createPair(program, { 1, 0 });
    Object* half = createPair(program, { 0 });
    AOT_TEST_CHECK(pair->structure() == reversedPair->structure() && pair->orderWord() == samePair->orderWord());
    AOT_TEST_CHECK(pair->orderWord() != reversedPair->orderWord() && pair->orderWord() != half->orderWord());
    AOT_TEST_CHECK(!publishedFor(program, pair));

    JSPropertyNameEnumerator* enumerator = pair->propertyNameEnumerator(program.realm());
    AOT_TEST_CHECK(enumerator && enumerator->isCurrentFor(pair) && enumerator->isCurrentFor(samePair) && !enumerator->isCurrentFor(reversedPair));
    AOT_TEST_CHECK(publishedFor(program, pair) == enumerator && publishedFor(program, samePair) == enumerator);
    AOT_TEST_CHECK(!publishedFor(program, reversedPair) && !publishedFor(program, half));

    JSPropertyNameEnumerator* reversed = reversedPair->propertyNameEnumerator(program.realm());
    AOT_TEST_CHECK(reversed != enumerator && publishedFor(program, reversedPair) == reversed && publishedFor(program, pair) == enumerator);
    AOT_TEST_CHECK(samePair->propertyNameEnumerator(program.realm()) == enumerator);
}

static void testObjectWithExtraPropertiesHasNoPublishedEnumerator(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* pair = createPair(program, { 0, 1 });
    AOT_TEST_CHECK(pair->propertyNameEnumerator(program.realm()) && publishedFor(program, pair));

    PutPropertySlot slot(pair);
    AOT_TEST_CHECK(pair->methodTable()->put(pair, program.realm(), Identifier::fromString(vm, "third"_s), jsNumber(3), slot) && isCompiledObject(pair) && pair->hasExtraProperties());
    AOT_TEST_CHECK(!publishedFor(program, pair));
    JSPropertyNameEnumerator* enumerator = pair->propertyNameEnumerator(program.realm());
    AOT_TEST_CHECK(enumerator && enumerator->sizeOfPropertyNames() == 3 && !publishedFor(program, pair));
}

static void testChangeOnPrototypeChainWithdrawsEnumerators(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* pair = createPair(program, { 0, 1 });
    Object* reversedPair = createPair(program, { 1, 0 });
    JSObject* prototype = program.realm()->objectPrototype();
    JSObject* unrelated = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(pair->getPrototypeDirect() == JSValue(prototype));
    AOT_TEST_CHECK(pair->propertyNameEnumerator(program.realm()) && reversedPair->propertyNameEnumerator(program.realm()));
    AOT_TEST_CHECK(publishedFor(program, pair) && publishedFor(program, reversedPair) && program.state().chainDependents->hasDependents(prototype));

    didChangeChainObject(vm, unrelated, nullptr);
    AOT_TEST_CHECK(publishedFor(program, pair) && publishedFor(program, reversedPair));
    didChangeChainObject(vm, prototype, Identifier::fromString(vm, "inherited"_s).impl());
    AOT_TEST_CHECK(!publishedFor(program, pair) && !publishedFor(program, reversedPair));
    AOT_TEST_CHECK(pair->propertyNameEnumerator(program.realm()) && publishedFor(program, pair) && program.state().chainDependents->hasDependents(prototype));
}

static void testNothingIsPublishedUnderPrototypeThatCanGetIndexes(LoadedProgram& program)
{
    Object* pair = createPair(program, { 0, 1 });
    JSObject* prototype = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(program.realm()->objectPrototype()->structure()->hasImmutableProperties() && !prototype->structure()->hasImmutableProperties());
    AOT_TEST_CHECK(pair->methodTable()->setPrototype(pair, program.realm(), prototype, false));

    JSPropertyNameEnumerator* enumerator = pair->propertyNameEnumerator(program.realm());
    AOT_TEST_CHECK(enumerator && enumerator->isCurrentFor(pair) && enumerator->sizeOfPropertyNames() == 2);
    program.state().objects->keyOrders.publishEnumerator(*pair, enumerator);
    AOT_TEST_CHECK(!publishedFor(program, pair) && !program.state().chainDependents->hasDependents(prototype));

    AOT_TEST_CHECK(prototype->putDirectIndex(program.realm(), 0, jsNumber(1)));
    AOT_TEST_CHECK(pair->propertyNameEnumerator(program.realm())->sizeOfPropertyNames() == 3);
}

static void testCollectionWithdrawsEnumerators(LoadedProgram& program)
{
    Object* pair = createPair(program, { 0, 1 });
    AOT_TEST_CHECK(pair->propertyNameEnumerator(program.realm()) && publishedFor(program, pair));
    program.vm().heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(!publishedFor(program, pair));
    AOT_TEST_CHECK(pair->propertyNameEnumerator(program.realm()) && publishedFor(program, pair));
    ensureStillAliveHere(pair);
}

template<LoadedProgramTest body>
static constexpr void (*withPairs)() = runNativeTestWithLoadedProgram<publishedEnumeratorTests, programText, body>;

std::span<const NativeTest> publishedEnumeratorTests()
{
    static constexpr NativeTest tests[] = {
        { "published-enumerators/by-structure-and-order-word"_s, withPairs<testEnumeratorIsPublishedByStructureAndOrderWord> },
        { "published-enumerators/none-with-extra-properties"_s, withPairs<testObjectWithExtraPropertiesHasNoPublishedEnumerator> },
        { "published-enumerators/change-on-prototype-chain-withdraws"_s, withPairs<testChangeOnPrototypeChainWithdrawsEnumerators> },
        { "published-enumerators/none-under-prototype-that-can-get-indexes"_s, withPairs<testNothingIsPublishedUnderPrototypeThatCanGetIndexes> },
        { "published-enumerators/collection-withdraws"_s, withPairs<testCollectionWithdrawsEnumerators> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
