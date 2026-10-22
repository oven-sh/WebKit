/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinIDs.h"
#include "AOTDecidedBranches.h"
#include "AOTEffects.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEliminateRedundantReads.h"
#include "AOTForwardDataflow.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTMemoryLocation.h"
#include "AOTTypePropagation.h"
#include "AOTTypeRelations.h"
#include "AOTUseEffectHistory.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static const OpcodeSet& opcodesThatMayWriteAccessedHeaps()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (!effectFactsOf(opcode).staysWithinDeclaredBound || declaredBoundOf(opcode).writes.containsAny(accessedHeapSet))
                result.add(opcode);
        }
        return result;
    }();
    return opcodes;
}

// Everything else is no access of findAccess() and no allocation whose fields count as stored.
static const OpcodeSet& opcodesOfAccesses()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result { Opcode::NewObject, Opcode::CheckInitialized, Opcode::AssumeType };
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (effectRuleOf(opcode) != EffectRule::Declared)
                continue;
            Effects bound = declaredBoundOf(opcode);
            if (bound.reads.containsAny(accessedHeapSet) || bound.writes.containsAny(accessedHeapSet))
                result.add(opcode);
        }
        return result;
    }();
    return opcodes;
}

namespace {

struct IndexRange {
    uint32_t first { 0 };
    uint32_t end { 0 };

    bool isEmpty() const { return first >= end; }
    uint32_t size() const { return end - first; }
    bool contains(uint32_t index) const { return index >= first && index < end; }
    IndexRange clippedTo(IndexRange other) const { return { std::max(first, other.first), std::min(end, other.end) }; }
};

class RedundantReadElimination {
public:
    explicit RedundantReadElimination(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_cfg(context.analyses.cfg())
        , m_opcodesThatMayWrite(opcodesThatMayWriteAccessedHeaps())
        , m_opcodesOfAccesses(opcodesOfAccesses())
        , m_checksScreens(context.build.options().verifiesAfterEveryPass)
    {
    }

    PassOutcome run()
    {
        findAccesses();
        if (m_accesses.size() < 2)
            return PassOutcome::Unchanged;
        assignBits();
        if (m_classes.isEmpty())
            return PassOutcome::Unchanged;
        prepareToSolve();
        solveGroups();
        return applyReplacements();
    }

private:
    struct Access {
        MemoryLocation location;
        ValueID base { };
        ValueID id { };
        ValueID stored { };
        BlockID block { };
        uint32_t index { 0 };
        bool isFixed { false };
        bool isAbsent { false };
        bool mayBeDeferredClosure { false };
        bool isSettling { false };
        bool mayPrecedeStore { false };
        bool waitsForStability { false };
        std::optional<uint32_t> bit { };

        bool isRead() const { return !stored && !isAbsent; }
        bool isAtAllocation() const { return id == base; }

        ValueID valueAfter() const
        {
            ASSERT(!isAbsent);
            return stored ? stored : id;
        }
    };

    struct Bit {
        uint32_t access;
        uint32_t accessClass;
    };

    struct AccessClass {
        IndexRange bits;
        ValueID base;
        uint32_t location;
        // Found when it is first asked for.
        ObjectID allocation;
        bool hasLookedForAllocation;
    };

    struct LocationClasses {
        bool isFixed;
        MemoryLocation location;
        uint64_t mask;
        IndexRange classes;
    };

    struct Action {
        // MayThrow stands in front of the actions of a value under a handler: its range counts the clears that follow, which the handler sees carried out. MeetsHandler: the handler sees the row as it is.
        enum class Kind : uint8_t { Defines, Looks, Clears, MayThrow, MeetsHandler };

        Kind kind;
        IndexRange bits;
    };

    using ActionList = Vector<Action, 8>;
    using Word = uint64_t;
    using Row = Vector<Word, 4>;

    struct OperandWrites {
        ValueID operand;
        LocationMasks writes;
    };

    struct Writer {
        ValueID id;
        ProgramPoint after;
        const EffectsOnEdges& effects;
        LocationMasks writes;
        std::optional<LocationMasks> beyond;
        std::optional<Vector<OperandWrites, 4>> operandWrites;
    };

    struct Replacement {
        ValueID read;
        ValueID value;
        ProgramPoint point;
    };

    struct Merge {
        struct Incoming {
            Edge edge;
            ValueID value;
        };

        BlockID block;
        ValueID read;
        uint32_t index;
        Vector<Incoming, 4> incoming;
    };

    struct IncomingState {
        Edge edge;
        Row available;
    };

    // Of the group that is being solved, but for the accesses.
    struct StateOfBlock {
        IndexRange accesses;
        IndexRange actions;
        uint32_t endOfActionsOfValues { 0 };
        // The rows at the head, at the end and on the way to the handler.
        uint32_t rows { 0 };
        bool isVisited { false };
    };

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    const Access& accessOf(size_t bit) const { return m_accesses[m_bits[bit].access]; }
    IndexRange bitsOf(IndexRange classes) const { return { m_classes[classes.first].bits.first, m_classes[classes.end - 1].bits.end }; }
    std::span<const Action> actionsIn(IndexRange range) const { return m_actions.span().subspan(range.first, range.size()); }

    std::optional<bool> m_makesObjectsOnEdges;
    Vector<uint8_t> m_namesNoObjectMadeHere;

    const EscapeAnalysis& escapeAnalysis()
    {
        if (!m_escape)
            m_escape = &m_analyses.escape();
        return *m_escape;
    }

    bool isSettling(VariableID id) const
    {
        const Variable& variable = m_program.at(id);
        return variable.initialization == VariableInitialization::Uninitialized
            && variable.flow.stores == StoreCount::AtMostOnce
            && variable.flow.writers == WriterState::AllKnown;
    }

    bool isSettling(const MemoryLocation& location) const
    {
        return location.heap() == AbstractHeap::ModuleVariables && isSettling(location.variable());
    }

    std::optional<MemoryLocation> locationOfField(LayoutID layoutID, uint32_t fieldIndex) const
    {
        const Layout& layout = m_program.at(layoutID);
        if (layout.isEnvironment())
            return MemoryLocation::environmentField(layoutID, fieldIndex);
        const Field& field = layout.fields[fieldIndex];
        if (!field.key || field.flags.contains(FieldFlags::IsPrivate))
            return std::nullopt;
        return MemoryLocation::objectField(field.key);
    }

    void addAllocationAccesses(ValueID id, ProgramPoint point)
    {
        const Value& allocation = m_function.at(id);
        LayoutID layoutID = allocation.layout();
        const Layout& layout = m_program.at(layoutID);
        auto add = [&](uint32_t field, ValueID stored) {
            std::optional<MemoryLocation> location = locationOfField(layoutID, field);
            if (!location)
                return;
            m_accesses.append(Access {
                .location = *location,
                .base = id,
                .id = id,
                .stored = stored,
                .block = point.block,
                .index = point.index,
                .isAbsent = !stored,
                .mayBeDeferredClosure = flowOf(m_program, { layoutID, field }).location.deferral != ClosureDeferral::Unknown,
            });
        };

        std::span<const uint32_t> initialFields = allocation.fields();
        std::span<const ValueID> initialValues = allocation.variadicOperands();
        BitVector isInitial(layout.fields.size());
        for (size_t i = 0; i < initialFields.size(); ++i) {
            isInitial.quickSet(initialFields[i]);
            add(initialFields[i], initialValues[i]);
        }
        if (layout.isEnvironment())
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!isInitial.quickGet(field))
                add(field, ValueID());
        }
    }

