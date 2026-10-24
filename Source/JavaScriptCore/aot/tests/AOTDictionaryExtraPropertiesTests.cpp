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
#include "AOTPropertyList.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> dictionaryExtraPropertiesTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Counter" fields=[{key="total" declared={type=anything} flags=[IsRequired]}]
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
function f1 name="makeCounter" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    Return v2
}
function f2 name="count" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    Return
}
)IR"_s;

static constexpr unsigned numberOfNames = Structure::s_maxTransitionLength + 40;

namespace {

struct Counter {
    Object* object;
    JSObject* extraProperties;
};

}

static Object* allocateCounter(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        uint32_t shape = tables.allocations()[number].shape;
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), "Counter"_s))
            return uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, "no allocation of Counter");
}

static Identifier nameAt(VM& vm, unsigned index)
{
    return Identifier::fromString(vm, makeString("word"_s, index));
}

static JSString* keyFor(VM& vm, const Identifier& name)
{
    return jsString(vm, name.string());
}

// The steps of the caching operations, which expect a compiled caller.
static JSValue getAndCache(LoadedProgram& program, Object* object, const Identifier& name)
{
    Object::ensureStructureForExtraPropertiesIfNeeded(program.vm(), object);
    Structure* structureBefore = object->structure();
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    JSValue value = JSValue(object).get(program.realm(), name, slot);
    program.state().propertyCaches->cache({ structureBefore, { name.impl(), std::nullopt }, SlotAccess::Read }, classifyRead({ program.realm(), object, structureBefore, name.impl(), slot }));
    return value;
}

static void putAndCache(LoadedProgram& program, Object* object, const Identifier& name, JSValue value)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    object->ensureStructureForExtraProperties(program.vm());
    Structure* structureBefore = object->structure();
    PutPropertySlot slot(object, true);
    AOT_TEST_CHECK(object->tryStoreExtraProperty(program.vm(), name, value, { Flag::IsKnownNotToBeField, Flag::NamesExtraPropertiesStructure }).kind != Object::ExtraPropertyStore::NotHandled);
    program.state().propertyCaches->cache({ structureBefore, { name.impl(), std::nullopt }, SlotAccess::Store }, classifyStore({ program.realm(), object, structureBefore, name.impl(), slot, PutMode::Strict }));
}

static bool hasOwn(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    return object->methodTable()->getOwnPropertySlot(object, program.realm(), name, slot);
}

static JSValue getInLeaf(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    return JSValue::decode(operationAOTGetByValGeneric(&program.instance(), object, keyFor(program.vm(), name)));
}

static LeafAnswer putInLeaf(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value)
{
    return static_cast<LeafAnswer>(operationAOTPutByValGeneric(&program.instance(), object, keyFor(program.vm(), name), JSValue::encode(value)));
}

static LeafPresence presenceInLeaf(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    return static_cast<LeafPresence>(operationAOTInByValGeneric(&program.instance(), object, keyFor(program.vm(), name)));
}

static const Slot* findSlot(LoadedProgram& program, JSObject* object, const Identifier& name, SlotAccess access)
{
    return program.state().propertyCaches->tables().hashedTable().find(object->structure()->id(), name.impl(), access);
}

static unsigned numberOfKeys(LoadedProgram& program, JSObject* object)
{
    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, program.realm(), names, DontEnumPropertiesMode::Exclude);
    return names.size();
}

static Counter createCounterWithDictionary(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* object = allocateCounter(program);
    AOT_TEST_CHECK(object->layout().extraPropertiesSlot() && object->store(vm, object->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))));
    for (unsigned index = 0; index < numberOfNames; ++index)
        putAndCache(program, object, nameAt(vm, index), jsNumber(index));
    JSObject* extraProperties = object->extraProperties();
    AOT_TEST_CHECK(isCompiledObject(object) && extraProperties);
    AOT_TEST_CHECK(extraProperties->structure()->isDictionary() && !extraProperties->structure()->isUncacheableDictionary());
    return { object, extraProperties };
}

static void testStructureNamesDictionary(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto [object, extraProperties] = createCounterWithDictionary(program);
    auto [other, otherExtraProperties] = createCounterWithDictionary(program);

    AOT_TEST_CHECK(getAndCache(program, object, nameAt(vm, 3)) == jsNumber(3) && getAndCache(program, other, nameAt(vm, 3)) == jsNumber(3));
    const CompiledStructure& structure = object->compiledStructure();
    AOT_TEST_CHECK(structure.extraPropertiesStructure() == extraProperties->structure() && !structure.listsEveryExtraProperty());
    AOT_TEST_CHECK(other->compiledStructure().extraPropertiesStructure() == otherExtraProperties->structure() && other->structure() != object->structure());

    Structure* named = object->structure();
    object->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(object->structure() == named && !Object::ensureStructureForExtraPropertiesIfNeeded(vm, object));
}

