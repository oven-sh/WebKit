/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectCopy.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeCounters.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include <array>

namespace JSC { namespace AOT {

std::span<const NativeTest> assignmentPlanTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Point" fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="y" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Copy"
layout L2 name="Tail" fields=[{key="z" declared={type=anything}}]
layout L3 name="Pair" fields=[{key="x" declared={type=anything}}, {key="y" declared={type=anything}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = Call v1, -, v2, v3, v4, v5, v6
    Return
}
function f1 name="makePoint" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    Return v3
}
function f2 name="makeCopy" results=[{type=anything}] {
  b0():
    v0 = NewObject layout=L1 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f3 name="storeAndDelete" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: string, v2: anything):
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    Return
}
function f4 name="makeTail" results=[{type=anything}] {
  b0():
    v0 = NewObject layout=L2 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f5 name="makePair" results=[{type=anything}] {
  b0():
    v0 = NewObject layout=L3 fields=[]
    v1 = FinishObject v0
    Return v1
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

static Vector<Identifier> enumerableKeysOf(LoadedProgram& program, JSObject* object)
{
    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, program.realm(), names, DontEnumPropertiesMode::Exclude);
    return Vector<Identifier>(names.size(), [&](size_t index) {
        return names[index];
    });
}

static Object* createPoint(LoadedProgram& program, int32_t x, int32_t y)
{
    Object* point = allocate(program, "Point"_s);
    AOT_TEST_CHECK(point->layout().extraPropertiesSlot());
    AOT_TEST_CHECK(point->store(program.vm(), point->layout().fields()[0], jsNumber(x)) && point->store(program.vm(), point->layout().fields()[1], jsNumber(y)));
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
    Vector<Identifier> keys = enumerableKeysOf(program, object);
    return std::ranges::equal(keys, expected, [](const Identifier& key, ASCIILiteral name) {
        return key.string() == name;
    });
}

static bool holdsPropertiesOf(LoadedProgram& program, JSObject* target, JSObject* source)
{
    Vector<Identifier> keys = enumerableKeysOf(program, source);
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

static void testMakesPlanAtSecondCopyWhereAsked(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint64_t plansMade = valueOf(vm, RuntimeCounterID::AssignmentPlansMade);
    AOT_TEST_CHECK(!createCopy(program)->tryAssignPropertiesOf(vm, createPointWithAddedName(program), { }, Object::PlanCreation::AtSecondCopy));
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::AssignmentPlansMade) == plansMade);
    for (int32_t turn = 0; turn < 2; ++turn) {
        Object* source = createPointWithAddedName(program);
        Object* target = createCopy(program);
        AOT_TEST_CHECK(target->tryAssignPropertiesOf(vm, source, { }, Object::PlanCreation::AtSecondCopy) && holdsPropertiesOf(program, target, source));
        AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::AssignmentPlansMade) == plansMade + 1);
    }
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
        { "assignment-plans/plans-outlive-collections-while-their-structures-live"_s, withPoints<testPlansOutliveCollectionsWhileTheirStructuresLive> },
        { "assignment-plans/makes-plan-at-second-copy-where-asked"_s, withPoints<testMakesPlanAtSecondCopyWhereAsked> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
