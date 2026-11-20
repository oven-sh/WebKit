/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTInterpreterSlowPaths.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "ObjectConstructor.h"
#include "PutPropertySlot.h"
#include "StrongInlines.h"
#include <algorithm>
#include <limits>
#include <tuple>
#include <wtf/SetForScope.h>

namespace JSC::AOT {

std::span<const NativeTest> interpreterSiteCacheTests();

static constexpr ASCIILiteral programText =
    "layout L0 name=\"Part\" module=m0 fields=[{key=\"id\" declared={type=number} flags=[IsRequired]}]\n"
    "layout L1 name=\"Mixed\" module=m0 fields=[{key=\"any\" declared={type=anything} flags=[IsRequired]}, {key=\"count\" declared={type=number} flags=[IsRequired]}, {key=\"flag\" declared={type=boolean} flags=[IsRequired]}, {key=\"part\" declared={type=L0} flags=[IsRequired]}, {key=\"text\" declared={type=string} flags=[IsRequired]}]\n"
    "layout L2 name=\"Open\" module=m0 fields=[{key=\"held\" declared={type=anything} flags=[IsRequired]}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v2 = PropGet v0 key=\"input\" [isFreeName]\n"
    "    v3 = ToNumber v2\n"
    "    v4 = ToBoolean v2\n"
    "    v5 = Check v2 expectedType=string\n"
    "    v6 = NewObject -, v3 layout=L0 fields=[0]\n"
    "    v7 = NewObject -, v2, v3, v4, v6, v5 layout=L1 fields=[0, 1, 2, 3, 4]\n"
    "    v8 = Call v1, -, v7\n"
    "    v9 = NewObject -, v2 layout=L2 fields=[0]\n"
    "    ElemSet v9, v5, v2\n"
    "    v11 = Call v1, -, v9\n"
    "    Return\n"
    "}\n"
    "entry module=m0\n"_s;

static Object* allocateObjectWithField(LoadedProgram& program, const Identifier& name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape >= tables.firstEnvironmentShape)
            continue;
        auto* object = uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
        if (object->fieldNamed(program.state(), name))
            return object;
    }
    failNativeTest(__FILE__, __LINE__, "the image allocates an object with the field");
}

static void putByName(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value)
{
    PutPropertySlot slot(object, true);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), name, value, slot) && !program.vm().exceptionForInspection());
}

static Object* createMixedObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* part = allocateObjectWithField(program, Identifier::fromString(vm, "id"_s));
    putByName(program, part, Identifier::fromString(vm, "id"_s), jsNumber(1));
    Object* object = allocateObjectWithField(program, Identifier::fromString(vm, "any"_s));
    putByName(program, object, Identifier::fromString(vm, "any"_s), jsNumber(7));
    putByName(program, object, Identifier::fromString(vm, "count"_s), jsNumber(2.5));
    putByName(program, object, Identifier::fromString(vm, "flag"_s), jsBoolean(true));
    putByName(program, object, Identifier::fromString(vm, "part"_s), part);
    putByName(program, object, Identifier::fromString(vm, "text"_s), jsNontrivialString(vm, "some text"_s));
    return object;
}

static constexpr unsigned readsUntilNameIsFiled = 16;

static std::optional<PropertyOffset> placeOnceFiled(LoadedProgram& program, Object* object, const Identifier& name)
{
    std::optional<PropertyOffset> place;
    for (unsigned turn = 0; turn < readsUntilNameIsFiled && !place; ++turn) {
        std::optional<JSValue> value = Interpreter::tryGetById(program.realm(), object, name);
        AOT_TEST_CHECK(value && *value && !program.vm().exceptionForInspection());
        place = Interpreter::fieldOffsetForSiteCache(program.realm(), object->structure(), name);
    }
    return place;
}

static void testPlaceOfEachRepresentation(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    Object* object = createMixedObject(program);

    AOT_TEST_CHECK(!Interpreter::fieldOffsetForSiteCache(program.realm(), object->structure(), Identifier::fromString(vm, "any"_s)));

    bool sawBoxedValue = false;
    bool sawNarrowReference = false;
    bool sawWhatCacheCannotHold = false;
    for (ASCIILiteral text : { "any"_s, "count"_s, "flag"_s, "part"_s, "text"_s }) {
        Identifier name = Identifier::fromString(vm, text);
        const FieldData* field = object->fieldNamed(program.state(), name);
        AOT_TEST_CHECK(field && !field->isOutOfLine());
        std::optional<PropertyOffset> place = placeOnceFiled(program, object, name);
        AOT_TEST_CHECK(place);
        switch (field->slot.representation()) {
        case SlotRepresentation::JSValue:
            sawBoxedValue = true;
            AOT_TEST_CHECK(*place == static_cast<PropertyOffset>((field->slot.offset - sizeof(JSObjectWithButterfly)) / sizeof(EncodedJSValue)) && isInlineOffset(*place));
            break;
        case SlotRepresentation::Cell32:
        case SlotRepresentation::OptionalCell32:
        case SlotRepresentation::DeferredClosure:
            sawNarrowReference = true;
            AOT_TEST_CHECK(*place == Interpreter::narrowReferenceInSiteCache + static_cast<PropertyOffset>(field->slot.offset));
            break;
        default:
            sawWhatCacheCannotHold = true;
            AOT_TEST_CHECK(*place == invalidOffset);
            break;
        }
    }
    AOT_TEST_CHECK(sawBoxedValue);
    AOT_TEST_CHECK(sawNarrowReference);
    AOT_TEST_CHECK(sawWhatCacheCannotHold);
}

