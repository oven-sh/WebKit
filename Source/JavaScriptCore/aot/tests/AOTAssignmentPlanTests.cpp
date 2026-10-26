/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTMappedCopyInlines.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationCallScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include <array>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> assignmentPlanTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Point" fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="y" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Copy"
layout L2 name="Tail" fields=[{key="z" declared={type=anything}}]
layout L3 name="Pair" fields=[{key="x" declared={type=anything}}, {key="y" declared={type=anything}}]
layout L4 name="Labeled" fields=[{key="label" declared={type=anything} flags=[IsRequired]}, {key="x" declared={type=anything}}, {key="y" declared={type=anything}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = NewClosure function=f6
    v8 = Call v1, -, v2, v3, v4, v5, v6, v7
    Return
}
function f1 name="makePoint" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    Return v3
}
function f2 name="makeCopy" results=[{type=anything}] {
    v0 = NewObject layout=L1 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f3 name="storeAndDelete" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    Return
}
function f4 name="makeTail" results=[{type=anything}] {
    v0 = NewObject layout=L2 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f5 name="makePair" results=[{type=anything}] {
    v0 = NewObject layout=L3 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f6 name="makeLabeled" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L4 fields=[0]
    v2 = FinishObject v1
    Return v2
}
)IR"_s;

static Object* allocate(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        uint32_t shape = tables.allocations()[number].shape;
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), layoutName))
            return uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, "no allocation of the layout");
}

static WTF::Vector<Identifier> enumerableKeysOf(LoadedProgram& program, JSObject* object)
{
    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, program.realm(), names, DontEnumPropertiesMode::Exclude);
    return WTF::Vector<Identifier>(names.size(), [&](size_t index) {
        return names[index];
    });
}

static Object* createPoint(LoadedProgram& program, int32_t x, int32_t y)
{
    Object* point = allocate(program, "Point"_s);
    AOT_TEST_CHECK(point->layout().extraPropertiesSlot());
    AOT_TEST_CHECK(point->store(program.vm(), point->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(x))) && point->store(program.vm(), point->layout().fields()[1], AcceptedValue::provenAtBuildTime(jsNumber(y))));
    return point;
}

static Object* createCopy(LoadedProgram& program)
{
    Object* copy = allocate(program, "Copy"_s);
    AOT_TEST_CHECK(copy->layout().extraPropertiesSlot());
    return copy;
}

static Object* createTail(LoadedProgram& program)
{
    Object* tail = allocate(program, "Tail"_s);
    AOT_TEST_CHECK(tail->layout().extraPropertiesSlot() && enumerableKeysOf(program, tail).isEmpty());
    return tail;
}

static void put(LoadedProgram& program, JSObject* object, ASCIILiteral name, int32_t value)
{
    PutPropertySlot slot(object, true);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), Identifier::fromString(program.vm(), name), jsNumber(value), slot));
}

static void addName(LoadedProgram& program, Object* object, ASCIILiteral name, int32_t value)
{
    Identifier identifier = Identifier::fromString(program.vm(), name);
    AOT_TEST_CHECK(object->tryStoreExtraProperty(program.vm(), identifier, jsNumber(value), { Object::ExtraPropertyStoreFlag::IsKnownNotToBeField }).kind == Object::ExtraPropertyStore::Added);
}

static bool hasKeys(LoadedProgram& program, JSObject* object, std::initializer_list<ASCIILiteral> expected)
{
    WTF::Vector<Identifier> keys = enumerableKeysOf(program, object);
    return std::ranges::equal(keys, expected, [](const Identifier& key, ASCIILiteral name) {
        return key.string() == name;
    });
}

static bool holdsPropertiesOf(LoadedProgram& program, JSObject* target, JSObject* source)
{
    WTF::Vector<Identifier> keys = enumerableKeysOf(program, source);
    if (keys != enumerableKeysOf(program, target))
        return false;
    return std::ranges::all_of(keys, [&](const Identifier& key) {
        return target->get(program.realm(), key) == source->get(program.realm(), key);
    });
}

static void testCopiesFieldsAndAddedNames(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* source = createPoint(program, turn, turn + 1);
        addName(program, source, "late"_s, turn + 2);
        addName(program, source, "later"_s, turn + 3);
        Object* target = createCopy(program);
        AOT_TEST_CHECK(target->tryAssignPropertiesOf(vm, source));
        AOT_TEST_CHECK(hasKeys(program, target, { "x"_s, "y"_s, "late"_s, "later"_s }) && holdsPropertiesOf(program, target, source));
        AOT_TEST_CHECK(target->extraProperties() != source->extraProperties() && hasKeys(program, source, { "x"_s, "y"_s, "late"_s, "later"_s }));
    }
}

