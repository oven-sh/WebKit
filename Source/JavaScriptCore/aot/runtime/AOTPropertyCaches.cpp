/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCaches.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTCompiledFrame.h"
#include "AOTCompiledStructure.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeCounters.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStubsPropertyKeys.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "SlotVisitorInlines.h"
#include <bit>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMPropertyCaches);
WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyCaches);

static std::optional<uint32_t> columnForNumber(std::optional<uint32_t> identifierNumber)
{
    if (!identifierNumber || !RowDisplacementTable::hasColumn(*identifierNumber))
        return std::nullopt;
    return identifierNumber;
}

VMPropertyCaches::VMPropertyCaches(VM& vm)
    : m_vm(vm)
    , m_customAccessors(vm, *vm.aotState().caches)
    , m_front(vm, *vm.aotState().caches, m_customAccessors)
    , m_hashedTable(vm, *vm.aotState().caches, m_customAccessors)
{
    // A filter names Structures by their numbers, and the number of one that has ended goes to another.
    vm.aotState().caches->addClearedEachCollection("absence filters"_s, asWritableBytes(std::span { m_absenceFilters }));
}

VMPropertyCaches::~VMPropertyCaches()
{
    for (UniquedStringImpl* uid : m_columnsByNameAddress.keys())
        uid->deref();
    for (NameOfText& entry : m_namesOfTexts) {
        if (entry.uid)
            entry.uid->deref();
    }
}

void VMPropertyCaches::rememberNameOfText(UniquedStringImpl* uid)
{
    uint32_t length = uid->length();
    if (uid->isSymbol() || !uid->is8Bit() || length < NameOfText::minLength || length > NameOfText::maxLength)
        return;
    const Latin1Character* characters = uid->span8().data();
    unsigned width = NameOfText::widthOfWordFor(length);
    uint64_t first = 0;
    uint64_t last = 0;
    static_assert(std::endian::native == std::endian::little);
    memcpy(&first, characters, width);
    memcpy(&last, characters + length - width, width);
    NameOfText* pair = &m_namesOfTexts[NameOfText::firstIndexFor(first, last, length)];
    if (pair[0].uid == uid || pair[1].uid == uid)
        return;
    uid->ref();
    if (pair[1].uid)
        pair[1].uid->deref();
    pair[1] = pair[0];
    pair[0] = { first, last, length, uid };
}

static bool listsEveryNameInItsTable(Structure* structure)
{
    // A dictionary gets a name without becoming another Structure. One that may be a prototype reports it.
    return (!structure->isDictionary() || structure->mayBePrototype()) && !structure->hasPolyProto() && !hasIndexedProperties(structure->indexingType())
        && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence()
        && !structure->typeInfo().overridesGetOwnPropertySlot() && !structure->hasNonReifiedStaticProperties();
}

void VMPropertyCaches::didCreateInstance(Instance& instance)
{
    // What an earlier instance filed under a Structure of the VM describes its realm, whether or not that instance is still there.
    if (m_hasInstance && !m_isSharedByInstances) {
        m_isSharedByInstances = m_vm.aotState().instances.size() > 1;
        m_front.clear();
        m_hashedTable.clear();
    }
    m_hasInstance = true;
    if (instance.globalObject)
        didChangeObjectPrototype(instance.globalObject->objectPrototype(), nullptr);
    publishWords(instance);
}

// The bits are never cleared, so after a change that is reported without a name the names that are there now are all that there is to add.
void VMPropertyCaches::didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull)
{
    Structure* structure = prototype->structure();
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (!identifiers || !listsEveryNameInItsTable(structure) || !structure->storedPrototype().isNull()) {
        m_namesOfObjectPrototypes.fill(UINT64_MAX);
        return;
    }
    auto add = [&](UniquedStringImpl* uid) {
        // Compiled code asks for a name by its number, so a name without one is never asked for.
        std::optional<uint32_t> number = identifiers->numberOf(uid);
        if (number && FieldTableEntry::hasColumn(*number))
            m_namesOfObjectPrototypes[*number / 64] |= uint64_t { 1 } << *number % 64;
    };
    if (uidOrNull) {
        add(uidOrNull);
        return;
    }
    structure->forEachProperty(m_vm, [&](const auto& entry) {
        add(entry.key());
        return true;
    });
}