    std::optional<Access> findSettlingCheck(ValueID id, ProgramPoint point) const
    {
        const Value& check = m_function.at(id);
        if (!leavesDeadZoneBehind(m_program, m_function, check))
            return std::nullopt;
        const Value& definition = m_function.at(m_analyses.definitionThroughUpcasts(check.valueOperand()));
        MemoryAccess read = memoryAccessOf(m_function, definition);
        if (read.kind != MemoryAccess::Kind::Read || !isSettling(read.location) || stabilityOf(m_program, definition, read) != LocationStability::Unknown)
            return std::nullopt;
        return Access {
            .location = read.location,
            .id = id,
            .stored = id,
            .block = point.block,
            .index = point.index,
            .isFixed = true,
            .isSettling = true,
        };
    }

    std::optional<Access> findAccess(ValueID id, ProgramPoint point) const
    {
        const Value& value = m_function.at(id);
        MemoryAccess access = memoryAccessOf(m_function, value);
        switch (access.kind) {
        case MemoryAccess::Kind::NotExact:
            return findSettlingCheck(id, point);
        case MemoryAccess::Kind::Read:
        case MemoryAccess::Kind::Write:
            break;
        }
        if (!accessedHeapSet.contains(access.location.heap()))
            return std::nullopt;
        RELEASE_ASSERT(access.kind != MemoryAccess::Kind::Read || opcodesReadingAccessedHeaps.contains(value.opcode));

        return Access {
            .location = access.location,
            .base = access.base ? m_analyses.sameObjectThroughCasts(access.base) : ValueID(),
            .id = id,
            .stored = access.stored,
            .block = point.block,
            .index = point.index,
            .waitsForStability = true,
        };
    }

    // Asked only of an access that shares its location and base with another one: the others get no bit.
    void findStability(Access& access) const
    {
        if (!std::exchange(access.waitsForStability, false))
            return;
        const Value& value = m_function.at(access.id);
        bool isStable = stabilityOf(m_program, value, memoryAccessOf(m_function, value)) != LocationStability::Unknown;
        bool settles = !isStable && isSettling(access.location);
        access.isFixed = isStable || settles;
        access.mayPrecedeStore = settles && access.isRead();
    }

    void findAccesses()
    {
        BitVector isReadBase(m_function.values.size());
        BitVector keptReads = m_context.build.validates() ? findReadsKeptForValidation(m_function) : BitVector();
        for (BlockID blockID : m_cfg.reversePostOrder() | std::views::reverse) {
            const Block& block = m_function.at(blockID);
            for (uint32_t index = static_cast<uint32_t>(block.values.size()); index--;) {
                ValueID id = block.values[index];
                if (!m_opcodesOfAccesses.contains(m_function.at(id).opcode)) {
                    if (m_checksScreens) [[unlikely]]
                        RELEASE_ASSERT(!findAccess(id, { blockID, index }));
                    continue;
                }
                ProgramPoint point { blockID, index };
                if (m_function.at(id).opcode == Opcode::NewObject) {
                    if (isReadBase.quickGet(id.index()))
                        addAllocationAccesses(id, point);
                    continue;
                }
                std::optional<Access> access = findAccess(id, point);
                if (!access || keptReads.get(id.index()))
                    continue;
                if (access->base && access->isRead())
                    isReadBase.quickSet(access->base.index());
                m_accesses.append(*access);
            }
        }
        m_accesses.reverse();
    }

    bool storesIntoDeferredClosureField(const Access& access) const
    {
        if (access.location.heap() != AbstractHeap::ObjectFields || !access.stored)
            return false;
        const Value& store = m_function.at(access.id);
        if (!Value::opcodesWithReceivers.contains(store.opcode) || !store.receivers())
            return true;
        const LayoutSet& receivers = m_program.at(store.receivers());
        return !receivers.isComplete || std::ranges::any_of(receivers.members, [&](LayoutID member) {
            std::optional<FieldReference> field = findField(m_program, member, access.location.key(), BaseChain::Proven);
            return !field || flowOf(m_program, *field).location.deferral != ClosureDeferral::Unknown;
        });
    }

    void forEachLocationAndBase(std::span<uint32_t> order, NOESCAPE const Invocable<void(std::span<uint32_t>)> auto& functor) const
    {
        for (size_t begin = 0; begin < order.size();) {
            const Access& first = m_accesses[order[begin]];
            size_t end = begin + 1;
            while (end < order.size() && m_accesses[order[end]].location == first.location && m_accesses[order[end]].base == first.base)
                ++end;
            functor(order.subspan(begin, end - begin));
            begin = end;
        }
    }

    void setStabilityAtAllocation(std::span<const uint32_t> members)
    {
        bool isFixed = std::ranges::all_of(members, [&](uint32_t index) {
            const Access& access = m_accesses[index];
            return access.isFixed || access.isAtAllocation();
        });
        for (uint32_t index : members) {
            Access& access = m_accesses[index];
            if (access.isAtAllocation())
                access.isFixed = isFixed;
        }
    }

    void addClasses(std::span<uint32_t> section)
    {
        forEachLocationAndBase(section, [&](std::span<uint32_t> members) {
            addClass(members);
        });
    }