static void testStoresIntoFieldsOfTarget(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* source = createPoint(program, 1, 2);
    addName(program, source, "late"_s, 3);
    Object* target = createPoint(program, 8, 9);
    AOT_TEST_CHECK(target->tryAssignPropertiesOf(vm, source));
    AOT_TEST_CHECK(hasKeys(program, target, { "x"_s, "y"_s, "late"_s }) && holdsPropertiesOf(program, target, source));
    AOT_TEST_CHECK(target->readInline(target->layout().fields()[0]) == jsNumber(1) && target->readInline(target->layout().fields()[1]) == jsNumber(2));
}

static void testOrderOfAddedNamesSelectsPlan(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* lateFirst = createPoint(program, 1, 2);
        addName(program, lateFirst, "late"_s, 3);
        addName(program, lateFirst, "later"_s, 4);
        Object* laterFirst = createPoint(program, 5, 6);
        addName(program, laterFirst, "later"_s, 7);
        addName(program, laterFirst, "late"_s, 8);
        Object* fieldsOnly = createPoint(program, 9, 10);

        Object* first = createCopy(program);
        Object* second = createCopy(program);
        Object* third = createCopy(program);
        AOT_TEST_CHECK(first->tryAssignPropertiesOf(vm, lateFirst) && second->tryAssignPropertiesOf(vm, laterFirst) && third->tryAssignPropertiesOf(vm, fieldsOnly));
        AOT_TEST_CHECK(hasKeys(program, first, { "x"_s, "y"_s, "late"_s, "later"_s }) && holdsPropertiesOf(program, first, lateFirst));
        AOT_TEST_CHECK(hasKeys(program, second, { "x"_s, "y"_s, "later"_s, "late"_s }) && holdsPropertiesOf(program, second, laterFirst));
        AOT_TEST_CHECK(hasKeys(program, third, { "x"_s, "y"_s }) && holdsPropertiesOf(program, third, fieldsOnly));
    }
}

static void testCreatesFieldBehindAddedNames(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* source = createTail(program);
        addName(program, source, "a"_s, turn);
        addName(program, source, "b"_s, turn + 1);
        put(program, source, "z"_s, turn + 2);
        AOT_TEST_CHECK(isCompiledObject(source) && hasKeys(program, source, { "a"_s, "b"_s, "z"_s }));

        Object* target = createTail(program);
        AOT_TEST_CHECK(target->tryAssignPropertiesOf(vm, source));
        AOT_TEST_CHECK(hasKeys(program, target, { "a"_s, "b"_s, "z"_s }) && holdsPropertiesOf(program, target, source));
        AOT_TEST_CHECK(target->readInline(target->layout().fields()[0]) == jsNumber(turn + 2));
    }
}

static void testKeepsOrderWhereFieldLiesBetweenAddedNames(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* source = createTail(program);
        addName(program, source, "a"_s, 1);
        put(program, source, "z"_s, 2);
        addName(program, source, "b"_s, 3);
        AOT_TEST_CHECK(isCompiledObject(source) && hasKeys(program, source, { "a"_s, "z"_s, "b"_s }));

        // A run that gives up has stored nothing that the copy by lookups behind it would list in another place.
        Object* target = createTail(program);
        if (!target->tryAssignPropertiesOf(vm, source)) {
            for (const Identifier& key : enumerableKeysOf(program, source)) {
                PutPropertySlot slot(target, true);
                AOT_TEST_CHECK(target->methodTable()->put(target, program.realm(), key, source->get(program.realm(), key), slot));
            }
        }
        AOT_TEST_CHECK(hasKeys(program, target, { "a"_s, "z"_s, "b"_s }) && holdsPropertiesOf(program, target, source));

        Object* filled = createTail(program);
        put(program, filled, "z"_s, 9);
        AOT_TEST_CHECK(filled->tryAssignPropertiesOf(vm, source) && hasKeys(program, filled, { "z"_s, "a"_s, "b"_s }));
        AOT_TEST_CHECK(filled->get(program.realm(), Identifier::fromString(vm, "z"_s)) == jsNumber(2));
    }
}

static void testLeavesOutExcludedNames(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier x = Identifier::fromString(vm, "x"_s);
    Identifier late = Identifier::fromString(vm, "late"_s);
    Identifier absent = Identifier::fromString(vm, "absent"_s);
    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* source = createPoint(program, 1, 2);
        addName(program, source, "late"_s, 3);
        addName(program, source, "later"_s, 4);

        std::array<UniquedStringImpl*, 2> fieldAndName { x.impl(), late.impl() };
        Object* rest = createCopy(program);
        AOT_TEST_CHECK(rest->tryAssignPropertiesOf(vm, source, fieldAndName) && hasKeys(program, rest, { "y"_s, "later"_s }));

        std::array<UniquedStringImpl*, 1> nameOnly { late.impl() };
        Object* withoutName = createCopy(program);
        AOT_TEST_CHECK(withoutName->tryAssignPropertiesOf(vm, source, nameOnly) && hasKeys(program, withoutName, { "x"_s, "y"_s, "later"_s }));

        std::array<UniquedStringImpl*, 1> nothingOfSource { absent.impl() };
        Object* everything = createCopy(program);
        AOT_TEST_CHECK(everything->tryAssignPropertiesOf(vm, source, nothingOfSource) && holdsPropertiesOf(program, everything, source));

        Object* copy = createCopy(program);
        AOT_TEST_CHECK(copy->tryAssignPropertiesOf(vm, source) && holdsPropertiesOf(program, copy, source));

        JSObject* ordinary = constructEmptyObject(program.realm());
        ordinary->putDirect(vm, x, jsNumber(5));
        ordinary->putDirect(vm, late, jsNumber(6));
        ordinary->putDirect(vm, absent, jsNumber(7));
        Object* restOfOrdinary = createCopy(program);
        AOT_TEST_CHECK(restOfOrdinary->tryAssignPropertiesOf(vm, ordinary, fieldAndName) && hasKeys(program, restOfOrdinary, { "absent"_s }));
    }
}

