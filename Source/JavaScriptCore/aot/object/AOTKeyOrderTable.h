/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTObject.h"
#include "AOTObjectWords.h"
#include "AOTProgramTables.h"
#include "RememberedSlot.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSCellButterfly;
class JSPropertyNameEnumerator;
class StructureChain;

namespace AOT {

struct State;

enum class PrefixRegistration : bool { EveryPrefix, WholeOrderOnly };

class KeyOrderTable final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(KeyOrderTable);
public:
    explicit KeyOrderTable(State&);
    ~KeyOrderTable();

    std::span<const uint16_t> entriesInOrder(const LayoutData&, uint32_t order);
    std::optional<uint32_t> orderStartingWith(const LayoutData&, std::span<const uint16_t> entries, PrefixRegistration = PrefixRegistration::EveryPrefix);
    std::optional<uint32_t> orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t entry);
    bool positionsExtraKeys(const LayoutData&, uint32_t orderWord, uint32_t numberOfExtraKeys);

    JSCellButterfly* keysInOrder(VM&, const LayoutData&, uint32_t orderWord);
    std::optional<std::span<const FieldAsProperty>> propertiesInOrder(VM&, const LayoutData&, uint32_t orderWord, Object::SymbolKeys = Object::SymbolKeys::Skip);
    JSPropertyNameEnumerator* enumerator(const LayoutData&, uint32_t orderWord, StructureChain* prototypeChain) const;
    void setEnumerator(VM&, const LayoutData&, uint32_t orderWord, JSPropertyNameEnumerator*, StructureChain* prototypeChain);
    // Compiled code finds a published enumerator by the Structure and the order word alone, until an object on the prototype chain changes. Nothing is published under a prototype that can change unreported.
    void publishEnumerator(const Object&, JSPropertyNameEnumerator*);
    void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const final;

    const FieldData* fieldAt(const LayoutData&, uint32_t slotKey);
    std::optional<uint32_t> publishedTransition(const LayoutData&, uint32_t orderAndCountBefore, const FieldData&, uint32_t numberOfExtraKeys = 0) const;
    void publishTransition(const LayoutData&, uint32_t orderAndCountBefore, const FieldData&, uint32_t orderAndCountAfter, uint32_t numberOfExtraKeys = 0);

    template<typename Visitor> void visitRoots(Visitor&);

private:
    struct OrderTable;

    OrderTable& orderTableFor(const LayoutData&);
    void growTransitions();

    State& m_state;
    const ProgramTables& m_tables;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<OrderTable>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_orderTables;
    RememberedSlotSet m_writes;
    uint32_t m_numberOfTransitions { 0 };
    std::optional<ChainDependentOwnerID> m_ownerOfPublishedEnumerators;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