    void assignBits()
    {
        Vector<uint32_t> order(m_accesses.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(order, { }, [&](uint32_t index) {
            const Access& access = m_accesses[index];
            return std::tuple { access.location, access.base, index };
        });
        size_t numberOfAccessesWithCompany = 0;
        forEachLocationAndBase(order.mutableSpan(), [&](std::span<uint32_t> members) {
            bool hasRead = std::ranges::any_of(members, [&](uint32_t index) {
                return m_accesses[index].isRead();
            });
            if (members.size() < 2 || !hasRead)
                return;
            for (uint32_t index : members)
                findStability(m_accesses[index]);
            setStabilityAtAllocation(members);
            for (uint32_t index : members)
                order[numberOfAccessesWithCompany++] = index;
        });
        order.shrink(numberOfAccessesWithCompany);
        // Two accesses through one base may name different receivers, so one location and base may have a class in each section.
        auto clobberableAccesses = std::ranges::stable_partition(order, [&](uint32_t index) {
            return m_accesses[index].isFixed;
        });
        size_t numberOfFixedAccesses = static_cast<size_t>(clobberableAccesses.begin() - order.begin());
        addClasses(order.mutableSpan().first(numberOfFixedAccesses));
        addClasses(order.mutableSpan().subspan(numberOfFixedAccesses));
    }

    void addClass(std::span<uint32_t> members)
    {
        size_t numberOfAccesses = 0;
        bool hasRead = false;
        bool hasSource = false;
        for (uint32_t index : members) {
            const Access& access = m_accesses[index];
            // No call clears the bits of a fixed class, and a call may be what fills the slot.
            if (access.isAbsent && access.isFixed)
                continue;
            hasRead |= access.isRead();
            hasSource |= !access.mayPrecedeStore;
            members[numberOfAccesses++] = index;
        }
        if (numberOfAccesses < 2 || !hasRead || !hasSource)
            return;

        uint32_t classIndex = static_cast<uint32_t>(m_classes.size());
        uint32_t firstBit = static_cast<uint32_t>(m_bits.size());
        for (uint32_t index : members.first(numberOfAccesses)) {
            Access& access = m_accesses[index];
            access.bit = static_cast<uint32_t>(m_bits.size());
            if (!access.isAtAllocation())
                access.mayBeDeferredClosure = storesIntoDeferredClosureField(access);
            m_bits.append(Bit { index, classIndex });
        }

        const Access& first = m_accesses[members.front()];
        if (m_locations.isEmpty() || m_locations.last().isFixed != first.isFixed || m_locations.last().location != first.location)
            m_locations.append(LocationClasses { first.isFixed, first.location, maskOf(m_program, first.location), { classIndex, classIndex } });
        m_locations.last().classes.end = classIndex + 1;
        uint32_t locationIndex = static_cast<uint32_t>(m_locations.size() - 1);
        m_classes.append(AccessClass { { firstBit, static_cast<uint32_t>(m_bits.size()) }, first.base, locationIndex, ObjectID(), false });
        if (first.isFixed)
            m_firstClobberableBit = m_bits.size();
    }

    IndexRange classesAt(bool isFixed, const MemoryLocation& location) const
    {
        auto found = std::ranges::lower_bound(m_locations, std::pair { !isFixed, location }, { }, [](const LocationClasses& entry) {
            return std::pair { !entry.isFixed, entry.location };
        });
        if (found == m_locations.end() || found->isFixed != isFixed || found->location != location)
            return { };
        return found->classes;
    }

    bool makesObjectsOnEdges()
    {
        if (!m_makesObjectsOnEdges) {
            m_makesObjectsOnEdges = std::ranges::any_of(m_cfg.reversePostOrder(), [&](BlockID block) {
                return freshnessOf(m_analyses.effectContext(), m_function, m_function.at(block).terminator) != ResultFreshness::Unknown;
            });
        }
        return *m_makesObjectsOnEdges;
    }

    bool mayDefineAllocation(ValueID base)
    {
        const Value& value = m_function.at(base);
        if (value.opcode == Opcode::Parameter)
            return makesObjectsOnEdges();
        return resultRuleOf(value.opcode) == ResultRule::Allocation || freshnessOf(m_analyses.effectContext(), m_function, value) != ResultFreshness::Unknown;
    }

    ObjectID trackedAllocationOf(ValueID base)
    {
        if (!mayDefineAllocation(base))
            return { };
        const EscapeAnalysis& escape = escapeAnalysis();
        if (escape.gaveUp())
            return { };
        std::optional<ObjectID> object = escape.objectDefinedBy(base);
        if (!object)
            return { };
        const TrackedObject& tracked = escape.at(*object);
        if (tracked.kind != ObjectKind::Allocation || tracked.state == EscapeState::Unknown)
            return { };
        return *object;
    }

    ObjectID allocationOf(AccessClass& accessClass)
    {
        if (!std::exchange(accessClass.hasLookedForAllocation, true))
            accessClass.allocation = trackedAllocationOf(accessClass.base);
        return accessClass.allocation;
    }

    bool isUncapturedBefore(ObjectID object, ProgramPoint point)
    {
        if (!m_captureHistory)
            m_captureHistory.emplace(m_analyses, UseEffectHistoryOptions { .uses = { OperandUse::Retains } });
        return m_captureHistory->occurredBefore(object, point) == Occurrence::HasNotOccurred;
    }

    ObjectID uncapturedAllocationOf(ValueID base, ProgramPoint point)
    {
        ObjectID allocation = trackedAllocationOf(base);
        if (!allocation || !isUncapturedBefore(allocation, point))
            return { };
        return allocation;
    }

    // What EscapeAnalysis::mayAlias() says, seen without the solve. Behind most accesses stands no object that the function makes:
    // two such values may name one object wherever their types allow it.
    enum class AliasSeenAlone : uint8_t { Unknown, NoAlias, MayAlias };

    bool namesNoObjectMadeHere(ValueID value)
    {
        enum : uint8_t { NotLookedAt, Yes, No };
        if (m_namesNoObjectMadeHere.isEmpty())
            m_namesNoObjectMadeHere.fill(NotLookedAt, m_function.values.size());
        uint8_t& kept = m_namesNoObjectMadeHere[value.index()];
        if (kept == NotLookedAt) {
            std::optional<Vector<ValueID, 8>> makers = findMakersOfObjectsBehind(m_analyses, value);
            kept = makers && makers->isEmpty() ? Yes : No;
        }
        return kept == Yes;
    }

    AliasSeenAlone aliasSeenAlone(ValueID a, ValueID b)
    {
        if (m_analyses.sameObjectThroughCasts(a) == m_analyses.sameObjectThroughCasts(b))
            return AliasSeenAlone::MayAlias;
        if (!mayBeSameObject(typeOf(a), typeOf(b), m_program.types))
            return AliasSeenAlone::NoAlias;
        return namesNoObjectMadeHere(a) && namesNoObjectMadeHere(b) ? AliasSeenAlone::MayAlias : AliasSeenAlone::Unknown;
    }

    bool areDistinctObjects(AccessClass& accessClass, ValueID storeBase, ObjectID uncapturedStoreAllocation, ProgramPoint afterStore)
    {
        if (!uncapturedStoreAllocation && !mayDefineAllocation(accessClass.base)) {
            AliasSeenAlone seen = aliasSeenAlone(accessClass.base, storeBase);
            if (m_checksScreens && seen != AliasSeenAlone::Unknown && !escapeAnalysis().gaveUp()) [[unlikely]]
                RELEASE_ASSERT(escapeAnalysis().mayAlias(accessClass.base, storeBase) == (seen == AliasSeenAlone::MayAlias));
            if (seen != AliasSeenAlone::Unknown)
                return seen == AliasSeenAlone::NoAlias;
        }
        const EscapeAnalysis& escape = escapeAnalysis();
        if (escape.gaveUp())
            return false;
        auto mayBe = [&](ValueID value, ObjectID object) {
            return escape.pointsTo(value).objects.contains(object);
        };
        if (uncapturedStoreAllocation && !mayBe(accessClass.base, uncapturedStoreAllocation))
            return true;
        ObjectID allocation = allocationOf(accessClass);
        if (allocation && !mayBe(storeBase, allocation) && isUncapturedBefore(allocation, afterStore))
            return true;
        return !escape.mayAlias(accessClass.base, storeBase);
    }

    std::span<const OperandWrites> operandWritesOf(Writer& writer)
    {
        if (writer.operandWrites)
            return writer.operandWrites->span();

        writer.operandWrites.emplace();
        const Value& value = m_function.at(writer.id);
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!mayBeReference(typeOf(operand)))
                return;
            LocationMasks writes = memoryAccessedThrough(m_analyses.effectContext(), m_function, value, operandIndex, writer.effects).writes;
            if (writes.containsAny(accessedHeapSet))
                writer.operandWrites->append(OperandWrites { operand, writes });
        });
        return writer.operandWrites->span();
    }

    bool isUnaffected(AccessClass& accessClass, Writer& writer)
    {
        if (!accessClass.base)
            return false;
        const LocationClasses& entry = m_locations[accessClass.location];
        AbstractHeap heap = entry.location.heap();
        if (!writer.beyond)
            writer.beyond = effectsBeyondOperands(m_analyses.effectContext(), m_function, m_function.at(writer.id), writer.effects).onAnyEdge().writes;
        bool isWrittenBeyondOperands = writer.beyond->overlaps(heap, entry.mask);
        if (isWrittenBeyondOperands && !mayDefineAllocation(accessClass.base))
            return false;
        if (!isWrittenBeyondOperands && !mayDefineAllocation(accessClass.base)) {
            bool isSettled = true;
            for (const OperandWrites& written : operandWritesOf(writer)) {
                if (!written.writes.overlaps(heap, entry.mask))
                    continue;
                AliasSeenAlone seen = aliasSeenAlone(written.operand, accessClass.base);
                if (m_checksScreens && seen != AliasSeenAlone::Unknown && !escapeAnalysis().gaveUp()) [[unlikely]]
                    RELEASE_ASSERT(escapeAnalysis().mayAlias(written.operand, accessClass.base) == (seen == AliasSeenAlone::MayAlias));
                if (seen == AliasSeenAlone::MayAlias)
                    return false;
                isSettled &= seen == AliasSeenAlone::NoAlias;
            }
            if (isSettled)
                return true;
        }
        if ((isWrittenBeyondOperands && !allocationOf(accessClass)) || escapeAnalysis().gaveUp())
            return false;
        if (isWrittenBeyondOperands && !isUncapturedBefore(accessClass.allocation, writer.after))
            return false;
        return std::ranges::none_of(operandWritesOf(writer), [&](const OperandWrites& written) {
            if (!written.writes.overlaps(heap, entry.mask) || !escapeAnalysis().mayAlias(written.operand, accessClass.base))
                return false;
            ObjectID allocation = allocationOf(accessClass);
            bool mayBeAllocation = !allocation || escapeAnalysis().pointsTo(written.operand).objects.contains(allocation);
            return mayBeAllocation || !isUncapturedBefore(allocation, writer.after);
        });
    }

    // Rows hold the bits of the group that is being solved.
    static bool isEmpty(std::span<const Word> row) { return std::ranges::all_of(row, [](Word word) { return !word; }); }
    static bool get(std::span<const Word> row, size_t bit) { return row[bit / 64] >> bit % 64 & 1; }
    static void set(std::span<Word> row, size_t bit) { row[bit / 64] |= Word { 1 } << bit % 64; }

    static size_t findBit(std::span<const Word> row, size_t from)
    {
        size_t wordIndex = from / 64;
        if (wordIndex >= row.size())
            return row.size() * 64;
        Word word = row[wordIndex] & ~Word { 0 } << from % 64;
        while (!word) {
            if (++wordIndex == row.size())
                return row.size() * 64;
            word = row[wordIndex];
        }
        return wordIndex * 64 + std::countr_zero(word);
    }

    static void clear(std::span<Word> row, size_t first, size_t end)
    {
        if (first >= end)
            return;
        size_t firstWord = first / 64;
        size_t lastWord = (end - 1) / 64;
        Word fromFirst = ~Word { 0 } << first % 64;
        Word upToEnd = ~Word { 0 } >> (63 - (end - 1) % 64);
        if (firstWord == lastWord) {
            row[firstWord] &= ~(fromFirst & upToEnd);
            return;
        }
        row[firstWord] &= ~fromFirst;
        for (size_t word = firstWord + 1; word < lastWord; ++word)
            row[word] = 0;
        row[lastWord] &= ~upToEnd;
    }

    void forEachClassWithBitIn(std::span<const Word> row, IndexRange bits, NOESCAPE const Invocable<void(AccessClass&)> auto& functor)
    {
        bits = bits.clippedTo(m_group);
        if (bits.isEmpty())
            return;
        size_t end = bits.end - m_group.first;
        for (size_t bit = findBit(row, bits.first - m_group.first); bit < end;) {
            AccessClass& accessClass = m_classes[m_bits[m_group.first + bit].accessClass];
            functor(accessClass);
            bit = findBit(row, accessClass.bits.end - m_group.first);
        }
    }

    void carryOut(const Action& action, std::span<Word> row) const
    {
        IndexRange bits = action.bits.clippedTo(m_group);
        if (bits.isEmpty())
            return;
        switch (action.kind) {
        case Action::Kind::Defines:
            set(row, bits.first - m_group.first);
            return;
        case Action::Kind::Clears:
            clear(row, bits.first - m_group.first, bits.end - m_group.first);
            return;
        case Action::Kind::Looks:
        case Action::Kind::MayThrow:
        case Action::Kind::MeetsHandler:
            return;
        }
    }

    static void appendClear(ActionList& actions, IndexRange bits)
    {
        if (!actions.isEmpty() && actions.last().kind == Action::Kind::Clears && actions.last().bits.end == bits.first) {
            actions.last().bits.end = bits.end;
            return;
        }
        actions.append(Action { Action::Kind::Clears, bits });
    }

    // What a value or a terminator clears is asked only of the classes with a bit in the row: nothing else is there to be cleared.
    void appendClearsOfStore(const Access& store, std::span<const Word> row, ActionList& actions)
    {
        ProgramPoint afterStore { store.block, store.index + 1 };
        std::optional<ObjectID> allocation;
        for (IndexRange classes : { classesAt(true, store.location), classesAt(false, store.location) }) {
            if (classes.isEmpty())
                continue;
            forEachClassWithBitIn(row, bitsOf(classes), [&](AccessClass& accessClass) {
                if (store.base && accessClass.base != store.base) {
                    if (!allocation)
                        allocation = uncapturedAllocationOf(store.base, afterStore);
                    if (areDistinctObjects(accessClass, store.base, *allocation, afterStore))
                        return;
                }
                appendClear(actions, accessClass.bits);
            });
        }
    }

    void appendClearsOfWrites(const LocationMasks& writes, std::span<const Word> row, ActionList& actions, NOESCAPE const Invocable<bool(AccessClass&)> auto& survives)
    {
        forEachClassWithBitIn(row, { m_firstClobberableBit, static_cast<uint32_t>(m_bits.size()) }, [&](AccessClass& accessClass) {
            const LocationClasses& entry = m_locations[accessClass.location];
            if (writes.overlaps(entry.location.heap(), entry.mask) && !survives(accessClass))
                appendClear(actions, accessClass.bits);
        });
    }

    void appendClearsOfEffects(ProgramPoint point, std::span<const Word> row, ActionList& actions)
    {
        ValueID id = m_function.at(point.block).values[point.index];
        if (!m_opcodesThatMayWrite.contains(m_function.at(id).opcode)) {
            if (m_checksScreens) [[unlikely]]
                RELEASE_ASSERT(!m_analyses.effectsOf(id).onAnyEdge().writes.containsAny(accessedHeapSet));
            return;
        }
        const EffectsOnEdges& effects = m_analyses.effectsOf(id);
        if (!effects.normal.writes.containsAny(accessedHeapSet) && !effects.handler.writes.containsAny(accessedHeapSet))
            return;

        Writer writer {
            .id = id,
            .after = { point.block, point.index + 1 },
            .effects = effects,
            .writes = effects.onAnyEdge().writes,
            .beyond = std::nullopt,
            .operandWrites = std::nullopt,
        };
        appendClearsOfWrites(writer.writes, row, actions, [&](AccessClass& accessClass) {
            return isUnaffected(accessClass, writer);
        });
    }

    void appendClearsOfTerminator(BlockID block, std::span<const Word> row, ActionList& actions)
    {
        const EffectsOnEdges& effects = m_analyses.effectsOfTerminator(block);
        if (!effects.normal.writes.containsAny(accessedHeapSet) && !effects.handler.writes.containsAny(accessedHeapSet))
            return;
        appendClearsOfWrites(effects.onAnyEdge().writes, row, actions, [](AccessClass&) {
            return false;
        });
    }

    // Goes through the block with the rows and hands each action to the functor in front of carrying it out on all of them. Writers are held against the classes that have a bit in one of the rows.
    // With one row that is empty the values up to the next access are passed over.
    void walk(BlockID blockID, std::span<Row*> rows, bool watchesThrows, NOESCAPE const Invocable<void(const Action&)> auto& functor)
    {
        const Block& block = m_function.at(blockID);
        std::span<const Access> accesses = m_accesses.span().subspan(m_states[blockID.index()].accesses.first, m_states[blockID.index()].accesses.size());
        Row unionOfRows(FillWith { }, m_numberOfWords, Word { 0 });
        auto liveBits = [&]() -> std::span<const Word> {
            if (rows.size() == 1)
                return rows[0]->span();
            std::ranges::fill(unionOfRows, 0);
            for (Row* row : rows) {
                for (uint32_t word = 0; word < m_numberOfWords; ++word)
                    unionOfRows[word] |= (*row)[word];
            }
            return unionOfRows.span();
        };
        ActionList actions;
        auto finish = [&](bool mayThrow) {
            if (mayThrow) {
                uint32_t numberOfClears = std::ranges::count(actions, Action::Kind::Clears, &Action::kind);
                functor(Action { Action::Kind::MayThrow, { 0, numberOfClears } });
            }
            for (const Action& action : actions) {
                functor(action);
                for (Row* row : rows)
                    carryOut(action, row->mutableSpan());
            }
            actions.shrink(0);
        };

        uint32_t numberOfValues = block.values.size();
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            std::span<const Word> live = liveBits();
            if (isEmpty(live) && !watchesThrows) {
                if (accesses.empty())
                    break;
                index = accesses.front().index;
            }
            ValueID id = block.values[index];
            size_t numberOfAccesses = 0;
            while (numberOfAccesses < accesses.size() && accesses[numberOfAccesses].id == id)
                ++numberOfAccesses;
            std::span<const Access> accessesOfValue = accesses.first(numberOfAccesses);
            accesses = accesses.subspan(numberOfAccesses);

            if (accessesOfValue.empty())
                appendClearsOfEffects({ blockID, index }, live, actions);
            else if (const Access& access = accessesOfValue.front(); access.stored && !access.isAtAllocation() && !access.isSettling)
                appendClearsOfStore(access, live, actions);
            for (const Access& access : accessesOfValue) {
                if (access.bit && m_group.contains(*access.bit))
                    actions.append(Action { access.mayPrecedeStore ? Action::Kind::Looks : Action::Kind::Defines, { *access.bit, *access.bit + 1 } });
            }
            finish(watchesThrows && m_analyses.effectsOf(id).mayThrow());
        }
    }

    void walkTerminator(BlockID block, Row& row, bool watchesThrows, NOESCAPE const Invocable<void(const Action&)> auto& functor)
    {
        bool mayThrow = watchesThrows && m_analyses.effectsOfTerminator(block).mayThrow();
        if (mayThrow)
            functor(Action { Action::Kind::MeetsHandler, { } });
        if (!isEmpty(row.span())) {
            ActionList actions;
            appendClearsOfTerminator(block, row.span(), actions);
            for (const Action& action : actions) {
                functor(action);
                carryOut(action, row.mutableSpan());
            }
        }
        if (mayThrow)
            functor(Action { Action::Kind::MeetsHandler, { } });
    }

    std::span<Word> headOf(const StateOfBlock& state) { return m_rows.mutableSpan().subspan(state.rows, m_numberOfWords); }
    std::span<Word> tailOf(const StateOfBlock& state) { return m_rows.mutableSpan().subspan(state.rows + m_numberOfWords, m_numberOfWords); }
    std::span<Word> rowForHandlerOf(const StateOfBlock& state) { return m_rows.mutableSpan().subspan(state.rows + 2 * m_numberOfWords, m_numberOfWords); }

    void meet(std::span<Word> into, std::span<const Word> row) const
    {
        for (uint32_t word = 0; word < m_numberOfWords; ++word)
            into[word] &= row[word];
    }

    // The actions of a block are made when it is first visited, for the row it is entered with then. Rows only lose bits from visit to visit, so the same actions serve every later row.
    void carryThrough(BlockID block, StateOfBlock& state, bool isFirstVisit, Row& row, Row& rowForHandler)
    {
        bool hasHandler = !!m_function.at(block).handler;
        // A handler that is entered with nothing stays so whatever throws.
        bool watchesThrows = hasHandler && !isEmpty(rowForHandler.span());
        auto meetHandler = [&](uint32_t actionIndex) {
            const Action& action = m_actions[actionIndex];
            if (action.kind == Action::Kind::MeetsHandler) {
                meet(rowForHandler.mutableSpan(), row.span());
                return;
            }
            if (action.kind != Action::Kind::MayThrow)
                return;
            Row onThrow = row;
            for (uint32_t clearIndex = 0; clearIndex < action.bits.end; ++clearIndex)
                carryOut(m_actions[actionIndex + 1 + clearIndex], onThrow.mutableSpan());
            meet(rowForHandler.mutableSpan(), onThrow.span());
        };

        if (!isFirstVisit) {
            for (uint32_t index = state.actions.first; index < state.actions.end; ++index) {
                meetHandler(index);
                carryOut(m_actions[index], row.mutableSpan());
            }
            return;
        }

        // The handler is met behind the walk: the clears of a value that may throw follow its mark.
        Row rowAtHead = row;
        state.actions.first = m_actions.size();
        auto record = [&](const Action& action) {
            m_actions.append(action);
        };
        Row* rows[] { &row };
        walk(block, rows, watchesThrows, record);
        state.endOfActionsOfValues = m_actions.size();
        walkTerminator(block, row, watchesThrows, record);
        state.actions.end = m_actions.size();
        if (!watchesThrows)
            return;
        Row replayed = WTF::move(rowAtHead);
        std::swap(replayed, row);
        for (uint32_t index = state.actions.first; index < state.actions.end; ++index) {
            meetHandler(index);
            carryOut(m_actions[index], row.mutableSpan());
        }
        RELEASE_ASSERT(row == replayed);
    }

    // A bit of a class is set at an access of the class and looked at in front of one, so behind the last access that a block may reach the bits of the class say nothing.
    // A way from a block to a position in front of it first goes back over an edge that spans the block, and from there on over edges that each span the target of the one before.
    // So the stretches of positions that such edges span, taken together wherever they overlap, bound what a block reaches.
    void findWhereClassesMatter()
    {
        m_lastPositionOfClass.fill(0, m_classes.size());
        for (const Bit& bit : m_bits) {
            uint32_t& last = m_lastPositionOfClass[bit.accessClass];
            last = std::max(last, m_cfg.reversePostOrderIndex(m_accesses[bit.access].block));
        }
        if (!m_cfg.hasCycle())
            return;

        std::span<const BlockID> order = m_cfg.reversePostOrder();
        m_firstPositionReachedFrom = Vector<uint32_t>(order.size(), [](size_t position) {
            return static_cast<uint32_t>(position);
        });
        for (uint32_t position = 0; position < order.size(); ++position) {
            for (BlockID successor : m_cfg.successors(order[position]))
                m_firstPositionReachedFrom[position] = std::min(m_firstPositionReachedFrom[position], m_cfg.reversePostOrderIndex(successor));
        }
        uint32_t firstTarget = std::numeric_limits<uint32_t>::max();
        for (uint32_t position = order.size(); position--;) {
            firstTarget = std::min(firstTarget, m_firstPositionReachedFrom[position]);
            m_firstPositionReachedFrom[position] = std::min(firstTarget, position);
        }
        for (uint32_t position = 0; position < order.size(); ++position)
            m_firstPositionReachedFrom[position] = m_firstPositionReachedFrom[m_firstPositionReachedFrom[position]];
    }

    void dropClassesThatNoLongerMatter(Row& row, uint32_t position)
    {
        uint32_t firstPositionReached = m_firstPositionReachedFrom.isEmpty() ? position : m_firstPositionReachedFrom[position];
        forEachClassWithBitIn(row.span(), m_group, [&](AccessClass& accessClass) {
            if (m_lastPositionOfClass[&accessClass - m_classes.span().data()] < firstPositionReached)
                clear(row.mutableSpan(), accessClass.bits.first - m_group.first, accessClass.bits.end - m_group.first);
        });
    }

    void prepareToSolve()
    {
        findWhereClassesMatter();
        m_states.grow(m_function.blocks.size());
        for (uint32_t index = 0; index < m_accesses.size(); ++index) {
            IndexRange& accesses = m_states[m_accesses[index].block.index()].accesses;
            if (accesses.isEmpty())
                accesses.first = index;
            accesses.end = index + 1;
        }
        m_isPending.ensureSize(m_cfg.reversePostOrder().size());
    }

    void markAsPending(BlockID block) { m_isPending.quickSet(m_cfg.reversePostOrderIndex(block)); }

    // A block that was not visited has empty rows. In the first sweep a block that comes later may still be visited, so an edge back from it is left out until the sweep is over.
    void meetAtHeadOf(BlockID block, uint32_t position, Row& head)
    {
        std::ranges::fill(head, 0);
        if (block == m_cfg.root())
            return;

        bool isFirstEdge = true;
        bool waitsForEdgeBack = false;
        for (Edge edge : m_cfg.incomingEdges(block)) {
            const StateOfBlock& source = m_states[edge.from.index()];
            if (!source.isVisited) {
                if (m_isFirstSweep && m_cfg.reversePostOrderIndex(edge.from) >= position) {
                    waitsForEdgeBack = true;
                    continue;
                }
                std::ranges::fill(head, 0);
                return;
            }
            std::span<const Word> row = edge.isHandlerEdge() ? rowForHandlerOf(source) : tailOf(source);
            if (std::exchange(isFirstEdge, false))
                std::ranges::copy(row, head.begin());
            else
                meet(head.mutableSpan(), row);
        }
        if (waitsForEdgeBack)
            m_blocksThatWaitForEdgesBack.append(block);
        if (isFirstEdge && waitsForEdgeBack) {
            for (size_t bit = 0; bit < m_group.size(); ++bit)
                set(head.mutableSpan(), bit);
        }
    }

    void visit(BlockID block, uint32_t position)
    {
        StateOfBlock& state = m_states[block.index()];
        Row row(FillWith { }, m_numberOfWords, Word { 0 });
        meetAtHeadOf(block, position, row);

        bool isFirstVisit = !state.isVisited;
        // Every block but the entry is entered over an edge forward, so a block that the first sweep passed over is entered with nothing.
        RELEASE_ASSERT(!isFirstVisit || m_isFirstSweep || isEmpty(row.span()));
        if (isFirstVisit) {
            state.isVisited = true;
            state.rows = m_rows.size();
            m_rows.grow(m_rows.size() + 3 * m_numberOfWords);
            m_visitedBlocks.append(block);
        } else if (std::ranges::equal(row, headOf(state)))
            return;
        std::ranges::copy(row, headOf(state).begin());

        dropClassesThatNoLongerMatter(row, position);
        Row rowForHandler = row;
        carryThrough(block, state, isFirstVisit, row, rowForHandler);
        if (!isFirstVisit && std::ranges::equal(row, tailOf(state)) && std::ranges::equal(rowForHandler, rowForHandlerOf(state)))
            return;
        std::ranges::copy(row, tailOf(state).begin());
        std::ranges::copy(rowForHandler, rowForHandlerOf(state).begin());

        bool hasBitsAtEnd = !isEmpty(row.span());
        bool hasBitsForHandler = !isEmpty(rowForHandler.span());
        forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
            if ((edge.isHandlerEdge() ? hasBitsForHandler : hasBitsAtEnd) || m_states[target.block.index()].isVisited)
                markAsPending(target.block);
        });
    }

    void solve(IndexRange group)
    {
        for (BlockID block : m_visitedBlocks)
            m_states[block.index()].isVisited = false;
        m_visitedBlocks.shrink(0);
        m_blocksThatWaitForEdgesBack.shrink(0);
        m_actions.shrink(0);
        m_rows.shrink(0);
        m_group = group;
        m_numberOfWords = (group.size() + 63) / 64;

        for (uint32_t bit = group.first; bit < group.end; ++bit)
            markAsPending(accessOf(bit).block);

        std::span<const BlockID> order = m_cfg.reversePostOrder();
        m_isFirstSweep = true;
        while (!m_isPending.isEmpty()) {
            for (size_t position = m_isPending.findBit(0, true); position < order.size(); position = m_isPending.findBit(position + 1, true)) {
                m_isPending.quickClear(position);
                visit(order[position], position);
            }
            if (std::exchange(m_isFirstSweep, false)) {
                for (BlockID block : m_blocksThatWaitForEdgesBack)
                    markAsPending(block);
            }
        }
    }

    void solveGroups()
    {
        uint64_t maxBitsPerGroup = m_context.limit(LimitID::MaxReadsPerSolve);
        bool hasSeveralGroups = false;
        IndexRange group;
        for (const AccessClass& accessClass : m_classes) {
            if (!group.isEmpty() && accessClass.bits.end - group.first > maxBitsPerGroup) {
                solve(group);
                findReplacements();
                group.first = accessClass.bits.first;
                hasSeveralGroups = true;
            }
            group.end = accessClass.bits.end;
        }
        solve(group);
        findReplacements();
        if (hasSeveralGroups)
            m_context.gaveUp(LimitID::MaxReadsPerSolve);
    }

    bool canForward(const Access& source, const Type& readType) const
    {
        if (source.mayBeDeferredClosure)
            return false;
        return typeOf(source.valueAfter()).isSubsetOf(readType, m_program);
    }

    std::optional<uint32_t> findAvailableAccess(uint32_t readBit, std::span<const Word> available, NOESCAPE const Invocable<bool(const Access&)> auto& isSuitable) const
    {
        IndexRange bits = m_classes[m_bits[readBit].accessClass].bits;
        for (size_t bit = m_group.first + findBit(available, bits.first - m_group.first); bit < bits.end; bit = m_group.first + findBit(available, bit + 1 - m_group.first)) {
            if (bit != readBit && isSuitable(accessOf(bit)))
                return static_cast<uint32_t>(bit);
        }
        return std::nullopt;
    }

    std::optional<Replacement> findReplacement(uint32_t bit, std::span<const Word> available) const
    {
        const Access& read = accessOf(bit);
        if (!read.isRead())
            return std::nullopt;
        const Type& readType = typeOf(read.id);
        bool readsAbsentAsUndefined = m_function.at(read.id).opcode == Opcode::GetSlotOrUndefined && (readType.primitives & Primitive::Undefined);
        std::optional<uint32_t> source = findAvailableAccess(bit, available, [&](const Access& access) {
            return access.isAbsent ? readsAbsentAsUndefined : canForward(access, readType);
        });
        if (!source)
            return std::nullopt;
        const Access& access = accessOf(*source);
        return Replacement { read.id, access.isAbsent ? ValueID() : access.valueAfter(), { read.block, read.index } };
    }

    bool canMergeInto(BlockID block, ValueID read)
    {
        std::span<const Edge> edges = m_cfg.incomingEdges(block);
        if (edges.size() < 2 || block == m_cfg.root() || m_function.at(block).isHandlerBlock() || m_analyses.isEnteredFromUnreachableBlock(block))
            return false;
        bool canAppendArguments = std::ranges::all_of(edges, [&](Edge edge) {
            if (edge.isHandlerEdge() || hasEdgeResults(m_function, edge))
                return false;
            TerminatorKind kind = m_function.at(edge.from).terminator.kind;
            return kind == TerminatorKind::Jump || kind == TerminatorKind::Branch;
        });
        return canAppendArguments && !m_context.exceeds(LimitID::MaxMergedReadPredecessors, edges.size(), read);
    }

    bool isMergeableRead(uint32_t bit) const
    {
        const Access& access = accessOf(bit);
        return access.isRead() && typeOf(access.id).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    }

    void tryMerge(uint32_t bit, std::span<const IncomingState> incomingStates)
    {
        const Access& read = accessOf(bit);
        const Type& readType = typeOf(read.id);
        Merge merge { read.block, read.id, read.index, { } };
        for (const IncomingState& state : incomingStates) {
            std::optional<uint32_t> source = findAvailableAccess(bit, state.available.span(), [&](const Access& access) {
                return !access.isAbsent && canForward(access, readType);
            });
            if (!source)
                return;
            merge.incoming.append(Merge::Incoming { state.edge, accessOf(*source).valueAfter() });
        }
        m_merges.append(WTF::move(merge));
    }

    // What is available on one way into the block need not be so at its head, and the actions of the block were made for the row at its head. So the block is gone through anew.
    void findMerges(BlockID block, std::span<const uint32_t> bitsOfReads)
    {
        if (!canMergeInto(block, accessOf(bitsOfReads.front()).id))
            return;
        Vector<IncomingState, 4> states;
        for (Edge edge : m_cfg.incomingEdges(block)) {
            const StateOfBlock& source = m_states[edge.from.index()];
            Row available(FillWith { }, m_numberOfWords, Word { 0 });
            if (source.isVisited)
                std::ranges::copy(tailOf(source), available.begin());
            states.append(IncomingState { edge, WTF::move(available) });
        }
        Vector<Row*, 4> rows;
        for (IncomingState& state : states)
            rows.append(&state.available);
        walk(block, rows.mutableSpan(), false, [&](const Action& action) {
            if (!bitsOfReads.empty() && (action.kind == Action::Kind::Defines || action.kind == Action::Kind::Looks) && action.bits.first == bitsOfReads.front()) {
                tryMerge(action.bits.first, states.span());
                bitsOfReads = bitsOfReads.subspan(1);
            }
        });
    }

    void findReplacements()
    {
        Vector<BlockID, 16> blocks;
        for (BlockID block : m_visitedBlocks) {
            if (!m_states[block.index()].accesses.isEmpty())
                blocks.append(block);
        }
        std::ranges::sort(blocks, { }, [&](BlockID block) {
            return m_cfg.reversePostOrderIndex(block);
        });

        Vector<uint32_t, 8> readsToMerge;
        for (BlockID block : blocks) {
            const StateOfBlock& state = m_states[block.index()];
            Row available(std::span<const Word>(headOf(state)));
            readsToMerge.shrink(0);
            for (uint32_t actionIndex = state.actions.first; actionIndex < state.endOfActionsOfValues; ++actionIndex) {
                const Action& action = m_actions[actionIndex];
                if (action.kind == Action::Kind::Defines || action.kind == Action::Kind::Looks) {
                    uint32_t bit = action.bits.first;
                    if (std::optional<Replacement> replacement = findReplacement(bit, available.span()))
                        m_replacements.append(*replacement);
                    else if (isMergeableRead(bit))
                        readsToMerge.append(bit);
                }
                carryOut(action, available.mutableSpan());
            }
            if (!readsToMerge.isEmpty())
                findMerges(block, readsToMerge.span());
        }
    }

    PassOutcome applyReplacements()
    {
        if (m_replacements.isEmpty() && m_merges.isEmpty())
            return PassOutcome::Unchanged;

        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        ValueSubstitution substitution(m_function);
        Vector<ValueID> replacingValues;
        replacingValues.reserveInitialCapacity(m_replacements.size() + m_merges.size());
        for (const Merge& merge : m_merges) {
            builder.setOrigin(m_function.at(merge.read).origin());
            ValueID parameter = builder.addParameter(merge.block, m_function.at(merge.read).type);
            m_function.edit(parameter, ValueEdit::Type).setExpected(m_function.at(merge.read).expected());
            for (const Merge::Incoming& incoming : merge.incoming)
                builder.appendArgument(incoming.edge, incoming.value);
            keepReadForValidation(builder, { merge.block, merge.index }, parameter);
            replaceReadWithValue(m_program, m_function, substitution, merge.read, parameter);
            replacingValues.append(parameter);
        }
        for (const Replacement& replacement : m_replacements) {
            if (replacement.value) {
                keepReadForValidation(builder, replacement.point, replacement.value);
                replaceReadWithValue(m_program, m_function, substitution, replacement.read, replacement.value);
                replacingValues.append(replacement.value);
                continue;
            }
            becomeUndefined(m_function, replacement.read);
            replacingValues.append(replacement.read);
        }
        builder.applyInsertions();
        substitution.apply(m_function, &m_analyses);
        m_analyses.didChangeValues();
        narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo }, replacingValues.span());
        // The value analyses are dropped behind the last edit, and typing drops what it makes stale.
        m_context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    const OpcodeSet& m_opcodesThatMayWrite;
    const OpcodeSet& m_opcodesOfAccesses;
    const bool m_checksScreens;
    const EscapeAnalysis* m_escape { nullptr };
    std::optional<UseEffectHistory> m_captureHistory;
    Vector<Access> m_accesses;
    Vector<Bit> m_bits;
    Vector<AccessClass> m_classes;
    Vector<LocationClasses> m_locations;
    uint32_t m_firstClobberableBit { 0 };
    Vector<uint32_t> m_lastPositionOfClass;
    // By position in the reverse postorder: a position that no block that the block may reach stands in front of. Empty where nothing leads back.
    Vector<uint32_t> m_firstPositionReachedFrom;
    IndexRange m_group;
    uint32_t m_numberOfWords { 0 };
    Vector<StateOfBlock> m_states;
    Vector<BlockID, 16> m_visitedBlocks;
    Vector<BlockID, 8> m_blocksThatWaitForEdgesBack;
    Vector<Action> m_actions;
    Vector<Word> m_rows;
    BitVector m_isPending;
    bool m_isFirstSweep { false };
    Vector<Replacement> m_replacements;
    Vector<Merge> m_merges;
};