static bool isRefused(LoadedProgram& program, JSObject* source)
{
    Object* target = createCopy(program);
    return !target->tryAssignPropertiesOf(program.vm(), source) && enumerableKeysOf(program, target).isEmpty();
}

// Whether a plan serves such a source or not, the target never ends up with other properties than the language says.
static bool isCopiedExactlyOrRefused(LoadedProgram& program, JSObject* source)
{
    Object* target = createCopy(program);
    return !target->tryAssignPropertiesOf(program.vm(), source) || holdsPropertiesOf(program, target, source);
}

// An object that gave up its layout is an ordinary object, whose properties say themselves what they are.
static bool isLeftToLookup(LoadedProgram& program, JSObject* source)
{
    return isCompiledObject(source) ? isRefused(program, source) : isCopiedExactlyOrRefused(program, source);
}

static Object* createPointWithAddedName(LoadedProgram& program)
{
    Object* source = createPoint(program, 1, 2);
    addName(program, source, "late"_s, 3);
    return source;
}

static bool define(LoadedProgram& program, JSObject* object, PropertyName name, const PropertyDescriptor& descriptor)
{
    return object->methodTable()->defineOwnProperty(object, program.realm(), name, descriptor, false);
}

static PropertyDescriptor enumerableGetter(LoadedProgram& program)
{
    PropertyDescriptor getter;
    getter.setGetter(program.realm()->objectConstructor());
    getter.setEnumerable(true);
    getter.setConfigurable(true);
    return getter;
}

static void testRefusesWhatOnlyLookupCanCopy(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(!isRefused(program, createPointWithAddedName(program)));

    JSObject* withGetter = createPointWithAddedName(program);
    AOT_TEST_CHECK(define(program, withGetter, Identifier::fromString(vm, "computed"_s), enumerableGetter(program)) && isLeftToLookup(program, withGetter));

    JSObject* withHiddenName = createPointWithAddedName(program);
    PropertyDescriptor hidden(jsNumber(7), static_cast<unsigned>(PropertyAttribute::DontEnum));
    AOT_TEST_CHECK(define(program, withHiddenName, Identifier::fromString(vm, "hidden"_s), hidden) && isLeftToLookup(program, withHiddenName));

    JSObject* withSymbol = createPointWithAddedName(program);
    AOT_TEST_CHECK(define(program, withSymbol, vm.propertyNames->iteratorSymbol, PropertyDescriptor(jsNumber(7), 0)) && isLeftToLookup(program, withSymbol));

    JSObject* withIndex = createPointWithAddedName(program);
    AOT_TEST_CHECK(withIndex->methodTable()->putByIndex(withIndex, program.realm(), 3, jsNumber(7), false) && isLeftToLookup(program, withIndex));

    Object* frozen = createPointWithAddedName(program);
    frozen->setIntegrity(program.realm(), Object::Integrity::Frozen);
    AOT_TEST_CHECK(isRefused(program, frozen));
}

static void testFollowsChangesToFields(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier x = Identifier::fromString(vm, "x"_s);

    JSObject* changed = createPointWithAddedName(program);
    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(changed->methodTable()->deleteProperty(changed, program.realm(), x, deleteSlot));
    AOT_TEST_CHECK(hasKeys(program, changed, { "y"_s, "late"_s }) && isCopiedExactlyOrRefused(program, changed));

    PutPropertySlot putSlot(changed, true);
    AOT_TEST_CHECK(changed->methodTable()->put(changed, program.realm(), x, jsNumber(9), putSlot));
    AOT_TEST_CHECK(hasKeys(program, changed, { "y"_s, "late"_s, "x"_s }) && isCopiedExactlyOrRefused(program, changed));

    JSObject* withAccessorField = createPointWithAddedName(program);
    AOT_TEST_CHECK(define(program, withAccessorField, x, enumerableGetter(program)) && isLeftToLookup(program, withAccessorField));
}

static Object* createPair(LoadedProgram& program, std::initializer_list<unsigned> fields)
{
    Object* pair = allocate(program, "Pair"_s);
    AOT_TEST_CHECK(enumerableKeysOf(program, pair).isEmpty());
    for (unsigned field : fields)
        put(program, pair, field ? "y"_s : "x"_s, static_cast<int32_t>(field + 1));
    return pair;
}

