/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "CustomGetterSetter.h"
#include "JSCInlines.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include <algorithm>
#include <array>
#include <numeric>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> objectSlotExitTests();

static constexpr ASCIILiteral mutatingProgramText = R"IR(layout L0 name="Mutable" fields=[{key="a" declared={type=number} flags=[IsRequired]}, {key="b" declared={type=string} flags=[IsRequired]}, {key="c" declared={type=anything} flags=[IsRequired]}, {key="d" declared={type=boolean} flags=[IsRequired]}]
layout L1 name="Deletable" fields=[{key="a" declared={type=number} flags=[IsRequired]}, {key="b" declared={type=string} flags=[IsRequired]}]
layout L2 name="Bag" fields=[{key="k0" declared={type=number}}, {key="k1" declared={type=number}}, {key="k2" declared={type=number}}, {key="k3" declared={type=number}}, {key="k4" declared={type=number}}, {key="k5" declared={type=number}}, {key="k6" declared={type=number}}, {key="k7" declared={type=number}}, {key="k8" declared={type=number}}]
variable g0 name="deletable" module=m0 declared={type=anything} initialization=Undefined
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
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="makeMutable" parameters=[{type=number}, {type=string}, {type=anything}, {type=boolean}] results=[{type=anything}] {
  b0(v0: number, v1: string, v2: anything, v3: boolean):
    v4 = NewObject -, v0, v1, v2, v3 layout=L0 fields=[0, 1, 2, 3]
    v5 = FinishObject v4
    Return v5
}
function f2 name="mutate" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: string, v2: anything):
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    v5 = Builtin builtin=ObjectFreeze
    v6 = Call v5, -, v0
    Return
}
function f3 name="useDeletable" parameters=[{type=number}, {type=string}] results=[{type=anything}] {
  b0(v0: number, v1: string):
    v2 = NewObject -, v0, v1 layout=L1 fields=[0, 1]
    v3 = FinishObject v2
    v4 = GetModuleVariable variable=g0
    PutModuleVariable v3 variable=g0
    v6 = PropGet v4 key="a"
    v7 = PropGet v4 key="b"
    v8 = PropDelete v4 key="a"
    v9 = PropDelete v4 key="b"
    v10 = NewArray v6, v7 contents=anything holes=[]
    Return v10
}
function f4 name="makeBag" results=[{type=anything}] {
  b0():
    v0 = NewObject layout=L2 fields=[]
    v1 = FinishObject v0
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral programWithoutMutationsText = R"IR(layout L0 name="Extensible" fields=[{key="a" declared={type=number} flags=[IsRequired]}, {key="b" declared={type=string} flags=[IsRequired]}, {key="c" declared={type=anything} flags=[IsRequired]}, {key="d" declared={type=boolean} flags=[IsRequired]}]
layout L1 name="Closed" fields=[{key="a" declared={type=number} flags=[IsRequired]}, {key="b" declared={type=string} flags=[IsRequired]}]
variable g0 name="closed" module=m0 declared={type=anything} initialization=Undefined
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="makeExtensible" parameters=[{type=number}, {type=string}, {type=anything}, {type=boolean}] results=[{type=anything}] {
  b0(v0: number, v1: string, v2: anything, v3: boolean):
    v4 = NewObject -, v0, v1, v2, v3 layout=L0 fields=[0, 1, 2, 3]
    v5 = FinishObject v4
    Return v5
}
function f2 name="extend" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="extra"
    Return
}
function f3 name="useClosed" parameters=[{type=number}, {type=string}] results=[{type=anything}] {
  b0(v0: number, v1: string):
    v2 = NewObject -, v0, v1 layout=L1 fields=[0, 1]
    v3 = FinishObject v2
    v4 = GetModuleVariable variable=g0
    PutModuleVariable v3 variable=g0
    v6 = PropGet v4 key="a"
    v7 = PropGet v4 key="b"
    v8 = NewArray v6, v7 contents=anything holes=[]
    Return v8
}
)IR"_s;

namespace {

enum class Event : uint8_t { Delete, Redefinition, StoreOfOtherType };

struct Costs {
    friend bool operator==(const Costs&, const Costs&) = default;

    uint64_t shapeEpoch;
    uint64_t shapeEpochBumps;
    uint64_t fieldsLeftSlotByDelete;
    uint64_t fieldsLeftSlotByRedefinition;
    uint64_t fieldsLeftSlotByStoreOfOtherType;
    uint64_t objectsBecameOrdinaryByIndexedAccessor;
    uint64_t objectsBecameOrdinaryByNativeDefinition;
    uint64_t objectsBecameOrdinaryByRepresentationLimit;
    uint64_t keyOrdersStoredOutOfLine;
};

struct OwnProperty {
    bool isFound { false };
    JSValue value;
    unsigned attributes { 0 };
};

struct Outcomes {
    unsigned kept { 0 };
    unsigned lost { 0 };
};

} // anonymous namespace

static constexpr unsigned noAttributes = 0;
static constexpr unsigned dontEnum = static_cast<unsigned>(PropertyAttribute::DontEnum);
static constexpr unsigned dontDelete = static_cast<unsigned>(PropertyAttribute::DontDelete);
static constexpr unsigned readOnly = static_cast<unsigned>(PropertyAttribute::ReadOnly);

static Costs costsOf(LoadedProgram& program)
{
    VM& vm = program.vm();
    return {
        program.instance().shapeEpoch,
        valueOf(vm, RuntimeCounterID::ShapeEpochBumps),
        valueOf(vm, RuntimeCounterID::FieldsLeftSlotByDelete),
        valueOf(vm, RuntimeCounterID::FieldsLeftSlotByRedefinition),
        valueOf(vm, RuntimeCounterID::FieldsLeftSlotByStoreOfOtherType),
        valueOf(vm, RuntimeCounterID::ObjectsBecameOrdinaryByIndexedAccessor),
        valueOf(vm, RuntimeCounterID::ObjectsBecameOrdinaryByNativeDefinition),
        valueOf(vm, RuntimeCounterID::ObjectsBecameOrdinaryByRepresentationLimit),
        valueOf(vm, RuntimeCounterID::KeyOrdersStoredOutOfLine),
    };
}

