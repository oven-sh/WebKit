/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCollectionTestSupport.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTNativeTest.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "DeferGC.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "ObjectConstructor.h"
#include "VMInlines.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

class TableFixture {
    WTF_MAKE_NONCOPYABLE(TableFixture);
public:
    TableFixture();
    ~TableFixture();

    VM& vm() { return *m_vm; }
    JSGlobalObject* globalObject() { return m_globalObject.get(); }
    VMPropertyCaches& caches() { return *m_vm->aotState().propertyCaches; }
    RowDisplacementTable& front() { return caches().front(); }
    HashedSlotTable& hashedTable() { return caches().hashedTable(); }

    const Identifier& identifier(uint32_t number);
    UniquedStringImpl* uid(uint32_t number) { return identifier(number).impl(); }
    CachedName name(uint32_t number) { return { uid(number), number }; }
    CachedName nameWithoutNumber(uint32_t number) { return { uid(number), std::nullopt }; }

    Structure* createStructure(JSValue prototype = jsNull());
    Structure* addProperty(Structure*, uint32_t number, PropertyOffset&, unsigned attributes = 0);
    JSObject* createObject();
    JSObject* createPrototype();

    const PackedSlot& cellAt(Structure*, uint32_t column, SlotAccess);
    size_t numberOfSlots();
    size_t sweep(CollectionScope);

private:
    struct NumberedIdentifier {
        uint32_t number;
        Identifier identifier;
    };

    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
    std::optional<DeferGC> m_deferGC;
    Strong<JSGlobalObject> m_globalObject;
    SegmentedVector<NumberedIdentifier, 16> m_identifiers;
};

inline TableFixture::TableFixture()
    : m_vm(VM::create())
{
    VM& vm = *m_vm;
    m_lock.emplace(vm);
    m_deferGC.emplace(vm);
    m_globalObject.set(vm, JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull())));
    AOT_TEST_CHECK(!vm.hasCompiledCode() && !vm.aotState().caches);
    vm.aotState().pages = makeUnique<PagesOfVM>(8 * MB);
    vm.aotState().caches = makeUnique<CacheRegistry>();
    vm.aotState().counters = makeUniqueWithoutFastMallocCheck<RuntimeCounters>();
    auto& parts = vm.aotState().parts;
    parts.grow(1);
    parts[0].propertyCaches = makeUnique<VMPropertyCaches>(vm);
    vm.aotState().propertyCaches = parts[0].propertyCaches.get();
}

inline TableFixture::~TableFixture()
{
    m_identifiers.clear();
    m_globalObject.clear();
    m_deferGC.reset();
    m_vm = nullptr;
    m_lock.reset();
}

inline const Identifier& TableFixture::identifier(uint32_t number)
{
    for (const NumberedIdentifier& entry : m_identifiers) {
        if (entry.number == number)
            return entry.identifier;
    }
    m_identifiers.append(NumberedIdentifier { number, Identifier::fromString(vm(), makeString("p"_s, number)) });
    return m_identifiers.last().identifier;
}

inline Structure* TableFixture::createStructure(JSValue prototype)
{
    return JSFinalObject::createStructure(vm(), m_globalObject.get(), prototype, JSFinalObject::defaultInlineCapacity);
}

inline Structure* TableFixture::addProperty(Structure* structure, uint32_t number, PropertyOffset& offset, unsigned attributes)
{
    return Structure::addPropertyTransition(vm(), structure, identifier(number), attributes, offset);
}

inline JSObject* TableFixture::createObject()
{
    return constructEmptyObject(vm(), createStructure());
}

inline JSObject* TableFixture::createPrototype()
{
    JSObject* object = createObject();
    object->didBecomePrototype(vm());
    return object;
}

inline const PackedSlot& TableFixture::cellAt(Structure* structure, uint32_t column, SlotAccess access)
{
    return front().plane(access)[StructureRows::ofImageAt(0).rowOf(structure->id()) + column];
}

inline size_t TableFixture::numberOfSlots()
{
    size_t result = 0;
    front().forEachSlot([&](const Slot&, uint32_t, SlotAccess) {
        ++result;
    });
    hashedTable().forEachEntry([&](const HashedSlot&) {
        ++result;
    });
    return result;
}

inline size_t TableFixture::sweep(CollectionScope scope)
{
    size_t numberBefore = numberOfSlots();
    collectWithCollectorHooks(vm(), scope);
    return numberBefore - numberOfSlots();
}

inline uint32_t rowOf(Structure* structure)
{
    return StructureRows::ofImageAt(0).rowOf(structure->id());
}

inline bool isSameSlot(const Slot& a, const Slot& b)
{
    return a.headerBits() == b.headerBits() && a.payloadBits() == b.payloadBits();
}

inline bool isSameSlot(const std::optional<Slot>& found, const Slot& expected)
{
    return found && isSameSlot(*found, expected);
}

inline bool isSameSlot(const Slot* found, const Slot& expected)
{
    return found && isSameSlot(*found, expected);
}

inline Slot ownValueSlot(Structure* structure, PropertyOffset offset)
{
    return Slot::createInStorage(structure->id(), Slot::Kind::Value, Slot::isOwn, nullptr, offset);
}

inline Slot inheritedValueSlot(Structure* structure, JSObject* holder, PropertyOffset offset, uint8_t flags = Slot::dependsOnChain)
{
    return Slot::createInStorage(structure->id(), Slot::Kind::Value, flags, holder, offset);
}

inline Slot absentSlot(Structure* structure, uint8_t flags = Slot::dependsOnChain)
{
    return Slot::createWithoutPayload(structure->id(), Slot::Kind::Absent, flags);
}

inline Slot transitionSlot(Structure* structure, Structure* newStructure, PropertyOffset offset, uint8_t flags = 0)
{
    bool isFirstOutOfLineProperty = offset == firstOutOfLineOffset;
    uint32_t newStorageCapacity = isFirstOutOfLineProperty ? Slot::storageCapacityAfterAdding(Slot::wordIndexFor(offset)) : 0;
    std::optional<Slot> slot = Slot::tryCreateTransition(structure->id(), flags, newStructure->id(), offset, newStorageCapacity);
    AOT_TEST_CHECK(slot);
    return *slot;
}

inline Slot fieldSlot(Structure* structure, SlotRepresentation representation, uint32_t type = 0, uint8_t flags = Slot::isOwn)
{
    constexpr uint32_t offsetOfField = 16;
    return Slot::createField(structure->id(), Slot::Kind::Field, flags, SlotData::create(offsetOfField, representation), type, 0);
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