static void testCopiesFieldsOfSameLayoutInAnyOrder(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (std::initializer_list<unsigned> order : { std::initializer_list<unsigned> { 0, 1 }, { 1, 0 }, { 1 }, { } }) {
        Object* source = createPair(program, order);
        Object* target = createPair(program, { });
        AOT_TEST_CHECK(target->tryCopyFieldsOf(vm, *source) && holdsPropertiesOf(program, target, source));
        put(program, target, "x"_s, 70);
        AOT_TEST_CHECK(source->get(program.realm(), Identifier::fromString(vm, "x"_s)) != jsNumber(70));
    }

    // The first position of a name stays, so a target that has a property takes the fields one by one.
    Object* yThenX = createPair(program, { 1, 0 });
    Object* withX = createPair(program, { 0 });
    AOT_TEST_CHECK(!withX->tryCopyFieldsOf(vm, *yThenX) || hasKeys(program, withX, { "x"_s, "y"_s }));

    Object* frozen = createPair(program, { 0, 1 });
    frozen->setIntegrity(program.realm(), Object::Integrity::Frozen);
    Object* copyOfFrozen = createPair(program, { });
    AOT_TEST_CHECK(!copyOfFrozen->tryCopyFieldsOf(vm, *frozen) || !(copyOfFrozen->orderWord() & orderWordIntegrityBits));
}

static void testDefinesFieldsAtOnceOrNotAtAll(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier x = Identifier::fromString(vm, "x"_s);
    Identifier y = Identifier::fromString(vm, "y"_s);
    Identifier other = Identifier::fromString(vm, "other"_s);
    std::array<EncodedJSValue, 2> values { JSValue::encode(jsNumber(1)), JSValue::encode(jsNumber(2)) };

    std::array<UniquedStringImpl*, 2> yThenX { y.impl(), x.impl() };
    Object* empty = createPair(program, { });
    AOT_TEST_CHECK(empty->tryDefineFields(vm, yThenX, values) && hasKeys(program, empty, { "y"_s, "x"_s }));
    AOT_TEST_CHECK(empty->get(program.realm(), y) == jsNumber(1) && empty->get(program.realm(), x) == jsNumber(2));

    Object* withX = createPair(program, { 0 });
    AOT_TEST_CHECK(withX->tryDefineFields(vm, yThenX, values) && hasKeys(program, withX, { "x"_s, "y"_s }) && withX->get(program.realm(), x) == jsNumber(2));

    std::array<UniquedStringImpl*, 2> fieldThenOtherName { x.impl(), other.impl() };
    Object* untouched = createPair(program, { });
    AOT_TEST_CHECK(!untouched->tryDefineFields(vm, fieldThenOtherName, values) && enumerableKeysOf(program, untouched).isEmpty());

    Object* withAddedName = createPointWithAddedName(program);
    AOT_TEST_CHECK(!withAddedName->tryDefineFields(vm, yThenX, values) && withAddedName->get(program.realm(), y) == jsNumber(2));
}

static void testCopiesAsOrdinaryObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier x = Identifier::fromString(vm, "x"_s);
    std::optional<uint32_t> numberOfX = vm.aotState().identifiers->numberOf(x.impl());
    AOT_TEST_CHECK(numberOfX);
    std::array<uint32_t, 1> identifiers { *numberOfX };
    ExcludedRun withoutX { 1, identifiers };

    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* source = createPair(program, { 1, 0 });
        JSFinalObject* copy = tryCopyAsOrdinaryObject(vm, program.realm(), source, { });
        AOT_TEST_CHECK(copy && !isCompiledObject(copy) && holdsPropertiesOf(program, copy, source) && copy->getPrototypeDirect() == program.realm()->objectPrototype());
        JSFinalObject* rest = tryCopyAsOrdinaryObject(vm, program.realm(), source, withoutX);
        AOT_TEST_CHECK(rest && hasKeys(program, rest, { "y"_s }) && rest->structure() != copy->structure());

        JSFinalObject* restOfOrdinary = tryCopyAsOrdinaryObject(vm, program.realm(), copy, withoutX);
        AOT_TEST_CHECK(restOfOrdinary && hasKeys(program, restOfOrdinary, { "y"_s }) && restOfOrdinary->structure() == rest->structure());
    }

    AOT_TEST_CHECK(!tryCopyAsOrdinaryObject(vm, program.realm(), createPointWithAddedName(program), { }));
    JSObject* withGetter = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(define(program, withGetter, x, enumerableGetter(program)) && !tryCopyAsOrdinaryObject(vm, program.realm(), withGetter, withoutX));
    JSObject* withIndex = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(withIndex->methodTable()->putByIndex(withIndex, program.realm(), 3, jsNumber(7), false) && !tryCopyAsOrdinaryObject(vm, program.realm(), withIndex, withoutX));
    JSObject* withHiddenName = constructEmptyObject(program.realm());
    withHiddenName->putDirect(vm, x, jsNumber(1), static_cast<unsigned>(PropertyAttribute::DontEnum));
    withHiddenName->putDirect(vm, Identifier::fromString(vm, "y"_s), jsNumber(2));
    JSFinalObject* withoutHiddenName = tryCopyAsOrdinaryObject(vm, program.realm(), withHiddenName, ExcludedRun { 2, { } });
    AOT_TEST_CHECK(withoutHiddenName && hasKeys(program, withoutHiddenName, { "y"_s }));
}