static Costs afterLosingShape(Costs costs)
{
    ++costs.shapeEpoch;
    ++costs.shapeEpochBumps;
    ++costs.objectsBecameOrdinaryByRepresentationLimit;
    return costs;
}

static Costs afterLeavingSlot(Costs costs, Event event)
{
    switch (event) {
    case Event::Delete:
        ++costs.fieldsLeftSlotByDelete;
        return costs;
    case Event::Redefinition:
        ++costs.fieldsLeftSlotByRedefinition;
        return costs;
    case Event::StoreOfOtherType:
        ++costs.fieldsLeftSlotByStoreOfOtherType;
        return costs;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const LayoutData& layoutNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static Object* allocate(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static Identifier nameOf(LoadedProgram& program, const FieldData& field)
{
    return Identifier::fromUid(program.vm(), program.state().objects->layouts.nameOf(program.vm(), field));
}

static JSValue fittingValueFor(LoadedProgram& program, const FieldData& field)
{
    switch (nameOf(program, field).string()[0]) {
    case 'a':
        return jsNumber(1.5);
    case 'b':
        return jsNontrivialString(program.vm(), "text"_s);
    case 'c':
        return jsNull();
    case 'd':
        return jsBoolean(true);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue valueOfOtherTypeFor(LoadedProgram& program, const FieldData& field)
{
    switch (nameOf(program, field).string()[0]) {
    case 'a':
        return jsNontrivialString(program.vm(), "not a number"_s);
    case 'b':
    case 'd':
        return jsNumber(7);
    case 'c':
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Object* createFilled(LoadedProgram& program, ASCIILiteral layoutName)
{
    Object* object = allocate(program, layoutName);
    for (const FieldData& field : object->layout().fields()) {
        AOT_TEST_CHECK(object->store(program.vm(), field, fittingValueFor(program, field)));
        AOT_TEST_CHECK(object->isInSlot(field) && object->isInKeyOrder(program.state(), field));
    }
    AOT_TEST_CHECK(!object->hasFlagsInOrderWord());
    return object;
}

static uint32_t wordAtShapeOffset(JSObject* object)
{
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(object) + offsetOfShapeWord);
}

static String keysOf(LoadedProgram& program, JSObject* object, DontEnumPropertiesMode mode = DontEnumPropertiesMode::Include)
{
    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, program.realm(), names, mode);
    StringBuilder keys;
    for (auto& name : names)
        keys.append(keys.isEmpty() ? ""_s : ","_s, name.string());
    return keys.toString();
}

static String keysOfLayoutWithout(LoadedProgram& program, const LayoutData& layout, const FieldData* leftOut)
{
    StringBuilder keys;
    for (const FieldData& field : layout.fields()) {
        if (&field != leftOut)
            keys.append(keys.isEmpty() ? ""_s : ","_s, nameOf(program, field).string());
    }
    return keys.toString();
}

static OwnProperty ownProperty(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    if (!object->methodTable()->getOwnPropertySlot(object, program.realm(), name, slot))
        return { };
    return { true, slot.getValue(program.realm(), name), slot.attributes() };
}

static bool hasOwnProperty(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value, unsigned attributes)
{
    OwnProperty property = ownProperty(program, object, name);
    return property.isFound && JSValue::strictEqual(program.realm(), property.value, value) && property.attributes == attributes;
}

static bool put(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value)
{
    PutPropertySlot slot(object);
    return object->methodTable()->put(object, program.realm(), name, value, slot);
}

static bool deleteProperty(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    DeletePropertySlot slot;
    return object->methodTable()->deleteProperty(object, program.realm(), name, slot);
}

static bool setEnumerable(LoadedProgram& program, JSObject* object, const Identifier& name, bool isEnumerable)
{
    PropertyDescriptor descriptor;
    descriptor.setEnumerable(isEnumerable);
    return object->methodTable()->defineOwnProperty(object, program.realm(), name, descriptor, false);
}

static bool apply(LoadedProgram& program, Event event, JSObject* object, const FieldData& field)
{
    Identifier name = nameOf(program, field);
    switch (event) {
    case Event::Delete:
        return deleteProperty(program, object, name);
    case Event::Redefinition:
        return setEnumerable(program, object, name, false);
    case Event::StoreOfOtherType:
        return put(program, object, name, valueOfOtherTypeFor(program, field));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ShapeStatus expectedStatus(const LayoutData& layout, const FieldData& field, Event event)
{
    if (!field.canLeaveSlot())
        return ShapeStatus::Lost;
    if (event == Event::Delete)
        return ShapeStatus::Kept;
    return layout.extraPropertiesSlot() && !(field.flags & fieldMayExistWithoutStorage) ? ShapeStatus::Kept : ShapeStatus::Lost;
}

static void checkWhatGenericCodeSees(LoadedProgram& program, Event event, JSObject* object, const LayoutData& layout, const FieldData& changed)
{
    String everyKey = keysOfLayoutWithout(program, layout, nullptr);
    String otherKeys = keysOfLayoutWithout(program, layout, &changed);
    Identifier name = nameOf(program, changed);
    switch (event) {
    case Event::Delete:
        AOT_TEST_CHECK(!ownProperty(program, object, name).isFound);
        AOT_TEST_CHECK(keysOf(program, object) == otherKeys);
        break;
    case Event::Redefinition:
        AOT_TEST_CHECK(hasOwnProperty(program, object, name, fittingValueFor(program, changed), dontEnum));
        AOT_TEST_CHECK(keysOf(program, object) == everyKey && keysOf(program, object, DontEnumPropertiesMode::Exclude) == otherKeys);
        break;
    case Event::StoreOfOtherType:
        AOT_TEST_CHECK(hasOwnProperty(program, object, name, valueOfOtherTypeFor(program, changed), noAttributes));
        AOT_TEST_CHECK(keysOf(program, object) == everyKey && keysOf(program, object, DontEnumPropertiesMode::Exclude) == everyKey);
        break;
    }
    for (const FieldData& field : layout.fields()) {
        if (&field != &changed)
            AOT_TEST_CHECK(hasOwnProperty(program, object, nameOf(program, field), fittingValueFor(program, field), noAttributes));
    }
}

static Outcomes checkEventOnEachField(LoadedProgram& program, ASCIILiteral layoutName, Event event)
{
    Outcomes outcomes;
    const LayoutData& layout = layoutNamed(program, layoutName);
    for (const FieldData& field : layout.fields()) {
        if (event == Event::StoreOfOtherType && !valueOfOtherTypeFor(program, field))
            continue;
        Object* object = createFilled(program, layoutName);
        uint32_t shapeWord = object->shapeWord();
        AOT_TEST_CHECK(shapeWord == shapeWordOf(layout.shape) && canPassShapeTest(shapeWord));
        Costs costsBefore = costsOf(program);
        AOT_TEST_CHECK(apply(program, event, object, field));

        if (expectedStatus(layout, field, event) == ShapeStatus::Lost) {
            ++outcomes.lost;
            AOT_TEST_CHECK(costsOf(program) == afterLosingShape(costsBefore));
            AOT_TEST_CHECK(!isCompiledObject(object) && object->type() == FinalObjectType && !canPassShapeTest(wordAtShapeOffset(object)));
        } else {
            ++outcomes.kept;
            AOT_TEST_CHECK(costsOf(program) == afterLeavingSlot(costsBefore, event));
            AOT_TEST_CHECK(isCompiledObject(object) && object->shapeWord() == shapeWord);
            AOT_TEST_CHECK(!object->isInSlot(field) && !object->read(field));
            AOT_TEST_CHECK(object->isInKeyOrder(program.state(), field) == (event != Event::Delete));
            AOT_TEST_CHECK(object->hasExtraProperties() == (event != Event::Delete) && object->compiledStructure().hasExtraProperties() == object->hasExtraProperties());
        }
        checkWhatGenericCodeSees(program, event, object, layout, field);
    }
    return outcomes;
}

static bool hasOutcomes(Outcomes outcomes, unsigned kept, unsigned lost)
{
    return outcomes.kept == kept && outcomes.lost == lost;
}

static void checkFieldsWithAbsentBitsAndExtraPropertiesWord(LoadedProgram& program)
{
    AOT_TEST_CHECK(layoutNamed(program, "Mutable"_s).extraPropertiesSlot());
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Mutable"_s, Event::Delete), 4, 0));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Mutable"_s, Event::Redefinition), 4, 0));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Mutable"_s, Event::StoreOfOtherType), 3, 0));
}

static void checkFieldsWithAbsentBitsWithoutExtraPropertiesWord(LoadedProgram& program)
{
    AOT_TEST_CHECK(!layoutNamed(program, "Deletable"_s).extraPropertiesSlot());
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Deletable"_s, Event::Delete), 2, 0));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Deletable"_s, Event::Redefinition), 0, 2));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Deletable"_s, Event::StoreOfOtherType), 0, 2));
}

static void checkFieldsWithoutAbsentBitsWithExtraPropertiesWord(LoadedProgram& program)
{
    const LayoutData& layout = layoutNamed(program, "Extensible"_s);
    AOT_TEST_CHECK(layout.extraPropertiesSlot() && std::ranges::count_if(layout.fields(), &FieldData::canLeaveSlot) == 2);
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Extensible"_s, Event::Delete), 2, 2));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Extensible"_s, Event::Redefinition), 2, 2));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Extensible"_s, Event::StoreOfOtherType), 1, 2));
}