static void testPresentNamesHaveSlots(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto [object, extraProperties] = createCounterWithDictionary(program);
    for (unsigned index : { 0u, static_cast<unsigned>(Structure::s_maxTransitionLength), numberOfNames - 1 }) {
        Identifier name = nameAt(vm, index);
        AOT_TEST_CHECK(getAndCache(program, object, name) == jsNumber(index));
        const Slot* read = findSlot(program, object, name, SlotAccess::Read);
        AOT_TEST_CHECK(read && read->kind == Slot::Kind::ExtraProperty && read->has(Slot::isOwn) && Slot::presenceOf(read->kind) == SlotPresence::Present);

        putAndCache(program, object, name, jsNumber(index + 1000));
        const Slot* store = findSlot(program, object, name, SlotAccess::Store);
        AOT_TEST_CHECK(store && store->kind == Slot::Kind::ExtraProperty);
        AOT_TEST_CHECK(extraProperties->getDirect(vm, name) == jsNumber(index + 1000) && object->extraProperties() == extraProperties);
    }
}

static void testAbsentNameIsNeverFiled(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto [object, extraProperties] = createCounterWithDictionary(program);
    Identifier late = Identifier::fromString(vm, "late"_s);
    AOT_TEST_CHECK(getAndCache(program, object, nameAt(vm, 0)) == jsNumber(0));
    Structure* structure = object->structure();

    AOT_TEST_CHECK(!Object::structureGuaranteesAbsence(structure, late.impl()));
    AOT_TEST_CHECK(getAndCache(program, object, late).isUndefined() && !hasOwn(program, object, late));
    AOT_TEST_CHECK(!findSlot(program, object, late, SlotAccess::Read) && object->structure() == structure);
    AOT_TEST_CHECK(getInLeaf(program, object, late).isUndefined() && presenceInLeaf(program, object, late) == LeafPresence::Absent);
    AOT_TEST_CHECK(!findSlot(program, object, late, SlotAccess::Read));

    AOT_TEST_CHECK(putInLeaf(program, object, late, jsNumber(5)) == LeafAnswer::Yes);
    AOT_TEST_CHECK(object->structure() == structure && object->extraProperties() == extraProperties && extraProperties->getDirect(vm, late) == jsNumber(5));
    AOT_TEST_CHECK(!findSlot(program, object, late, SlotAccess::Store));
    AOT_TEST_CHECK(hasOwn(program, object, late) && getAndCache(program, object, late) == jsNumber(5));
    const Slot* read = findSlot(program, object, late, SlotAccess::Read);
    AOT_TEST_CHECK(read && read->kind == Slot::Kind::ExtraProperty);
}

static void testLeafFindsInheritedProperty(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto [object, extraProperties] = createCounterWithDictionary(program);
    Identifier inherited = Identifier::fromString(vm, "inherited"_s);
    JSObject* prototype = constructEmptyObject(program.realm());
    prototype->putDirect(vm, inherited, jsNumber(9));
    AOT_TEST_CHECK(object->methodTable()->setPrototype(object, program.realm(), prototype, false) && isCompiledObject(object));
    AOT_TEST_CHECK(object->get(program.realm(), inherited) == jsNumber(9));

    AOT_TEST_CHECK(getInLeaf(program, object, inherited) == jsNumber(9) && presenceInLeaf(program, object, inherited) == LeafPresence::Inherited);
    AOT_TEST_CHECK(!findSlot(program, object, inherited, SlotAccess::Read));
    AOT_TEST_CHECK(putInLeaf(program, object, inherited, jsNumber(10)) == LeafAnswer::Yes && extraProperties->getDirect(vm, inherited) == jsNumber(10));
    AOT_TEST_CHECK(object->get(program.realm(), inherited) == jsNumber(10) && prototype->getDirect(vm, inherited) == jsNumber(9));
}