void VMPropertyCaches::publishWords(Instance& instance) const
{
    PropertyCacheWords& words = instance.propertyCaches;
    const uint32_t* rowsForStructureIDZero = StructureRows::singleton().rowsForStructureIDZero();
    static_assert(numberOfSlotAccesses == 3);
    for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define })
        words.front[static_cast<unsigned>(access)] = { rowsForStructureIDZero, m_front.plane(access) };
    words.hashedSlots = m_hashedTable.entries();
    words.hashedSlotOffsetMask = m_hashedTable.offsetMask();
    words.customAccessors = m_customAccessors.records();
    words.absenceFilters = const_cast<AbsenceFilter*>(m_absenceFilters.data());
    words.absenceFilterGeneration = m_absenceFilterGeneration;
    words.absenceFilterGenerationForStores = m_absenceFilterGenerationForStores;
    words.namesChangedOnChains = m_namesChangedOnChains.data();
    words.namesOfObjectPrototypes = m_namesOfObjectPrototypes.data();
    words.missesOfPairs = const_cast<uint8_t*>(m_missesOfPairs.data());
    words.namesOfTexts = m_namesOfTexts.data();
    words.infoOfClassPrototypes = ClassPrototype::info();
}

// Adds every name that the object may answer for by itself.
enum class PlaceInFilter : bool { Lowest, Above };

static bool addOwnNames(VM& vm, const ProgramIdentifiers& identifiers, JSObject* object, PlaceInFilter place, AbsenceFilter& filter)
{
    constexpr unsigned attributesThatStopStores = PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue;
    auto add = [&](UniquedStringImpl* uid, bool stopsStores) {
        // Compiled code asks for a name by its number, so a name without one is never asked for.
        if (std::optional<uint32_t> number = identifiers.numberOf(uid)) {
            filter.add(*number);
            if (stopsStores && place == PlaceInFilter::Above)
                filter.addNameThatStopsStores(*number);
        }
    };
    auto addNamesInTable = [&](Structure* structure) {
        if (!listsEveryNameInItsTable(structure))
            return false;
        // An object that has a say of its own in a store that comes past it: no list of names speaks for it.
        if (place == PlaceInFilter::Above && (structure->typeInfo().overridesPut() || structure->typeInfo().overridesGetPrototype()))
            filter.generationForStores = 0;
        structure->forEachProperty(vm, [&](const auto& entry) {
            add(entry.key(), entry.attributes() & attributesThatStopStores);
            return true;
        });
        return true;
    };

    Structure* structure = object->structure();
    if (structure->isCompiledStructure()) {
        auto* compiled = uncheckedDowncast<CompiledStructure>(structure);
        if (!compiled->describesObject() || (compiled->hasExtraProperties() && !compiled->listsEveryExtraProperty()))
            return false;
        // A field that has left its slot lies among the extra properties, in whatever form it has now.
        bool isFrozen = uncheckedDowncast<Object>(object)->orderWord() & orderWordIsFrozen;
        for (const FieldData& field : compiled->layout().fields()) {
            filter.add(field.name);
            if (place == PlaceInFilter::Above && (isFrozen || field.flags & (fieldIsReadOnly | fieldIsAccessor) || field.slot.representation() == SlotRepresentation::DeferredClosure))
                filter.addNameThatStopsStores(field.name);
        }
        return !compiled->hasExtraProperties() || addNamesInTable(compiled->extraPropertiesStructure());
    }
    if (object->classInfo() == ClassPrototype::info()) {
        // It stands for the prototype object of a class until somebody needs more than the declared members, and has those of the class's record.
        Instance* instance = Instance::of(structure->realm());
        State* state = instance ? instance->state : nullptr;
        if (!state || object->butterfly())
            return false;
        const LayoutData* classLayout = state->objects->classes.classWithIndex(uncheckedDowncast<ClassPrototype>(object)->classIndex());
        for (const MethodData& method : classLayout->methods()) {
            if (method.flags & methodHasComputedName)
                return false;
            filter.add(method.name);
            if (place == PlaceInFilter::Above && method.kind() != MethodKind::Method)
                filter.addNameThatStopsStores(method.name);
        }
        add(vm.propertyNames->constructor.impl(), false);
        return true;
    }
    return addNamesInTable(structure);
}