static void checkFieldsWithoutAbsentBitsWithoutExtraPropertiesWord(LoadedProgram& program)
{
    const LayoutData& layout = layoutNamed(program, "Closed"_s);
    AOT_TEST_CHECK(!layout.extraPropertiesSlot() && !layout.fields()[0].canLeaveSlot() && layout.fields()[1].canLeaveSlot());
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Closed"_s, Event::Delete), 1, 1));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Closed"_s, Event::Redefinition), 0, 2));
    AOT_TEST_CHECK(hasOutcomes(checkEventOnEachField(program, "Closed"_s, Event::StoreOfOtherType), 0, 2));
}

static void checkStoreOfFittingValueCostsNothing(LoadedProgram& program)
{
    for (ASCIILiteral layoutName : { "Mutable"_s, "Deletable"_s }) {
        Object* object = createFilled(program, layoutName);
        Structure* structure = object->structure();
        uint64_t words = object->orderAndShapeWords();
        Costs costsBefore = costsOf(program);
        for (const FieldData& field : object->layout().fields()) {
            AOT_TEST_CHECK(put(program, object, nameOf(program, field), fittingValueFor(program, field)));
            AOT_TEST_CHECK(setEnumerable(program, object, nameOf(program, field), true));
            AOT_TEST_CHECK(object->isInSlot(field));
        }
        for (JSValue value : { jsNumber(1), jsUndefined(), JSValue(object), jsBoolean(false) }) {
            if (layoutName == "Mutable"_s)
                AOT_TEST_CHECK(put(program, object, Identifier::fromString(program.vm(), "c"_s), value) && object->isInSlot(object->layout().fields()[2]));
        }
        AOT_TEST_CHECK(costsOf(program) == costsBefore && object->structure() == structure && object->orderAndShapeWords() == words);
    }
}

static void checkDeletedFieldIsCreatedAgainInItsSlot(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    Structure* structure = object->structure();
    const FieldData& field = object->layout().fields()[0];
    Identifier name = nameOf(program, field);
    AOT_TEST_CHECK(deleteProperty(program, object, name) && keysOf(program, object) == "b,c,d"_s && object->structure() == structure);

    Costs costsBefore = costsOf(program);
    AOT_TEST_CHECK(deleteProperty(program, object, name) && costsOf(program) == costsBefore);
    AOT_TEST_CHECK(put(program, object, name, jsNumber(2.5)));
    AOT_TEST_CHECK(object->isInSlot(field) && object->isInKeyOrder(program.state(), field) && !object->hasExtraProperties());
    AOT_TEST_CHECK(hasOwnProperty(program, object, name, jsNumber(2.5), noAttributes) && keysOf(program, object) == "b,c,d,a"_s);
    AOT_TEST_CHECK(costsOf(program) == costsBefore && object->structure() == structure);
}