static void testCopiesTwoObjectsAtOnce(LoadedProgram& program)
{
    VM& vm = program.vm();
    State& state = program.state();
    JSGlobalObject* realm = program.realm();
    Identifier x = Identifier::fromString(vm, "x"_s);
    Identifier y = Identifier::fromString(vm, "y"_s);
    Identifier first = Identifier::fromString(vm, "first"_s);
    auto createOrdinary = [&] {
        JSObject* object = constructEmptyObject(realm);
        object->putDirect(vm, first, jsNumber(10));
        object->putDirect(vm, y, jsNumber(20));
        return object;
    };

    // The first pair of a kind is copied one behind the other.
    AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, createOrdinary(), createPair(program, { 1, 0 })));
    Structure* structure = nullptr;
    for (int32_t turn = 0; turn < 3; ++turn) {
        Object* second = createPair(program, { 1, 0 });
        JSFinalObject* copy = tryCopyTwoObjects(vm, state, realm, createOrdinary(), second);
        AOT_TEST_CHECK(copy && !isCompiledObject(copy) && copy->getPrototypeDirect() == realm->objectPrototype() && !copy->structure()->outOfLineSize());
        AOT_TEST_CHECK(hasKeys(program, copy, { "first"_s, "y"_s, "x"_s }));
        AOT_TEST_CHECK(copy->get(realm, first) == jsNumber(10) && copy->get(realm, y) == second->get(realm, y) && copy->get(realm, x) == second->get(realm, x));
        AOT_TEST_CHECK(!structure || copy->structure() == structure);
        structure = copy->structure();
    }

    AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, jsUndefined(), createPointWithAddedName(program)));
    Object* withAddedName = createPointWithAddedName(program);
    JSFinalObject* ofSecondAlone = tryCopyTwoObjects(vm, state, realm, jsUndefined(), withAddedName);
    AOT_TEST_CHECK(ofSecondAlone && holdsPropertiesOf(program, ofSecondAlone, withAddedName));
    AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, createOrdinary(), jsNull()));
    JSFinalObject* ofFirstAlone = tryCopyTwoObjects(vm, state, realm, createOrdinary(), jsNull());
    AOT_TEST_CHECK(ofFirstAlone && hasKeys(program, ofFirstAlone, { "first"_s, "y"_s }));

    // The copy of a compiled object is an object of its layout. A getter is called, an element is no property, and a string has elements.
    JSObject* withGetter = constructEmptyObject(realm);
    AOT_TEST_CHECK(define(program, withGetter, x, enumerableGetter(program)));
    JSObject* withIndex = constructEmptyObject(realm);
    AOT_TEST_CHECK(withIndex->methodTable()->putByIndex(withIndex, realm, 3, jsNumber(7), false));
    for (int32_t turn = 0; turn < 3; ++turn) {
        AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, createPair(program, { 0, 1 }), createOrdinary()));
        AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, createOrdinary(), withGetter) && !tryCopyTwoObjects(vm, state, realm, withGetter, createOrdinary()));
        AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, createOrdinary(), withIndex) && !tryCopyTwoObjects(vm, state, realm, withIndex, jsUndefined()));
        AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, createOrdinary(), jsNontrivialString(vm, "ab"_s)));
        AOT_TEST_CHECK(!tryCopyTwoObjects(vm, state, realm, jsUndefined(), jsNull()));
    }

    // A name that is not listed is left out.
    JSObject* withHiddenName = constructEmptyObject(realm);
    withHiddenName->putDirect(vm, x, jsNumber(1), static_cast<unsigned>(PropertyAttribute::DontEnum));
    withHiddenName->putDirect(vm, y, jsNumber(2));
    tryCopyTwoObjects(vm, state, realm, withHiddenName, jsUndefined());
    JSFinalObject* withoutHiddenName = tryCopyTwoObjects(vm, state, realm, withHiddenName, jsUndefined());
    AOT_TEST_CHECK(withoutHiddenName && hasKeys(program, withoutHiddenName, { "y"_s }));
}