static void testPlaceOfExtraProperty(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    Object* object = allocateObjectWithField(program, Identifier::fromString(vm, "held"_s));
    putByName(program, object, Identifier::fromString(vm, "held"_s), jsNumber(1));
    Identifier added = Identifier::fromString(vm, "added"_s);
    putByName(program, object, added, jsNumber(9));

    std::optional<PropertyOffset> place = placeOnceFiled(program, object, added);
    const Slot* slot = program.state().propertyCaches->findByKey(object->structure(), added.impl(), SlotAccess::Read);
    std::optional<SlotData> hiddenWord = uncheckedDowncast<CompiledStructure>(object->structure())->extraPropertiesSlot();
    AOT_TEST_CHECK(place && slot && slot->kind == Slot::Kind::ExtraProperty && hiddenWord);
    AOT_TEST_CHECK(hiddenWord->representation() == SlotRepresentation::Cell32);
    AOT_TEST_CHECK(*place != invalidOffset);
    AOT_TEST_CHECK(*place == Interpreter::extraPropertyInSiteCache + static_cast<PropertyOffset>(hiddenWord->offset << 16 | static_cast<uint16_t>(slot->extraProperty.wordIndex)));
    AOT_TEST_CHECK(*place < Interpreter::narrowReferenceInSiteCache);
}

static const GetByIdModeMetadata& cacheOfOnlyRead(JSFunction* function)
{
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    AOT_TEST_CHECK(codeBlock);
    const GetByIdModeMetadata* found = nullptr;
    for (const auto& instruction : codeBlock->instructions()) {
        if (!instruction->is<OpGetById>())
            continue;
        AOT_TEST_CHECK(!found);
        found = &instruction->as<OpGetById>().metadata(codeBlock).m_modeMetadata;
    }
    AOT_TEST_CHECK(found);
    return *found;
}

static JSValue callWith(LoadedProgram& program, JSFunction* function, JSValue argument)
{
    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    JSValue result = call(program.realm(), function, jsUndefined(), arguments, "the reader is a function"_s);
    AOT_TEST_CHECK(!program.vm().exceptionForInspection());
    return result;
}

static void testReadOfTextFillsItsSite(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    Object* object = createMixedObject(program);
    Identifier any = Identifier::fromString(vm, "any"_s);

    auto* readsAny = uncheckedDowncast<JSFunction>(program.realms().evaluate(program.realm(), "(o => o.any)"_s));
    for (unsigned turn = 0; turn < readsUntilNameIsFiled; ++turn)
        AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(7));
    const GetByIdModeMetadata& cache = cacheOfOnlyRead(readsAny);
    std::optional<PropertyOffset> place = Interpreter::fieldOffsetForSiteCache(program.realm(), object->structure(), any);
    AOT_TEST_CHECK(place && *place >= Interpreter::fieldInSiteCache && isInlineOffset(*place - Interpreter::fieldInSiteCache));
    AOT_TEST_CHECK(cache.mode == GetByIdMode::Default && cache.defaultMode.structureID == object->structureID() && cache.defaultMode.cachedOffset == *place);

    putByName(program, object, any, jsNumber(8));
    AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(8));
}

