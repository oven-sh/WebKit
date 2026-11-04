/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "ObjectPrototype.h"

namespace JSC { namespace AOT {

static JSObject* prototypeOf(DetachedInstance& fixture, TypedArrayType type)
{
    return fixture.globalObject()->typedArrayStructure(type, false)->storedPrototypeObject();
}

static JSObject* sharedPrototype(DetachedInstance& fixture)
{
    return prototypeOf(fixture, TypeUint8)->getPrototypeDirect().getObject();
}

static void makePrototypeImmutable(DetachedInstance& fixture, TypedArrayType type)
{
    AOT_TEST_CHECK(prototypeOf(fixture, type)->makePropertiesImmutable(fixture.vm()));
}

static bool offerForMarking(DetachedInstance& fixture, ASCIILiteral expression)
{
    JSObject* object = asObject(fixture.evaluate(expression));
    Structure* structure = object->structure();
    markPristineIfOriginal(fixture.instance(), object);
    AOT_TEST_CHECK_FOR_SOURCE(object->structure() == structure, expression);
    bool isPristine = structure->isPristine();
    bool hasPristineElements = structure->hasPristineElements();

    markPristineIfOriginal(fixture.instance(), object);
    AOT_TEST_CHECK_FOR_SOURCE(object->structure() == structure, expression);
    AOT_TEST_CHECK_FOR_SOURCE(structure->isPristine() == isPristine && structure->hasPristineElements() == hasPristineElements, expression);
    AOT_TEST_CHECK_FOR_SOURCE(structure->tryMarkPristine(fixture.vm()) == isPristine, expression);
    return isPristine;
}

static void checkNotOfferedForMarking(DetachedInstance& fixture, ASCIILiteral expression)
{
    JSObject* object = asObject(fixture.evaluate(expression));
    Structure* structure = object->structure();
    bool wasPristine = structure->isPristine();
    bool hadPristineElements = structure->hasPristineElements();
    markPristineIfOriginal(fixture.instance(), object);
    AOT_TEST_CHECK_FOR_SOURCE(object->structure() == structure, expression);
    AOT_TEST_CHECK_FOR_SOURCE(structure->isPristine() == wasPristine && structure->hasPristineElements() == hadPristineElements, expression);
}

static void testLazyStructureIsOfferedForMarking()
{
    static constexpr ASCIILiteral instancesOfLazyClasses[] = { "new Uint8Array(4)"_s, "new Float64Array(1)"_s, "new Date(0)"_s };
    static constexpr ASCIILiteral otherObjects[] = { "new (class extends Uint8Array { })(4)"_s, "new (class extends Date { })(0)"_s, "[1]"_s, "({})"_s };
    for (TestRealm realm : { TestRealm::Plain, TestRealm::Locked }) {
        DetachedInstance fixture(realm);
        for (ASCIILiteral expression : otherObjects)
            checkNotOfferedForMarking(fixture, expression);
        for (ASCIILiteral expression : instancesOfLazyClasses) {
            bool isPristine = offerForMarking(fixture, expression);
            AOT_TEST_CHECK_FOR_SOURCE(realm == TestRealm::Locked || !isPristine, expression);
        }
    }

    DetachedInstance fixture(TestRealm::Plain);
    makePrototypeImmutable(fixture, TypeUint8);
    AOT_TEST_CHECK(sharedPrototype(fixture)->makePropertiesImmutable(fixture.vm()));
    AOT_TEST_CHECK(!offerForMarking(fixture, "new Uint8Array(4)"_s));
    AOT_TEST_CHECK(fixture.globalObject()->objectPrototype()->makePropertiesImmutable(fixture.vm()));
    AOT_TEST_CHECK(offerForMarking(fixture, "new Uint8Array(4)"_s));
    AOT_TEST_CHECK(!offerForMarking(fixture, "new Int8Array(4)"_s));
}

std::span<const NativeTest> pristineStructureTests()
{
    static constexpr NativeTest tests[] = {
        { "pristine-structures/lazy-structure-is-offered-for-marking"_s, testLazyStructureIsOfferedForMarking },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