static void testKeysFollowDictionary(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto [object, extraProperties] = createCounterWithDictionary(program);
    AOT_TEST_CHECK(getAndCache(program, object, nameAt(vm, 0)) == jsNumber(0));
    AOT_TEST_CHECK(object->compiledStructure().extraPropertiesStructure() == extraProperties->structure() && !object->keysDependOnlyOnStructureAndOrderWord());
    AOT_TEST_CHECK(numberOfKeys(program, object) == numberOfNames + 1);
    AOT_TEST_CHECK(putInLeaf(program, object, Identifier::fromString(vm, "late"_s), jsNumber(5)) == LeafAnswer::Yes);
    AOT_TEST_CHECK(numberOfKeys(program, object) == numberOfNames + 2);
}

static void testDeletedNameLosesItsSlot(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto [object, extraProperties] = createCounterWithDictionary(program);
    Identifier deleted = nameAt(vm, 10);
    Identifier kept = nameAt(vm, numberOfNames - 1);
    AOT_TEST_CHECK(getAndCache(program, object, deleted) == jsNumber(10) && getAndCache(program, object, kept) == jsNumber(numberOfNames - 1));
    Structure* structureBefore = object->structure();

    DeletePropertySlot slot;
    AOT_TEST_CHECK(object->methodTable()->deleteProperty(object, program.realm(), deleted, slot));
    AOT_TEST_CHECK(object->structure() != structureBefore && !findSlot(program, object, deleted, SlotAccess::Read));
    AOT_TEST_CHECK(getAndCache(program, object, deleted).isUndefined() && !hasOwn(program, object, deleted));
    AOT_TEST_CHECK(getAndCache(program, object, kept) == jsNumber(numberOfNames - 1) && numberOfKeys(program, object) == numberOfNames);

    if (!extraProperties->structure()->isUncacheableDictionary())
        return;
    Structure* unnamed = object->structure();
    AOT_TEST_CHECK(!object->compiledStructure().extraPropertiesStructure());
    object->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(object->structure() == unnamed && !findSlot(program, object, kept, SlotAccess::Read));
    AOT_TEST_CHECK(getInLeaf(program, object, kept) == jsNumber(numberOfNames - 1) && presenceInLeaf(program, object, kept) == LeafPresence::Own);
    AOT_TEST_CHECK(getInLeaf(program, object, deleted).isUndefined() && presenceInLeaf(program, object, deleted) == LeafPresence::Absent);
    AOT_TEST_CHECK(putInLeaf(program, object, kept, jsNumber(1)) == LeafAnswer::Yes && extraProperties->getDirect(vm, kept) == jsNumber(1));
}

static bool addWithoutNamingStructure(LoadedProgram& program, Object* object, const Identifier& name, JSValue value)
{
    return object->tryStoreExtraProperty(program.vm(), name, value, { Object::ExtraPropertyStoreFlag::IsKnownNotToBeField }).kind == Object::ExtraPropertyStore::Added;
}

static void testNamingFlattensDictionaryOnce(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* object = allocateCounter(program);
    AOT_TEST_CHECK(object->store(vm, object->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))));
    for (unsigned index = 0; index < numberOfNames; ++index)
        AOT_TEST_CHECK(addWithoutNamingStructure(program, object, nameAt(vm, index), jsNumber(index)));
    JSObject* extraProperties = object->extraProperties();
    AOT_TEST_CHECK(extraProperties->structure()->isDictionary() && !object->fieldsAndExtraPropertiesInKeyOrder(vm));

    object->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(object->extraProperties() == extraProperties && !extraProperties->structure()->isDictionary());
    AOT_TEST_CHECK(object->compiledStructure().listsEveryExtraProperty() && object->keysDependOnlyOnStructureAndOrderWord());
    auto* listed = object->fieldsAndExtraPropertiesInKeyOrder(vm);
    AOT_TEST_CHECK(listed && listed->size() == numberOfNames + 1 && numberOfKeys(program, object) == numberOfNames + 1);
    AOT_TEST_CHECK(getAndCache(program, object, nameAt(vm, numberOfNames - 1)) == jsNumber(numberOfNames - 1));

    AOT_TEST_CHECK(addWithoutNamingStructure(program, object, Identifier::fromString(vm, "late"_s), jsNumber(5)) && extraProperties->structure()->isDictionary());
    object->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(extraProperties->structure()->isDictionary() && !object->compiledStructure().listsEveryExtraProperty());
    AOT_TEST_CHECK(!object->fieldsAndExtraPropertiesInKeyOrder(vm) && numberOfKeys(program, object) == numberOfNames + 2);
}