static void testSiteGoesOverToWhatKeepsComingBy(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    Object* object = createMixedObject(program);
    AOT_TEST_CHECK(placeOnceFiled(program, object, Identifier::fromString(vm, "any"_s)));
    JSValue ordinary = program.realms().evaluate(program.realm(), "({ any: 3 })"_s);
    auto* readsAny = uncheckedDowncast<JSFunction>(program.realms().evaluate(program.realm(), "(o => o.any)"_s));

    AOT_TEST_CHECK(callWith(program, readsAny, ordinary) == jsNumber(3));
    const GetByIdModeMetadata& cache = cacheOfOnlyRead(readsAny);
    auto holds = [&](JSCell* cell) {
        return cache.mode == GetByIdMode::Default && cache.defaultMode.structureID == cell->structureID();
    };
    AOT_TEST_CHECK(holds(ordinary.asCell()));

    constexpr unsigned shortestWait = 16;
    for (unsigned turn = 1; turn < shortestWait; ++turn) {
        AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(7));
        AOT_TEST_CHECK(holds(ordinary.asCell()));
    }
    AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(7));
    AOT_TEST_CHECK(holds(object));
    AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(7));

    for (unsigned turn = 1; turn < 2 * shortestWait; ++turn) {
        AOT_TEST_CHECK(callWith(program, readsAny, ordinary) == jsNumber(3));
        AOT_TEST_CHECK(holds(object));
    }
    AOT_TEST_CHECK(callWith(program, readsAny, ordinary) == jsNumber(3));
    AOT_TEST_CHECK(holds(ordinary.asCell()));
    for (unsigned turn = 1; turn < 4 * shortestWait; ++turn) {
        AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(7));
        AOT_TEST_CHECK(holds(ordinary.asCell()));
    }
    AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(7));
    AOT_TEST_CHECK(holds(object));
}

static constexpr ASCIILiteral programWithTypedSlots =
    "layout L0 name=\"Noted\" module=m0 fields=[{key=\"count\" declared={type=number} flags=[IsRequired]}, {key=\"flag\" declared={type=boolean} flags=[IsRequired]}, {key=\"note\" declared={type=undefined|string}}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v2 = PropGet v0 key=\"input\" [isFreeName]\n"
    "    v3 = ToNumber v2\n"
    "    v4 = ToBoolean v2\n"
    "    v5 = Check v2 expectedType=string\n"
    "    v6 = NewObject -, v3, v4, v5 layout=L0 fields=[0, 1, 2]\n"
    "    v7 = Call v1, -, v6\n"
    "    Return\n"
    "}\n"
    "entry module=m0\n"_s;

static Object* createNotedObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* object = allocateObjectWithField(program, Identifier::fromString(vm, "note"_s));
    putByName(program, object, Identifier::fromString(vm, "count"_s), jsNumber(2.5));
    putByName(program, object, Identifier::fromString(vm, "flag"_s), jsBoolean(true));
    putByName(program, object, Identifier::fromString(vm, "note"_s), jsNontrivialString(vm, "a note"_s));
    object->tryFinishBirth();
    return object;
}

static void storeWith(LoadedProgram& program, JSFunction* function, JSValue object, JSValue value)
{
    MarkedArgumentBuffer arguments;
    arguments.append(object);
    arguments.append(value);
    call(program.realm(), function, jsUndefined(), arguments, "the writer is a function"_s);
    AOT_TEST_CHECK(!program.vm().exceptionForInspection());
}

static void storeUntilNameIsFiled(LoadedProgram& program, JSFunction* function, Object* object, const Identifier& name, JSValue fitting)
{
    for (unsigned turn = 0; turn < readsUntilNameIsFiled; ++turn)
        storeWith(program, function, object, fitting);
    AOT_TEST_CHECK(program.state().propertyCaches->findByKey(object->structure(), name.impl(), SlotAccess::Store));
}

static void testMarkerStoredByTextMarksCell(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    RuntimeLayoutTable& layouts = program.state().objects->layouts;
    const std::tuple<ASCIILiteral, ASCIILiteral, JSValue> fields[] = {
        { "count"_s, "((o, v) => { o.count = v; })"_s, jsNumber(3.5) },
        { "flag"_s, "((o, v) => { o.flag = v; })"_s, jsBoolean(false) },
    };
    bool sawRawField = false;
    for (const auto& [text, writer, fitting] : fields) {
        Identifier name = Identifier::fromString(vm, text);
        auto* stores = uncheckedDowncast<JSFunction>(program.realms().evaluate(program.realm(), writer));
        for (JSValue marker : { jsNull(), jsUndefined() }) {
            Object* object = createNotedObject(program);
            const FieldData* field = object->fieldNamed(program.state(), name);
            AOT_TEST_CHECK(field);
            if (!field->holdsRawValueFromBirth)
                continue;
            sawRawField = true;
            AOT_TEST_CHECK(!layouts.fieldAccepts(*field, marker));

            storeUntilNameIsFiled(program, stores, object, name, fitting);
            AOT_TEST_CHECK(!object->bearsMark() && object->isInSlot(*field));

            storeWith(program, stores, object, marker);
            AOT_TEST_CHECK(object->bearsMark());
            AOT_TEST_CHECK(object->get(program.realm(), name) == marker);
        }
    }
    AOT_TEST_CHECK(sawRawField);
}