static void checkDeletedFieldIsCreatedOutsideItsSlot(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    const FieldData& field = object->layout().fields()[0];
    Identifier name = nameOf(program, field);
    AOT_TEST_CHECK(deleteProperty(program, object, name));

    Costs costsBefore = costsOf(program);
    JSValue text = valueOfOtherTypeFor(program, field);
    AOT_TEST_CHECK(put(program, object, name, text));
    AOT_TEST_CHECK(costsOf(program) == afterLeavingSlot(costsBefore, Event::StoreOfOtherType));
    AOT_TEST_CHECK(isCompiledObject(object) && !object->isInSlot(field) && object->isInKeyOrder(program.state(), field));
    AOT_TEST_CHECK(hasOwnProperty(program, object, name, text, noAttributes) && keysOf(program, object) == "b,c,d,a"_s);
}

static void checkStoreThatFitsMovesFieldBack(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    for (const FieldData& field : object->layout().fields()) {
        JSValue other = valueOfOtherTypeFor(program, field);
        if (!other)
            continue;
        Identifier name = nameOf(program, field);
        AOT_TEST_CHECK(put(program, object, name, other) && !object->isInSlot(field));

        Costs costsBefore = costsOf(program);
        AOT_TEST_CHECK(put(program, object, name, jsUndefined()) && !object->isInSlot(field));
        AOT_TEST_CHECK(hasOwnProperty(program, object, name, jsUndefined(), noAttributes));

        AOT_TEST_CHECK(put(program, object, name, fittingValueFor(program, field)));
        AOT_TEST_CHECK(object->isInSlot(field) && JSValue::strictEqual(program.realm(), object->read(field), fittingValueFor(program, field)));
        AOT_TEST_CHECK(hasOwnProperty(program, object, name, fittingValueFor(program, field), noAttributes));
        AOT_TEST_CHECK(keysOf(program, object) == "a,b,c,d"_s && costsOf(program) == costsBefore);
        if (JSObject* extraProperties = object->extraProperties())
            AOT_TEST_CHECK(!extraProperties->getDirect(program.vm(), name));
    }
}

static void checkUsualAttributesMoveFieldBack(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    const FieldData& field = object->layout().fields()[0];
    Identifier name = nameOf(program, field);
    AOT_TEST_CHECK(setEnumerable(program, object, name, false) && !object->isInSlot(field));

    Costs costsBefore = costsOf(program);
    AOT_TEST_CHECK(put(program, object, name, jsNumber(2.5)));
    AOT_TEST_CHECK(!object->isInSlot(field) && hasOwnProperty(program, object, name, jsNumber(2.5), dontEnum));

    AOT_TEST_CHECK(setEnumerable(program, object, name, true));
    AOT_TEST_CHECK(object->isInSlot(field) && hasOwnProperty(program, object, name, jsNumber(2.5), noAttributes));
    AOT_TEST_CHECK(keysOf(program, object, DontEnumPropertiesMode::Exclude) == "a,b,c,d"_s && costsOf(program) == costsBefore);
}

static void checkAccessorInPlaceOfField(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    const FieldData& field = object->layout().fields()[1];
    Identifier name = nameOf(program, field);
    Costs costsBefore = costsOf(program);

    PropertyDescriptor accessor;
    accessor.setGetter(program.realms().evaluate(program.realm(), "(function () { return this })"_s));
    AOT_TEST_CHECK(object->methodTable()->defineOwnProperty(object, program.realm(), name, accessor, false));
    AOT_TEST_CHECK(costsOf(program) == afterLeavingSlot(costsBefore, Event::Redefinition) && isCompiledObject(object) && !object->isInSlot(field));
    AOT_TEST_CHECK(ownProperty(program, object, name).attributes & PropertyAttribute::Accessor);
    AOT_TEST_CHECK(object->get(program.realm(), name) == JSValue(object) && keysOf(program, object) == "a,b,c,d"_s);

    AOT_TEST_CHECK(!put(program, object, name, fittingValueFor(program, field)) && !object->isInSlot(field));

    PropertyDescriptor data;
    data.setValue(fittingValueFor(program, field));
    data.setWritable(true);
    AOT_TEST_CHECK(object->methodTable()->defineOwnProperty(object, program.realm(), name, data, false));
    AOT_TEST_CHECK(object->isInSlot(field) && hasOwnProperty(program, object, name, fittingValueFor(program, field), noAttributes));
    AOT_TEST_CHECK(costsOf(program) == afterLeavingSlot(costsBefore, Event::Redefinition));
}

// The first read of an object whose Structure does not name the Structure of its extra properties gives the object one that does, and files nothing.
static PropertyAnswer classifySecondRead(LoadedProgram& program, Object* object, const Identifier& name)
{
    PropertyAnswer answer;
    for (unsigned read = 0; read < 2; ++read) {
        Structure* structureBefore = object->structure();
        PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
        JSValue(object).get(program.realm(), name, slot);
        answer = classifyRead({ program.realm(), object, structureBefore, name.impl(), slot });
    }
    return answer;
}

static void checkReadOfFieldOutsideItsSlotIsCached(LoadedProgram& program)
{
    for (Event event : { Event::Redefinition, Event::StoreOfOtherType }) {
        Object* object = createFilled(program, "Mutable"_s);
        std::span<const FieldData> fields = object->layout().fields();
        const FieldData& field = fields[0];
        Identifier name = nameOf(program, field);
        AOT_TEST_CHECK(classifySecondRead(program, object, name).kind == PropertyAnswerKind::Field);

        AOT_TEST_CHECK(apply(program, event, object, field) && isCompiledObject(object) && !object->isInSlot(field));
        PropertyAnswer outsideSlot = classifySecondRead(program, object, name);
        AOT_TEST_CHECK(outsideSlot.kind == PropertyAnswerKind::ExtraProperty && outsideSlot.isOwn && !outsideSlot.dependsOnChain());
        AOT_TEST_CHECK(object->compiledStructure().extraPropertiesStructure() == object->extraProperties()->structure());
        AOT_TEST_CHECK(object->extraProperties()->getDirect(outsideSlot.offset) == ownProperty(program, object, name).value);
        AOT_TEST_CHECK(slotFor(outsideSlot, object->structure()).kind == Slot::Kind::ExtraProperty);

        PropertyAnswer neighbor = classifySecondRead(program, object, nameOf(program, fields[1]));
        AOT_TEST_CHECK(neighbor.kind == PropertyAnswerKind::Field && neighbor.field == &fields[1]);

        AOT_TEST_CHECK(setEnumerable(program, object, name, true) && put(program, object, name, fittingValueFor(program, field)) && object->isInSlot(field));
        AOT_TEST_CHECK(classifySecondRead(program, object, name).kind == PropertyAnswerKind::Field);
    }
}