void VMPropertyCaches::fileAbsenceFilter(JSGlobalObject* globalObject, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->typeInfo().type() != FinalObjectType && !structure->isCompiledStructure())
        return;
    // A flag of such a Structure answers exactly. A filter behind it would answer what that leaves now and then for a slot to be filed.
    if (structure->isCompiledStructure() && uncheckedDowncast<CompiledStructure>(structure)->flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        return;
    fileAbsenceFilterOf(globalObject, object, FilterUse::Reads);
}

void VMPropertyCaches::fileAbsenceFilterForStores(JSGlobalObject* globalObject, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->hasPolyProto() || structure->typeInfo().overridesGetPrototype())
        return;
    // Only a Structure that names the extra properties says which there are.
    for (JSObject* holder = structure->storedPrototypeObject(); holder; holder = holder->getPrototypeDirect().getObject())
        Object::ensureStructureForExtraPropertiesIfNeeded(m_vm, holder);
    fileAbsenceFilterOf(globalObject, object, FilterUse::Stores);
}

void VMPropertyCaches::fileAbsenceFilterOf(JSGlobalObject* globalObject, JSObject* object, FilterUse use)
{
    constexpr unsigned maxObjectsAbove = 8;
    Structure* structure = object->structure();
    AbsenceFilter& filter = m_absenceFilters[AbsenceFilter::indexFor(structure->id().bits())];
    if (filter.structureID == structure->id().bits() && (filter.generation == m_absenceFilterGeneration || (use == FilterUse::Stores && filter.generationForStores == m_absenceFilterGenerationForStores)))
        return;
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (!identifiers || structure->realm() != globalObject)
        return;

    AbsenceFilter filled { structure->id().bits(), m_absenceFilterGeneration, { }, 0, m_absenceFilterGenerationForStores, { } };
    unsigned numberOfObjects = 0;
    for (JSObject* holder = object; holder; holder = holder->getPrototypeDirect().getObject()) {
        if (numberOfObjects++ > maxObjectsAbove)
            return;
        if (addOwnNames(m_vm, *identifiers, holder, holder == object ? PlaceInFilter::Lowest : PlaceInFilter::Above, filled))
            continue;
        // What the lowest object has itself is nothing to a store that creates a property of it. No read is answered then: zero is no generation.
        if (holder != object || use == FilterUse::Reads)
            return;
        filled.generation = 0;
    }
    filter = filled;
}

// The change has been made.
static bool mayStopStores(JSObject* changed, UniquedStringImpl* uid)
{
    constexpr unsigned attributesThatStopStores = PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue;
    Structure* structure = changed->structure();
    bool isField = false;
    if (Object* object = compiledObjectOrNull(changed)) {
        const FieldData* field = object->fieldNamed(object->state(), uid);
        if (field && object->isInSlot(*field))
            return object->attributesOf(*field) & attributesThatStopStores || field->slot.representation() == SlotRepresentation::DeferredClosure;
        isField = field;
        JSObject* extra = extraPropertiesIfMade(*object);
        if (!extra)
            return hasPendingStars(*object);
        structure = extra->structure();
        if (structure->isUncacheableDictionary())
            return true;
    } else if (structure->isCompiledStructure() || !listsEveryNameInItsTable(structure))
        return true;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset)
        return true;
    return isValidOffset(*offset) && (isField || attributes & attributesThatStopStores);
}

