/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTRuntimeWords.h"
#include "AOTWatchpoints.h"
#include "Identifier.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class GlobalBindings final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(GlobalBindings);
    WTF_MAKE_TZONE_ALLOCATED(GlobalBindings);
public:
    static constexpr uint32_t seesScriptBindingsFlag = 1u << 31;
    static constexpr uint32_t isStoredByNameFlag = 1u << 30;
    static constexpr uint32_t isForTypeOfFlag = 1u << 29;
    static constexpr uint32_t isStoredThroughBindingFlag = 1u << 28;
    static constexpr uint32_t identifierMask = isStoredThroughBindingFlag - 1;

    explicit GlobalBindings(State&);

    GlobalBinding& at(uint32_t number)
    {
        RELEASE_ASSERT(number < m_records.size());
        return m_records[number];
    }
    Identifier nameOf(uint32_t number) const;
    bool seesScriptBindings(uint32_t number) const { return m_names[number] & seesScriptBindingsFlag; }
    bool filesLocationAlone(uint32_t number) const { return m_names[number] & isStoredThroughBindingFlag; }
    bool answersUndefinedWhereAbsent(uint32_t number) const { return !seesScriptBindings(number) || m_names[number] & isForTypeOfFlag; }

    bool fileConstant(VM&, uint32_t number, JSObject* holderToWatch, JSValue);
    void fileAbsence(VM&, uint32_t number, std::span<JSObject* const> objectsAboveGlobalObject);
    void fileLocation(uint32_t number, Structure*, int64_t location);
    void didFileStore(uint32_t number);

    void dropConstants();
    void dropLocations();

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using NumberHashTraits = WTF::UnsignedWithZeroKeyHashTraits<uint32_t>;

    void dropChainDependent(VM&, uint32_t number, UniquedStringImpl*) final;
    bool hasChainDependent(VM&, uint32_t number, UniquedStringImpl*) const final { return m_isFiledAsAbsent.get(number); }

    uint32_t identifierNumberOf(uint32_t number) const { return m_names[number] & identifierMask; }
    void setConstant(uint32_t number, JSValue, bool isAbsence);

    State& m_state;
    std::span<GlobalBinding> m_records;
    std::span<const uint32_t> m_names;
    Vector<uint32_t> m_filedConstants;
    Vector<uint32_t> m_filedLocations;
    BitVector m_isListedAsConstant;
    BitVector m_isFiledAsAbsent;
    ChainDependentOwnerID m_chainDependentOwner;
    BitVector m_isStoredThrough;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, NumberHashTraits> m_identifiersStoredThrough;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<PropertyValueWatchpoint>, IntHash<uint32_t>, NumberHashTraits> m_watches;
    RememberedSlotSet m_writtenSinceCollection;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
