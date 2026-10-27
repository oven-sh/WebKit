/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowBuiltinLocations.h"
#include "AOTLedger.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

// Stands for a lock that the run time carries out. It turns every write away, whoever makes it: so each actor has accounted for the place, and it holds what stood there at creation.
class LockOnPlace {
public:
    Writers writers() const { return { { }, { } }; }

private:
    friend struct Locks;
    constexpr LockOnPlace() = default;
};

// Nobody has the word of a lock without the lock: the table's are laid when the realm is made, and the image carries out the record of those of start-up.
struct Locks {
    static std::optional<LockOnPlace> byTable(const BuiltinTable& builtins, BuiltinID member) { return when(isLockedLocation(builtins, member)); }
    static std::optional<LockOnPlace> ofWholeObject(const BuiltinTable& builtins, BuiltinID holder) { return when(builtins.at(holder).flags.contains(BuiltinFlag::IsLockedObject)); }
    static std::optional<LockOnPlace> atStartUp(const FlowBuiltinTables& tables, BuiltinID member) { return when(tables.membersLockedAtStartUp.get(member.index())); }
    static std::optional<LockOnPlace> on(const BuiltinTable& builtins, const FlowBuiltinTables& tables, BuiltinID member) { return when(isLockedLocation(builtins, member) || tables.membersLockedAtStartUp.get(member.index())); }

private:
    static std::optional<LockOnPlace> when(bool isLocked) { return isLocked ? std::optional(LockOnPlace()) : std::nullopt; }
};

// The part of the ledger that holds the places on the objects of the table.
class LedgerOfBuiltinObjects final : public LedgerPart {
    WTF_MAKE_NONCOPYABLE(LedgerOfBuiltinObjects);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LedgerOfBuiltinObjects);
public:
    LedgerOfBuiltinObjects(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowBuiltinTables&);

    std::optional<Writers> writersOf(const Place&) const final;
    std::optional<Reachers> reachersOf(ValueFlowObjectID) const final;

    // As long as no lock is asked: what start-up chooses its locks from.
    Writers writersOfMemberWithoutLocks(BuiltinID) const;
    Writers writersOfMember(BuiltinID) const;
    Writers behindLocks(BuiltinID member, const Writers& withoutLocks) const;
    Writers writersOfEveryOwnProperty(BuiltinID canonicalHolder) const;
    Writers writersOfPrototypeLink(BuiltinID canonicalHolder) const;
    Reachers reachersOf(BuiltinID canonicalHolder) const;

    bool isCoveredByUnknownObject(BuiltinID canonicalHolder) const { return m_escaped.get(canonicalHolder.index()); }
    bool operandMayHaveProperty(BuiltinID builtin, BuiltinName property) const { return m_propertiesOfOperands.contains(static_cast<uint64_t>(builtin.index()) << 32 | property.index()); }

    void forEachHolderWrittenUnderKeysNobodyKnows(NOESCAPE const Invocable<void(BuiltinID)> auto& functor) const
    {
        BitVector written = m_underKeysNobodyKnows;
        written.merge(m_underIndexes);
        forEachHolderIn(written, functor);
    }
    void forEachHolderOfWrittenPrototypeLink(NOESCAPE const Invocable<void(BuiltinID)> auto& functor) const { forEachHolderIn(m_ofPrototypeLinks, functor); }
    void forEachNameWritten(NOESCAPE const Invocable<void(BuiltinID, PropertyKeyID)> auto& functor) const
    {
        for (uint32_t holder = 0; holder < m_unknownObject; ++holder) {
            for (size_t key : m_keysWritten[holder])
                functor(BuiltinID(holder), PropertyKeyID(static_cast<uint32_t>(key)));
        }
    }

    Vector<BuiltinID> holdersIn(ValueSetMaker&, ValueSetID) const;

private:
    // A holder by the index of its canonical entry. The two objects that stand for others come behind the last entry.
    using Receiver = uint32_t;
    using List = Vector<Write>;
    using Lists = Vector<std::span<const Write>, 8>;
    struct Notes;
    struct Account {
        Lists seen;
        OptionSet<Actor> open;
    };

    void forEachHolderIn(const BitVector& receivers, NOESCAPE const Invocable<void(BuiltinID)> auto& functor) const
    {
        for (size_t receiver : receivers) {
            if (receiver < m_unknownObject)
                functor(BuiltinID(static_cast<uint32_t>(receiver)));
        }
    }

    bool writesUnderKeysNobodyKnowsThrough(Receiver placeholder) const { return m_underKeysNobodyKnows.get(placeholder) || m_underIndexes.get(placeholder); }
    bool mayWriteThrough(Receiver placeholder, PropertyKeyID key) const { return writesUnderKeysNobodyKnowsThrough(placeholder) || (key && m_keysWritten[placeholder].get(key.index())); }
    bool writesPropertiesThrough(Receiver placeholder) const { return writesUnderKeysNobodyKnowsThrough(placeholder) || !m_keysWritten[placeholder].isEmpty(); }

    HashSet<uint64_t> findNodesWrittenByCodeNobodySeesAlone() const;
    void note(Notes&, ValueSetMaker&, const HashSet<uint64_t>& writtenByCodeNobodySeesAlone, ValueFlowName, Write throughListed, const ValueFlowHeapIndex::WrittenNode&) const;
    void noteWrites(const ValueFlowHeapIndex&);
    void appendWritesUnder(Lists&, Receiver, PropertyKeyID) const;
    std::span<const Write> writesUnderIndexesAndKeysNobodyKnows(Receiver) const;
    std::span<const Write> writesUnderKeysNobodyKnows(Receiver) const;
    void appendWritesThrough(Lists&, Receiver placeholder, PropertyKeyID) const;
    OptionSet<Actor> codeNobodySeesThatWrites(BuiltinID holder) const;
    // No key: the elements.
    Account accountOfProperty(BuiltinID holder, PropertyKeyID) const;
    Account accountOfEveryOwnProperty(BuiltinID holder) const;
    Account accountOfPrototypeLink(BuiltinID holder) const;
    Writers keep(PlaceKind, const Account&) const;

    const ValueFlowResultInputs& m_inputs;
    const BuiltinTable& m_builtins;
    const FlowBuiltinTables& m_tables;
    const bool m_unseenCodeChangesRealm;
    const bool m_unseenCodeRuns;
    const bool m_nativeCodeWrites;
    const Receiver m_unknownObject;
    const Receiver m_untrackedObject;
    BitVector m_escaped;
    BitVector m_lost;
    BitVector m_mayBeHeldWithoutBeingListed;
    BitVector m_underKeysNobodyKnows;
    BitVector m_underIndexes;
    BitVector m_ofPrototypeLinks;
    BitVector m_writtenByCodeNobodySees;
    Vector<BitVector> m_keysWritten;
    // One write for each written node: statements that write through the same receivers under the same name share it, and nobody tells apart what each of them lets arrive.
    // Each list by receiver. The writes of properties: first those under names, whose keys stand beside them, then those under indexes, then those under keys nobody knows.
    Vector<Vector<uint32_t>> m_keysOfWritesUnderNames;
    Vector<List> m_writesOfProperties;
    HashMap<Receiver, uint32_t, IntHash<Receiver>, WTF::UnsignedWithZeroKeyHashTraits<Receiver>> m_numberOfWritesUnderIndexes;
    Vector<List> m_writesOfPrototypeLinks;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_propertiesOfOperands;
    // An account names a list that lives as long as the ledger.
    mutable Lock m_listsLock;
    mutable Vector<std::unique_ptr<List>> m_lists WTF_GUARDED_BY_LOCK(m_listsLock);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