static void testMarkerThatFieldNeverTakesLeavesSlot(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    Object* object = createNotedObject(program);
    Identifier note = Identifier::fromString(vm, "note"_s);
    const FieldData* field = object->fieldNamed(program.state(), note);
    AOT_TEST_CHECK(field && traitsOf(field->slot.representation()).hasUndefined && !program.state().objects->layouts.slotAdmits(*field, jsNull()));

    auto* stores = uncheckedDowncast<JSFunction>(program.realms().evaluate(program.realm(), "((o, v) => { o.note = v; })"_s));
    storeUntilNameIsFiled(program, stores, object, note, jsNontrivialString(vm, "another note"_s));
    AOT_TEST_CHECK(object->isInSlot(*field));

    object->setBearsMark(true);
    storeWith(program, stores, object, jsNull());
    AOT_TEST_CHECK(!object->isInSlot(*field));
    AOT_TEST_CHECK(object->get(program.realm(), note) == jsNull());
}

static constexpr ASCIILiteral programWithUnboxedNumbers =
    "layout L0 name=\"Measured\" module=m0 fields=[{key=\"ratio\" declared={type=number} flags=[IsRequired]}, {key=\"whole\" declared={type=number} flags=[IsRequired]}, {key=\"any\" declared={type=anything} flags=[IsRequired]}]\n"
    "variable g0 name=\"measured\" module=m0 declared={type=anything} initialization=Undefined\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v2 = NewClosure function=f1\n"
    "    v3 = Call v1, -, v2\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"measure\" module=m0 parameters=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = Double number=2.5\n"
    "    v2 = Integer integer=7\n"
    "    v3 = NewObject -, v1, v2, v0 layout=L0 fields=[0, 1, 2]\n"
    "    v4 = FinishObject v3\n"
    "    v5 = GetModuleVariable variable=g0\n"
    "    PutModuleVariable v4 variable=g0\n"
    "    v7 = PropGet v5 key=\"ratio\"\n"
    "    v8 = PropGet v5 key=\"whole\"\n"
    "    v9 = PropGet v5 key=\"any\"\n"
    "    v10 = NewArray v7, v8, v9 contents=anything holes=[]\n"
    "    v11 = Double number=0.5\n"
    "    v12 = Integer integer=9\n"
    "    v13 = NewObject -, v11, v12, v0 layout=L0 fields=[0, 1, 2]\n"
    "    v14 = FinishObject v13\n"
    "    Return v10\n"
    "}\n"
    "entry module=m0\n"_s;

static bool isSameValue(LoadedProgram& program, JSValue first, JSValue second)
{
    return sameValue(program.realm(), first, second);
}

static void testTextStoresIntoUnboxedSlots(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    vm.aotState().nativeTestChangesObjectsUnseen = true;
    RuntimeLayoutTable& layouts = program.state().objects->layouts;
    JSValue text = jsNontrivialString(vm, "no number"_s);
    JSValue notANumber = jsNaN();
    JSValue negativeZero = jsDoubleNumber(-0.0);
    JSValue beyondInt32 = jsNumber(1099511627776.0);

    struct Case {
        ASCIILiteral name;
        ASCIILiteral writer;
        ASCIILiteral reader;
        SlotRepresentation representation;
        Vector<JSValue> fitting;
        Vector<JSValue> others;
    };
    const Case cases[] = {
        { "ratio"_s, "((o, v) => { o.ratio = v; })"_s, "(o => o.ratio)"_s, SlotRepresentation::OptionalDouble,
            { jsNumber(3.5), jsNumber(4), jsNumber(-1), negativeZero, notANumber, beyondInt32, jsNumber(std::numeric_limits<double>::infinity()) },
            { text, jsBoolean(true), jsBoolean(false), JSValue(program.realm()), jsUndefined(), jsNull() } },
        { "whole"_s, "((o, v) => { o.whole = v; })"_s, "(o => o.whole)"_s, SlotRepresentation::OptionalInt64,
            { jsNumber(8), jsNumber(0), jsNumber(std::numeric_limits<int32_t>::max()) },
            { jsNumber(0.5), jsNumber(-1), jsNumber(std::numeric_limits<int32_t>::min()), negativeZero, notANumber, jsNumber(std::numeric_limits<double>::infinity()), text, jsBoolean(true), JSValue(program.realm()), beyondInt32, jsUndefined(), jsNull() } },
    };
    for (const Case& testCase : cases) {
        Identifier name = Identifier::fromString(vm, testCase.name);
        auto* stores = uncheckedDowncast<JSFunction>(program.realms().evaluate(program.realm(), testCase.writer));
        auto* reads = uncheckedDowncast<JSFunction>(program.realms().evaluate(program.realm(), testCase.reader));
        Object* object = allocateObjectWithField(program, name);
        putByName(program, object, Identifier::fromString(vm, "ratio"_s), jsNumber(2.5));
        putByName(program, object, Identifier::fromString(vm, "whole"_s), jsNumber(7));
        putByName(program, object, Identifier::fromString(vm, "any"_s), jsNumber(1));
        const FieldData* field = object->fieldNamed(program.state(), name);
        AOT_TEST_CHECK(field && field->slot.representation() == testCase.representation && object->isInSlot(*field));

        storeUntilNameIsFiled(program, stores, object, name, testCase.fitting[0]);
        for (unsigned turn = 0; turn < readsUntilNameIsFiled; ++turn)
            AOT_TEST_CHECK(isSameValue(program, callWith(program, reads, object), testCase.fitting[0]));

        auto holds = [&](JSValue stored, bool inSlot) {
            return object->isInSlot(*field) == inSlot && isSameValue(program, callWith(program, reads, object), stored) && isSameValue(program, object->get(program.realm(), name), stored);
        };
        for (JSValue fitting : testCase.fitting) {
            AOT_TEST_CHECK(layouts.slotAdmits(*field, fitting));
            storeWith(program, stores, object, fitting);
            AOT_TEST_CHECK(holds(fitting, true));
            for (JSValue other : testCase.others) {
                AOT_TEST_CHECK(!layouts.slotAdmits(*field, other));
                storeWith(program, stores, object, other);
                AOT_TEST_CHECK(holds(other, false));
                storeWith(program, stores, object, fitting);
                AOT_TEST_CHECK(holds(fitting, true));
            }
        }
        AOT_TEST_CHECK(isSameValue(program, object->get(program.realm(), Identifier::fromString(vm, "any"_s)), jsNumber(1)));
    }
}