template<unsigned numberOfKinds, unsigned maxKindsWithoutPlan>
static void testKindsOfPairsThatComeInTurnsKeepTheirPlans(LoadedProgram& program)
{
    VM& vm = program.vm();
    State& state = program.state();
    JSGlobalObject* realm = program.realm();
    Identifier shared = Identifier::fromString(vm, "shared"_s);
    MarkedArgumentBuffer firsts;
    for (unsigned kind = 0; kind < numberOfKinds; ++kind) {
        JSObject* first = constructEmptyObject(realm);
        first->putDirect(vm, Identifier::fromString(vm, makeString("name"_s, kind)), jsNumber(kind));
        first->putDirect(vm, shared, jsNumber(-1));
        firsts.append(first);
    }
    JSObject* second = constructEmptyObject(realm);
    second->putDirect(vm, shared, jsNumber(7));
    auto copyEachKind = [&] {
        unsigned copied = 0;
        for (unsigned kind = 0; kind < numberOfKinds; ++kind) {
            JSFinalObject* copy = tryCopyTwoObjects(vm, state, realm, firsts.at(kind), second);
            if (!copy)
                continue;
            ++copied;
            AOT_TEST_CHECK(copy->getDirect(vm, Identifier::fromString(vm, makeString("name"_s, kind))) == jsNumber(kind) && copy->getDirect(vm, shared) == jsNumber(7));
        }
        return copied;
    };

    // A collection behind the first plan of all finds no other plans.
    copyEachKind();
    copyEachKind();
    vm.heap.collectSync(CollectionScope::Full);
    for (unsigned turn = 0; turn < 3; ++turn)
        copyEachKind();
    uint64_t plansMade = valueOf(vm, RuntimeCounterID::AssignmentPlansMade);
    for (unsigned turn = 0; turn < 4; ++turn)
        AOT_TEST_CHECK(copyEachKind() >= numberOfKinds - maxKindsWithoutPlan);
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::AssignmentPlansMade) == plansMade);
}

static NEVER_INLINE void assignPointWithAddedName(LoadedProgram& program)
{
    Object* source = createPointWithAddedName(program);
    Object* target = createCopy(program);
    AOT_TEST_CHECK(target->tryAssignPropertiesOf(program.vm(), source) && holdsPropertiesOf(program, target, source));
}

static void testPlansOutliveCollectionsWhileTheirStructuresLive(LoadedProgram& program)
{
    VM& vm = program.vm();
    Strong<JSObject> keepsStructuresAlive(vm, createPointWithAddedName(program));
    Object* firstTarget = createCopy(program);
    AOT_TEST_CHECK(firstTarget->tryAssignPropertiesOf(vm, keepsStructuresAlive.get()));
    Strong<JSObject> keepsStructuresOfTargetAlive(vm, firstTarget);

    assignPointWithAddedName(program);
    uint64_t plansMade = valueOf(vm, RuntimeCounterID::AssignmentPlansMade);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden }) {
        vm.heap.collectSync(scope);
        assignPointWithAddedName(program);
    }
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::AssignmentPlansMade) == plansMade);
}

static void testClonedStructuresAreForgottenAtEachCollection(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        JSObject* source = constructEmptyObject(program.realm());
        put(program, source, "a"_s, 1);
        put(program, source, "b"_s, 2);
        ClonedStructure& cloned = program.instance().allocation.clonedStructures[AllocationWords::placeOfClonedStructure(source->structureID().bits())];
        JSObject* copy = uncheckedDowncast<JSObject>(callOperationFromTest(vm, operationAOTCopyObject, &program.instance(), JSValue::encode(source), 0, 0, nullptr).value);
        AOT_TEST_CHECK(copy != source && copy->structureID() == source->structureID() && holdsPropertiesOf(program, copy, source));
        AOT_TEST_CHECK(static_cast<uint32_t>(cloned.header) == source->structureID().bits() && cloned.cellSize == JSFinalObject::allocationSize(source->structure()->inlineCapacity()));
        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(!cloned.header && !cloned.cellSize);
    }
}