static void checkReadOfGetterInPlaceOfFieldIsCached(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    const FieldData& field = object->layout().fields()[1];
    Identifier name = nameOf(program, field);
    PropertyDescriptor accessor;
    accessor.setGetter(program.realms().evaluate(program.realm(), "(function () { return this })"_s));
    AOT_TEST_CHECK(object->methodTable()->defineOwnProperty(object, program.realm(), name, accessor, false) && !object->isInSlot(field));

    PropertyAnswer answer = classifySecondRead(program, object, name);
    AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::ExtraAccessor && slotFor(answer, object->structure()).kind == Slot::Kind::ExtraGetter);
}

static void checkReadOfDeletedFieldStaysWithItsSlot(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    std::span<const FieldData> fields = object->layout().fields();
    AOT_TEST_CHECK(apply(program, Event::Delete, object, fields[0]) && !object->isInSlot(fields[0]) && !object->hasExtraProperties());
    PropertyAnswer withoutExtraProperties = classifySecondRead(program, object, nameOf(program, fields[0]));
    AOT_TEST_CHECK(withoutExtraProperties.kind == PropertyAnswerKind::Field && withoutExtraProperties.absentIsUndefined);

    AOT_TEST_CHECK(apply(program, Event::Redefinition, object, fields[1]) && object->hasExtraProperties());
    PropertyAnswer besideExtraProperty = classifySecondRead(program, object, nameOf(program, fields[0]));
    AOT_TEST_CHECK(besideExtraProperty.kind == PropertyAnswerKind::Field && besideExtraProperty.absentIsUndefined);
}

static void checkEmptyExtraPropertiesAreDropped(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    Structure* structure = object->structure();
    const FieldData& field = object->layout().fields()[0];
    Identifier name = nameOf(program, field);
    auto hasNoExtraProperties = [&] {
        return !object->hasExtraProperties() && !object->compiledStructure().hasExtraProperties() && !object->extraProperties() && object->structure() == structure;
    };

    AOT_TEST_CHECK(put(program, object, name, valueOfOtherTypeFor(program, field)) && object->hasExtraProperties() && object->structure() != structure);
    AOT_TEST_CHECK(put(program, object, name, fittingValueFor(program, field)) && object->isInSlot(field));
    AOT_TEST_CHECK(hasNoExtraProperties());

    Identifier extra = Identifier::fromString(program.vm(), "extra"_s);
    AOT_TEST_CHECK(put(program, object, extra, jsNumber(1)) && object->hasExtraProperties());
    AOT_TEST_CHECK(deleteProperty(program, object, extra) && hasNoExtraProperties());

    AOT_TEST_CHECK(setEnumerable(program, object, name, false) && object->hasExtraProperties());
    AOT_TEST_CHECK(deleteProperty(program, object, name) && hasNoExtraProperties());
}

static void checkNameThatIsNoField(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    Structure* structure = object->structure();
    uint32_t shapeWord = object->shapeWord();
    Identifier extra = Identifier::fromString(program.vm(), "extra"_s);
    Costs costsBefore = costsOf(program);

    AOT_TEST_CHECK(put(program, object, extra, jsNumber(1)));
    AOT_TEST_CHECK(object->hasExtraProperties() && object->compiledStructure().hasExtraProperties() && object->structure() != structure);
    AOT_TEST_CHECK(hasOwnProperty(program, object, extra, jsNumber(1), noAttributes) && keysOf(program, object) == "a,b,c,d,extra"_s);
    for (const FieldData& field : object->layout().fields())
        AOT_TEST_CHECK(object->isInSlot(field));

    AOT_TEST_CHECK(deleteProperty(program, object, nameOf(program, object->layout().fields()[0])));
    AOT_TEST_CHECK(put(program, object, nameOf(program, object->layout().fields()[0]), jsNumber(3)) && keysOf(program, object) == "b,c,d,extra,a"_s);
    AOT_TEST_CHECK(deleteProperty(program, object, extra) && !ownProperty(program, object, extra).isFound && keysOf(program, object) == "b,c,d,a"_s);
    AOT_TEST_CHECK(costsOf(program) == afterLeavingSlot(costsBefore, Event::Delete) && object->shapeWord() == shapeWord);

    Object* withoutWord = createFilled(program, "Deletable"_s);
    costsBefore = costsOf(program);
    AOT_TEST_CHECK(put(program, withoutWord, extra, jsNumber(1)));
    AOT_TEST_CHECK(costsOf(program) == afterLosingShape(costsBefore) && !isCompiledObject(withoutWord));
    AOT_TEST_CHECK(hasOwnProperty(program, withoutWord, extra, jsNumber(1), noAttributes) && keysOf(program, withoutWord) == "a,b,extra"_s);
}

static void checkIntegrityIsBitsOfTheOrderWord(LoadedProgram& program)
{
    struct Level {
        Object::Integrity integrity;
        uint32_t bits;
        unsigned attributes;
    };
    for (Level level : { Level { Object::Integrity::NotExtensible, orderWordIsNotExtensible, noAttributes }, Level { Object::Integrity::Sealed, orderWordIsNotExtensible | orderWordIsSealed, dontDelete }, Level { Object::Integrity::Frozen, orderWordIntegrityBits, dontDelete | readOnly } }) {
        Object* object = createFilled(program, "Mutable"_s);
        Structure* structure = object->structure();
        uint32_t orderWord = object->orderWord();
        uint32_t shapeWord = object->shapeWord();
        Costs costsBefore = costsOf(program);
        object->setIntegrity(program.realm(), level.integrity);
        AOT_TEST_CHECK(object->orderWord() == (orderWord | level.bits) && object->shapeWord() == shapeWord && object->structure() == structure);

        for (const FieldData& field : object->layout().fields()) {
            Identifier name = nameOf(program, field);
            AOT_TEST_CHECK(hasOwnProperty(program, object, name, fittingValueFor(program, field), level.attributes));
            if (level.attributes & dontDelete)
                AOT_TEST_CHECK(!deleteProperty(program, object, name) && !setEnumerable(program, object, name, false));
            if (level.attributes & readOnly)
                AOT_TEST_CHECK(!put(program, object, name, fittingValueFor(program, field)));
            AOT_TEST_CHECK(object->isInSlot(field));
        }
        AOT_TEST_CHECK(!put(program, object, Identifier::fromString(program.vm(), "extra"_s), jsNumber(1)) && !object->hasExtraProperties());
        AOT_TEST_CHECK(costsOf(program) == costsBefore && keysOf(program, object) == "a,b,c,d"_s);
    }
}

