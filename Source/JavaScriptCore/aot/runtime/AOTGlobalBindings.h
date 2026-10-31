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
#include "AOTBitVector.h"

namespace JSC { namespace AOT {

// Fills and drops the records that compiled code reads at GetGlobalBinding and SetGlobalBinding. A record is filed by the first access that misses it.
class GlobalBindings final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(GlobalBindings);
    WTF_MAKE_TZONE_ALLOCATED(GlobalBindings);
public:
    // A word of the program table globalBindings: the identifier number of the name and what placement says about it.
    static constexpr uint32_t seesScriptBindingsFlag = 1u << 31;
    static constexpr uint32_t isStoredByNameFlag = 1u << 30;
    static constexpr uint32_t isForTypeOfFlag = 1u << 29;
    static constexpr uint32_t identifierMask = isForTypeOfFlag - 1;

    explicit GlobalBindings(State&);

    GlobalBinding& at(uint32_t number)
    {
        RELEASE_ASSERT(number < m_records.size());
        return m_records[number];
    }
    Identifier nameOf(uint32_t number) const;
    bool seesScriptBindings(uint32_t number) const { return m_names[number] & seesScriptBindingsFlag; }
    // A free name that nothing is bound to throws. `typeof` of it and a read of the global object answer undefined.
    bool answersUndefinedWhereAbsent(uint32_t number) const { return !seesScriptBindings(number) || m_names[number] & isForTypeOfFlag; }

    // Without a holder to watch, the value is fixed for ever. With one, false where no watch can be set: then nothing is filed.
    bool fileConstant(VM&, uint32_t number, JSObject* holderToWatch, JSValue);
    // The record answers undefined until the global object or one of the objects above it reports a change under the name.
    void fileAbsence(VM&, uint32_t number, std::span<JSObject* const> objectsAboveGlobalObject);
    void fileLocation(uint32_t number, Structure*, int64_t location);
    // An inline store replaces the value without a report, so no record of the name holds a watched constant again.
    void didFileStore(uint32_t number);

    void dropConstants();
    // The epoch of the lexical bindings starts over, so a number that is filed will come again.
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
