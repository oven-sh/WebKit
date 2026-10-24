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
#include "AOTOperationCallScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTVector.h"
#include "BrandedStructure.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "ObjectConstructor.h"
#include "StrongInlines.h"
#include "WeakInlines.h"
#include <array>

#if ENABLE(WEBASSEMBLY)
#include "WebAssemblyGCStructure.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> compiledStructureTests();

static_assert(sizeof(Structure) == 112);

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Circle" fields=[{key="radius" declared={type=number} flags=[IsRequired]}]
layout L1 name="Tagged" fields=[{key="name" declared={type=anything} flags=[IsRequired]}, {key=symbol("tag")@m0:120 declared={type=anything} flags=[IsRequired]}]
layout L2 kind=Environment name="scope" fields=[{key="count" declared={type=number}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="make" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Check v1 castType=number
    v4 = Check v2 castType=string
    v5 = NewObject -, v3 layout=L0 fields=[0]
    ElemSet v5, v4, v3
    v7 = Call v0, -, v5
    v8 = NewObject -, v0, v0 layout=L1 fields=[0, 1]
    ElemSet v8, v4, v3
    v10 = Call v0, -, v8
    v11 = NewObject -, v3 layout=L2 fields=[0]
    v12 = NewClosure v11 function=f2
    v13 = Call v0, -, v12
    v14 = NewClosure v11 function=f3
    v15 = Call v0, -, v14
    Return
}
function f2 name="increment" kind=Arrow captures=[{type=env L2}] results=[{type=anything}] {
  b0(v0: env L2):
    v1 = GetEnvironmentField v0 layout=L2 field=0
    v2 = Integer integer=1
    v3 = Add v1, v2
    PutEnvironmentField v0, v3 layout=L2 field=0
    Return v3
}
function f3 name="read" kind=Arrow captures=[{type=env L2}] results=[{type=anything}] {
  b0(v0: env L2):
    v1 = GetEnvironmentField v0 layout=L2 field=0
    Return v1
}
)IR"_s;

static const LayoutData& layoutNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static uint32_t shapeOfEnvironment(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = tables.firstEnvironmentShape; shape <= tables.numberOfShapes; ++shape) {
        if (tables.environmentForShape(shape).kind == LayoutKind::Environment)
            return shape;
    }
    failNativeTest(__FILE__, __LINE__, "the image has an environment");
}

static StructureTable& structures(LoadedProgram& program)
{
    return program.state().objects->structures;
}

static CompiledStructure* usualStructure(LoadedProgram& program, ASCIILiteral layoutName)
{
    return structures(program).objectStructure(program.vm(), layoutNamed(program, layoutName).shape);
}