void VMPropertyCaches::didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull)
{
    if (!++m_absenceFilterGeneration) [[unlikely]] {
        m_absenceFilters.fill(AbsenceFilter { });
        m_absenceFilterGeneration = 1;
    }
    // Compiled code asks for a name by its number, and a store asks the filters only for a name that has a column.
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (uidOrNull && identifiers) {
        std::optional<uint32_t> number = identifiers->numberOf(uidOrNull);
        if (number && FieldTableEntry::hasColumn(*number) && mayStopStores(changed, uidOrNull))
            m_namesChangedOnChains[*number / 64] |= uint64_t { 1 } << *number % 64;
    } else if (!++m_absenceFilterGenerationForStores) [[unlikely]] {
        m_absenceFilters.fill(AbsenceFilter { });
        m_absenceFilterGenerationForStores = 1;
    }
    for (Instance* instance : m_vm.aotState().instances) {
        instance->propertyCaches.absenceFilterGeneration = m_absenceFilterGeneration;
        instance->propertyCaches.absenceFilterGenerationForStores = m_absenceFilterGenerationForStores;
    }
}

void VMPropertyCaches::publishWordsToEveryInstance() const
{
    for (Instance* instance : m_vm.aotState().instances)
        publishWords(*instance);
}

std::optional<uint32_t> VMPropertyCaches::columnOf(UniquedStringImpl* uid) const
{
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (!identifiers)
        return std::nullopt;
    return columnForNumber(identifiers->numberOf(uid));
}

// A ChainDependent holds no reference on its name, which may be freed by now: only the address is looked at.
std::optional<uint32_t> VMPropertyCaches::findColumnByAddress(UniquedStringImpl* uid) const
{
    auto iterator = m_columnsByNameAddress.find(uid);
    if (iterator == m_columnsByNameAddress.end())
        return std::nullopt;
    return iterator->value;
}

void VMPropertyCaches::ensureColumnByAddress(UniquedStringImpl* uid, uint32_t column)
{
    auto result = m_columnsByNameAddress.add(uid, static_cast<uint16_t>(column));
    ASSERT(result.iterator->value == column);
    if (result.isNewEntry)
        uid->ref();
}

void VMPropertyCaches::set(Structure* structure, const CachedName& name, SlotAccess access, const Slot& slot)
{
    StructureID id = structure->id();
    ASSERT(slot.structureID == id);
    if (std::optional<uint32_t> column = columnForNumber(name.identifierNumber)) {
        if (m_front.trySet(structure, *column, access, slot)) {
            if (slot.has(Slot::dependsOnChain))
                ensureColumnByAddress(name.uid, *column);
            m_hashedTable.remove(id, name.uid, access);
            return;
        }
        m_front.remove(id, *column, access);
    }
    if (m_hashedTable.set(name.uid, access, slot) == HashedSlotTable::SetResult::StoredAndGrew)
        publishWordsToEveryInstance();
}

std::optional<Slot> VMPropertyCaches::find(StructureID id, const CachedName& name, SlotAccess access) const
{
    if (std::optional<uint32_t> column = columnForNumber(name.identifierNumber)) {
        if (std::optional<Slot> slot = m_front.find(id, *column, access))
            return slot;
    }
    if (const Slot* slot = m_hashedTable.find(id, name.uid, access))
        return *slot;
    return std::nullopt;
}

void VMPropertyCaches::remove(StructureID id, const CachedName& name, SlotAccess access)
{
    std::optional<uint32_t> column = name.identifierNumber ? columnForNumber(name.identifierNumber) : columnOf(name.uid);
    if (column)
        m_front.remove(id, *column, access);
    m_hashedTable.remove(id, name.uid, access);
}

void VMPropertyCaches::dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uid)
{
    ASSERT(uid);
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    if (std::optional<uint32_t> column = findColumnByAddress(uid))
        m_front.removeChainDependentCells(id, *column);
    m_hashedTable.removeChainDependentEntries(id, uid);
}

bool VMPropertyCaches::hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uid) const
{
    ASSERT(uid);
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    std::optional<uint32_t> column = findColumnByAddress(uid);
    if (column && m_front.hasChainDependentCell(id, *column))
        return true;
    return m_hashedTable.hasChainDependentEntry(id, uid);
}

static void fillHandlerTables(PropertyCacheWords& words, const Image& image)
{
    if (image.sharedCodeCopies().empty())
        return;
    uint8_t* sharedCode = const_cast<uint8_t*>(image.sharedCodeCopy(0));
    std::span offsets = image.slotHandlerOffsets();
    std::span handlers(&words.handlers[0][0], numberOfSlotHandlerTables * numberOfSlotKinds);
    RELEASE_ASSERT(offsets.size() == handlers.size());
    for (size_t index = 0; index < handlers.size(); ++index)
        handlers[index] = offsets[index] == offsetOfAbsentStub ? nullptr : sharedCode + offsets[index];
}