static void checkIndexedProperties(LoadedProgram& program)
{
    Object* object = createFilled(program, "Mutable"_s);
    Costs costsBefore = costsOf(program);
    AOT_TEST_CHECK(object->methodTable()->putByIndex(object, program.realm(), 0, jsNumber(1), false));
    AOT_TEST_CHECK(costsOf(program) == costsBefore && isCompiledObject(object) && object->hasExtraProperties() && keysOf(program, object) == "0,a,b,c,d"_s);

    PropertyDescriptor accessor;
    accessor.setGetter(program.realms().evaluate(program.realm(), "(function () { return 2 })"_s));
    AOT_TEST_CHECK(object->methodTable()->defineOwnProperty(object, program.realm(), Identifier::from(program.vm(), 1u), accessor, false));
    Costs expected = costsBefore;
    ++expected.shapeEpoch;
    ++expected.shapeEpochBumps;
    ++expected.objectsBecameOrdinaryByIndexedAccessor;
    AOT_TEST_CHECK(costsOf(program) == expected && !isCompiledObject(object) && !canPassShapeTest(wordAtShapeOffset(object)));
    AOT_TEST_CHECK(keysOf(program, object) == "0,1,a,b,c,d"_s && object->get(program.realm(), 1u) == jsNumber(2) && object->get(program.realm(), 0u) == jsNumber(1));
}

static JSC_DECLARE_CUSTOM_GETTER(getSeven);

JSC_DEFINE_CUSTOM_GETTER(getSeven, (JSGlobalObject*, EncodedJSValue, PropertyName))
{
    return JSValue::encode(jsNumber(7));
}

static bool hasDirectProperty(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value, unsigned attributes)
{
    unsigned foundAttributes = 0;
    return PropertyWalk::getDirect(program.vm(), object, name, &foundAttributes) == value && foundAttributes == attributes;
}

static void checkDefinitionsByNativeCode(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* object = createFilled(program, "Mutable"_s);
    const FieldData& field = object->layout().fields()[0];
    Identifier name = nameOf(program, field);
    Costs expected = costsOf(program);

    PropertyWalk::putDirect(vm, object, name, jsNumber(2.5), noAttributes);
    AOT_TEST_CHECK(object->isInSlot(field) && hasDirectProperty(program, object, name, jsNumber(2.5), noAttributes) && costsOf(program) == expected);

    PropertyWalk::putDirect(vm, object, name, jsNull(), noAttributes);
    expected = afterLeavingSlot(expected, Event::StoreOfOtherType);
    AOT_TEST_CHECK(!object->isInSlot(field) && hasDirectProperty(program, object, name, jsNull(), noAttributes) && costsOf(program) == expected);

    PropertyWalk::putDirect(vm, object, name, jsNumber(3), noAttributes);
    AOT_TEST_CHECK(object->isInSlot(field) && hasDirectProperty(program, object, name, jsNumber(3), noAttributes) && costsOf(program) == expected);

    PropertyWalk::putDirect(vm, object, name, jsNumber(4), dontEnum | readOnly);
    expected = afterLeavingSlot(expected, Event::Redefinition);
    AOT_TEST_CHECK(!object->isInSlot(field) && hasDirectProperty(program, object, name, jsNumber(4), dontEnum | readOnly) && costsOf(program) == expected);

    PropertyWalk::putDirect(vm, object, name, jsNumber(5), noAttributes);
    AOT_TEST_CHECK(object->isInSlot(field) && hasDirectProperty(program, object, name, jsNumber(5), noAttributes) && costsOf(program) == expected);
    AOT_TEST_CHECK(keysOf(program, object) == "a,b,c,d"_s && !PropertyWalk::getDirect(vm, object, Identifier::fromString(vm, "absent"_s), nullptr));

    AOT_TEST_CHECK(deleteProperty(program, object, name) && !PropertyWalk::getDirect(vm, object, name, nullptr));
    expected = afterLeavingSlot(expected, Event::Delete);
    PropertyWalk::putDirect(vm, object, name, jsNumber(6), noAttributes);
    AOT_TEST_CHECK(object->isInSlot(field) && keysOf(program, object) == "b,c,d,a"_s && costsOf(program) == expected);

    PropertyWalk::putDirect(vm, object, name, CustomGetterSetter::create(vm, getSeven, nullptr), static_cast<unsigned>(PropertyAttribute::CustomAccessor));
    ++expected.shapeEpoch;
    ++expected.shapeEpochBumps;
    ++expected.objectsBecameOrdinaryByNativeDefinition;
    AOT_TEST_CHECK(costsOf(program) == expected && !isCompiledObject(object) && !canPassShapeTest(wordAtShapeOffset(object)));
    AOT_TEST_CHECK(object->get(program.realm(), name) == jsNumber(7) && keysOf(program, object) == "b,c,d,a"_s);
}

static constexpr unsigned numberOfBagFields = 9;
using CreationOrder = std::array<uint16_t, numberOfBagFields>;

static Object* createBag(LoadedProgram& program, const CreationOrder& order, const Invocable<void(Object&, const FieldData&, uint64_t wordsBefore)> auto& didCreateField)
{
    Object* bag = allocate(program, "Bag"_s);
    std::span<const FieldData> fields = bag->layout().fields();
    for (uint16_t index : order) {
        uint64_t wordsBefore = bag->orderAndShapeWords();
        AOT_TEST_CHECK(put(program, bag, nameOf(program, fields[index]), jsNumber(index)));
        didCreateField(*bag, fields[index], wordsBefore);
    }
    return bag;
}

static String keysInOrder(const CreationOrder& order)
{
    StringBuilder keys;
    for (uint16_t index : order)
        keys.append(keys.isEmpty() ? ""_s : ","_s, 'k', index);
    return keys.toString();
}