static JSObject* allocateObject(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return asObject(structures(program).allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static Structure* createOrdinaryStructure(LoadedProgram& program, unsigned numberOfProperties)
{
    VM& vm = program.vm();
    Structure* structure = JSFinalObject::createStructure(vm, program.realm(), program.realm()->objectPrototype(), JSFinalObject::defaultInlineCapacity);
    for (unsigned i = 0; i < numberOfProperties; ++i) {
        PropertyOffset offset;
        structure = Structure::addPropertyTransition(vm, structure, Identifier::fromString(vm, makeString("extra"_s, i)), 0, offset);
    }
    return structure;
}

// Without the flags that repeat what the layout says, which are the same in every Structure of the layout.
static OptionSet<CompiledStructure::Flag> flagsOfVariant(const CompiledStructure* structure)
{
    return structure->flags() - OptionSet { CompiledStructure::Flag::HasFieldsWithSymbolKeys, CompiledStructure::Flag::HasOnlyPlainSlots, CompiledStructure::Flag::IsCopiedByWords };
}

template<typename T>
static T wordAt(const CompiledStructure* structure, ptrdiff_t offset)
{
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(structure) + offset);
}

static void testUsualStructureIsCreatedInItsSpace(LoadedProgram& program)
{
    VM& vm = program.vm();
    const LayoutData& layout = layoutNamed(program, "Circle"_s);
    CompiledStructure* structure = structures(program).objectStructure(vm, layout.shape);
    AOT_TEST_CHECK(structure->subspace() == vm.heap.compiledStructureSpace<SubspaceAccess::OnMainThread>());
    AOT_TEST_CHECK(program.realm()->objectPrototype()->structure()->subspace() != vm.heap.compiledStructureSpace<SubspaceAccess::OnMainThread>());
    AOT_TEST_CHECK(structure->structure() == vm.structureStructure.get() && static_cast<JSCell*>(structure)->type() == StructureType);

    AOT_TEST_CHECK(structure->isCompiledStructure() && structure->describesObject() && !structure->describesEnvironment());
    AOT_TEST_CHECK(structure->typeInfo().type() == CompiledObjectType);
    AOT_TEST_CHECK(&structure->layout() == &layout);
    AOT_TEST_CHECK(structure->realm() == program.realm());
    AOT_TEST_CHECK(structure->storedPrototype() == JSValue(program.realm()->objectPrototype()));
    AOT_TEST_CHECK(flagsOfVariant(structure) == OptionSet { CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype } && !structure->mayBePrototype());
    AOT_TEST_CHECK(!structure->extraPropertiesStructure() && !structure->extraPropertiesCellSize());

    AOT_TEST_CHECK(structures(program).objectStructure(vm, layout.shape) == structure);
    AOT_TEST_CHECK(structures(program).structure(vm, layout.shape) == structure);
    AOT_TEST_CHECK(usualStructure(program, "Tagged"_s) != structure);
    AOT_TEST_CHECK(allocateObject(program, "Circle"_s)->structure() == structure);
}

static void testWordsCompiledCodeReads(LoadedProgram& program)
{
    VM& vm = program.vm();
    const LayoutData& layout = layoutNamed(program, "Circle"_s);
    AOT_TEST_CHECK(layout.extraPropertiesSlot() && !layout.extensionSlot());
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    Structure* extra = createOrdinaryStructure(program, 2);
    CompiledStructure* named = structures(program).withExtraProperties(vm, *usual, extra);

    for (const CompiledStructure* structure : { usual, named }) {
        AOT_TEST_CHECK(wordAt<const LayoutData*>(structure, CompiledStructure::offsetOfLayout()) == &layout);
        AOT_TEST_CHECK(wordAt<SlotData>(structure, CompiledStructure::offsetOfExtraPropertiesSlot()) == *layout.extraPropertiesSlot());
        AOT_TEST_CHECK(structure->extraPropertiesSlot() == layout.extraPropertiesSlot());
        AOT_TEST_CHECK(!wordAt<SlotData>(structure, CompiledStructure::offsetOfExtensionSlot()).encoded() && !structure->extensionSlot());
        AOT_TEST_CHECK(wordAt<uint8_t>(structure, CompiledStructure::offsetOfFlags()) == structure->flags().toRaw());
    }
    // Compiled code reads the words about the extra properties behind a test of the flag. In a Structure without the flag they hold something else.
    AOT_TEST_CHECK(!usual->hasExtraProperties() && !usual->extraPropertiesStructureID() && !usual->extraPropertiesCellSize() && !usual->numberOfExtraKeys());
    AOT_TEST_CHECK(named->hasExtraProperties() && named->extraPropertiesStructureID() == extra->id());
    AOT_TEST_CHECK(wordAt<uint32_t>(named, CompiledStructure::offsetOfExtraPropertiesStructureID()) == extra->id().bits());
    AOT_TEST_CHECK(wordAt<uint16_t>(named, CompiledStructure::offsetOfExtraPropertiesCellSize()) == named->extraPropertiesCellSize());
    AOT_TEST_CHECK(wordAt<uint16_t>(named, CompiledStructure::offsetOfNumberOfExtraKeys()) == CompiledStructure::unknownNumberOfExtraKeys && !named->numberOfExtraKeys());
    AOT_TEST_CHECK(wordAt<uint16_t>(named, CompiledStructure::offsetOfExtraPropertiesCellSize()) == roundUpToMultipleOf<MarkedSpace::sizeStep>(JSFinalObject::allocationSize(extra->inlineCapacity())));
}

static void testWhatTheBaseIsLeftWith(LoadedProgram& program)
{
    CompiledStructure* structure = usualStructure(program, "Circle"_s);
    AOT_TEST_CHECK(structure->maxOffset() == invalidOffset);
    AOT_TEST_CHECK(!structure->inlineCapacity() && !structure->outOfLineCapacity());
    AOT_TEST_CHECK(structure->indexingType() == NonArray);
    AOT_TEST_CHECK(!structure->isDictionary());
    AOT_TEST_CHECK(!structure->isQuickPropertyAccessAllowedForEnumeration());
    AOT_TEST_CHECK(!structure->didPreventExtensions());
    AOT_TEST_CHECK(structure->hasAnyKindOfGetterSetterProperties());
    AOT_TEST_CHECK(structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto());
    AOT_TEST_CHECK(structure->hasNonEnumerableProperties());
    AOT_TEST_CHECK(structure->hasNonConfigurableProperties());
    AOT_TEST_CHECK(structure->hasNonConfigurableReadOnlyOrGetterSetterProperties());
}

static void testFlagOfFieldsWithSymbolKeys(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(layoutNamed(program, "Tagged"_s).hasFieldsWithSymbolKeys && !layoutNamed(program, "Circle"_s).hasFieldsWithSymbolKeys);
    for (ASCIILiteral name : { "Tagged"_s, "Circle"_s }) {
        CompiledStructure* usual = usualStructure(program, name);
        const CompiledStructure* const all[] = { usual, structures(program).asPrototype(vm, *usual), structures(program).withExtraProperties(vm, *usual, nullptr), structures(program).withPrototype(vm, *usual, jsNull()) };
        for (const CompiledStructure* structure : all)
            AOT_TEST_CHECK(structure->flags().contains(CompiledStructure::Flag::HasFieldsWithSymbolKeys) == !!structure->layout().hasFieldsWithSymbolKeys && structure->hasOnlyPlainSlots() == !!structure->layout().hasOnlyPlainSlots && structure->isCopiedByWords() == !!structure->layout().isCopiedByWords);
    }
}

static void testStructureOfEnvironments(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t shape = shapeOfEnvironment(program);
    Structure* structure = structures(program).structure(vm, shape);
    auto* compiled = dynamicDowncast<CompiledStructure>(structure);
    AOT_TEST_CHECK(compiled && compiled->describesEnvironment() && !compiled->describesObject());
    AOT_TEST_CHECK(compiled->subspace() == vm.heap.compiledStructureSpace<SubspaceAccess::OnMainThread>());
    AOT_TEST_CHECK(compiled->typeInfo().type() == CompiledEnvironmentType);
    AOT_TEST_CHECK(compiled->storedPrototype().isNull() && compiled->flags().isEmpty());
    AOT_TEST_CHECK(compiled->environmentShapePage() == shape >> environmentShapesPerPageShift);
    AOT_TEST_CHECK(wordAt<uint16_t>(compiled, CompiledStructure::offsetOfEnvironmentShapePage()) == compiled->environmentShapePage());
    AOT_TEST_CHECK(structures(program).structure(vm, shape) == structure);
    AOT_TEST_CHECK(!compiledObjectStructureOrNull(structure) && !compiledLayoutOrNull(structure));
}

static void testDowncastOnTheFourVariants(LoadedProgram& program)
{
    Realms& realms = program.realms();
    Structure* normal = realms.object(realms.plain(), "({ a: 1 })"_s)->structure();
    Structure* branded = realms.object(realms.plain(), "new (class { #method() { } })"_s)->structure();
    Structure* compiled = usualStructure(program, "Circle"_s);
    AOT_TEST_CHECK(normal->variant() == Structure::StructureVariant::Normal);
    AOT_TEST_CHECK(branded->variant() == Structure::StructureVariant::Branded);
    AOT_TEST_CHECK(compiled->variant() == Structure::StructureVariant::Compiled);

    AOT_TEST_CHECK(!dynamicDowncast<CompiledStructure>(normal) && !dynamicDowncast<CompiledStructure>(branded));
    AOT_TEST_CHECK(dynamicDowncast<CompiledStructure>(compiled) == compiled);
    AOT_TEST_CHECK(branded->isBrandedStructure() && !compiled->isBrandedStructure() && !normal->isBrandedStructure());
    AOT_TEST_CHECK(compiledObjectStructureOrNull(compiled) == compiled && compiledLayoutOrNull(compiled) == &layoutNamed(program, "Circle"_s));
    for (Structure* other : { normal, branded })
        AOT_TEST_CHECK(!compiledObjectStructureOrNull(other) && !compiledLayoutOrNull(other));

#if ENABLE(WEBASSEMBLY)
    Structure* webAssembly = realms.object(realms.plain(),
        "new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 0x0a, 2, 0x5f, 1, 0x7f, 0, 0x60, 0, 1, 0x64, 0, 3, 2, 1, 1, 7, 8, 1, 4, 0x6d, 0x61, 0x6b, 0x65, 0, 0, 0x0a, 7, 1, 5, 0, 0xfb, 1, 0, 0x0b]))).exports.make()"_s)->structure();
    AOT_TEST_CHECK(webAssembly->variant() == Structure::StructureVariant::WebAssemblyGC);
    AOT_TEST_CHECK(!dynamicDowncast<CompiledStructure>(webAssembly) && !compiledObjectStructureOrNull(webAssembly));
    AOT_TEST_CHECK(dynamicDowncast<WebAssemblyGCStructure>(webAssembly) && !dynamicDowncast<WebAssemblyGCStructure>(compiled));
#endif
}

static void testVariantWithAnotherPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    JSObject* prototype = constructEmptyObject(program.realm());
    JSObject* otherPrototype = constructEmptyObject(program.realm());

    CompiledStructure* variant = table.withPrototype(vm, *usual, prototype);
    AOT_TEST_CHECK(variant != usual && table.withPrototype(vm, *usual, prototype) == variant);
    AOT_TEST_CHECK(variant->storedPrototype() == JSValue(prototype) && variant->hasOtherPrototype());
    AOT_TEST_CHECK(flagsOfVariant(variant) == OptionSet { CompiledStructure::Flag::HasOtherPrototype });
    AOT_TEST_CHECK(&variant->layout() == &usual->layout());
    AOT_TEST_CHECK(table.objectStructure(vm, usual->layout().shape, prototype) == variant);

    AOT_TEST_CHECK(table.withPrototype(vm, *usual, otherPrototype) != variant);
    AOT_TEST_CHECK(table.withPrototype(vm, *usual, jsNull()) != variant && table.withPrototype(vm, *usual, jsNull())->storedPrototype().isNull());
    AOT_TEST_CHECK(table.withPrototype(vm, *usual, usual->storedPrototype()) == usual);
    AOT_TEST_CHECK(table.objectStructure(vm, usual->layout().shape, usual->storedPrototype()) == usual);
    AOT_TEST_CHECK(table.withPrototype(vm, *usualStructure(program, "Tagged"_s), prototype) != variant);
}