// The instances of a VM share its lookup tables, so a probe here may hit what another instance has filed: the handlers are in place from the start.
void initializePropertyCacheWords(Instance& instance)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    PropertyCacheWords& words = instance.propertyCaches;

    // The realm creates the classes of numbers and booleans at their first use, and most programs never look a property up in either.
    Structure* numberStructure = globalObject->numberObjectStructureConcurrently();
    Structure* booleanStructure = globalObject->booleanObjectStructureConcurrently();
    Structure* structureOfNoReceiver = vm.structureStructure.get();
    words.numberStructureID = (numberStructure ? numberStructure : structureOfNoReceiver)->id().bits();
    words.booleanStructureID = (booleanStructure ? booleanStructure : structureOfNoReceiver)->id().bits();

    words.hasInstanceUID = vm.propertyNames->hasInstanceSymbol.impl();
    words.prototypeUID = vm.propertyNames->prototype.impl();
    words.defaultHasInstanceFunction = globalObject->functionProtoHasInstanceSymbolFunction();
    fillHandlerTables(words, *vm.aotState().image);
}

LazyPropertyCaches::LazyPropertyCaches(State& state)
    : m_state(state)
{
}

LazyPropertyCaches::~LazyPropertyCaches() = default;

NEVER_INLINE PropertyCaches& LazyPropertyCaches::create()
{
    m_caches = makeUnique<PropertyCaches>(m_state);
    return *m_caches;
}

PropertyCaches::PropertyCaches(State& state)
    : m_state(state)
    , m_vm(*state.instance.vm)
    , m_tables(*m_vm.aotState().propertyCaches)
    , m_tablesOwnerID(state.chainDependents->addOwner(m_tables))
    , m_siteCaches(*state.siteCaches)
    , m_watchedValues(state)
    , m_stringKeys(state)
{
    state.caches->add(CacheKind::StrongRoots, "Structures of additions in an Eden collection"_s, *this);
    state.caches->addObserver("PropertyCaches"_s, *this);
    if (Options::aotCountsPath())
        m_countedFilings = makeUnique<UncheckedKeyHashSet<uint64_t>>();
}

PropertyCaches::~PropertyCaches() = default;

void PropertyCaches::didAddProperty(Structure* before, Structure* after)
{
    if (before == after || after->isDictionary())
        return;
    Locker locker { m_state.caches->lock() };
    m_additionsSinceCollection.append({ before->id(), after->id() });
}

// The objects that take an addition may all be gone by the next Eden collection, and nothing but an object keeps the Structure it leads to.
template<typename Visitor>
void PropertyCaches::visitRoots(Visitor& visitor)
{
    if (visitor.collectionScope() != CollectionScope::Eden)
        return;
    auto keepsStructure = [&](const Addition& addition) {
        if (!visitor.isMarked(addition.before.decode()))
            return false;
        visitor.appendUnbarriered(addition.after.decode());
        return true;
    };
    // This runs again whenever more has been marked, and the list ends with the collection: only an addition that still waits for its first Structure is asked again.
    if constexpr (std::is_same_v<Visitor, SlotVisitor>)
        m_additionsSinceCollection.removeAllMatching(keepsStructure);
    else {
        for (const Addition& addition : m_additionsSinceCollection)
            keepsStructure(addition);
    }
}

template void PropertyCaches::visitRoots(AbstractSlotVisitor&);
template void PropertyCaches::visitRoots(SlotVisitor&);

void PropertyCaches::publishIfStandsForPrimitives(Structure* structure)
{
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    PropertyCacheWords& words = m_state.instance.propertyCaches;
    if (structure == globalObject->numberObjectStructureConcurrently())
        words.numberStructureID = structure->id().bits();
    else if (structure == globalObject->booleanObjectStructureConcurrently())
        words.booleanStructureID = structure->id().bits();
}