static void testCopiesWithoutFrameFillEmptyObjectsAndAreForgottenAtEachCollection(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    SetForScope withoutValidation(vm.aotState().validatesRuntime, false);
    auto tryCopy = [&](Object* target, Object* source) {
        return tryCopyByFieldMapWithoutFrame(instance, target, JSValue::encode(source), 0);
    };
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        Object* source = createPoint(program, 1, 2);
        Object* first = allocate(program, "Pair"_s);
        const MappedCopy*& place = instance.allocation.copiesWithoutFrame[AllocationWords::placeOfCopyWithoutFrame(first->orderAndShapeWords(), source->orderAndShapeWords(), 0)];
        AOT_TEST_CHECK(!place && !tryCopy(first, source) && enumerableKeysOf(program, first).isEmpty());
        RepeatedCopy way = tryRepeatAssignment(vm, program.state(), first, source, 0);
        AOT_TEST_CHECK(way == RepeatedCopy::ByMap || way == RepeatedCopy::ByMapForTheFirstTime);
        AOT_TEST_CHECK(hasKeys(program, first, { "x"_s, "y"_s }) && holdsPropertiesOf(program, first, source));
        if (way == RepeatedCopy::ByMapForTheFirstTime) {
            AOT_TEST_CHECK(!place && !tryCopy(allocate(program, "Pair"_s), source));
            AOT_TEST_CHECK(tryRepeatAssignment(vm, program.state(), allocate(program, "Pair"_s), source, 0) == RepeatedCopy::ByMap);
        }
        AOT_TEST_CHECK(place && place->takesNoFrame);

        Object* second = allocate(program, "Pair"_s);
        AOT_TEST_CHECK(tryCopy(second, createPoint(program, 3, 4)) && hasKeys(program, second, { "x"_s, "y"_s }));
        AOT_TEST_CHECK(second->orderWord() == first->orderWord() && holdsPropertiesOf(program, second, createPoint(program, 3, 4)));

        AOT_TEST_CHECK(!tryCopy(second, source) && holdsPropertiesOf(program, second, createPoint(program, 3, 4)));
        Object* withOneField = allocate(program, "Pair"_s);
        put(program, withOneField, "y"_s, 5);
        AOT_TEST_CHECK(!tryCopy(withOneField, source) && hasKeys(program, withOneField, { "y"_s }));
        Object* frozenSource = createPoint(program, 6, 7);
        objectConstructorFreeze(program.realm(), frozenSource);
        AOT_TEST_CHECK(!tryCopy(allocate(program, "Pair"_s), frozenSource));
        AOT_TEST_CHECK(!tryCopy(allocate(program, "Tail"_s), source) && !tryCopyByFieldMapWithoutFrame(instance, allocate(program, "Pair"_s), JSValue::encode(source), 1));
        AOT_TEST_CHECK(!tryCopyByFieldMapWithoutFrame(instance, allocate(program, "Pair"_s), JSValue::encode(jsNumber(1)), 0) && !tryCopyByFieldMapWithoutFrame(instance, allocate(program, "Pair"_s), JSValue::encode(constructEmptyObject(program.realm())), 0));

        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(!place);
    }

    Object* source = createPoint(program, 1, 2);
    AOT_TEST_CHECK(tryRepeatAssignment(vm, program.state(), allocate(program, "Pair"_s), source, 0) == RepeatedCopy::ByMap);
    Object* prototype = allocate(program, "Pair"_s);
    AOT_TEST_CHECK(tryCopy(allocate(program, "Pair"_s), source));
    prototype->didBecomePrototype(vm);
    AOT_TEST_CHECK(!tryCopy(prototype, source) && enumerableKeysOf(program, prototype).isEmpty());
}

// Where a later move fails, a field that the target lists keeps what the copy has stored there, which whoever copies in another way stores again.
static void testCopiesWithoutFrameOverwriteFieldsThatTheTargetLists(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    SetForScope withoutValidation(vm.aotState().validatesRuntime, false);
    auto createWithFirstField = [&](ASCIILiteral layoutName) {
        Object* target = allocate(program, layoutName);
        AOT_TEST_CHECK(target->store(vm, target->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(5))));
        return target;
    };
    auto copyAfterTwoCopiesInFrame = [&](ASCIILiteral layoutName, Object* source) {
        AOT_TEST_CHECK(tryRepeatAssignment(vm, program.state(), createWithFirstField(layoutName), source, 0) == RepeatedCopy::ByMapForTheFirstTime);
        AOT_TEST_CHECK(tryRepeatAssignment(vm, program.state(), createWithFirstField(layoutName), source, 0) == RepeatedCopy::ByMap);
        Object* target = createWithFirstField(layoutName);
        bool didCopy = tryCopyByFieldMapWithoutFrame(instance, target, JSValue::encode(source), 0);
        return std::pair { target, didCopy };
    };

    auto [labeled, didCopyBesideLabel] = copyAfterTwoCopiesInFrame("Labeled"_s, createPoint(program, 1, 2));
    AOT_TEST_CHECK(didCopyBesideLabel && hasKeys(program, labeled, { "label"_s, "x"_s, "y"_s }));
    AOT_TEST_CHECK(labeled->get(program.realm(), Identifier::fromString(vm, "label"_s)) == jsNumber(5) && labeled->get(program.realm(), Identifier::fromString(vm, "x"_s)) == jsNumber(1) && labeled->get(program.realm(), Identifier::fromString(vm, "y"_s)) == jsNumber(2));

    auto createPair = [&](ASCIILiteral first, ASCIILiteral second) {
        Object* pair = allocate(program, "Pair"_s);
        put(program, pair, first, 3);
        put(program, pair, second, 4);
        return pair;
    };
    auto valueOf = [&](Object* object, ASCIILiteral name) {
        return object->get(program.realm(), Identifier::fromString(vm, name));
    };
    for (auto [first, second] : { std::pair { "x"_s, "y"_s }, std::pair { "y"_s, "x"_s } }) {
        auto [point, didCopy] = copyAfterTwoCopiesInFrame("Point"_s, createPair(first, second));
        AOT_TEST_CHECK(didCopy && hasKeys(program, point, { "x"_s, "y"_s }) && valueOf(point, first) == jsNumber(3) && valueOf(point, second) == jsNumber(4));

        Object* pairWithoutSecond = createPair(first, second);
        pairWithoutSecond->clear(*pairWithoutSecond->fieldNamed(program.state(), Identifier::fromString(vm, second).impl()));
        Object* target = createWithFirstField("Point"_s);
        AOT_TEST_CHECK(!tryCopyByFieldMapWithoutFrame(instance, target, JSValue::encode(pairWithoutSecond), 0));
        AOT_TEST_CHECK(valueOf(target, first) == jsNumber(3) && valueOf(target, "x"_s) == jsNumber(first == "x"_s ? 3 : 5));
    }
}