// Deletes the names with the even numbers below twice the number of deletions.
static Object* createCounterAfterDeletions(LoadedProgram& program, unsigned numberOfAdditions, unsigned numberOfDeletions)
{
    VM& vm = program.vm();
    Object* object = allocateCounter(program);
    AOT_TEST_CHECK(object->store(vm, object->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))));
    for (unsigned index = 0; index < numberOfAdditions; ++index)
        AOT_TEST_CHECK(addWithoutNamingStructure(program, object, nameAt(vm, index), jsNumber(index)));
    for (unsigned index = 0; index < numberOfDeletions; ++index) {
        DeletePropertySlot slot;
        AOT_TEST_CHECK(object->methodTable()->deleteProperty(object, program.realm(), nameAt(vm, 2 * index), slot));
    }
    return object;
}

static void checkKeyListAfterDeletions(LoadedProgram& program, unsigned numberOfAdditions, unsigned numberOfDeletions, bool becomesDictionary)
{
    VM& vm = program.vm();
    Object* object = createCounterAfterDeletions(program, numberOfAdditions, numberOfDeletions);
    JSObject* extraProperties = object->extraProperties();
    AOT_TEST_CHECK(extraProperties->structure()->isUncacheableDictionary() == becomesDictionary);

    object->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(!extraProperties->structure()->isDictionary() && object->keysDependOnlyOnStructureAndOrderWord());
    unsigned numberOfProperties = 1 + numberOfAdditions - numberOfDeletions;
    auto* listed = object->fieldsAndExtraPropertiesInKeyOrder(vm);
    AOT_TEST_CHECK(listed && listed->size() == numberOfProperties && numberOfKeys(program, object) == numberOfProperties);
    AOT_TEST_CHECK(listed->at(1).name == nameAt(vm, 1).impl() && object->readPropertyListEntry(listed->at(1), extraProperties) == jsNumber(1));
    AOT_TEST_CHECK(listed->last().name == nameAt(vm, numberOfAdditions - 1).impl() && object->readPropertyListEntry(listed->last(), extraProperties) == jsNumber(numberOfAdditions - 1));
}

static void testDeletionsKeepKeyList(LoadedProgram& program)
{
    checkKeyListAfterDeletions(program, 12, 1, false);
    checkKeyListAfterDeletions(program, 100, 40, true);
}

static void testSymbolsAndHiddenNamesHaveNoKeyList(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* withSymbol = createCounterAfterDeletions(program, 12, 1);
    AOT_TEST_CHECK(addWithoutNamingStructure(program, withSymbol, vm.propertyNames->iteratorSymbol, jsNumber(1)));
    withSymbol->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(!withSymbol->fieldsAndExtraPropertiesInKeyOrder(vm) && numberOfKeys(program, withSymbol) == 12);

    Object* withHiddenName = createCounterAfterDeletions(program, 12, 1);
    PropertyDescriptor hidden(jsNumber(1), static_cast<unsigned>(PropertyAttribute::DontEnum));
    AOT_TEST_CHECK(withHiddenName->methodTable()->defineOwnProperty(withHiddenName, program.realm(), Identifier::fromString(vm, "hidden"_s), hidden, false));
    withHiddenName->ensureStructureForExtraProperties(vm);
    AOT_TEST_CHECK(!withHiddenName->fieldsAndExtraPropertiesInKeyOrder(vm) && numberOfKeys(program, withHiddenName) == 12);
}

static bool chainIsCacheableByStructures(LoadedProgram& program, JSObject* prototype)
{
    bool sawPolyProto;
    return normalizePrototypeChain(program.realm(), constructEmptyObject(program.realm(), prototype), sawPolyProto) != InvalidPrototypeChain;
}

static void testChainThroughCompiledObjectIsNotCachedByStructures(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(chainIsCacheableByStructures(program, constructEmptyObject(program.realm())));

    Object* counter = allocateCounter(program);
    AOT_TEST_CHECK(counter->store(vm, counter->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))) && !chainIsCacheableByStructures(program, counter));
    Structure* structureBefore = counter->structure();
    counter->setIntegrity(program.realm(), Object::Integrity::Frozen);
    AOT_TEST_CHECK(counter->structure() == structureBefore && counter->isFrozen());

    auto [object, extraProperties] = createCounterWithDictionary(program);
    AOT_TEST_CHECK(!chainIsCacheableByStructures(program, object));
    structureBefore = object->structure();
    PropertyDescriptor readOnly(jsNumber(1), static_cast<unsigned>(PropertyAttribute::ReadOnly));
    AOT_TEST_CHECK(object->methodTable()->defineOwnProperty(object, program.realm(), Identifier::fromString(vm, "late"_s), readOnly, false));
    AOT_TEST_CHECK(isCompiledObject(object) && object->structure() == structureBefore);
    ensureStillAliveHere(extraProperties);
}