// A kept answer holds until an object that it depends on reports a change, and only an object that may be a prototype reports.
void PropertyCaches::ensureChangesAreReported(std::span<JSObject* const> objects)
{
    for (JSObject* object : objects) {
        if (object->mayBePrototype()) [[likely]]
            continue;
        // The caller may be an operation that is not expected to collect.
        DeferGCForAWhile deferGC(m_vm);
        object->didBecomePrototype(m_vm);
    }
}

CachedName PropertyCaches::nameOf(uint32_t identifierNumber) const
{
    return { m_vm.aotState().identifiers->identifier(identifierNumber).impl(), identifierNumber };
}

CachedName PropertyCaches::nameOf(UniquedStringImpl* uid) const
{
    return { uid, m_vm.aotState().identifiers->numberOf(uid) };
}

// A cacheable dictionary gets another Structure when one of its properties is deleted or redefined, so the offset of a property it has stays. What it lacks may be added under the same Structure.
static bool isValueHeldByCacheableDictionary(Structure* structure, const PropertyAnswer& answer)
{
    return !structure->isUncacheableDictionary() && answer.kind == PropertyAnswerKind::Value && answer.isOwn && !answer.dependsOnChain();
}

bool PropertyCaches::canCache(Structure* structure, const PropertyAnswer& answer) const
{
    if (structure->isDictionary() && !isValueHeldByCacheableDictionary(structure, answer))
        return false;
    bool isVariable = answer.kind == PropertyAnswerKind::Variable;
    if (answer.dependsOnChain() || isVariable) {
        JSGlobalObject* realm = structure->realm();
        if (realm && Instance::of(realm) != &m_state.instance)
            return false;
    }
    return !m_tables.isSharedByInstances() || (!isVariable && structure->typeInfo().isObject());
}

// The front table has a row per Structure, for Structures that many objects have.
static bool belongsToOneObject(Structure* structure)
{
    if (structure->isDictionary() || structure->typeInfo().type() == GlobalProxyType)
        return true;
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    return compiled && compiled->extraPropertiesStructure() && !compiled->listsEveryExtraProperty();
}

void PropertyCaches::registerChainDependents(const CachedAccess& access, const PropertyAnswer& answer)
{
    m_state.chainDependents->add(answer.changeableObjects.span(), { access.name.uid, access.structure->id().bits(), m_tablesOwnerID });
}

void PropertyCaches::cache(const CachedAccess& access, const PropertyAnswer& answer)
{
    if (answer.didFlatten)
        return;
    ASSERT(answer.access == access.access);
    Structure* structure = access.structure;
    if (!canCache(structure, answer)) {
        count(m_state.instance, RuntimeCounterID::AnswersNotFiled);
        return;
    }
    if (m_countedFilings) [[unlikely]]
        countFiling(access);
    publishIfStandsForPrimitives(structure);
    ensureChangesAreReported(answer.changeableObjects.span());
    Slot slot = slotFor(answer, structure);
    m_tables.set(structure, belongsToOneObject(structure) ? CachedName { access.name.uid, std::nullopt } : access.name, access.access, slot);
    if (slot.has(Slot::dependsOnChain))
        registerChainDependents(access, answer);
    if (access.site)
        m_siteCaches.fill(access.site, slot, { access.name.uid, access.access, answer.changeableObjects.span() });
    if (answer.newStructure)
        didAddProperty(structure, answer.newStructure);
}

void PropertyCaches::countFiling(const CachedAccess& access)
{
    StructureID id = access.structure->id();
    uint64_t pair = (static_cast<uint64_t>(id.bits()) << 32 | WTF::PtrHash<UniquedStringImpl*>::hash(access.name.uid)) + static_cast<uint64_t>(access.access) + 1;
    if (m_countedFilings->add(pair).isNewEntry)
        return;
    count(m_state.instance, RuntimeCounterID::PairsFiledAgain);
    static constexpr std::array kindNames {
#define AOT_NAME_SLOT_KIND(name, payload, location, presence, kindFlag) #name ""_s,
        FOR_EACH_AOT_SLOT_KIND(AOT_NAME_SLOT_KIND)
#undef AOT_NAME_SLOT_KIND
    };
    std::optional<Slot> present = m_tables.find(id, nameOf(access.name.uid), access.access);
    if (!present) {
        countNamed(access.access == SlotAccess::Read ? "read filed again"_s : "other access filed again"_s, access.name.uid);
        return;
    }
    count(m_state.instance, RuntimeCounterID::PairsFiledAgainWhilePresent);
    countNamed(kindNames[static_cast<size_t>(present->kind)], access.name.uid);
}