static JSGlobalObject* createRealmThatBorrowsTables(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    AOT_TEST_CHECK(!Instance::of(realm) && realm->aotTableInstance() == &program.instance());
    return realm;
}

// The cache of the site goes to `other`, so the table has to answer for `object`.
static constexpr ASCIILiteral readerOfTwoStructures = R"JS(
var above = { name: 1 }, object = Object.create(above), other = { name: 0 };
function read(o) { return o.name; }
function readOften() { for (let i = 0; i < 12; i++) read(other), read(object); }
)JS"_s;

static const Slot* slotFiledInBorrower(LoadedProgram& program, JSGlobalObject* realm, ASCIILiteral text, SlotAccess access)
{
    Identifier name = Identifier::fromString(program.vm(), text);
    Structure* structure = program.realms().object(realm, "object"_s)->structure();
    return realm->aotTableInstance()->state->propertyCaches->findByKey(structure, name.impl(), access);
}

static Slot::Kind kindFiledInBorrower(LoadedProgram& program, JSGlobalObject* realm, ASCIILiteral text, SlotAccess access = SlotAccess::Read)
{
    const Slot* slot = slotFiledInBorrower(program, realm, text, access);
    return slot ? slot->kind : Slot::Kind::Empty;
}

struct ChangeAboveObject {
    ASCIILiteral changesAndReads;
    JSValue value;
    Slot::Kind kindOnceFiled;
};

static void checkChangesAboveObject(LoadedProgram& program, JSGlobalObject* realm, std::initializer_list<ChangeAboveObject> changes)
{
    for (const ChangeAboveObject& change : changes) {
        AOT_TEST_CHECK_FOR_SOURCE(program.realms().evaluate(realm, change.changesAndReads) == change.value, change.changesAndReads);
        for (unsigned turn = 0; turn < readsUntilNameIsFiled; ++turn)
            program.realms().evaluate(realm, "read = o => o.name, readOften()"_s);
        AOT_TEST_CHECK_FOR_SOURCE(kindFiledInBorrower(program, realm, "name"_s) == change.kindOnceFiled, change.changesAndReads);
        AOT_TEST_CHECK_FOR_SOURCE(program.realms().evaluate(realm, "read(object)"_s) == change.value, change.changesAndReads);
    }
}

static void testBorrowerFilesWhatLiesAbove(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSGlobalObject* realm = createRealmThatBorrowsTables(program);
    program.realms().evaluate(realm, readerOfTwoStructures);
    checkChangesAboveObject(program, realm, {
        { "read(object)"_s, jsNumber(1), Slot::Kind::Value },
        { "above.name = 2, read(object)"_s, jsNumber(2), Slot::Kind::Value },
        { "delete above.name, read(object)"_s, jsUndefined(), Slot::Kind::Absent },
        { "Object.prototype.name = 3, read(object)"_s, jsNumber(3), Slot::Kind::Value },
        { "Object.defineProperty(above, 'name', { get: () => 4, configurable: true }), read(object)"_s, jsNumber(4), Slot::Kind::Getter },
        { "delete above.name, read(object)"_s, jsNumber(3), Slot::Kind::Value },
        { "Object.setPrototypeOf(above, { name: 5 }), read(object)"_s, jsNumber(5), Slot::Kind::Value },
        { "Object.setPrototypeOf(above, null), read(object)"_s, jsUndefined(), Slot::Kind::Absent },
    });
}

