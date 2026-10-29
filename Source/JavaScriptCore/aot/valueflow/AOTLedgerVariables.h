/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLedger.h"
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

struct AccessesThroughHolders;
struct AccessesThroughHoldersDeleter {
    void operator()(AccessesThroughHolders*) const;
};

// Who writes and who reads each variable and each field of an environment, and who reaches each namespace object.
class LedgerOfVariables final : public LedgerPart {
    WTF_MAKE_NONCOPYABLE(LedgerOfVariables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LedgerOfVariables);
public:
    // What has to have been heard before an actor counts as having accounted for every place. An actor that owes a report is open everywhere.
    enum class Report : uint8_t {
        OperationsThatNamePlaces = 1 << 0,
        BodiesWithoutSolve = 1 << 1,
        ScopesOfEvaluatedText = 1 << 2,
        Modules = 1 << 3,
        HoldersOfVariables = 1 << 4,
        AccessesThroughHolders = 1 << 5,
    };

    static constexpr OptionSet<Report> reportsOwedBy(Actor actor)
    {
        switch (actor) {
        case Actor::Statements:
            return { Report::OperationsThatNamePlaces, Report::AccessesThroughHolders };
        case Actor::Creation:
            return { Report::OperationsThatNamePlaces };
        case Actor::Table:
        case Actor::KeysNobodyKnows:
        case Actor::ReceiversNobodyKnows:
            return { Report::HoldersOfVariables, Report::AccessesThroughHolders };
        case Actor::NativeCode:
            return { Report::HoldersOfVariables };
        case Actor::Loader:
            return { Report::Modules };
        case Actor::UnseenCode:
            return { Report::BodiesWithoutSolve, Report::ScopesOfEvaluatedText, Report::HoldersOfVariables };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    struct WriteAt {
        uint32_t location;
        Write write;
    };
    struct ReadAt {
        uint32_t location;
        Read read;
    };
    enum class ReadNoted : uint8_t {
        AsValueByOperationThatNamesPlace = 1 << 0,
        AsCalleeByOperationThatNamesPlace = 1 << 1,
        ByLoader = 1 << 2,
    };
    struct NamespaceObject {
        OptionSet<Actor> open { everyActor };
        bool isCreated { false };
        // In a body that is reached, whether or not the operation ever runs.
        uint8_t isNamedByOperation { false };
        std::optional<Actor> readerUnderNamesNobodyLists;
    };

    explicit LedgerOfVariables(const Program&);
    ~LedgerOfVariables();

    uint32_t numberOfLocations() const { return m_leftOpenToWriters.size(); }
    uint32_t of(VariableID variable) const { return variable.index(); }
    uint32_t of(LayoutID environment, uint32_t field) const { return m_firstFieldOfLayout[environment.index()] + field; }

    void didHear(Report report) { AtomicRef(m_heard).fetch_or(static_cast<uint8_t>(report), std::memory_order_relaxed); }
    void leaveOpenToWriter(uint32_t location, Actor actor) { AtomicRef(m_leftOpenToWriters[location]).fetch_or(static_cast<uint8_t>(actor), std::memory_order_relaxed); }
    void leaveOpenToReader(uint32_t location, Actor actor) { AtomicRef(m_leftOpenToReaders[location]).fetch_or(static_cast<uint8_t>(actor), std::memory_order_relaxed); }
    void leaveOpen(uint32_t location, OptionSet<Actor> actors)
    {
        AtomicRef(m_leftOpenToWriters[location]).fetch_or(actors.toRaw(), std::memory_order_relaxed);
        AtomicRef(m_leftOpenToReaders[location]).fetch_or(actors.toRaw(), std::memory_order_relaxed);
    }
    void note(uint32_t location, ReadNoted read) { AtomicRef(m_readsNoted[location]).fetch_or(static_cast<uint8_t>(read), std::memory_order_relaxed); }
    void addWritesThatNamePlaces(Vector<WriteAt>&& writes) { m_writesThatNamePlaces = WTF::move(writes); }
    void addThroughHolders(Vector<WriteAt>&& writes, Vector<ReadAt>&& reads)
    {
        m_writesThroughHolders = WTF::move(writes);
        m_readsThroughHolders = WTF::move(reads);
    }
    NamespaceObject& namespaceOf(ModuleID module) LIFETIME_BOUND { return m_namespaces[module.index()]; }
    void noteOperationNamingNamespaceOf(ModuleID module) { AtomicRef(m_namespaces[module.index()].isNamedByOperation).store(true, std::memory_order_relaxed); }
    bool isNamespaceNamedByOperation(ModuleID module) const { return m_namespaces[module.index()].isNamedByOperation; }
    void setClassObjectsStoredUnderStaticFieldsAlone(BitVector&& layouts) { m_classObjectsStoredUnderStaticFieldsAlone = WTF::move(layouts); }

    // Lists what was handed in by place. Nothing is added behind it.
    void close();

    Writers writersAt(uint32_t location) const { return { m_writes.subspan(m_firstWrite[location], m_firstWrite[location + 1] - m_firstWrite[location]), openAt(location, m_leftOpenToWriters, actorsThatCanWrite(kindOf(location))) }; }
    Readers readersAt(uint32_t location) const { return { m_reads.subspan(m_firstRead[location], m_firstRead[location + 1] - m_firstRead[location]), openAt(location, m_leftOpenToReaders, actorsThatCanRead(kindOf(location))) }; }
    Reachers reachersOfNamespace(ModuleID) const;
    Readers readersOfNamespaceUnderNamesNobodyLists(ModuleID) const;

    std::optional<Writers> writersOf(const Place&) const final;
    std::optional<Readers> readersOf(const Place&) const final;

    // Whether anything but its static fields was ever stored on the class object. It goes to the part of classes and closures.
    bool isClassObjectStoredUnderStaticFieldsAlone(LayoutID layout) const { return m_classObjectsStoredUnderStaticFieldsAlone.get(layout.index()); }

    std::unique_ptr<AccessesThroughHolders, AccessesThroughHoldersDeleter> accessesThroughHolders;

private:
    PlaceKind kindOf(uint32_t location) const { return location < m_numberOfVariables ? PlaceKind::Variable : PlaceKind::EnvironmentField; }
    OptionSet<Actor> actorsThatOweReports() const
    {
        OptionSet<Report> heard = OptionSet<Report>::fromRaw(m_heard);
        OptionSet<Actor> owing;
        for (Actor actor : actorsInOrder) {
            if (!heard.containsAll(reportsOwedBy(actor)))
                owing.add(actor);
        }
        return owing;
    }
    OptionSet<Actor> openAt(uint32_t location, const Vector<uint8_t>& leftOpen, OptionSet<Actor> able) const { return (OptionSet<Actor>::fromRaw(leftOpen[location]) | m_owing) & able; }

    uint32_t m_numberOfVariables;
    Vector<uint32_t> m_firstFieldOfLayout;
    uint8_t m_heard { 0 };
    OptionSet<Actor> m_owing { everyActor };
    Vector<uint8_t> m_leftOpenToWriters;
    Vector<uint8_t> m_leftOpenToReaders;
    Vector<uint8_t> m_readsNoted;
    Vector<WriteAt> m_writesThatNamePlaces;
    Vector<WriteAt> m_writesThroughHolders;
    Vector<ReadAt> m_readsThroughHolders;
    Vector<NamespaceObject> m_namespaces;
    BitVector m_classObjectsStoredUnderStaticFieldsAlone;

    Vector<Write> m_writes;
    Vector<uint32_t> m_firstWrite;
    Vector<Read> m_reads;
    Vector<uint32_t> m_firstRead;
};

void reportOperationsOnVariables(const ValueFlowResultInputs&, const FlowResultTables&, LedgerOfVariables&);
void collectAccessesThroughHolders(const ValueFlowResultInputs&, LedgerOfVariables&);
void reportAccessesThroughHolders(const ValueFlowResultInputs&, const ValueFlowUsesOfHeap&, LedgerOfVariables&);
void computeLocationResults(const ValueFlowResultInputs&, Ledger&, LedgerOfVariables&, FlowResultTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