void PropertyCaches::countNamed(ASCIILiteral what, UniquedStringImpl* name)
{
    if (!m_countedFilings)
        return;
    ++m_namedCounts.add(makeString(what, ':', StringView(name)), 0).iterator->value;
}

static ASCIILiteral describeReceiverOfMiss(JSValue receiver)
{
    if (!receiver.isCell())
        return "primitive"_s;
    JSCell* cell = receiver.asCell();
    if (PropertyWalk::isCompiledObject(cell)) {
        auto& structure = *uncheckedDowncast<CompiledStructure>(cell->structure());
        if (structure.hasExtraProperties())
            return structure.hasOtherPrototype() ? "compiled object with extra properties and another prototype"_s : "compiled object with extra properties"_s;
        return structure.hasOtherPrototype() ? "compiled object with another prototype"_s : "compiled object as its layout says"_s;
    }
    if (cell->type() == FinalObjectType)
        return cell->structure()->isDictionary() ? "ordinary object, dictionary"_s : "ordinary object"_s;
    if (cell->type() == SlimClosureType) {
        auto* closure = uncheckedDowncast<SlimClosure>(cell);
        if (closure->isBound())
            return closure->perCellBit() ? "slim closure, bound, name read before"_s : "slim closure, bound"_s;
        return closure->perCellBit() ? "slim closure, name read before"_s : "slim closure"_s;
    }
    return cell->classInfo()->className;
}

static ASCIILiteral describeFieldOfMiss(JSValue receiver, UniquedStringImpl* name)
{
    if (!receiver.isCell() || !PropertyWalk::isCompiledObject(receiver.asCell()))
        return ""_s;
    Object& object = *uncheckedDowncast<Object>(receiver.asCell());
    const FieldData* field = object.fieldNamed(object.state(), name);
    if (!field)
        return ", no such field"_s;
    if (field->isAccessor())
        return ", an accessor field"_s;
    return object.isInSlot(*field) ? ", a field in its slot"_s : ", a field outside its slot"_s;
}

NEVER_INLINE void PropertyCaches::countMiss(ASCIILiteral access, bool shouldFile, JSValue receiver, UniquedStringImpl* name)
{
    if (!m_countedFilings)
        return;
    ASCIILiteral which = shouldFile ? "later"_s : "first"_s;
    ++m_namedCounts.add(makeString("miss by receiver:"_s, access, ':', which, ':', describeReceiverOfMiss(receiver), describeFieldOfMiss(receiver, name)), 0).iterator->value;
    ++m_namedCounts.add(makeString("miss by name:"_s, access, ':', which, ':', StringView(name)), 0).iterator->value;
    auto frames = CompiledFrameIterator::tryCreateAtTop(m_vm);
    if (!frames || frames->isAtEnd())
        return;
    uintptr_t offset = std::bit_cast<uintptr_t>(frames->callSite().address) - std::bit_cast<uintptr_t>(m_vm.aotState().image->code().data());
    ++m_namedCounts.add(makeString("miss by site:"_s, access, ':', offset, ':', StringView(name), ':', describeReceiverOfMiss(receiver), describeFieldOfMiss(receiver, name)), 0).iterator->value;
}

void PropertyCaches::dumpNamedCounts(PrintStream& out) const
{
    for (auto& [name, times] : m_namedCounts)
        out.println(times, "\tnamed:", name);
}