static void testBorrowerFilesWhatArraysInherit(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSGlobalObject* realm = createRealmThatBorrowsTables(program);
    program.realms().evaluate(realm, readerOfTwoStructures);
    checkChangesAboveObject(program, realm, {
        { "object = [], read(object)"_s, jsUndefined(), Slot::Kind::Absent },
        { "Object.prototype.name = 1, read(object)"_s, jsNumber(1), Slot::Kind::Value },
        { "Array.prototype.name = 2, read(object)"_s, jsNumber(2), Slot::Kind::Value },
    });
}

static void testOtherRealmsFileWhatBorrowerWouldFile(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    static constexpr ASCIILiteral belowArrayPrototype = "Reflect.ownKeys(Array.prototype).map(key => Array.prototype[key]); var object = Object.create(Array.prototype); () => Object.create(Array.prototype)"_s;
    static constexpr ASCIILiteral belowObjectPrototype = "var above = { }, object = Object.create(above); () => Object.create(above)"_s;
    struct Access {
        ASCIILiteral makerOfObjects;
        ASCIILiteral name;
        SlotAccess access;
        Slot::Kind kindOnceFiled;
        ASCIILiteral text;
    };
    static constexpr Access accesses[] = {
        { belowArrayPrototype, "map"_s, SlotAccess::Read, Slot::Kind::Value, "(make => { const read = o => o.map; for (let i = 0; i < 64; i++) read({ map: 0 }), read(make()); })"_s },
        { belowArrayPrototype, "absent"_s, SlotAccess::Read, Slot::Kind::Absent, "(make => { const read = o => o.absent; for (let i = 0; i < 64; i++) read({ absent: 0 }), read(make()); })"_s },
        { belowArrayPrototype, "length"_s, SlotAccess::Store, Slot::Kind::Transition, "(make => { const add = o => { o.length = 1; }; for (let i = 0; i < 64; i++) add({ }), add(make()); })"_s },
        { belowObjectPrototype, "__proto__"_s, SlotAccess::Read, Slot::Kind::Getter, "(make => { const read = o => o.__proto__; for (let i = 0; i < 64; i++) read({ }), read(make()); })"_s },
    };
    for (JSGlobalObject* asker : { program.realm(), createRealmThatBorrowsTables(program) }) {
        for (const Access& access : accesses) {
            JSGlobalObject* realm = createRealmThatBorrowsTables(program);
            callWith(program, uncheckedDowncast<JSFunction>(program.realms().evaluate(asker, access.text)), program.realms().evaluate(realm, access.makerOfObjects));
            AOT_TEST_CHECK_FOR_SOURCE(kindFiledInBorrower(program, realm, access.name, access.access) == access.kindOnceFiled, access.text);
        }
    }
}

static void testBorrowerLeavesGlobalProxyAlone(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSGlobalObject* realm = createRealmThatBorrowsTables(program);
    program.realms().evaluate(realm, readerOfTwoStructures);
    checkChangesAboveObject(program, realm, {
        { "object = globalThis, read(object)"_s, jsUndefined(), Slot::Kind::Uncacheable },
        { "globalThis.name = 1, read(object)"_s, jsNumber(1), Slot::Kind::Uncacheable },
    });
    AOT_TEST_CHECK(!realm->mayBePrototype());
}

static void testHeirOfGlobalObjectSeesReplacedValue(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = createRealmThatBorrowsTables(program);
    program.realms().evaluate(realm, readerOfTwoStructures);
    Identifier name = Identifier::fromString(vm, "name"_s);
    for (JSGlobalObject* above : { program.realm(), realm }) {
        above->putDirect(vm, name, jsNumber(0));
        above->didBecomePrototype(vm);
        JSObject* heir = constructEmptyObject(realm, above);
        PutPropertySlot slot(realm);
        JSGlobalObject::put(realm, realm, Identifier::fromString(vm, "object"_s), heir, slot);
        for (int value = 1; value <= 3; ++value) {
            for (unsigned turn = 0; turn < readsUntilNameIsFiled; ++turn)
                program.realms().evaluate(realm, "readOften()"_s);
            above->putDirect(vm, name, jsNumber(value));
            AOT_TEST_CHECK(program.realms().evaluate(realm, "read(object)"_s) == jsNumber(value));
            vm.heap.collectSync(CollectionScope::Full);
            AOT_TEST_CHECK(program.realms().evaluate(realm, "read(object)"_s) == jsNumber(value));
        }
    }
}

static JSC_DECLARE_CUSTOM_GETTER(getRealmOfRecord);
JSC_DEFINE_CUSTOM_GETTER(getRealmOfRecord, (JSGlobalObject* globalObject, EncodedJSValue, PropertyName))
{
    return JSValue::encode(globalObject);
}