// `map.set(key, value)` behind the same call, with nothing between them that writes or runs code, finds the key and stores what is there. Its result is the map, as the first one's.
static bool isMapSet(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    BuiltinCall call = builtinCallOf(program, value);
    const BuiltinOverload& overload = program.builtins().at(call.overload);
    return overload.builtin == Builtins::MapPrototypeSet && overload.mode == BuiltinCallMode::Call && call.form == BuiltinCallForm::Whole && call.receiver && call.arguments.size() == 2;
}

static PassOutcome dropRepeatedMapSets(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::CallBuiltin))
        return PassOutcome::Unchanged;

    const Program& program = context.program;
    Function& function = context.function;
    auto hasTwoMapSets = [&](const BlockValues& values) {
        unsigned count = 0;
        for (ValueID id : values)
            count += function.at(id).opcode == Opcode::CallBuiltin && isMapSet(program, function.at(id));
        return count > 1;
    };
    ValueSubstitution substitution(function);
    std::optional<IRBuilder> builder;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        if (!hasTwoMapSets(values))
            continue;
        BitVector erasedIndexes(values.size());
        ValueID last;
        for (size_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (isMapSet(program, value)) {
                if (last) {
                    BuiltinCall first = builtinCallOf(program, function.at(last));
                    BuiltinCall second = builtinCallOf(program, value);
                    auto isSame = [&](ValueID a, ValueID b) {
                        return sameObjectThroughCasts(function, a) == sameObjectThroughCasts(function, b);
                    };
                    if (first.overload == second.overload && isSame(first.receiver, second.receiver) && std::ranges::equal(first.arguments, second.arguments, isSame)) {
                        substitution.replace(values[index], last);
                        erasedIndexes.set(index);
                        continue;
                    }
                }
                last = values[index];
                continue;
            }
            if (!last)
                continue;
            auto comesBetween = [](const Effects& effects) {
                return !effects.writes.isEmpty() || effects.mayRunOtherCode();
            };
            const EffectsOnEdges& effects = context.analyses.effectsOf(values[index]);
            if (comesBetween(effects.normal) || comesBetween(effects.handler))
                last = ValueID();
        }
        if (erasedIndexes.isEmpty())
            continue;
        if (!builder)
            builder.emplace(context.analyses.context(), function, context.types, &context.analyses);
        builder->eraseValues(BlockID(blockIndex), erasedIndexes);
    }
    if (substitution.isEmpty())
        return PassOutcome::Unchanged;
    substitution.apply(function, &context.analyses);
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

} // anonymous namespace

PassOutcome eliminateRedundantReads(FunctionPassContext& context)
{
    PassOutcome outcome = RedundantReadElimination(context).run();
    return outcome | dropRepeatedMapSets(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