// A test in front of a member call answers for the read before it, so the flag is never taken back.
static void testUsualPrototypeSetAgainKeepsTheFlag(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    CompiledStructure* variant = table.withPrototype(vm, *usual, constructEmptyObject(program.realm()));
    AOT_TEST_CHECK(variant != usual && variant->hasOtherPrototype());
    CompiledStructure* restored = table.withPrototype(vm, *variant, usual->storedPrototype());
    AOT_TEST_CHECK(restored->storedPrototype() == usual->storedPrototype());
    AOT_TEST_CHECK(restored->hasOtherPrototype() && restored != usual && restored != variant);
    AOT_TEST_CHECK(&restored->layout() == &usual->layout());
    AOT_TEST_CHECK(table.withPrototype(vm, *variant, usual->storedPrototype()) == restored && table.withPrototype(vm, *restored, usual->storedPrototype()) == restored);
    AOT_TEST_CHECK(table.objectStructure(vm, usual->layout().shape, usual->storedPrototype()) == usual);
}

static void testVariantThatMayBePrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    CompiledStructure* variant = table.asPrototype(vm, *usual);
    AOT_TEST_CHECK(variant != usual && table.asPrototype(vm, *usual) == variant && table.asPrototype(vm, *variant) == variant);
    AOT_TEST_CHECK(variant->mayBePrototype() && !usual->mayBePrototype());
    AOT_TEST_CHECK(&variant->layout() == &usual->layout() && variant->storedPrototype() == usual->storedPrototype() && flagsOfVariant(variant) == flagsOfVariant(usual));
}