static void testBorrowerCallsCustomGetterOfTable(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> ownAndInherited[] = {
        { "multiline"_s, "var object = RegExp, other = { }, read = o => o.multiline; RegExp.multiline = true"_s },
        { "numeric"_s, "var object = new Intl.Locale('de-u-kn'), other = { }, read = o => o.numeric"_s },
    };
    for (auto [name, receiver] : ownAndInherited) {
        JSGlobalObject* realm = createRealmThatBorrowsTables(program);
        program.realms().evaluate(realm, receiver);
        for (unsigned turn = 0; turn < 4 * readsUntilNameIsFiled; ++turn)
            AOT_TEST_CHECK_FOR_SOURCE(program.realms().evaluate(realm, "read(other), read(object)"_s) == jsBoolean(true), receiver);
        const Slot* slot = slotFiledInBorrower(program, realm, name, SlotAccess::Read);
        AOT_TEST_CHECK_FOR_SOURCE(slot && slot->kind == Slot::Kind::CustomGetter, receiver);
        if (!slot || slot->kind != Slot::Kind::CustomGetter)
            continue;
        auto& record = const_cast<CustomAccessorRecord&>(program.vm().aotState().propertyCaches->customAccessors().at(slot->customAccessor.index));
        SetForScope function(record.function, std::bit_cast<void*>(GetValueFunc(getRealmOfRecord).taggedPtr()));
        AOT_TEST_CHECK_FOR_SOURCE(program.realms().evaluate(realm, "read(object)"_s) == realm, receiver);
    }
}

static void testBorrowerFilesStores(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSGlobalObject* realm = createRealmThatBorrowsTables(program);
    program.realms().evaluate(realm, R"JS(
var above = { }, object = Object.create(above), seen;
function add(o) { o.name = 1; return Object.hasOwn(o, "name"); }
function addOften() { for (let i = 0; i < 64; i++) add({ other: 0 }), add(Object.create(above)); }
addOften();
)JS"_s);
    AOT_TEST_CHECK(kindFiledInBorrower(program, realm, "name"_s, SlotAccess::Store) == Slot::Kind::Transition);
    AOT_TEST_CHECK(program.realms().evaluate(realm, "Object.defineProperty(above, 'name', { set(value) { seen = value; }, configurable: true }), add(Object.create(above))"_s) == jsBoolean(false));
    AOT_TEST_CHECK(program.realms().evaluate(realm, "add = o => (o.name = 1, Object.hasOwn(o, 'name')), addOften(), seen"_s) == jsNumber(1));
    AOT_TEST_CHECK(kindFiledInBorrower(program, realm, "name"_s, SlotAccess::Store) == Slot::Kind::Setter);
    AOT_TEST_CHECK(program.realms().evaluate(realm, "delete above.name, add(Object.create(above))"_s) == jsBoolean(true));
    AOT_TEST_CHECK(program.realms().evaluate(realm, "addOften(), Object.defineProperty(Object.prototype, 'name', { value: 0 }), add(Object.create(above))"_s) == jsBoolean(false));
}

static void testBorrowerFilesStoresBelowArrayPrototype(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSGlobalObject* realm = createRealmThatBorrowsTables(program);
    program.realms().evaluate(realm, R"JS(
Reflect.ownKeys(Array.prototype).map(key => Array.prototype[key]);
var object = Object.create(Array.prototype);
function add(o) { o.length = 1; return Object.hasOwn(o, "length"); }
for (let i = 0; i < 64; i++) add({ other: 0 }), add(Object.create(Array.prototype));
)JS"_s);
    AOT_TEST_CHECK(kindFiledInBorrower(program, realm, "length"_s, SlotAccess::Store) == Slot::Kind::Transition);
    AOT_TEST_CHECK(program.realms().evaluate(realm, "Object.defineProperty(Array.prototype, 'length', { writable: false }), add(Object.create(Array.prototype))"_s) == jsBoolean(false));
}

static NEVER_INLINE Strong<JSGlobalObject> borrowFromRealmThatNothingHolds(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSGlobalObject* lender = JSGlobalObject::createWithCustomMethodTable(vm, JSGlobalObject::createStructure(vm, jsNull()), program.realm()->globalObjectMethodTable());
    JSGlobalObject* borrower = createRealmThatBorrowsTables(program);
    borrower->setAOTTableInstance(Instance::of(lender));
    program.realms().evaluate(borrower, readerOfTwoStructures);
    checkChangesAboveObject(program, borrower, { { "delete above.name, read(object)"_s, jsUndefined(), Slot::Kind::Absent } });
    return Strong<JSGlobalObject>(vm, borrower);
}

