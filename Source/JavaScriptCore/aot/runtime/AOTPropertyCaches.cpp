/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCaches.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTProgramIdentifiers.h"
#include "AOTState.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCInlines.h"
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
}

VMPropertyCaches::~VMPropertyCaches()
{
    for (UniquedStringImpl* uid : m_columnsByNameAddress.keys())
        uid->deref();
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
    publishWords(instance);
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

PropertyCaches::PropertyCaches(State& state)
    : m_state(state)
    , m_vm(*state.instance.vm)
    , m_tables(*m_vm.aotState().propertyCaches)
    , m_tablesOwnerID(state.chainDependents->addOwner(m_tables))
    , m_siteCaches(state)
    , m_watchedValues(state)
    , m_stringKeys(state)
{
    JSGlobalObject* globalObject = state.instance.globalObject;
    PropertyCacheWords& words = state.instance.propertyCaches;
    words.numberStructureID = globalObject->numberObjectStructure()->id().bits();
    words.booleanStructureID = globalObject->booleanObjectStructure()->id().bits();
    words.hasInstanceUID = m_vm.propertyNames->hasInstanceSymbol.impl();
    words.prototypeUID = m_vm.propertyNames->prototype.impl();
    words.defaultHasInstanceFunction = globalObject->functionProtoHasInstanceSymbolFunction();
    fillHandlerTables();
    globalObject->didBecomePrototype(m_vm);
    state.caches->addObserver("PropertyCaches"_s, *this);
}

PropertyCaches::~PropertyCaches() = default;

void PropertyCaches::fillHandlerTables()
{
    const Image& image = *m_vm.aotState().image;
    if (image.sharedCodeCopies().empty())
        return;
    uint8_t* sharedCode = const_cast<uint8_t*>(image.sharedCodeCopy(0));
    std::span offsets = image.slotHandlerOffsets();
    std::span handlers(&m_state.instance.propertyCaches.handlers[0][0], numberOfSlotHandlerTables * numberOfSlotKinds);
    RELEASE_ASSERT(offsets.size() == handlers.size());
    for (size_t index = 0; index < handlers.size(); ++index)
        handlers[index] = offsets[index] == offsetOfAbsentStub ? nullptr : sharedCode + offsets[index];
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
    if (!canCache(structure, answer))
        return;
    Slot slot = slotFor(answer, structure);
    m_tables.set(structure, belongsToOneObject(structure) ? CachedName { access.name.uid, std::nullopt } : access.name, access.access, slot);
    if (slot.has(Slot::dependsOnChain))
        registerChainDependents(access, answer);
    if (access.site)
        m_siteCaches.fill(access.site, slot, { access.name.uid, access.access, answer.changeableObjects.span() });
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
    ChainWalkUse use = access == SlotAccess::Read ? ChainWalkUse::KeptForReads : ChainWalkUse::KeptForStores;
    ChainWalk walk = structure->typeInfo().type() == GlobalProxyType
        ? walkChainThroughGlobalProxy(globalObject, *slot, name.uid)
        : walkChain({ globalObject, structure, base->isObject() ? asObject(base) : nullptr, ChainWalkMode::AllocatesNothing, use }, name.uid);
    if (walk.result == ChainWalkResult::Unknown) {
        m_siteCaches.didFailToFill(site);
        return;
    }
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
    m_siteCaches.fill(site, slot, { uid, access, objects.span() });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