static void testVariantsWithExtraProperties(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    Structure* extra = createOrdinaryStructure(program, 1);
    Structure* otherExtra = createOrdinaryStructure(program, 2);

    CompiledStructure* unnamed = table.withExtraProperties(vm, *usual, nullptr);
    AOT_TEST_CHECK(unnamed != usual && table.withExtraProperties(vm, *usual, nullptr) == unnamed);
    AOT_TEST_CHECK(unnamed->hasExtraProperties() && !unnamed->extraPropertiesStructure() && !unnamed->extraPropertiesStructureID());

    AOT_TEST_CHECK(!table.existingWithExtraProperties(*usual, *extra));
    CompiledStructure* named = table.withExtraProperties(vm, *usual, extra);
    AOT_TEST_CHECK(named != usual && named != unnamed && table.withExtraProperties(vm, *usual, extra) == named);
    AOT_TEST_CHECK(named->hasExtraProperties() && named->extraPropertiesStructure() == extra);
    AOT_TEST_CHECK(table.existingWithExtraProperties(*usual, *extra) == named);
    AOT_TEST_CHECK(table.withExtraProperties(vm, *unnamed, extra) == named && table.withExtraProperties(vm, *named, nullptr) == unnamed);

    AOT_TEST_CHECK(!table.existingWithExtraProperties(*usual, *otherExtra));
    AOT_TEST_CHECK(table.withExtraProperties(vm, *usual, otherExtra) != named);
    AOT_TEST_CHECK(!table.existingWithExtraProperties(*usualStructure(program, "Tagged"_s), *extra));

    JSObject* dictionary = constructEmptyObject(vm, createOrdinaryStructure(program, 1));
    dictionary->convertToDictionary(vm);
    AOT_TEST_CHECK(dictionary->structure()->isDictionary());
    CompiledStructure* namingDictionary = table.withExtraProperties(vm, *usual, dictionary->structure());
    AOT_TEST_CHECK(namingDictionary->extraPropertiesStructure() == dictionary->structure() && table.withExtraProperties(vm, *unnamed, dictionary->structure()) == namingDictionary);
    AOT_TEST_CHECK(!namingDictionary->listsEveryExtraProperty() && named->listsEveryExtraProperty() && !unnamed->listsEveryExtraProperty());

    dictionary->convertToUncacheableDictionary(vm);
    AOT_TEST_CHECK(table.withExtraProperties(vm, *usual, dictionary->structure()) == unnamed);
    AOT_TEST_CHECK(table.withExtraProperties(vm, *unnamed, dictionary->structure()) == unnamed && table.withExtraProperties(vm, *namingDictionary, dictionary->structure()) == unnamed);
}

