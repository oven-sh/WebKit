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
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="makePair" results=[{type=anything}] {
    v0 = NewObject layout=L0 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f2 name="fill" parameters=[{type=anything}, {type=anything}, {type=string}] results=[{type=anything}] {
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
        if (!tables.hasLayoutForShape(shape) || !isNamedInTextOfLoadedProgram(tables.layoutForShape(shape), "Pair"_s))
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

static void testAccessorAmongOwnNamesLeavesNoNameThatIsLoaded(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Object* withAccessor = createPair(program, { 0, 1 });
    PropertyDescriptor getter;
    getter.setGetter(realm->objectConstructor());
    getter.setEnumerable(true);
    getter.setConfigurable(true);
    AOT_TEST_CHECK(withAccessor->methodTable()->defineOwnProperty(withAccessor, realm, Identifier::fromString(vm, "third"_s), getter, false));
    withAccessor->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(withAccessor->compiledStructure().listsEveryExtraProperty());

    JSPropertyNameEnumerator* enumerator = withAccessor->propertyNameEnumerator(realm);
    AOT_TEST_CHECK(enumerator && enumerator->isCurrentFor(withAccessor) && enumerator->sizeOfPropertyNames() == 3);
    AOT_TEST_CHECK(!enumerator->endStructurePropertyIndex() && enumerator->endOwnPropertyIndex() == 3);
    AOT_TEST_CHECK(publishedFor(program, withAccessor) == enumerator);

    PropertyDescriptor value(jsNumber(3), 0);
    AOT_TEST_CHECK(withAccessor->methodTable()->defineOwnProperty(withAccessor, realm, Identifier::fromString(vm, "third"_s), value, false));
    withAccessor->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(!enumerator->isCurrentFor(withAccessor));
}

template<LoadedProgramTest body>
static constexpr void (*withPairs)() = runNativeTestWithLoadedProgram<publishedEnumeratorTests, programText, body>;

std::span<const NativeTest> publishedEnumeratorTests()
{
    static constexpr NativeTest tests[] = {
        { "published-enumerators/an-accessor-leaves-no-name-that-is-loaded"_s, withPairs<testAccessorAmongOwnNamesLeavesNoNameThatIsLoaded> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