static void checkManyKeyOrdersOnOneLayout(LoadedProgram& program)
{
    constexpr unsigned numberOfOrders = 65536 + 512;
    static_assert(numberOfOrders > maxKeyOrdersPerLayout);
    const LayoutData& layout = layoutNamed(program, "Bag"_s);
    AOT_TEST_CHECK(layout.numberOfFields == numberOfBagFields && !layout.keysAreNeverEnumerated);
    Costs costsBefore = costsOf(program);

    CreationOrder order;
    std::iota(order.begin(), order.end(), 0);
    unsigned numberStoredOutOfLine = 0;
    for (unsigned count = 0; count < numberOfOrders; ++count) {
        Object* bag = createBag(program, order, [](Object&, const FieldData&, uint64_t) { });
        AOT_TEST_CHECK(isCompiledObject(bag) && bag->shapeWord() == shapeWordOf(layout.shape) && !bag->hasExtraProperties());
        Object::KeyOrderStorage storage;
        AOT_TEST_CHECK(std::ranges::equal(bag->keyOrder(program.state(), storage), order));
        AOT_TEST_CHECK(keysOf(program, bag) == keysInOrder(order));
        for (const FieldData& field : layout.fields())
            AOT_TEST_CHECK(bag->read(field) == jsNumber(layout.indexOf(field)));
        numberStoredOutOfLine += bag->hasOutOfLineKeyOrder();
        AOT_TEST_CHECK(std::ranges::next_permutation(order).found);
    }

    AOT_TEST_CHECK(numberStoredOutOfLine >= numberOfOrders - maxKeyOrdersPerLayout && numberStoredOutOfLine < numberOfOrders);
    Costs costsAfter = costsOf(program);
    AOT_TEST_CHECK(costsAfter.keyOrdersStoredOutOfLine >= costsBefore.keyOrdersStoredOutOfLine + numberStoredOutOfLine);
    costsAfter.keyOrdersStoredOutOfLine = costsBefore.keyOrdersStoredOutOfLine;
    AOT_TEST_CHECK(costsAfter == costsBefore);
}

static Vector<String> namesOf(LoadedProgram& program, JSObject* object)
{
    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, program.realm(), names, DontEnumPropertiesMode::Include);
    Vector<String> result;
    for (auto& name : names)
        result.append(name.string());
    return result;
}

// One entry of a key order counts fewer than 65,536 extra keys. A field that is created behind more of them is listed behind them, in its slot, and the object keeps its shape.
static void checkMoreExtraKeysThanOneEntryCounts(LoadedProgram& program)
{
    constexpr unsigned numberOfKeysInRun = maxKeyOrderEntry + 5;
    VM& vm = program.vm();
    Object* object = createFilled(program, "Mutable"_s);
    std::span<const FieldData> fields = object->layout().fields();
    AOT_TEST_CHECK(deleteProperty(program, object, nameOf(program, fields[0])) && deleteProperty(program, object, nameOf(program, fields[1])));
    Costs costsBefore = costsOf(program);

    Vector<String> expected { "c"_s, "d"_s };
    unsigned numberOfExtraKeys = 0;
    auto addExtraKeys = [&](unsigned count) {
        for (unsigned end = numberOfExtraKeys + count; numberOfExtraKeys < end; ++numberOfExtraKeys) {
            String name = makeString('x', numberOfExtraKeys);
            AOT_TEST_CHECK(put(program, object, Identifier::fromString(vm, name), jsNumber(numberOfExtraKeys)));
            expected.append(name);
        }
    };
    auto createField = [&](const FieldData& field) {
        AOT_TEST_CHECK(put(program, object, nameOf(program, field), fittingValueFor(program, field)) && object->isInSlot(field));
        expected.append(nameOf(program, field).string());
    };
    auto deleteExtraKey = [&](unsigned number) {
        String name = makeString('x', number);
        AOT_TEST_CHECK(deleteProperty(program, object, Identifier::fromString(vm, name)) && expected.removeFirst(name));
    };
    auto keepsShapeAndOrder = [&] {
        Costs costs = costsOf(program);
        costs.keyOrdersStoredOutOfLine = costsBefore.keyOrdersStoredOutOfLine;
        return isCompiledObject(object) && canPassShapeTest(wordAtShapeOffset(object)) && costs == costsBefore && namesOf(program, object) == expected;
    };

    addExtraKeys(numberOfKeysInRun);
    AOT_TEST_CHECK(keepsShapeAndOrder());
    createField(fields[0]);
    AOT_TEST_CHECK(keepsShapeAndOrder());
    addExtraKeys(numberOfKeysInRun);
    createField(fields[1]);
    addExtraKeys(3);
    AOT_TEST_CHECK(keepsShapeAndOrder());

    deleteExtraKey(3);
    AOT_TEST_CHECK(keepsShapeAndOrder());
    deleteExtraKey(numberOfKeysInRun + 1);
    AOT_TEST_CHECK(keepsShapeAndOrder());
    deleteExtraKey(numberOfExtraKeys - 1);
    AOT_TEST_CHECK(keepsShapeAndOrder());
    AOT_TEST_CHECK(object->get(program.realm(), Identifier::fromString(vm, makeString('x', numberOfKeysInRun))) == jsNumber(numberOfKeysInRun));
}

// The order of an object that is used as a dictionary positions no extra key, so a delete looks at no other key. A walk over the keys for each delete would take minutes here.
static void checkDeletesInLargeDictionary(LoadedProgram& program)
{
    constexpr unsigned numberOfKeys = 200000;
    VM& vm = program.vm();
    Object* object = createFilled(program, "Mutable"_s);
    Costs costsBefore = costsOf(program);
    auto nameOfKey = [&](unsigned number) {
        return Identifier::fromString(vm, makeString('x', number));
    };
    for (unsigned number = 0; number < numberOfKeys; ++number)
        AOT_TEST_CHECK(put(program, object, nameOfKey(number), jsNumber(number)));
    for (unsigned number = 0; number < numberOfKeys; number += 2)
        AOT_TEST_CHECK(deleteProperty(program, object, nameOfKey(number)));

    Vector<String> names = namesOf(program, object);
    AOT_TEST_CHECK(isCompiledObject(object) && costsOf(program) == costsBefore && names.size() == 4 + numberOfKeys / 2);
    AOT_TEST_CHECK(names[3] == "d"_s && names[4] == "x1"_s && names.last() == makeString('x', numberOfKeys - 1));
}