static void testVariantsWithOutOfLineKeyOrder(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Tagged"_s);
    const std::array<uint16_t, 2> entries { 1, 0 };
    const std::array<uint16_t, 2> otherEntries { 0, 1 };
    const std::array<uint16_t, 1> shorterEntries { 1 };

    CompiledStructure* variant = table.withOutOfLineKeyOrder(vm, *usual, entries);
    AOT_TEST_CHECK(variant != usual && !usual->hasOutOfLineKeyOrder());
    AOT_TEST_CHECK(variant->hasOutOfLineKeyOrder() && equalSpans(variant->outOfLineKeyOrder(), std::span<const uint16_t>(entries)));
    AOT_TEST_CHECK(variant->outOfLineKeyOrder().data() != entries.data());
    AOT_TEST_CHECK(table.withOutOfLineKeyOrder(vm, *usual, entries) == variant);
    AOT_TEST_CHECK(table.withOutOfLineKeyOrder(vm, *variant, variant->outOfLineKeyOrder()) == variant);
    AOT_TEST_CHECK(table.withOutOfLineKeyOrder(vm, *usual, otherEntries) != variant);
    AOT_TEST_CHECK(table.withOutOfLineKeyOrder(vm, *usual, shorterEntries) != variant);
    AOT_TEST_CHECK(table.withNumberedKeyOrder(vm, *variant) == usual && table.withNumberedKeyOrder(vm, *usual) == usual);
}

static void testComponentsOfVariantAreIndependent(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Tagged"_s);
    JSObject* prototype = constructEmptyObject(program.realm());
    Structure* extra = createOrdinaryStructure(program, 1);
    const std::array<uint16_t, 2> entries { 1, 0 };

    CompiledStructure* oneWay = table.withOutOfLineKeyOrder(vm, *table.withExtraProperties(vm, *table.asPrototype(vm, *table.withPrototype(vm, *usual, prototype)), extra), entries);
    CompiledStructure* otherWay = table.withPrototype(vm, *table.asPrototype(vm, *table.withExtraProperties(vm, *table.withOutOfLineKeyOrder(vm, *usual, entries), extra)), prototype);
    AOT_TEST_CHECK(oneWay == otherWay);
    AOT_TEST_CHECK(oneWay->storedPrototype() == JSValue(prototype) && oneWay->mayBePrototype() && oneWay->extraPropertiesStructure() == extra);
    AOT_TEST_CHECK(flagsOfVariant(oneWay) == OptionSet({ CompiledStructure::Flag::HasExtraProperties, CompiledStructure::Flag::HasOtherPrototype, CompiledStructure::Flag::HasOutOfLineKeyOrder }));

    CompiledStructure* numbered = table.withNumberedKeyOrder(vm, *oneWay);
    AOT_TEST_CHECK(numbered != oneWay && !numbered->hasOutOfLineKeyOrder());
    AOT_TEST_CHECK(numbered->storedPrototype() == JSValue(prototype) && numbered->mayBePrototype() && numbered->extraPropertiesStructure() == extra);
}

static NEVER_INLINE void createNamedVariant(LoadedProgram& program, Strong<Structure>& variant)
{
    VM& vm = program.vm();
    variant.set(vm, structures(program).withExtraProperties(vm, *usualStructure(program, "Circle"_s), createOrdinaryStructure(program, 3)));
}

