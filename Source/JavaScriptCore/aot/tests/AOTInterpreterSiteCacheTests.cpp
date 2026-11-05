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
#include "PutPropertySlot.h"
#include <limits>
#include <tuple>

namespace JSC::AOT {

std::span<const NativeTest> interpreterSiteCacheTests();

// The store under a key that nobody can name gives its layout a word for extra properties, and may put anything in any of its fields.
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

// A name is filed once it has been missed a few times: PropertyCaches::shouldFile().
static constexpr unsigned readsUntilNameIsFiled = 16;

static std::optional<PropertyOffset> placeOnceFiled(LoadedProgram& program, Object* object, const Identifier& name)
{
    std::optional<PropertyOffset> place;
    for (unsigned turn = 0; turn < readsUntilNameIsFiled && !place; ++turn) {
        std::optional<JSValue> value = Interpreter::tryGetById(program.realm(), object, name);
        AOT_TEST_CHECK(value && *value && !program.vm().exceptionForInspection());
        place = Interpreter::placeOfFieldInSiteCache(program.realm(), object->structure(), name);
    }
    return place;
}

static void testPlaceOfEachRepresentation(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    Object* object = createMixedObject(program);

    AOT_TEST_CHECK(!Interpreter::placeOfFieldInSiteCache(program.realm(), object->structure(), Identifier::fromString(vm, "any"_s)));

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
        case SlotRepresentation::LiteralString32:
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

// What no program sees: the read of text holds the place of the field at its own site, under the Structure of the compiled object.
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
    std::optional<PropertyOffset> place = Interpreter::placeOfFieldInSiteCache(program.realm(), object->structure(), any);
    AOT_TEST_CHECK(place && isInlineOffset(*place));
    AOT_TEST_CHECK(cache.mode == GetByIdMode::Default && cache.defaultMode.structureID == object->structureID() && cache.defaultMode.cachedOffset == *place);

    putByName(program, object, any, jsNumber(8));
    AOT_TEST_CHECK(callWith(program, readsAny, object) == jsNumber(8));
}

// JSON.parse() makes an ordinary object of a shape that it sees for the first time and records from then on. The site goes over to what keeps coming by, and each wait is twice as long as the one in front of it.
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

    // An ordinary object takes a site over at once, as it does in every process. The wait that begins there is the third.
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

// From then on llint_aot_op_put_by_id finds the field in the hashed table and goes by the type in its record.
static void storeUntilNameIsFiled(LoadedProgram& program, JSFunction* function, Object* object, const Identifier& name, JSValue fitting)
{
    for (unsigned turn = 0; turn < readsUntilNameIsFiled; ++turn)
        storeWith(program, function, object, fitting);
    AOT_TEST_CHECK(program.state().propertyCaches->findByKey(object->structure(), name.impl(), SlotAccess::Store));
}

// Compiled code takes what lies in such a slot of a cell without the mark for a number or a boolean, so the marker of undefined or null comes with the mark.
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

// `note?: string`: the form of the slot has a marker for null, and the field never takes it. Whatever the cell says, the store moves the field out of its slot.
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

// No object of the layout leaves the program, so Link knows every writer of its fields, and those that hold numbers alone lie unboxed: `ratio` as a double, `whole` as an integer.
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
    "    Return v10\n"
    "}\n"
    "entry module=m0\n"_s;

static bool isSameValue(LoadedProgram& program, JSValue first, JSValue second)
{
    return sameValue(program.realm(), first, second);
}

// Text stores into an unboxed slot through the hashed table, by the type in the record of the field. What fits lies in the slot, what does not fit takes the field out of it, and text and the run time read back what was stored either way.
static void testTextStoresIntoUnboxedSlots(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    // The record rules out what Link has proven that nobody does to these objects. The test does it all the same.
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
            { jsNumber(3.5), jsNumber(4), jsNumber(-1), negativeZero, notANumber, beyondInt32, jsNumber(std::numeric_limits<double>::infinity()), jsUndefined(), jsNull() },
            { text, jsBoolean(true), jsBoolean(false), JSValue(program.realm()) } },
        { "whole"_s, "((o, v) => { o.whole = v; })"_s, "(o => o.whole)"_s, SlotRepresentation::OptionalInt64,
            { jsNumber(8), jsNumber(0), jsNumber(-1), jsNumber(std::numeric_limits<int32_t>::min()), jsNumber(std::numeric_limits<int32_t>::max()), beyondInt32, jsUndefined(), jsNull() },
            { jsNumber(0.5), negativeZero, notANumber, jsNumber(std::numeric_limits<double>::infinity()), text, jsBoolean(true), JSValue(program.realm()) } },
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
        { "interpreter-stores/text-stores-into-unboxed-slots"_s, withUnboxedNumbers<testTextStoresIntoUnboxedSlots> },
        { "interpreter-stores/marker-in-a-raw-field-marks-the-cell"_s, withTypedSlots<testMarkerStoredByTextMarksCell>, noTypedSlots },
        { "interpreter-stores/marker-that-a-field-never-takes-leaves-the-slot"_s, withTypedSlots<testMarkerThatFieldNeverTakesLeavesSlot>, noTypedSlots },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