static const KeyOrderTransition* findTransition(LoadedProgram& program, uint64_t orderAndShapeWordsBefore, uint32_t slotKey)
{
    const ObjectWords& words = program.instance().objects;
    for (uint32_t index = KeyOrderTransition::hashOf(orderAndShapeWordsBefore, slotKey); ; ++index) {
        const KeyOrderTransition& transition = words.keyOrderTransitions[index & words.keyOrderTransitionMask];
        if (transition.isEmpty())
            return nullptr;
        if (transition.orderAndShapeWordsBefore == orderAndShapeWordsBefore && transition.key == slotKey)
            return &transition;
    }
}

static void checkTransitionTableGrows(LoadedProgram& program)
{
    constexpr unsigned numberOfOrders = 4000;
    constexpr uint32_t initialMask = 1023;
    AOT_TEST_CHECK(program.instance().objects.keyOrderTransitionMask == initialMask);

    Vector<KeyOrderTransition> seen;
    CreationOrder order;
    std::iota(order.begin(), order.end(), 0);
    for (unsigned count = 0; count < numberOfOrders; ++count) {
        createBag(program, order, [&](Object& bag, const FieldData& field, uint64_t wordsBefore) {
            AOT_TEST_CHECK(!bag.hasOutOfLineKeyOrder());
            seen.append({ wordsBefore & orderCountAndShapeMask, slotKeyFor(field.slot.offset, field.isOutOfLine()), bag.orderWord() & orderAndCountMask });
        });
        AOT_TEST_CHECK(std::ranges::next_permutation(order).found);
    }

    uint32_t mask = program.instance().objects.keyOrderTransitionMask;
    AOT_TEST_CHECK(mask > initialMask && !(mask & (mask + 1)));
    for (const KeyOrderTransition& expected : seen) {
        const KeyOrderTransition* found = findTransition(program, expected.orderAndShapeWordsBefore, expected.key);
        AOT_TEST_CHECK(found && found->orderAndCountAfter == expected.orderAndCountAfter);
    }

    uint64_t wordsOfOtherShape = static_cast<uint64_t>(shapeWordOf(layoutNamed(program, "Mutable"_s).shape)) << 32 | (seen[0].orderAndShapeWordsBefore & UINT32_MAX);
    AOT_TEST_CHECK(!findTransition(program, wordsOfOtherShape, seen[0].key) && !findTransition(program, seen[0].orderAndShapeWordsBefore, outOfLineSlotKey - 8));
}

template<LoadedProgramTest body>
static constexpr void (*withMutations)() = runNativeTestWithLoadedProgram<objectSlotExitTests, mutatingProgramText, body>;

template<LoadedProgramTest body>
static constexpr void (*withoutMutations)() = runNativeTestWithLoadedProgram<objectSlotExitTests, programWithoutMutationsText, body>;

std::span<const NativeTest> objectSlotExitTests()
{
    static constexpr NativeTest tests[] = {
        { "slot-exit/absent-bits-and-extra-properties-word"_s, withMutations<checkFieldsWithAbsentBitsAndExtraPropertiesWord> },
        { "slot-exit/absent-bits-without-extra-properties-word"_s, withMutations<checkFieldsWithAbsentBitsWithoutExtraPropertiesWord> },
        { "slot-exit/no-absent-bits-with-extra-properties-word"_s, withoutMutations<checkFieldsWithoutAbsentBitsWithExtraPropertiesWord> },
        { "slot-exit/no-absent-bits-and-no-extra-properties-word"_s, withoutMutations<checkFieldsWithoutAbsentBitsWithoutExtraPropertiesWord> },
        { "slot-exit/store-of-fitting-value-costs-nothing"_s, withMutations<checkStoreOfFittingValueCostsNothing> },
        { "slot-exit/deleted-field-is-created-again-in-its-slot"_s, withMutations<checkDeletedFieldIsCreatedAgainInItsSlot> },
        { "slot-exit/deleted-field-is-created-outside-its-slot"_s, withMutations<checkDeletedFieldIsCreatedOutsideItsSlot> },
        { "slot-exit/fitting-store-moves-field-back"_s, withMutations<checkStoreThatFitsMovesFieldBack> },
        { "slot-exit/usual-attributes-move-field-back"_s, withMutations<checkUsualAttributesMoveFieldBack> },
        { "slot-exit/accessor-in-place-of-field"_s, withMutations<checkAccessorInPlaceOfField> },
        { "slot-exit/read-of-field-outside-its-slot-is-cached"_s, withMutations<checkReadOfFieldOutsideItsSlotIsCached> },
        { "slot-exit/read-of-getter-in-place-of-field-is-cached"_s, withMutations<checkReadOfGetterInPlaceOfFieldIsCached> },
        { "slot-exit/read-of-deleted-field-stays-with-its-slot"_s, withMutations<checkReadOfDeletedFieldStaysWithItsSlot> },
        { "slot-exit/empty-extra-properties-are-dropped"_s, withMutations<checkEmptyExtraPropertiesAreDropped> },
        { "slot-exit/name-of-no-field"_s, withMutations<checkNameThatIsNoField> },
        { "slot-exit/integrity-is-bits-of-the-order-word"_s, withMutations<checkIntegrityIsBitsOfTheOrderWord> },
        { "slot-exit/indexed-properties"_s, withMutations<checkIndexedProperties> },
        { "slot-exit/definitions-by-native-code"_s, withMutations<checkDefinitionsByNativeCode> },
        { "slot-exit/many-key-orders-on-one-layout"_s, withMutations<checkManyKeyOrdersOnOneLayout> },
        { "slot-exit/more-extra-keys-than-one-entry-counts"_s, withMutations<checkMoreExtraKeysThanOneEntryCounts> },
        { "slot-exit/deletes-in-a-large-dictionary"_s, withMutations<checkDeletesInLargeDictionary> },
        { "slot-exit/transition-table-grows"_s, withMutations<checkTransitionTableGrows> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