static void testExtraStructureLivesWithTheCompiledStructure(LoadedProgram& program)
{
    VM& vm = program.vm();
    Strong<Structure> root;
    createNamedVariant(program, root);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        auto* variant = uncheckedDowncast<CompiledStructure>(root.get());
        Structure* extra = variant->extraPropertiesStructure();
        AOT_TEST_CHECK(extra && vm.heap.isMarked(extra) && extra->id() == variant->extraPropertiesStructureID());
        AOT_TEST_CHECK(static_cast<JSCell*>(extra)->type() == StructureType && extra->typeInfo().type() == FinalObjectType);
        AOT_TEST_CHECK(isValidOffset(extra->get(vm, Identifier::fromString(vm, "extra2"_s))));
    }
    root.clear();
}

static constexpr unsigned numberOfShortLivedVariants = 64;

static NEVER_INLINE void createVariants(LoadedProgram& program, MarkedArgumentBuffer& prototypes, Vector<Weak<Structure>>& variants)
{
    VM& vm = program.vm();
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    for (unsigned i = 0; i < numberOfShortLivedVariants; ++i) {
        prototypes.append(constructEmptyObject(program.realm()));
        variants.append(Weak<Structure>(structures(program).withPrototype(vm, *usual, prototypes.at(i))));
    }
}

static NEVER_INLINE void checkVariantsAreThoseMadeBefore(LoadedProgram& program, const MarkedArgumentBuffer& prototypes, const Vector<Weak<Structure>>& variants)
{
    VM& vm = program.vm();
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    AOT_TEST_CHECK(vm.heap.isMarked(usual));
    for (unsigned i = 0; i < numberOfShortLivedVariants; ++i) {
        CompiledStructure* variant = structures(program).withPrototype(vm, *usual, prototypes.at(i));
        AOT_TEST_CHECK(variant != usual && variant->storedPrototype() == prototypes.at(i) && variant->hasOtherPrototype());
        AOT_TEST_CHECK(variants[i] && variants[i].get() == variant);
    }
}

// No object has one of the variants. What the program has learned about them is kept for the next object under the same prototype, and for no longer.
static void testVariantNobodyHoldsLivesAsLongAsItsPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSObject* object = allocateObject(program, "Circle"_s);
    MarkedArgumentBuffer prototypes;
    Vector<Weak<Structure>> variants;
    createVariants(program, prototypes, variants);
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    checkVariantsAreThoseMadeBefore(program, prototypes, variants);

    AOT_TEST_CHECK(!prototypes.hasOverflowed());
    prototypes.clear();
    sanitizeStackForVM(vm);
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    unsigned numberCollected = 0;
    for (const Weak<Structure>& variant : variants)
        numberCollected += !variant;
    AOT_TEST_CHECK(numberCollected > numberOfShortLivedVariants / 2);
    ensureStillAliveHere(object);
}

static void testUsualStructureLivesWithItsObjects(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSObject* object = allocateObject(program, "Tagged"_s);
    StructureID id = object->structureID();
    AOT_TEST_CHECK(usualStructure(program, "Tagged"_s)->id() == id);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        CompiledStructure* structure = usualStructure(program, "Tagged"_s);
        AOT_TEST_CHECK(structure->id() == id && vm.heap.isMarked(structure) && &structure->layout() == &layoutNamed(program, "Tagged"_s));
    }
    ensureStillAliveHere(object);
}

static NEVER_INLINE void allocateAndDropObject(LoadedProgram& program, Weak<Structure>& structure)
{
    structure = Weak<Structure>(allocateObject(program, "Tagged"_s)->structure());
}

static void testUsualStructureWithoutObjectsOutlastsCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    Weak<Structure> dropped;
    allocateAndDropObject(program, dropped);
    AOT_TEST_CHECK(dropped);
    sanitizeStackForVM(vm);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(dropped);
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(dropped);

    JSObject* object = allocateObject(program, "Tagged"_s);
    CompiledStructure* structure = usualStructure(program, "Tagged"_s);
    AOT_TEST_CHECK(structure == dropped.get());
    AOT_TEST_CHECK(object->structure() == structure && &structure->layout() == &layoutNamed(program, "Tagged"_s));
    AOT_TEST_CHECK(!structure->hasExtraProperties() && !structure->hasOtherPrototype() && !structure->mayBePrototype());
    AOT_TEST_CHECK(structure->storedPrototype() == JSValue(program.realm()->objectPrototype()));
    AOT_TEST_CHECK(allocateObject(program, "Tagged"_s)->structure() == structure);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<compiledStructureTests, programText, body>;