static void testIntegersMoveIntoSlotsThatMayHoldNone()
{
    MappedCopy::Move move { };
    move.kind = moveKindFor(SlotRepresentation::Int32, SlotRepresentation::OptionalInt32, false);
    AOT_TEST_CHECK(move.kind == MappedCopy::MoveKind::Int32AsOptionalInt32);
    auto moves = [&](int32_t integer) {
        uint64_t stored = 0;
        bool isLeftToOtherWay = false;
        bool didMove = tryMove(0, move, reinterpret_cast<const uint8_t*>(&integer), [&](auto bits) {
            static_assert(sizeof(bits) <= sizeof(uint64_t));
            stored = bits;
        }, [&] {
            isLeftToOtherWay = true;
            return false;
        });
        AOT_TEST_CHECK(didMove != isLeftToOtherWay && (!didMove || stored == static_cast<uint32_t>(integer)));
        return didMove;
    };
    for (int32_t integer : { minInlineInt32, -1, 0, 1, INT32_MAX })
        AOT_TEST_CHECK(moves(integer));
    for (int32_t integer : { INT32_MIN, absentInt32, minInlineInt32 - 1 })
        AOT_TEST_CHECK(!moves(integer));
}

template<LoadedProgramTest body>
static constexpr void (*withPoints)() = runNativeTestWithLoadedProgram<assignmentPlanTests, programText, body>;

std::span<const NativeTest> assignmentPlanTests()
{
    static constexpr NativeTest tests[] = {
        { "assignment-plans/copies-fields-and-added-names"_s, withPoints<testCopiesFieldsAndAddedNames> },
        { "assignment-plans/stores-into-fields-of-target"_s, withPoints<testStoresIntoFieldsOfTarget> },
        { "assignment-plans/order-of-added-names-selects-plan"_s, withPoints<testOrderOfAddedNamesSelectsPlan> },
        { "assignment-plans/creates-field-behind-added-names"_s, withPoints<testCreatesFieldBehindAddedNames> },
        { "assignment-plans/keeps-order-where-field-lies-between-added-names"_s, withPoints<testKeepsOrderWhereFieldLiesBetweenAddedNames> },
        { "assignment-plans/leaves-out-excluded-names"_s, withPoints<testLeavesOutExcludedNames> },
        { "assignment-plans/refuses-what-only-lookup-can-copy"_s, withPoints<testRefusesWhatOnlyLookupCanCopy> },
        { "assignment-plans/follows-changes-to-fields"_s, withPoints<testFollowsChangesToFields> },
        { "assignment-plans/copies-fields-of-same-layout-in-any-order"_s, withPoints<testCopiesFieldsOfSameLayoutInAnyOrder> },
        { "assignment-plans/defines-fields-at-once-or-not-at-all"_s, withPoints<testDefinesFieldsAtOnceOrNotAtAll> },
        { "assignment-plans/copies-as-ordinary-object"_s, withPoints<testCopiesAsOrdinaryObject> },
        { "assignment-plans/copies-two-objects-at-once"_s, withPoints<testCopiesTwoObjectsAtOnce> },
        { "assignment-plans/kinds-of-pairs-that-come-in-turns-keep-their-plans"_s, withPoints<testKindsOfPairsThatComeInTurnsKeepTheirPlans<24, 4>> },
        { "assignment-plans/kinds-of-pairs-that-find-no-place-leave-the-plans-of-others"_s, withPoints<testKindsOfPairsThatComeInTurnsKeepTheirPlans<60, 60>> },
        { "assignment-plans/plans-outlive-collections-while-their-structures-live"_s, withPoints<testPlansOutliveCollectionsWhileTheirStructuresLive> },
        { "assignment-plans/cloned-structures-are-forgotten-at-each-collection"_s, withPoints<testClonedStructuresAreForgottenAtEachCollection> },
        { "assignment-plans/copies-without-frame-fill-empty-objects-and-are-forgotten-at-each-collection"_s, withPoints<testCopiesWithoutFrameFillEmptyObjectsAndAreForgottenAtEachCollection> },
        { "assignment-plans/copies-without-frame-overwrite-fields-that-the-target-lists"_s, withPoints<testCopiesWithoutFrameOverwriteFieldsThatTheTargetLists> },
        { "assignment-plans/integers-move-into-slots-that-may-hold-none"_s, testIntegersMoveIntoSlotsThatMayHoldNone },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