static void testBorrowerOutlivesLender(LoadedProgram& program)
{
    constexpr size_t numberOfBorrowers = 8;
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        Vector<Strong<JSGlobalObject>> borrowers;
        for (size_t number = 0; number < numberOfBorrowers; ++number)
            borrowers.append(borrowFromRealmThatNothingHolds(program));
        program.vm().heap.collectSync(scope);
        auto hasOtherLender = [&](const Strong<JSGlobalObject>& borrower) {
            return borrower->aotTableInstance() == &program.instance();
        };
        AOT_TEST_CHECK(static_cast<size_t>(std::ranges::count_if(borrowers, hasOtherLender)) > numberOfBorrowers / 2);
        for (const Strong<JSGlobalObject>& borrower : borrowers) {
            checkChangesAboveObject(program, borrower.get(), {
                { "above.name = 2, read(object)"_s, jsNumber(2), Slot::Kind::Value },
                { "delete above.name, read(object)"_s, jsUndefined(), Slot::Kind::Absent },
            });
        }
    }
}

static constexpr ASCIILiteral nameOfTestOfLenders = "borrowed-tables/borrower-outlives-lender"_s;

static void runTestOfLenders()
{
    runWithLoadedProgram(nameOfTestOfLenders, programText, testBorrowerOutlivesLender, { "several-realms"_s });
}

static void testRecordWithoutEntryIsNotKept(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSObject* prototype = constructEmptyObject(program.realm());
    Identifier name = Identifier::fromString(vm, "a name that is not filed"_s);
    AOT_TEST_CHECK(!vm.aotState().propertyCaches->keepChainDependent(vm, prototype->structureID().bits(), name.impl(), { prototype, nullptr }));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<interpreterSiteCacheTests, programText, body>;

template<LoadedProgramTest body>
static constexpr void (*withUnboxedNumbers)() = runNativeTestWithLoadedProgram<interpreterSiteCacheTests, programWithUnboxedNumbers, body>;

template<LoadedProgramTest body>
static constexpr void (*withTypedSlots)() = runNativeTestWithLoadedProgram<interpreterSiteCacheTests, programWithTypedSlots, body>;

static constexpr ASCIILiteral noNarrowReferences = "plan/AOTSlotPlan.cpp: narrow references in slots: no field lies in a Cell32 or an OptionalCell32"_s;
static constexpr ASCIILiteral noTypedSlots = "plan/AOTSlotPlan.cpp: fields that hold a raw value from birth, and slots with a marker that the field never takes: a slot is a JSValue, an OptionalInt64 or an OptionalDouble, and the last two take undefined and null"_s;

std::span<const NativeTest> interpreterSiteCacheTests()
{
    static constexpr NativeTest tests[] = {
        { "interpreter-site-cache/place-of-each-representation"_s, withProgram<testPlaceOfEachRepresentation>, noNarrowReferences },
        { "interpreter-site-cache/place-of-extra-property"_s, withProgram<testPlaceOfExtraProperty> },
        { "interpreter-site-cache/read-of-text-fills-its-site"_s, withProgram<testReadOfTextFillsItsSite> },
        { "interpreter-site-cache/site-goes-over-to-what-keeps-coming-by"_s, withProgram<testSiteGoesOverToWhatKeepsComingBy> },
        { "borrowed-tables/what-lies-above-is-filed"_s, withProgram<testBorrowerFilesWhatLiesAbove> },
        { "borrowed-tables/what-arrays-inherit-is-filed"_s, withProgram<testBorrowerFilesWhatArraysInherit> },
        { "borrowed-tables/other-realms-file-the-same"_s, withProgram<testOtherRealmsFileWhatBorrowerWouldFile> },
        { "borrowed-tables/global-proxy-is-left-alone"_s, withProgram<testBorrowerLeavesGlobalProxyAlone> },
        { "borrowed-tables/heir-of-a-global-object-sees-a-replaced-value"_s, withProgram<testHeirOfGlobalObjectSeesReplacedValue> },
        { "borrowed-tables/custom-getter-is-called-from-table"_s, withProgram<testBorrowerCallsCustomGetterOfTable> },
        { "borrowed-tables/stores-are-filed"_s, withProgram<testBorrowerFilesStores> },
        { "borrowed-tables/stores-below-array-prototype-are-filed"_s, withProgram<testBorrowerFilesStoresBelowArrayPrototype> },
        { "borrowed-tables/record-without-entry-is-not-kept"_s, withProgram<testRecordWithoutEntryIsNotKept> },
        { nameOfTestOfLenders, runTestOfLenders },
        { "interpreter-stores/text-stores-into-unboxed-slots"_s, withUnboxedNumbers<testTextStoresIntoUnboxedSlots> },
        { "interpreter-stores/marker-in-a-raw-field-marks-the-cell"_s, withTypedSlots<testMarkerStoredByTextMarksCell>, noTypedSlots },
        { "interpreter-stores/marker-that-a-field-never-takes-leaves-the-slot"_s, withTypedSlots<testMarkerThatFieldNeverTakesLeavesSlot>, noTypedSlots },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