static uint32_t allocationOf(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return number;
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

// Compiled code goes on behind the operation as behind the stub. Whatever lay in the free cell is gone: until compiled code has written a slot, nothing in it can be taken for a reference.
static void testSlowAllocationWritesWhatTheStubWrites(LoadedProgram& program)
{
    // Under AddressSanitizer no two cells that are allocated lie side by side.
    if (ASAN_ENABLED)
        return;
    constexpr uint8_t leftInFreeCell = 0xa5;
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    uint32_t number = allocationOf(program, "Tagged"_s);
    const AllocationData& allocation = program.state().tables.allocations()[number];
    AOT_TEST_CHECK(!program.instance().objects.allocationSite(number) && allocation.size() > 2 * sizeof(uint64_t));

    auto makeVisitable = [&](JSCell* cell) {
        zeroSpan(std::span { reinterpret_cast<uint8_t*>(cell) + 2 * sizeof(uint64_t), allocation.size() - 2 * sizeof(uint64_t) });
        for (const FieldData& field : uncheckedDowncast<Object>(cell)->layout().fields())
            uncheckedDowncast<Object>(cell)->clear(field);
    };
    auto checkHeaderAndWords = [&](JSCell* cell) {
        const AllocationSiteRecord* record = program.instance().objects.allocationSite(number);
        AOT_TEST_CHECK(record && record->header == cellHeaderFor(usualStructure(program, "Tagged"_s)));
        AOT_TEST_CHECK(reinterpret_cast<uint64_t*>(cell)[0] == record->header && reinterpret_cast<uint64_t*>(cell)[1] == record->orderAndShapeWords);
        AOT_TEST_CHECK(uncheckedDowncast<Object>(cell)->orderWord() == allocation.orderWord() && &uncheckedDowncast<Object>(cell)->layout() == &layoutNamed(program, "Tagged"_s));
    };

    JSCell* first = structures(program).allocateAsCompiledCodeDoes(vm, number);
    checkHeaderAndWords(first);
    makeVisitable(first);

    // The first cells of a size that has no block yet lie in larger cells of another allocator.
    const uint8_t* freeList = static_cast<const uint8_t*>(program.instance().objects.allocationSite(number)->allocator) + LocalAllocator::offsetOfFreeList();
    size_t cellSize = MarkedSpace::optimalSizeFor(allocation.size());
    auto startOfInterval = [&] { return *reinterpret_cast<uint8_t* const*>(freeList + FreeList::offsetOfIntervalStart()); };
    auto endOfInterval = [&] { return *reinterpret_cast<uint8_t* const*>(freeList + FreeList::offsetOfIntervalEnd()); };
    for (unsigned attempt = 0; attempt < 64 && startOfInterval() + 2 * cellSize > endOfInterval(); ++attempt)
        makeVisitable(structures(program).allocateAsCompiledCodeDoes(vm, number));
    uint8_t* next = startOfInterval();
    AOT_TEST_CHECK(next + 2 * cellSize <= endOfInterval());
    std::span body { next + 2 * sizeof(uint64_t), allocation.size() - 2 * sizeof(uint64_t) };
    memsetSpan(body, leftInFreeCell);
    JSCell* second = structures(program).allocateAsCompiledCodeDoes(vm, number);
    AOT_TEST_CHECK(reinterpret_cast<uint8_t*>(second) == next);
    checkHeaderAndWords(second);
    AOT_TEST_CHECK(std::ranges::all_of(body, [](uint8_t byte) { return !byte; }));
    makeVisitable(second);

    // Whoever is not compiled code gets an object that it may hand on as it is.
    memsetSpan(std::span { next + cellSize + 2 * sizeof(uint64_t), allocation.size() - 2 * sizeof(uint64_t) }, leftInFreeCell);
    auto* third = uncheckedDowncast<Object>(structures(program).allocate(vm, number));
    AOT_TEST_CHECK(reinterpret_cast<uint8_t*>(third) == next + cellSize);
    for (const FieldData& field : third->layout().fields())
        AOT_TEST_CHECK(!third->isInSlot(field));
}

// An object in a frame needs the header alone. A cell allocated for it and dropped would lie in the heap with that header and without its fields.
static void testHeaderOfAnAllocationComesWithoutACell(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    uint32_t number = allocationOf(program, "Tagged"_s);
    AOT_TEST_CHECK(!program.instance().objects.allocationSite(number));
    auto numberOfCellsWithHeader = [&](uint64_t header) {
        unsigned count = 0;
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
            count += isJSCellKind(kind) && *reinterpret_cast<uint64_t*>(cell) == header;
            return IterationStatus::Continue;
        });
        return count;
    };

    uint64_t header = callOperationFromTest(vm, operationAOTGetHeaderOfAllocation, &program.instance(), number);
    const AllocationSiteRecord* record = program.instance().objects.allocationSite(number);
    AOT_TEST_CHECK(record && record->header == header && header == cellHeaderFor(usualStructure(program, "Tagged"_s)));
    AOT_TEST_CHECK(!numberOfCellsWithHeader(header));
    AOT_TEST_CHECK(callOperationFromTest(vm, operationAOTGetHeaderOfAllocation, &program.instance(), number) == header && !numberOfCellsWithHeader(header));

    // The count sees a cell that is there.
    JSCell* cell = callOperationFromTest(vm, operationAOTAllocateSlow, &program.instance(), number);
    AOT_TEST_CHECK(*reinterpret_cast<uint64_t*>(cell) == header && numberOfCellsWithHeader(header) == 1);
}