bool PropertyCaches::shouldFile(StructureID structureID, UniquedStringImpl* uid, SlotAccess access)
{
    // The stub has counted the misses of the pair, and left because of the count.
    if (std::exchange(m_state.instance.propertyCaches.missIsDueForFiling, 0))
        return true;
    if (m_shouldForgetMisses.load(std::memory_order_relaxed)) [[unlikely]] {
        m_shouldForgetMisses.store(false, std::memory_order_relaxed);
        m_missCounters.fill(0);
    }
    constexpr uint8_t maskOfCount = (1u << bitsOfMissCount) - 1;
    static_assert(hasOneBitSet(numberOfMissCounters) && numberOfMissCounters <= 1u << 24 && missesBeforeFiling <= maskOfCount && storeMissesBeforeFiling <= maskOfCount);
    uint32_t hash = WTF::pairIntHash(structureID.bits(), WTF::PtrHash<UniquedStringImpl*>::hash(uid) + static_cast<uint32_t>(access));
    uint8_t& counter = m_missCounters[hash % numberOfMissCounters];
    uint8_t bitsOfPair = static_cast<uint8_t>(hash >> 24) & ~maskOfCount;
    if ((counter & ~maskOfCount) != bitsOfPair)
        counter = bitsOfPair;
    if ((counter & maskOfCount) >= (access == SlotAccess::Read ? missesBeforeFiling : storeMissesBeforeFiling))
        return true;
    ++counter;
    count(m_state.instance, RuntimeCounterID::FirstMissesNotFiled);
    return false;
}

const Slot* PropertyCaches::findByKey(Structure* structure, UniquedStringImpl* uid, SlotAccess access)
{
    StructureID id = structure->id();
    if (const Slot* slot = m_tables.hashedTable().find(id, uid, access)) [[likely]]
        return slot;
    std::optional<Slot> slot = m_tables.find(id, nameOf(uid), access);
    if (!slot)
        return nullptr;
    m_tables.set(structure, { uid, std::nullopt }, access, *slot);
    return m_tables.hashedTable().find(id, uid, access);
}

void PropertyCaches::fillSiteCacheFromTables(SiteCache site, JSCell* base, uint32_t identifierNumber, SlotAccess access)
{
    Structure* structure = base->structure();
    StructureID id = structure->id();
    CachedName name = nameOf(identifierNumber);
    std::optional<Slot> slot = m_tables.find(id, name, access);
    if (!slot) {
        m_siteCaches.didFailToFill(site);
        return;
    }
    m_siteCaches.didMissRepeatedly(site);
    if (!belongsToOneObject(structure) && RowDisplacementTable::hasColumn(identifierNumber) && !m_tables.front().find(id, identifierNumber, access))
        m_tables.set(structure, name, access, *slot);
    if (!slot->has(Slot::dependsOnChain)) {
        m_siteCaches.fill(site, *slot, { name.uid, access, { } });
        return;
    }
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    ChainWalkUse use = access == SlotAccess::Read ? ChainWalkUse::KeptForReadsThatAskObjects : ChainWalkUse::KeptForStores;
    ChainWalk walk = structure->typeInfo().type() == GlobalProxyType
        ? walkChainThroughGlobalProxy(globalObject, *slot, name.uid)
        : walkChain({ globalObject, structure, base->isObject() ? asObject(base) : nullptr, ChainWalkMode::AllocatesNothing, use }, name.uid);
    if (walk.result == ChainWalkResult::Unknown) {
        m_siteCaches.didFailToFill(site);
        return;
    }
    ensureChangesAreReported(walk.changeableObjects.span());
    m_siteCaches.fill(site, *slot, { name.uid, access, walk.changeableObjects.span() });
}

void PropertyCaches::cacheFreeName(SiteCache site, UniquedStringImpl* uid, SlotAccess access, const PropertyAnswer& answer)
{
    if (answer.didFlatten)
        return;
    ASSERT(site);
    ASSERT(answer.access == access);
    Slot slot = slotFor(answer, nullptr);
    if (answer.kind == PropertyAnswerKind::Constant && !answer.dependsOnChain()) {
        m_siteCaches.fill(site, slot, { uid, access, { } });
        return;
    }
    slot.flags |= Slot::dependsOnChain;
    ChainObjects objects;
    objects.append(m_state.instance.globalObject);
    objects.appendVector(answer.changeableObjects);
    ensureChangesAreReported(objects.span());
    m_siteCaches.fill(site, slot, { uid, access, objects.span() });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