static void testFirstAdditionNamesNoStructure(LoadedProgram& program)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = program.vm();
    Identifier first = nameAt(vm, 0);
    Identifier second = nameAt(vm, 1);
    Object* object = allocateCounter(program);
    AOT_TEST_CHECK(object->store(vm, object->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))) && !object->repeatsExtraPropertyStore(vm, first));
    AOT_TEST_CHECK(object->tryStoreExtraProperty(vm, first, jsNumber(1), { Flag::IsKnownNotToBeField }).kind == Object::ExtraPropertyStore::Added);
    AOT_TEST_CHECK(object->compiledStructure().hasExtraProperties() && !object->compiledStructure().extraPropertiesStructure());
    AOT_TEST_CHECK(object->repeatsExtraPropertyStore(vm, first) && !object->repeatsExtraPropertyStore(vm, second));
    AOT_TEST_CHECK(getAndCache(program, object, first) == jsNumber(1) && object->compiledStructure().extraPropertiesStructure() == object->extraProperties()->structure());
}

static void testSecondObjectRepeatsAddition(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier first = nameAt(vm, 0);
    Identifier second = nameAt(vm, 1);
    Object* object = allocateCounter(program);
    AOT_TEST_CHECK(object->store(vm, object->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))));
    putAndCache(program, object, first, jsNumber(1));

    Object* other = allocateCounter(program);
    AOT_TEST_CHECK(other->store(vm, other->layout().fields()[0], AcceptedValue::provenAtBuildTime(jsNumber(0))));
    AOT_TEST_CHECK(other->repeatsExtraPropertyStore(vm, first) && !other->repeatsExtraPropertyStore(vm, second));
    putAndCache(program, other, first, jsNumber(2));
    AOT_TEST_CHECK(other->structure() == object->structure() && !other->repeatsExtraPropertyStore(vm, second));

    JSObject* ordinary = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(!repeatsPropertyAddition(ordinary->structure(), first.impl()));
    ordinary->putDirect(vm, first, jsNumber(1));
    AOT_TEST_CHECK(repeatsPropertyAddition(constructEmptyObject(program.realm())->structure(), first.impl()) && !repeatsPropertyAddition(ordinary->structure(), second.impl()));
}

template<LoadedProgramTest body>
static constexpr void (*withCounter)() = runNativeTestWithLoadedProgram<dictionaryExtraPropertiesTests, programText, body>;

std::span<const NativeTest> dictionaryExtraPropertiesTests()
{
    static constexpr NativeTest tests[] = {
        { "dictionary-extra-properties/structure-names-dictionary"_s, withCounter<testStructureNamesDictionary> },
        { "dictionary-extra-properties/present-names-have-slots"_s, withCounter<testPresentNamesHaveSlots> },
        { "dictionary-extra-properties/absent-name-is-never-filed"_s, withCounter<testAbsentNameIsNeverFiled> },
        { "dictionary-extra-properties/leaf-finds-inherited-property"_s, withCounter<testLeafFindsInheritedProperty> },
        { "dictionary-extra-properties/keys-follow-dictionary"_s, withCounter<testKeysFollowDictionary> },
        { "dictionary-extra-properties/deleted-name-loses-its-slot"_s, withCounter<testDeletedNameLosesItsSlot> },
        { "dictionary-extra-properties/naming-flattens-dictionary-once"_s, withCounter<testNamingFlattensDictionaryOnce> },
        { "dictionary-extra-properties/deletions-keep-key-list"_s, withCounter<testDeletionsKeepKeyList> },
        { "dictionary-extra-properties/symbols-and-hidden-names-have-no-key-list"_s, withCounter<testSymbolsAndHiddenNamesHaveNoKeyList> },
        { "dictionary-extra-properties/chain-through-compiled-object-is-not-cached-by-structures"_s, withCounter<testChainThroughCompiledObjectIsNotCachedByStructures> },
        { "dictionary-extra-properties/first-addition-names-no-structure"_s, withCounter<testFirstAdditionNamesNoStructure> },
        { "dictionary-extra-properties/second-object-repeats-addition"_s, withCounter<testSecondObjectRepeatsAddition> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