// An environment and a closure have more than a header that compiled code leaves to whoever allocates them.
static void testSlowAllocationOfAnEnvironment(LoadedProgram& program)
{
    VM& vm = program.vm();
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = shapeOfEnvironment(program);
    bool found = false;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape != shape)
            continue;
        found = true;
        JSCell* cell = structures(program).allocateAsCompiledCodeDoes(vm, number);
        AOT_TEST_CHECK(cell->structure() == structures(program).structure(vm, shape));
        AOT_TEST_CHECK(*reinterpret_cast<uint64_t*>(cell) == program.instance().objects.allocationSite(number)->header);
    }
    AOT_TEST_CHECK(found);
}

std::span<const NativeTest> compiledStructureTests()
{
    static constexpr NativeTest tests[] = {
        { "compiled-structures/usual-structure-is-created-in-its-space"_s, withProgram<testUsualStructureIsCreatedInItsSpace> },
        { "compiled-structures/words-compiled-code-reads"_s, withProgram<testWordsCompiledCodeReads> },
        { "compiled-structures/what-the-base-is-left-with"_s, withProgram<testWhatTheBaseIsLeftWith> },
        { "compiled-structures/flag-of-fields-with-symbol-keys"_s, withProgram<testFlagOfFieldsWithSymbolKeys> },
        { "compiled-structures/structure-of-environments"_s, withProgram<testStructureOfEnvironments> },
        { "compiled-structures/downcast-on-the-four-variants"_s, withProgram<testDowncastOnTheFourVariants> },
        { "compiled-structures/variant-with-another-prototype"_s, withProgram<testVariantWithAnotherPrototype> },
        { "compiled-structures/usual-prototype-set-again-keeps-the-flag"_s, withProgram<testUsualPrototypeSetAgainKeepsTheFlag> },
        { "compiled-structures/variant-that-may-be-prototype"_s, withProgram<testVariantThatMayBePrototype> },
        { "compiled-structures/variants-with-extra-properties"_s, withProgram<testVariantsWithExtraProperties> },
        { "compiled-structures/variants-with-out-of-line-key-order"_s, withProgram<testVariantsWithOutOfLineKeyOrder> },
        { "compiled-structures/components-of-variant-are-independent"_s, withProgram<testComponentsOfVariantAreIndependent> },
        { "compiled-structures/extra-structure-lives-with-the-compiled-structure"_s, withProgram<testExtraStructureLivesWithTheCompiledStructure> },
        { "compiled-structures/variant-nobody-holds-lives-as-long-as-its-prototype"_s, withProgram<testVariantNobodyHoldsLivesAsLongAsItsPrototype> },
        { "compiled-structures/usual-structure-lives-with-its-objects"_s, withProgram<testUsualStructureLivesWithItsObjects> },
        { "compiled-structures/usual-structure-without-objects-outlasts-collections"_s, withProgram<testUsualStructureWithoutObjectsOutlastsCollections> },
        { "compiled-structures/slow-allocation-writes-what-the-stub-writes"_s, withProgram<testSlowAllocationWritesWhatTheStubWrites> },
        { "compiled-structures/header-of-an-allocation-comes-without-a-cell"_s, withProgram<testHeaderOfAnAllocationComesWithoutACell> },
        { "compiled-structures/slow-allocation-of-an-environment"_s, withProgram<testSlowAllocationOfAnEnvironment> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
