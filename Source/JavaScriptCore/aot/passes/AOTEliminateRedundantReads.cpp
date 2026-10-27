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
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTDominators.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTMemoryLocation.h"
#include "AOTMethodDispatch.h"
#include "AOTNaturalLoops.h"
#include "AOTProgramPasses.h"
#include "AOTRegionCloner.h"
#include "AOTRepresentation.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeRelations.h"
#include "AOTUseEffectHistory.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVirtualObject.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>
#include <tuple>
#include <wtf/BitVector.h>

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
static constexpr OpcodeSet opcodesOfAccesses = [] {
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

namespace {

struct IndexRange {
    uint32_t first { 0 };
    uint32_t end { 0 };

    bool isEmpty() const { return first >= end; }
    uint32_t size() const { return end - first; }
    bool contains(uint32_t index) const { return index >= first && index < end; }
    IndexRange clippedTo(IndexRange other) const { return { std::max(first, other.first), std::min(end, other.end) }; }
};

struct RedundantReadElimination {
    static constexpr uint32_t maxBitsOfClassSolvedTwice = 16384;

    struct Access {
        MemoryLocation location;
        ValueID base { };
        ValueID id { };
        ValueID stored { };
        ProgramPoint point { };
        bool isFixed { false };
        bool isAbsent { false };
        bool mayBeDeferredClosure { false };
        bool isSettling { false };
        bool mayPrecedeStore { false };
        std::optional<uint32_t> bit { };

        bool isRead() const { return !stored && !isAbsent; }
        bool isAtAllocation() const { return id == base; }
        bool isOperationOnMemory() const { return !isAtAllocation() && !isSettling; }

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
        bool isFixed;
        MemoryLocation location;
        uint64_t mask;
        ValueID base;
        // Found when it is first asked for.
        ObjectID allocation;
        bool hasLookedForAllocation;
        // Behind the bits of the last class of the section with the same location.
        uint32_t endOfBitsAtLocation { 0 };
        // Behind the bits of the last of the classes that follow one another in the same heap, and with the same base.
        uint32_t endOfBitsInHeap { 0 };
        uint32_t endOfBitsWithBase { 0 };
    };

    enum class Survival : uint8_t { None, OneClass, EveryClassWithBase };

    struct PositionRange {
        uint32_t first { std::numeric_limits<uint32_t>::max() };
        uint32_t last { 0 };
    };

    struct Action {
        // MayThrow stands in front of the actions of a value under a handler: its range counts the clears that follow, which the handler sees carried out. MeetsHandler: the handler sees the row as it is.
        enum class Kind : uint8_t { Defines, Looks, Clears, MayThrow, MeetsHandler };

        Kind kind;
        IndexRange bits;

        bool standsAtAccess() const { return kind == Kind::Defines || kind == Kind::Looks; }
    };

    using ActionList = Vector<Action, 8>;
    using Word = uint64_t;
    using Row = Vector<Word, 4>;

    struct Replacement {
        ValueID read;
        ValueID value;
        ProgramPoint point;
    };

    struct Merge {
        ValueID read;
        ProgramPoint point;
        Vector<std::pair<Edge, ValueID>, 4> incoming;
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

    FunctionPassContext& m_context;
    const Program& m_program { m_context.program };
    Function& m_function { m_context.function };
    FunctionAnalyses& m_analyses { m_context.analyses };
    const CFG& m_cfg { m_analyses.cfg() };
    const OpcodeSet& m_opcodesThatMayWrite { opcodesThatMayWriteAccessedHeaps() };
    const bool m_checksScreens { m_context.build.options().verifiesAfterEveryPass };
    std::optional<bool> m_makesObjectsOnEdges { };
    Vector<uint8_t> m_namesNoObjectMadeHere { };
    std::optional<UseEffectHistory> m_captureHistory { };
    Vector<Access> m_accesses { };
    Vector<Bit> m_bits { };
    Vector<AccessClass> m_classes { };
    uint32_t m_firstClobberableBit { 0 };
    Vector<uint32_t> m_lastPositionOfClass { };
    // For each word of a row of the group: the least and the greatest of those positions among the classes with a bit in the word.
    Vector<PositionRange> m_lastPositionsInWord { };
    // By position in the reverse postorder: a position that no block that the block may reach stands in front of. Empty where nothing leads back.
    Vector<uint32_t> m_firstPositionReachedFrom { };
    IndexRange m_group { };
    // A class with more bits than a group may hold is solved in parts, from its first bits on: the group is one part, and these are all the bits of the class. Empty otherwise.
    IndexRange m_bitsOfClassInParts { };
    // A read that was replaced by a value looks no more and defines no more: whatever its bit would reach, the bit of that value reaches, it comes first, and its type is as narrow.
    BitVector m_isReplacedByValue { };
    BitVector m_isReplacedByUndefined { };
    Vector<uint32_t> m_bitsReplacedByValue { };
    // By the bit of a read of the class that is solved in parts: the value found for each edge into its block so far.
    UncheckedKeyHashMap<uint32_t, Vector<ValueID, 4>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesOnEdges { };
    uint32_t m_numberOfWords { 0 };
    Vector<StateOfBlock> m_states { };
    Vector<BlockID, 16> m_visitedBlocks { };
    Vector<BlockID, 8> m_blocksThatWaitForEdgesBack { };
    Vector<Action> m_actions { };
    Vector<Word> m_rows { };
    BitVector m_isPending { };
    bool m_isFirstSweep { false };
    Vector<Replacement> m_replacements { };
    Vector<Merge> m_merges { };

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    const Access& accessOf(size_t bit) const { return m_accesses[m_bits[bit].access]; }

    bool isSettling(const MemoryLocation& location) const
    {
        if (location.heap() != AbstractHeap::ModuleVariables)
            return false;
        const Variable& variable = m_program.at(location.variable());
        return variable.initialization == VariableInitialization::Uninitialized
            && variable.flow.stores == StoreCount::AtMostOnce
            && variable.flow.writers == WriterState::AllKnown;
    }

    void addAllocationAccesses(ValueID id, ProgramPoint point)
    {
        const Value& allocation = m_function.at(id);
        LayoutID layoutID = allocation.layout();
        const Layout& layout = m_program.at(layoutID);
        auto add = [&](uint32_t fieldIndex, ValueID stored) {
            const Field& field = layout.fields[fieldIndex];
            if (!layout.isEnvironment() && (!field.key || field.flags.contains(FieldFlags::IsPrivate)))
                return;
            m_accesses.append(Access {
                .location = layout.isEnvironment() ? MemoryLocation::environmentField(layoutID, fieldIndex) : MemoryLocation::objectField(field.key),
                .base = id,
                .id = id,
                .stored = stored,
                .point = point,
                .isAbsent = !stored,
                .mayBeDeferredClosure = flowOf(m_program, { layoutID, fieldIndex }).location.deferral != ClosureDeferral::Unknown,
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

    std::optional<Access> findAccess(ValueID id, ProgramPoint point) const
    {
        const Value& value = m_function.at(id);
        MemoryAccess access = memoryAccessOf(m_function, value);
        switch (access.kind) {
        case MemoryAccess::Kind::NotExact: {
            if (!leavesDeadZoneBehind(m_program, m_function, value))
                return std::nullopt;
            const Value& definition = m_function.at(m_analyses.definitionThroughUpcasts(value.valueOperand()));
            MemoryAccess read = memoryAccessOf(m_function, definition);
            if (read.kind != MemoryAccess::Kind::Read || !isSettling(read.location) || stabilityOf(m_program, definition, read) != LocationStability::Unknown)
                return std::nullopt;
            return Access { .location = read.location, .id = id, .stored = id, .point = point, .isFixed = true, .isSettling = true };
        }
        case MemoryAccess::Kind::Read:
        case MemoryAccess::Kind::Write:
            break;
        }
        if (!accessedHeapSet.contains(access.location.heap()))
            return std::nullopt;
        RELEASE_ASSERT(access.kind != MemoryAccess::Kind::Read || opcodesReadingAccessedHeaps.contains(value.opcode));
        ValueID base = access.base ? m_analyses.sameObjectThroughCasts(access.base).valueInFrontOfFacts() : ValueID();
        return Access { .location = access.location, .base = base, .id = id, .stored = access.stored, .point = point };
    }

    void findAccesses()
    {
        BitVector isReadBase(m_function.values.size());
        BitVector keptReads = m_context.build.validates() ? findReadsKeptForValidation(m_function) : BitVector();
        for (BlockID blockID : m_cfg.reversePostOrder() | std::views::reverse) {
            const Block& block = m_function.at(blockID);
            for (uint32_t index = static_cast<uint32_t>(block.values.size()); index--;) {
                ValueID id = block.values[index];
                ProgramPoint point { blockID, index };
                Opcode opcode = m_function.at(id).opcode;
                if (!opcodesOfAccesses.contains(opcode)) {
                    if (m_checksScreens) [[unlikely]]
                        RELEASE_ASSERT(!findAccess(id, point));
                    continue;
                }
                if (opcode == Opcode::NewObject) {
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
        auto receivers = m_program.at(store.receivers()).all();
        return !receivers || std::ranges::any_of(*receivers, [&](LayoutID member) {
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

    void assignBits()
    {
        auto accessAt = [&](uint32_t index) -> Access& { return m_accesses[index]; };
        Vector<uint32_t> order(m_accesses.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        AOT::sort(order, { }, [&](uint32_t index) {
            return std::tuple { accessAt(index).location, accessAt(index).base, index };
        });
        size_t numberOfAccessesWithCompany = 0;
        forEachLocationAndBase(order.mutableSpan(), [&](std::span<uint32_t> members) {
            if (members.size() < 2 || std::ranges::none_of(members, &Access::isRead, accessAt))
                return;
            // Asked only of an access that shares its location and base with another one: the others get no bit.
            bool isFixedAtAllocation = true;
            for (uint32_t index : members) {
                Access& access = accessAt(index);
                if (access.isOperationOnMemory()) {
                    const Value& value = m_function.at(access.id);
                    bool isStable = stabilityOf(m_program, value, memoryAccessOf(m_function, value)) != LocationStability::Unknown;
                    bool settles = !isStable && isSettling(access.location);
                    access.isFixed = isStable || settles;
                    access.mayPrecedeStore = settles && access.isRead();
                }
                isFixedAtAllocation &= access.isFixed || access.isAtAllocation();
            }
            for (uint32_t index : members) {
                if (accessAt(index).isAtAllocation())
                    accessAt(index).isFixed = isFixedAtAllocation;
                order[numberOfAccessesWithCompany++] = index;
            }
        });
        order.shrink(numberOfAccessesWithCompany);
        // Two accesses through one base may name different receivers, so one location and base may have a class in each section.
        size_t numberOfFixedAccesses = std::ranges::stable_partition(order, &Access::isFixed, accessAt).begin() - order.begin();
        for (std::span<uint32_t> section : { order.mutableSpan().first(numberOfFixedAccesses), order.mutableSpan().subspan(numberOfFixedAccesses) }) {
            forEachLocationAndBase(section, [&](std::span<uint32_t> members) {
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

                uint32_t firstBit = m_bits.size();
                for (uint32_t index : members.first(numberOfAccesses)) {
                    Access& access = m_accesses[index];
                    access.bit = static_cast<uint32_t>(m_bits.size());
                    if (!access.isAtAllocation())
                        access.mayBeDeferredClosure = storesIntoDeferredClosureField(access);
                    m_bits.append(Bit { index, static_cast<uint32_t>(m_classes.size()) });
                }

                const Access& first = m_accesses[members.front()];
                uint64_t mask = !m_classes.isEmpty() && m_classes.last().location == first.location ? m_classes.last().mask : maskOf(m_program, first.location);
                m_classes.append(AccessClass { { firstBit, static_cast<uint32_t>(m_bits.size()) }, first.isFixed, first.location, mask, first.base, ObjectID(), false });
                if (first.isFixed)
                    m_firstClobberableBit = m_bits.size();
            });
        }
        for (size_t index = m_classes.size(); index--;) {
            AccessClass& accessClass = m_classes[index];
            bool isFollowedAtLocation = index + 1 < m_classes.size() && m_classes[index + 1].isFixed == accessClass.isFixed && m_classes[index + 1].location == accessClass.location;
            accessClass.endOfBitsAtLocation = isFollowedAtLocation ? m_classes[index + 1].endOfBitsAtLocation : accessClass.bits.end;
            bool isFollowedInHeap = index + 1 < m_classes.size() && m_classes[index + 1].isFixed == accessClass.isFixed && m_classes[index + 1].location.heap() == accessClass.location.heap();
            accessClass.endOfBitsInHeap = isFollowedInHeap ? m_classes[index + 1].endOfBitsInHeap : accessClass.bits.end;
            bool isFollowedWithBase = index + 1 < m_classes.size() && m_classes[index + 1].isFixed == accessClass.isFixed && m_classes[index + 1].base == accessClass.base;
            accessClass.endOfBitsWithBase = isFollowedWithBase ? m_classes[index + 1].endOfBitsWithBase : accessClass.bits.end;
        }
    }

    IndexRange bitsOfClassesAt(bool isFixed, const MemoryLocation& location) const
    {
        auto found = std::ranges::equal_range(m_classes, std::pair { !isFixed, location }, { }, [](const AccessClass& accessClass) {
            return std::pair { !accessClass.isFixed, accessClass.location };
        });
        if (found.empty())
            return { };
        return { found.front().bits.first, found.back().bits.end };
    }

    bool mayDefineAllocation(ValueID base)
    {
        const Value& value = m_function.at(base);
        if (value.opcode != Opcode::Parameter)
            return resultRuleOf(value.opcode) == ResultRule::Allocation || freshnessOf(m_analyses.effectContext(), m_function, value) != ResultFreshness::Unknown;
        if (!m_makesObjectsOnEdges) {
            m_makesObjectsOnEdges = std::ranges::any_of(m_cfg.reversePostOrder(), [&](BlockID block) {
                return freshnessOf(m_analyses.effectContext(), m_function, m_function.at(block).terminator) != ResultFreshness::Unknown;
            });
        }
        return *m_makesObjectsOnEdges;
    }

    ObjectID trackedAllocationOf(ValueID base)
    {
        if (!mayDefineAllocation(base))
            return { };
        const EscapeAnalysis& escape = m_analyses.escape();
        if (escape.gaveUp())
            return { };
        std::optional<ObjectID> object = escape.objectDefinedBy(base);
        if (!object || escape.at(*object).kind != ObjectKind::Allocation || escape.at(*object).state == EscapeState::Unknown)
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

    // What EscapeAnalysis::mayAlias() says, where it can be seen without the solve.
    std::optional<bool> mayAliasSeenAlone(ValueID a, ValueID b)
    {
        std::optional<bool> seen;
        if (m_analyses.sameObjectThroughCasts(a) == m_analyses.sameObjectThroughCasts(b))
            seen = true;
        else if (!mayBeSameObject(typeOf(a), typeOf(b), m_program.types))
            seen = false;
        else if (namesNoObjectMadeHere(a) && namesNoObjectMadeHere(b))
            seen = true;
        if (m_checksScreens && seen && !m_analyses.escape().gaveUp()) [[unlikely]]
            RELEASE_ASSERT(m_analyses.escape().mayAlias(a, b) == *seen);
        return seen;
    }

    bool areDistinctObjects(AccessClass& accessClass, ValueID storeBase, ObjectID uncapturedStoreAllocation, ProgramPoint afterStore)
    {
        if (!uncapturedStoreAllocation && !mayDefineAllocation(accessClass.base)) {
            if (std::optional<bool> mayAlias = mayAliasSeenAlone(accessClass.base, storeBase))
                return !*mayAlias;
        }
        const EscapeAnalysis& escape = m_analyses.escape();
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

    // Rows hold the bits of the group that is being solved.
    static bool isEmpty(std::span<const Word> row) { return std::ranges::all_of(row, [](Word word) { return !word; }); }
    static void set(std::span<Word> row, size_t bit) { row[bit / 64] |= Word { 1 } << bit % 64; }

    static size_t findBit(std::span<const Word> row, size_t from)
    {
        for (size_t wordIndex = from / 64; wordIndex < row.size(); ++wordIndex) {
            if (Word word = row[wordIndex] & ~Word { 0 } << from % 64)
                return wordIndex * 64 + std::countr_zero(word);
            from = 0;
        }
        return row.size() * 64;
    }

    static void clear(std::span<Word> row, size_t first, size_t end)
    {
        for (size_t wordIndex = first / 64; wordIndex * 64 < end; ++wordIndex) {
            Word upToEnd = end - wordIndex * 64 < 64 ? ~(~Word { 0 } << (end - wordIndex * 64)) : ~Word { 0 };
            row[wordIndex] &= ~(~Word { 0 } << first % 64 & upToEnd);
            first = 0;
        }
    }

    void meet(std::span<Word> into, std::span<const Word> row) const
    {
        for (uint32_t word = 0; word < m_numberOfWords; ++word)
            into[word] &= row[word];
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
        if (!actions.isEmpty() && actions.last().kind == Action::Kind::Clears && actions.last().bits.end == bits.first)
            actions.last().bits.end = bits.end;
        else
            actions.append(Action { Action::Kind::Clears, bits });
    }

    // What a value or a terminator clears is asked only of the classes with a bit in the row: nothing else is there to be cleared.
    void appendClearsOfStore(const Access& store, std::span<const Word> row, ActionList& actions)
    {
        ProgramPoint afterStore { store.point.block, store.point.index + 1 };
        std::optional<ObjectID> allocation;
        for (bool isFixed : { true, false }) {
            forEachClassWithBitIn(row, bitsOfClassesAt(isFixed, store.location), [&](AccessClass& accessClass) {
                if (store.base && accessClass.base != store.base) {
                    if (!allocation) {
                        allocation = trackedAllocationOf(store.base);
                        if (*allocation && !isUncapturedBefore(*allocation, afterStore))
                            allocation = ObjectID();
                    }
                    if (areDistinctObjects(accessClass, store.base, *allocation, afterStore) || m_context.build.isSabotaged(Sabotage::RememberedReadsOutlastStoresThroughOtherNames))
                        return;
                }
                appendClear(actions, accessClass.bits);
            });
        }
    }

    void appendClearsOfWrites(const EffectsOnEdges& effects, std::span<const Word> row, ActionList& actions, NOESCAPE const Invocable<Survival(AccessClass&)> auto& survives)
    {
        if (!effects.normal.writes.containsAny(accessedHeapSet) && !effects.handler.writes.containsAny(accessedHeapSet))
            return;
        LocationMasks writes = effects.onAnyEdge().writes;
        IndexRange bits = IndexRange { m_firstClobberableBit, static_cast<uint32_t>(m_bits.size()) }.clippedTo(m_group);
        if (bits.isEmpty())
            return;
        // The classes of one location have one mask, so a location that is not written is passed over with all its classes, and so is a heap.
        size_t end = bits.end - m_group.first;
        for (size_t bit = findBit(row, bits.first - m_group.first); bit < end;) {
            AccessClass& accessClass = m_classes[m_bits[m_group.first + bit].accessClass];
            if (!writes.overlaps(accessClass.location.heap(), accessClass.mask)) {
                uint32_t endOfUnwrittenBits = writes.contains(accessClass.location.heap()) ? accessClass.endOfBitsAtLocation : accessClass.endOfBitsInHeap;
                bit = findBit(row, endOfUnwrittenBits - m_group.first);
                continue;
            }
            Survival survival = survives(accessClass);
            if (survival == Survival::None && !m_context.build.isSabotaged(Sabotage::RememberedReadsOutlastWrites))
                appendClear(actions, accessClass.bits);
            uint32_t endOfSettledBits = survival == Survival::EveryClassWithBase ? accessClass.endOfBitsWithBase : accessClass.bits.end;
            bit = findBit(row, endOfSettledBits - m_group.first);
        }
    }

    void appendClearsOfEffects(ProgramPoint point, std::span<const Word> row, ActionList& actions)
    {
        ValueID id = m_function.at(point.block).values[point.index];
        const Value& value = m_function.at(id);
        if (!m_opcodesThatMayWrite.contains(value.opcode)) {
            if (m_checksScreens) [[unlikely]]
                RELEASE_ASSERT(!m_analyses.effectsOf(id).onAnyEdge().writes.containsAny(accessedHeapSet));
            return;
        }

        const EffectsOnEdges& effects = m_analyses.effectsOf(id);
        ProgramPoint after { point.block, point.index + 1 };
        std::optional<LocationMasks> writesBeyondOperands;
        std::optional<Vector<std::pair<ValueID, LocationMasks>, 4>> writesThroughOperands;
        auto findWritesThroughOperands = [&] {
            if (writesThroughOperands)
                return;
            writesThroughOperands.emplace();
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!mayBeReference(typeOf(operand)))
                    return;
                LocationMasks writes = memoryAccessedThrough(m_analyses.effectContext(), m_function, value, operandIndex, effects).writes;
                if (writes.containsAny(accessedHeapSet))
                    writesThroughOperands->append({ operand, writes });
            });
        };
        // Whatever the value writes: nobody else holds the object yet, and no operand that is written through is the object.
        auto isOutOfReach = [&](AccessClass& accessClass) {
            if (!mayDefineAllocation(accessClass.base))
                return false;
            const EscapeAnalysis& escape = m_analyses.escape();
            if (escape.gaveUp())
                return false;
            ObjectID allocation = allocationOf(accessClass);
            if (!allocation || !isUncapturedBefore(allocation, after))
                return false;
            findWritesThroughOperands();
            return std::ranges::none_of(*writesThroughOperands, [&](const auto& written) {
                return escape.mayAlias(written.first, accessClass.base) && escape.pointsTo(written.first).objects.contains(allocation);
            });
        };
        auto survives = [&](AccessClass& accessClass) {
            if (!accessClass.base)
                return false;
            auto overlapsClass = [&](const LocationMasks& writes) {
                return writes.overlaps(accessClass.location.heap(), accessClass.mask);
            };
            if (!writesBeyondOperands)
                writesBeyondOperands = effectsBeyondOperands(m_analyses.effectContext(), m_function, value, effects).onAnyEdge().writes;
            bool isWrittenBeyondOperands = overlapsClass(*writesBeyondOperands);
            if (!mayDefineAllocation(accessClass.base)) {
                if (isWrittenBeyondOperands)
                    return false;
                findWritesThroughOperands();
                bool isSettled = true;
                for (const auto& [operand, writes] : *writesThroughOperands) {
                    if (!overlapsClass(writes))
                        continue;
                    std::optional<bool> mayAlias = mayAliasSeenAlone(operand, accessClass.base);
                    if (mayAlias && *mayAlias)
                        return false;
                    isSettled &= mayAlias.has_value();
                }
                if (isSettled)
                    return true;
            }
            if ((isWrittenBeyondOperands && !allocationOf(accessClass)) || m_analyses.escape().gaveUp())
                return false;
            if (isWrittenBeyondOperands && !isUncapturedBefore(accessClass.allocation, after))
                return false;
            findWritesThroughOperands();
            const EscapeAnalysis& escape = m_analyses.escape();
            return std::ranges::none_of(*writesThroughOperands, [&](const auto& written) {
                if (!overlapsClass(written.second) || !escape.mayAlias(written.first, accessClass.base))
                    return false;
                ObjectID allocation = allocationOf(accessClass);
                bool mayBeAllocation = !allocation || escape.pointsTo(written.first).objects.contains(allocation);
                return mayBeAllocation || !isUncapturedBefore(allocation, after);
            });
        };
        appendClearsOfWrites(effects, row, actions, [&](AccessClass& accessClass) {
            if (accessClass.base && accessClass.endOfBitsWithBase > accessClass.bits.end && isOutOfReach(accessClass))
                return Survival::EveryClassWithBase;
            return survives(accessClass) ? Survival::OneClass : Survival::None;
        });
    }

    // Goes through the block with the rows and hands each action to the functor in front of carrying it out on all of them.
    void walk(BlockID blockID, std::span<Row* const> rows, bool watchesThrows, NOESCAPE const Invocable<void(const Action&)> auto& functor)
    {
        const Block& block = m_function.at(blockID);
        IndexRange accessesOfBlock = m_states[blockID.index()].accesses;
        std::span<const Access> accesses = m_accesses.span().subspan(accessesOfBlock.first, accessesOfBlock.size());
        Row unionOfRows(FillWith { }, m_numberOfWords, Word { 0 });
        ActionList actions;
        uint32_t numberOfValues = block.values.size();
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            std::span<const Word> live = rows[0]->span();
            if (rows.size() > 1) {
                std::ranges::fill(unionOfRows, 0);
                for (Row* row : rows) {
                    for (uint32_t word = 0; word < m_numberOfWords; ++word)
                        unionOfRows[word] |= (*row)[word];
                }
                live = unionOfRows.span();
            }
            if (isEmpty(live) && !watchesThrows) {
                if (accesses.empty())
                    break;
                index = accesses.front().point.index;
            }
            ValueID id = block.values[index];
            size_t numberOfAccesses = 0;
            while (numberOfAccesses < accesses.size() && accesses[numberOfAccesses].id == id)
                ++numberOfAccesses;
            std::span<const Access> accessesOfValue = accesses.first(numberOfAccesses);
            accesses = accesses.subspan(numberOfAccesses);

            if (accessesOfValue.empty())
                appendClearsOfEffects({ blockID, index }, live, actions);
            else if (const Access& access = accessesOfValue.front(); access.stored && access.isOperationOnMemory())
                appendClearsOfStore(access, live, actions);
            for (const Access& access : accessesOfValue) {
                if (!access.bit || m_isReplacedByValue.quickGet(*access.bit))
                    continue;
                if (m_group.contains(*access.bit))
                    actions.append(Action { access.mayPrecedeStore ? Action::Kind::Looks : Action::Kind::Defines, { *access.bit, *access.bit + 1 } });
                else if (m_bitsOfClassInParts.contains(*access.bit) && access.isRead())
                    actions.append(Action { Action::Kind::Looks, { *access.bit, *access.bit + 1 } });
            }
            if (watchesThrows && m_analyses.effectsOf(id).mayThrow())
                functor(Action { Action::Kind::MayThrow, { 0, static_cast<uint32_t>(std::ranges::count(actions, Action::Kind::Clears, &Action::kind)) } });
            for (const Action& action : actions) {
                functor(action);
                for (Row* row : rows)
                    carryOut(action, row->mutableSpan());
            }
            actions.shrink(0);
        }
    }

    std::span<Word> headOf(const StateOfBlock& state) { return m_rows.mutableSpan().subspan(state.rows, m_numberOfWords); }
    std::span<Word> tailOf(const StateOfBlock& state) { return m_rows.mutableSpan().subspan(state.rows + m_numberOfWords, m_numberOfWords); }
    std::span<Word> rowForHandlerOf(const StateOfBlock& state) { return m_rows.mutableSpan().subspan(state.rows + 2 * m_numberOfWords, m_numberOfWords); }

    // The actions of a block are made when it is first visited, for the row it is entered with then. Rows only lose bits from visit to visit, so the same actions serve every later row.
    void carryThrough(BlockID block, StateOfBlock& state, bool isFirstVisit, Row& row, Row& rowForHandler)
    {
        Row walked;
        if (isFirstVisit) {
            walked = row;
            // A handler that is entered with nothing stays so whatever throws.
            bool watchesThrows = m_function.at(block).handler && !isEmpty(rowForHandler.span());
            Row* rows[] { &walked };
            state.actions.first = m_actions.size();
            walk(block, rows, watchesThrows, [&](const Action& action) {
                m_actions.append(action);
            });
            state.endOfActionsOfValues = m_actions.size();

            bool terminatorMayThrow = watchesThrows && m_analyses.effectsOfTerminator(block).mayThrow();
            if (terminatorMayThrow)
                m_actions.append(Action { Action::Kind::MeetsHandler, { } });
            if (!isEmpty(walked.span())) {
                ActionList actions;
                appendClearsOfWrites(m_analyses.effectsOfTerminator(block), walked.span(), actions, [](AccessClass&) {
                    return Survival::None;
                });
                for (const Action& action : actions)
                    carryOut(action, walked.mutableSpan());
                m_actions.appendVector(actions);
            }
            if (terminatorMayThrow)
                m_actions.append(Action { Action::Kind::MeetsHandler, { } });
            state.actions.end = m_actions.size();
            if (!watchesThrows) {
                row = WTF::move(walked);
                return;
            }
        }
        // The handler is met behind the walk: the clears of a value that may throw follow its mark.
        for (uint32_t index = state.actions.first; index < state.actions.end; ++index) {
            const Action& action = m_actions[index];
            if (action.kind == Action::Kind::MeetsHandler)
                meet(rowForHandler.mutableSpan(), row.span());
            else if (action.kind == Action::Kind::MayThrow) {
                Row onThrow = row;
                for (uint32_t clearIndex = 0; clearIndex < action.bits.end; ++clearIndex)
                    carryOut(m_actions[index + 1 + clearIndex], onThrow.mutableSpan());
                meet(rowForHandler.mutableSpan(), onThrow.span());
            }
            carryOut(action, row.mutableSpan());
        }
        RELEASE_ASSERT(!isFirstVisit || row == walked);
    }

    void prepareToSolve()
    {
        std::span<const BlockID> order = m_cfg.reversePostOrder();
        m_isPending.ensureSize(order.size());
        m_states.grow(m_function.blocks.size());
        for (uint32_t index = 0; index < m_accesses.size(); ++index) {
            IndexRange& accesses = m_states[m_accesses[index].point.block.index()].accesses;
            if (accesses.isEmpty())
                accesses.first = index;
            accesses.end = index + 1;
        }

        // A bit of a class is set at an access of the class and looked at in front of one, so behind the last access that a block may reach the bits of the class say nothing.
        m_lastPositionOfClass.fill(0, m_classes.size());
        for (const Bit& bit : m_bits) {
            uint32_t& last = m_lastPositionOfClass[bit.accessClass];
            last = std::max(last, m_cfg.reversePostOrderIndex(m_accesses[bit.access].point.block));
        }
        if (!m_cfg.hasCycle())
            return;

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

    bool isPartOfClass() const { return !m_bitsOfClassInParts.isEmpty(); }

    void markAsPending(BlockID block) { m_isPending.quickSet(m_cfg.reversePostOrderIndex(block)); }

    // A block that was not visited has empty rows. In the first sweep a block that comes later may still be visited, so an edge back from it is left out until the sweep is over.
    void meetAtHeadOf(BlockID block, uint32_t position, Row& head)
    {
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

    void clearClassesLastAccessedBefore(uint32_t position, Row& row)
    {
        for (uint32_t wordIndex = 0; wordIndex < m_numberOfWords; ++wordIndex) {
            PositionRange lastPositions = m_lastPositionsInWord[wordIndex];
            if (!row[wordIndex] || lastPositions.first >= position)
                continue;
            if (lastPositions.last < position) {
                row[wordIndex] = 0;
                continue;
            }
            IndexRange bitsOfWord { m_group.first + wordIndex * 64, m_group.first + (wordIndex + 1) * 64 };
            forEachClassWithBitIn(row.span(), bitsOfWord, [&](AccessClass& accessClass) {
                if (m_lastPositionOfClass[&accessClass - m_classes.span().data()] < position)
                    carryOut(Action { Action::Kind::Clears, accessClass.bits }, row.mutableSpan());
            });
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

        uint32_t firstPositionReached = m_firstPositionReachedFrom.isEmpty() ? position : m_firstPositionReachedFrom[position];
        clearClassesLastAccessedBefore(firstPositionReached, row);
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

    void solve(IndexRange group, IndexRange bitsOfClassInParts = { })
    {
        for (BlockID block : m_visitedBlocks)
            m_states[block.index()].isVisited = false;
        m_visitedBlocks.shrink(0);
        m_blocksThatWaitForEdgesBack.shrink(0);
        m_actions.shrink(0);
        m_rows.shrink(0);
        m_group = group;
        m_bitsOfClassInParts = bitsOfClassInParts;
        m_numberOfWords = (group.size() + 63) / 64;

        m_lastPositionsInWord.fill(PositionRange { }, m_numberOfWords);
        for (uint32_t bit = group.first; bit < group.end; ++bit) {
            PositionRange& lastPositions = m_lastPositionsInWord[(bit - group.first) / 64];
            uint32_t lastPosition = m_lastPositionOfClass[m_bits[bit].accessClass];
            lastPositions.first = std::min(lastPositions.first, lastPosition);
            lastPositions.last = std::max(lastPositions.last, lastPosition);
            if (!m_isReplacedByValue.quickGet(bit))
                markAsPending(accessOf(bit).point.block);
        }

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
        findReplacements();
        for (uint32_t bit : m_bitsReplacedByValue)
            m_isReplacedByValue.quickSet(bit);
        m_bitsReplacedByValue.shrink(0);
    }

    // The parts are solved in the order of the bits, and a read takes the first suitable bit, so what an earlier part has found stands.
    void solveInParts(IndexRange bitsOfClass, uint32_t maxBitsPerPart)
    {
        size_t firstReplacement = m_replacements.size();
        for (IndexRange part { bitsOfClass.first, bitsOfClass.first }; part.end < bitsOfClass.end; part.first = part.end) {
            part.end = static_cast<uint32_t>(std::min<uint64_t>(uint64_t { part.first } + maxBitsPerPart, bitsOfClass.end));
            solve(part, bitsOfClass);
        }
        std::span<Replacement> replacementsOfClass = m_replacements.mutableSpan().subspan(firstReplacement);
        AOT::sort(replacementsOfClass, { }, [&](const Replacement& replacement) {
            return std::pair { m_cfg.reversePostOrderIndex(replacement.point.block), replacement.point.index };
        });

        Vector<uint32_t> readsToMerge;
        for (const auto& [bit, values] : m_valuesOnEdges) {
            if (!m_isReplacedByValue.quickGet(bit) && !m_isReplacedByUndefined.quickGet(bit) && !values.contains(ValueID()))
                readsToMerge.append(bit);
        }
        AOT::sort(readsToMerge);
        for (uint32_t bit : readsToMerge) {
            const Access& read = accessOf(bit);
            std::span<const Edge> edges = m_cfg.incomingEdges(read.point.block);
            const Vector<ValueID, 4>& values = m_valuesOnEdges.find(bit)->value;
            Merge merge { read.id, read.point, { } };
            for (size_t index = 0; index < edges.size(); ++index)
                merge.incoming.append({ edges[index], values[index] });
            m_merges.append(WTF::move(merge));
        }
        m_valuesOnEdges.clear();
    }

    void checkAgainstSolveOfWholeClass(IndexRange bitsOfClass, size_t firstReplacement, size_t firstMerge)
    {
        Vector<Replacement> replacements(m_replacements.span().subspan(firstReplacement));
        Vector<Merge> merges(m_merges.span().subspan(firstMerge));
        m_replacements.shrink(firstReplacement);
        m_merges.shrink(firstMerge);
        m_isReplacedByValue.clearAll();
        m_isReplacedByUndefined.clearAll();
        solve(bitsOfClass);
        RELEASE_ASSERT(std::ranges::equal(m_replacements.span().subspan(firstReplacement), replacements, [](const Replacement& a, const Replacement& b) {
            return a.read == b.read && a.value == b.value;
        }));
        RELEASE_ASSERT(std::ranges::equal(m_merges.span().subspan(firstMerge), merges, [](const Merge& a, const Merge& b) {
            return a.read == b.read && std::ranges::equal(a.incoming, b.incoming, { }, &std::pair<Edge, ValueID>::second, &std::pair<Edge, ValueID>::second);
        }));
    }

    bool canForward(const Access& source, const Type& readType) const
    {
        return !source.mayBeDeferredClosure && typeOf(source.valueAfter()).isSubsetOf(readType, m_program);
    }

    const Access* findAvailableAccess(uint32_t readBit, std::span<const Word> available, NOESCAPE const Invocable<bool(const Access&)> auto& isSuitable) const
    {
        IndexRange bits = m_classes[m_bits[readBit].accessClass].bits.clippedTo(m_group);
        for (size_t bit = m_group.first + findBit(available, bits.first - m_group.first); bit < bits.end; bit = m_group.first + findBit(available, bit + 1 - m_group.first)) {
            if (bit != readBit && isSuitable(accessOf(bit)))
                return &accessOf(bit);
        }
        return nullptr;
    }

    // What is available on one way into the block need not be so at its head, and the actions of the block were made for the row at its head. So the block is gone through anew.
    void findMerges(BlockID block, std::span<const uint32_t> bitsOfReads)
    {
        std::span<const Edge> edges = m_cfg.incomingEdges(block);
        if (edges.size() < 2 || block == m_cfg.root() || m_function.at(block).isHandlerBlock() || m_analyses.isEnteredFromUnreachableBlock(block))
            return;
        bool canAppendArguments = std::ranges::all_of(edges, [&](Edge edge) {
            if (edge.isHandlerEdge() || hasEdgeResults(m_function, edge))
                return false;
            TerminatorKind kind = m_function.at(edge.from).terminator.kind;
            return kind == TerminatorKind::Jump || kind == TerminatorKind::Branch;
        });
        if (!canAppendArguments || m_context.exceeds(LimitID::MaxMergedReadPredecessors, edges.size(), accessOf(bitsOfReads.front()).id))
            return;

        Vector<Row, 4> available;
        for (Edge edge : edges) {
            const StateOfBlock& source = m_states[edge.from.index()];
            available.append(Row(FillWith { }, m_numberOfWords, Word { 0 }));
            if (source.isVisited)
                std::ranges::copy(tailOf(source), available.last().begin());
        }
        Vector<Row*, 4> rows;
        for (Row& row : available)
            rows.append(&row);
        walk(block, rows.span(), false, [&](const Action& action) {
            if (bitsOfReads.empty() || !action.standsAtAccess() || action.bits.first != bitsOfReads.front())
                return;
            bitsOfReads = bitsOfReads.subspan(1);
            const Access& read = accessOf(action.bits.first);
            const Type& readType = typeOf(read.id);
            auto findSourceOn = [&](size_t index) {
                return findAvailableAccess(action.bits.first, available[index].span(), [&](const Access& access) {
                    return !access.isAbsent && canForward(access, readType);
                });
            };
            if (isPartOfClass()) {
                Vector<ValueID, 4>& values = m_valuesOnEdges.ensure(action.bits.first, [&] {
                    return Vector<ValueID, 4>(FillWith { }, edges.size(), ValueID());
                }).iterator->value;
                for (size_t index = 0; index < edges.size(); ++index) {
                    if (values[index])
                        continue;
                    if (const Access* source = findSourceOn(index))
                        values[index] = source->valueAfter();
                }
                return;
            }
            Merge merge { read.id, read.point, { } };
            for (size_t index = 0; index < edges.size(); ++index) {
                const Access* source = findSourceOn(index);
                if (!source)
                    return;
                merge.incoming.append({ edges[index], source->valueAfter() });
            }
            m_merges.append(WTF::move(merge));
        });
    }

    void findReplacements()
    {
        Vector<BlockID, 16> blocks;
        for (BlockID block : m_visitedBlocks) {
            if (!m_states[block.index()].accesses.isEmpty())
                blocks.append(block);
        }
        AOT::sort(blocks, { }, [&](BlockID block) {
            return m_cfg.reversePostOrderIndex(block);
        });

        Vector<uint32_t, 8> readsToMerge;
        for (BlockID block : blocks) {
            const StateOfBlock& state = m_states[block.index()];
            Row available(std::span<const Word>(headOf(state)));
            readsToMerge.shrink(0);
            for (uint32_t actionIndex = state.actions.first; actionIndex < state.endOfActionsOfValues; ++actionIndex) {
                const Action& action = m_actions[actionIndex];
                if (action.standsAtAccess() && accessOf(action.bits.first).isRead() && !m_isReplacedByUndefined.quickGet(action.bits.first)) {
                    const Access& read = accessOf(action.bits.first);
                    const Type& readType = typeOf(read.id);
                    bool readsAbsentAsUndefined = m_function.at(read.id).opcode == Opcode::GetSlotOrUndefined && (readType.primitives & Primitive::Undefined);
                    const Access* source = findAvailableAccess(action.bits.first, available.span(), [&](const Access& access) {
                        return access.isAbsent ? readsAbsentAsUndefined : canForward(access, readType);
                    });
                    if (source) {
                        m_replacements.append({ read.id, source->isAbsent ? ValueID() : source->valueAfter(), read.point });
                        if (isPartOfClass()) {
                            if (source->isAbsent)
                                m_isReplacedByUndefined.quickSet(action.bits.first);
                            else
                                m_bitsReplacedByValue.append(action.bits.first);
                        }
                    } else if (readType.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
                        readsToMerge.append(action.bits.first);
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
        auto replace = [&](ValueID read, ProgramPoint point, ValueID value) {
            keepReadForValidation(builder, point, value);
            replaceReadWithValue(m_program, m_function, substitution, read, value);
            replacingValues.append(value);
        };
        for (const Merge& merge : m_merges) {
            builder.setOrigin(m_function.at(merge.read).origin());
            ValueID parameter = builder.addParameter(merge.point.block, m_function.at(merge.read).type);
            m_function.edit(parameter, ValueEdit::Type).setExpected(m_function.at(merge.read).expected());
            for (auto [edge, value] : merge.incoming)
                builder.appendArgument(edge, value);
            replace(merge.read, merge.point, parameter);
        }
        for (const Replacement& replacement : m_replacements) {
            if (replacement.value) {
                replace(replacement.read, replacement.point, replacement.value);
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

    PassOutcome run()
    {
        findAccesses();
        if (m_accesses.size() < 2)
            return PassOutcome::Unchanged;
        assignBits();
        if (m_classes.isEmpty())
            return PassOutcome::Unchanged;
        prepareToSolve();

        m_isReplacedByValue.ensureSize(m_bits.size());
        m_isReplacedByUndefined.ensureSize(m_bits.size());
        uint64_t maxBitsPerGroup = std::max<uint64_t>(m_context.limit(LimitID::MaxReadsPerSolve), 1);
        auto solveGroup = [&](IndexRange group) {
            if (group.size() <= maxBitsPerGroup) {
                solve(group);
                return;
            }
            size_t firstReplacement = m_replacements.size();
            size_t firstMerge = m_merges.size();
            solveInParts(group, static_cast<uint32_t>(maxBitsPerGroup));
            if (m_checksScreens && group.size() <= maxBitsOfClassSolvedTwice) [[unlikely]]
                checkAgainstSolveOfWholeClass(group, firstReplacement, firstMerge);
        };
        IndexRange group;
        for (const AccessClass& accessClass : m_classes) {
            if (!group.isEmpty() && accessClass.bits.end - group.first > maxBitsPerGroup) {
                solveGroup(group);
                group.first = accessClass.bits.first;
            }
            group.end = accessClass.bits.end;
        }
        solveGroup(group);
        bool hasSeveralGroups = group.first;
        if (hasSeveralGroups)
            m_context.gaveUp(LimitID::MaxReadsPerSolve);
        return applyReplacements();
    }
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
    auto isSame = [&](ValueID a, ValueID b) {
        return sameObjectThroughCasts(function, a) == sameObjectThroughCasts(function, b);
    };
    auto comesBetween = [](const Effects& effects) {
        return !effects.writes.isEmpty() || effects.mayRunOtherCode();
    };
    ValueSubstitution substitution(function);
    std::optional<IRBuilder> builder;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        auto numberOfMapSets = std::ranges::count_if(values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::CallBuiltin && isMapSet(program, function.at(id));
        });
        if (numberOfMapSets < 2)
            continue;
        BitVector erasedIndexes(values.size());
        ValueID last;
        for (size_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (isMapSet(program, value)) {
                if (last) {
                    BuiltinCall first = builtinCallOf(program, function.at(last));
                    BuiltinCall second = builtinCallOf(program, value);
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

// An object keeps the brand that it has taken. `this.#method()` behind another one, or behind the definition in the constructor, has nothing left to ask.
static PassOutcome dropRepeatedBrandTests(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::PrivateGet))
        return PassOutcome::Unchanged;

    struct Brand {
        ValueID object;
        ValueID token;
        LayoutID layout;
        uint32_t field;
        BlockID block;
    };
    const Program& program = context.program;
    Function& function = context.function;
    auto isBrandOperation = [&](const Value& value) {
        return (value.opcode == Opcode::PrivateGet || value.opcode == Opcode::PrivateDefine) && program.at(value.layout()).fields[value.field()].flags.contains(FieldFlags::IsBrand);
    };
    size_t numberOfBrandOperations = 0;
    for (const Block& block : function.blocks) {
        numberOfBrandOperations += std::ranges::count_if(block.values, [&](ValueID id) {
            return isBrandOperation(function.at(id));
        });
    }
    if (numberOfBrandOperations < 2)
        return PassOutcome::Unchanged;

    const Dominators& dominators = context.analyses.dominators();
    Vector<Brand, 8> taken;
    Vector<size_t, 16> numberTakenAtEntry;
    Vector<std::pair<BlockID, BitVector>> erased;
    ValueSubstitution substitution(function);
    dominators.walkTree([&](BlockID block) {
        numberTakenAtEntry.append(taken.size());
        const BlockValues& values = function.at(block).values;
        BitVector erasedIndexes;
        for (size_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (!isBrandOperation(value))
                continue;
            Brand brand { sameObjectThroughCasts(function, value.objectOperand()).valueInFrontOfFacts(), sameObjectThroughCasts(function, value.tokenOperand()).valueInFrontOfFacts(), value.layout(), value.field(), block };
            // What throws in the middle of a block goes to its handler, which the end of the block does not dominate.
            bool isTaken = std::ranges::any_of(taken, [&](const Brand& earlier) {
                return earlier.object == brand.object && earlier.token == brand.token && earlier.layout == brand.layout && earlier.field == brand.field
                    && (earlier.block == block || dominators.terminatorDominates(earlier.block, block));
            });
            if (!isTaken) {
                taken.append(brand);
                continue;
            }
            // The second definition throws.
            if (value.opcode == Opcode::PrivateDefine)
                continue;
            substitution.replace(values[index], value.tokenOperand());
            erasedIndexes.set(index);
        }
        if (!erasedIndexes.isEmpty())
            erased.append({ block, WTF::move(erasedIndexes) });
    }, [&](BlockID) {
        taken.shrink(numberTakenAtEntry.takeLast());
    });
    if (erased.isEmpty())
        return PassOutcome::Unchanged;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (auto& [block, erasedIndexes] : erased)
        builder.eraseValues(block, erasedIndexes);
    substitution.apply(function, &context.analyses);
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

static PassOutcome dropRepeatedInitializations(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::InitializeBuiltin))
        return PassOutcome::Unchanged;

    struct Initialization {
        BuiltinID builtin;
        BlockID block;
    };
    // Two names of one object are two members: each is added where it is first read.
    Function& function = context.function;
    Vector<BuiltinID, 8> initializedBuiltins;
    bool hasRepeatedBuiltin = false;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::InitializeBuiltin)
                continue;
            BuiltinID builtin = value.builtin();
            hasRepeatedBuiltin |= initializedBuiltins.contains(builtin);
            initializedBuiltins.append(builtin);
        }
    }
    if (!hasRepeatedBuiltin)
        return PassOutcome::Unchanged;

    const Dominators& dominators = context.analyses.dominators();
    Vector<Initialization, 8> done;
    Vector<size_t, 16> numberDoneAtEntry;
    Vector<std::pair<BlockID, BitVector>> erased;
    dominators.walkTree([&](BlockID block) {
        numberDoneAtEntry.append(done.size());
        const BlockValues& values = function.at(block).values;
        BitVector erasedIndexes;
        for (size_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::InitializeBuiltin)
                continue;
            BuiltinID builtin = value.builtin();
            bool isDone = std::ranges::any_of(done, [&](const Initialization& earlier) {
                return earlier.builtin == builtin && (earlier.block == block || dominators.terminatorDominates(earlier.block, block));
            });
            if (isDone)
                erasedIndexes.set(index);
            else
                done.append({ builtin, block });
        }
        if (!erasedIndexes.isEmpty())
            erased.append({ block, WTF::move(erasedIndexes) });
    }, [&](BlockID) {
        done.shrink(numberDoneAtEntry.takeLast());
    });
    if (erased.isEmpty())
        return PassOutcome::Unchanged;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (auto& [block, erasedIndexes] : erased)
        builder.eraseValues(block, erasedIndexes);
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

} // anonymous namespace

PassOutcome eliminateRedundantReads(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    if (context.analyses.opcodesOfValues().intersects(opcodesReadingAccessedHeaps))
        outcome = RedundantReadElimination { context }.run();
    return outcome | dropRepeatedMapSets(context) | dropRepeatedBrandTests(context) | dropRepeatedInitializations(context);
}

namespace {

constexpr AbstractHeapSet carriedHeaps = heapsOfVariables;
AOT_THRESHOLD(unsigned, maxCarriedVariablesPerLoop, 8);
AOT_THRESHOLD(unsigned, maxRoundsPerStep, 4);

struct VariableOfLoop {
    BlockID header;
    ValueID parameter;
    MemoryLocation location;
    ValueID base;
};

struct LoopEditor {
    FunctionPassContext& m_context;
    const Program& m_program { m_context.program };
    Function& m_function { m_context.function };
    FunctionAnalyses& m_analyses { m_context.analyses };

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_program.typeOf(m_function, value); }
    bool hasType(ValueID value, TypeID type) const { return typeOf(value).isSubsetOf(m_context.types.at(type), m_program); }
    // A test can prove less than the type it starts from: a built-in object that the tested type does not name passes as any member of a class.
    bool isNarrowedByTest(ValueID value, TypeID tested) const
    {
        const Type& given = typeOf(value);
        return given.castTo(m_context.types.at(tested), m_context.types).isSubsetOf(given, m_program);
    }
    TypePropagationInputs typingInputs() { return { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo }; }
    IRBuilder makeBuilder() { return IRBuilder(m_analyses.context(), m_function, m_context.types, &m_analyses); }

    // The cycle through the copy of a separated loop starts at a block that SeparateUnlikelyLoopPaths made, which has no hint.
    bool isHeaderOfLoopOfProgram(BlockID header) const
    {
        return std::ranges::any_of(m_function.at(header).values, [&](ValueID value) {
            return m_function.at(value).opcode == Opcode::LoopHint;
        });
    }

    Vector<BlockID, 8> headersFromInnermost()
    {
        const NaturalLoops& loops = m_analyses.naturalLoops();
        Vector<const NaturalLoop*, 8> sorted;
        for (unsigned index = 0; index < loops.numLoops(); ++index)
            sorted.append(&loops.loop(index));
        AOT::stableSort(sorted, { }, &NaturalLoop::size);
        Vector<BlockID, 8> headers;
        for (const NaturalLoop* loop : sorted)
            headers.append(loop->header());
        return headers;
    }

    bool dominatesAll(BlockID block, std::span<const BlockID> latches)
    {
        const Dominators& dominators = m_analyses.dominators();
        return std::ranges::all_of(latches, [&](BlockID latch) {
            return dominators.dominates(block, latch);
        });
    }

    Terminator takeTerminator(IRBuilder& builder, BlockID block)
    {
        Terminator terminator = std::exchange(m_function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator { });
        builder.moveToEndOf(block);
        return terminator;
    }

    ValueID insertCopy(IRBuilder& builder, ProgramPoint point, ValueID value)
    {
        Value copy = IRBuilder::make(Opcode::Upcast, { value });
        copy.setCastType(m_function.at(value).type);
        return builder.insertBefore(point, WTF::move(copy));
    }

    // The first definition of each list stands where the others are used.
    void repair(IRBuilder& builder, std::span<const Vector<ValueID, 4>> definitionsOfVariables)
    {
        m_analyses.didChangeControlFlow();
        SSARepair repair(builder, m_analyses);
        for (const Vector<ValueID, 4>& definitions : definitionsOfVariables) {
            if (definitions.size() == 1)
                continue;
            SSARepair::Variable repaired = repair.addVariable(definitions.first());
            for (ValueID definition : definitions.span().subspan(1))
                repair.addDefinition(repaired, definition);
        }
        repair.run();
        m_analyses.didChangeControlFlow();
    }
};

// A loop that reads a variable of an enclosing function or of a module in every turn reads what it stored or read in the turn before.
struct VariableCarrier : LoopEditor {
    struct VariableAccess {
        MemoryLocation location;
        ValueID base;
        ValueID id;
        ValueID stored;
        ProgramPoint point;
    };

    struct CarriedVariable {
        MemoryLocation location;
        ValueID base;
        Vector<ValueID, 4> reads { };
        Vector<VariableAccess, 2> stores { };
        Vector<BlockID, 2> blocksOfOtherWrites { };
        Expected<bool> isReadInEveryTurn { false };
        bool isWrittenOnLeavingPath { false };
        // Whoever gets to the end of the block ahead of the loop has read the variable or is about to, whichever way the tests turn out.
        bool isReadWheneverLoopIsEntered { false };
        // The read ahead of the loop is a copy of one of the reads, and names what that one names.
        bool isReadThroughCastInLoop { false };
    };

    // False for a loop that is entered from its copy, behind a whole turn: what every turn reads has been read when it is read ahead of the loop.
    bool m_firstTurnRunsHere { true };
    // The copy of such a loop.
    const RegionCloner* m_copy { nullptr };
    BitVector m_keptReads { m_context.build.validates() ? findReadsKeptForValidation(m_function) : BitVector() };
    BitVector m_stays { };
    Vector<CarriedVariable, 8> m_variables { };
    Vector<VariableOfLoop> m_variablesWithStores { };

    // False: blocks and values have other numbers than before.
    bool retype()
    {
        simplifyCasts(m_context);
        if (typeChangedValues(typingInputs()).cutBlocks)
            return false;
        simplifyCasts(m_context);
        return true;
    }

    // In every loop that no path has to leave for it: the variables that the first turn reads before the loop can end. What is read ahead of an inner loop is a read of the loop around it.
    PassOutcome run()
    {
        BitVector isHeaderOfChangedLoop;
        for (BlockID header : headersFromInnermost()) {
            BitVector leaves;
            if (carryAround(header, leaves))
                isHeaderOfChangedLoop.set(header.index());
        }
        if (isHeaderOfChangedLoop.isEmpty())
            return PassOutcome::Unchanged;

        for (unsigned round = 0; round < maxRoundsPerStep && retype(); ++round) {
            bool moved = false;
            for (BlockID header : headersFromInnermost()) {
                const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
                if (loop && isHeaderOfChangedLoop.get(header.index()))
                    moved |= moveLeadingTestsAheadOf(*loop);
            }
            if (!moved) {
                if (delayStores(m_variablesWithStores.span()))
                    retype();
                break;
            }
        }
        return PassOutcome::Changed;
    }

    bool isReadBy(const CarriedVariable& variable, Opcode opcode)
    {
        const UseLists& uses = m_analyses.useLists();
        auto isUsedBy = [&](ValueID value) {
            return std::ranges::any_of(uses.usesOf(value), [&](const Use& use) {
                return use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == opcode;
            });
        };
        // A read that gives a word which may be anything is looked at through the cast that names the fact.
        return std::ranges::any_of(variable.reads, [&](ValueID read) {
            return isUsedBy(read) || std::ranges::any_of(uses.usesOf(read), [&](const Use& use) {
                return use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == Opcode::AssumeType && m_function.at(use.user).factOperand() && isUsedBy(use.user);
            });
        });
    }

    // Two environments of one layout may be one object: a store through another one counts as a write by something else.
    void findVariables(const NaturalLoop& loop, const BitVector& leaves)
    {
        m_stays = BitVector(m_function.blocks.size());
        m_variables.shrink(0);
        Vector<VariableAccess, 16> accesses;
        Vector<std::pair<BlockID, LocationMasks>, 8> otherWrites;
        LocationMasks writesOnLeavingPaths;
        Vector<BlockID, 8> leavingBlocksThatWrite;
        auto writesOf = [](const EffectsOnEdges& effects) {
            Effects any = effects.onAnyEdge();
            return (any.mayRunOtherCode() ? LocationMasks::top() : any.writes).restrictedTo(carriedHeaps);
        };
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID block = loop.at(position);
            bool stays = !leaves.get(block.index());
            if (stays)
                m_stays.set(block.index());
            LocationMasks writes;
            const BlockValues& values = m_function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                ValueID id = values[index];
                MemoryAccess access = memoryAccessOf(m_function, m_function.at(id));
                if (stays && access && carriedHeaps.contains(access.location.heap()) && !access.location.isEntireHeap()) {
                    // A cast that names a fact says from where on the word may be looked at: what stands in front of it is another base.
                    ValueID base = access.base ? sameObjectBehindFacts(m_function, access.base) : ValueID();
                    accesses.append(VariableAccess { access.location, base, id, access.stored, { block, index } });
                    continue;
                }
                writes.merge(writesOf(m_analyses.effectsOf(id)));
            }
            writes.merge(writesOf(m_analyses.effectsOfTerminator(block)));
            if (writes.isEmpty())
                continue;
            if (stays)
                otherWrites.append({ block, writes });
            else {
                writesOnLeavingPaths.merge(writes);
                leavingBlocksThatWrite.append(block);
            }
        }

        const UseLists& useLists = m_analyses.useLists();
        const Dominators& dominators = m_analyses.dominators();
        const NaturalLoops& loops = m_analyses.naturalLoops();
        Vector<BlockID, 4> latches;
        for (Edge edge : m_analyses.cfg().incomingEdges(loop.header())) {
            if (m_stays.get(edge.from.index()))
                latches.append(edge.from);
        }
        if (latches.isEmpty())
            return;
        // The read ahead of the loop stands for the read of the first turn, so the first turn has to get as far: a loop may well end at its first test.
        BitVector runsInFirstTurn(m_function.blocks.size());
        // A path that is seldom taken does not count above. That a variable has been stored may hang on the very test that such a path begins with.
        std::optional<ProgramPoint> firstWayOut;
        BitVector beginsBeforeAnyWayOut(m_function.blocks.size());
        for (BlockID block = loop.header(); m_firstTurnRunsHere && !runsInFirstTurn.set(block.index());) {
            const BlockValues& values = m_function.at(block).values;
            if (!firstWayOut)
                beginsBeforeAnyWayOut.set(block.index());
            for (uint32_t index = 0; index < values.size() && !firstWayOut; ++index) {
                if (mayLeaveOrStoreToVariable(m_analyses.effectsOf(values[index]).onAnyEdge()))
                    firstWayOut = ProgramPoint { block, index };
            }
            BlockID next;
            bool mayEndHere = false;
            for (const Target& target : m_function.at(block).terminator.targets) {
                if (m_stays.get(target.block.index())) {
                    mayEndHere |= !!next;
                    next = target.block;
                    continue;
                }
                if (!firstWayOut)
                    firstWayOut = pointBeforeTerminatorOf(m_function, block);
                if (!loops.belongsTo(target.block, loop))
                    mayEndHere |= (m_function.at(target.block).frequency <= m_function.at(loop.header()).frequency).predicted();
            }
            if (mayEndHere || !next)
                break;
            block = next;
        }
        std::optional<Edge> entry = m_copy ? loops.findSingleEntry(loop) : std::nullopt;
        auto isReachedWheneverLoopIsEntered = [&](const VariableAccess& read) {
            if (typeOf(read.id).mayBeUnspecified())
                return false;
            BlockID block = read.point.block;
            // A turn of the copy that threw in front of the read and was caught inside the loop has passed the block and not the read.
            if (BlockID blockOfCopy = entry ? m_copy->cloneOf(block) : BlockID())
                return dominators.terminatorDominates(blockOfCopy, entry->from);
            return beginsBeforeAnyWayOut.get(block.index()) && (!firstWayOut || firstWayOut->block != block || read.point.index < firstWayOut->index);
        };

        for (const VariableAccess& access : accesses) {
            if (access.stored || m_keptReads.get(access.id.index()) || !typeOf(access.id).isJavaScriptValue())
                continue;
            if (access.base && loops.belongsTo(useLists.locationOf(access.base).block, loop))
                continue;
            CarriedVariable* variable = nullptr;
            for (CarriedVariable& candidate : m_variables) {
                if (candidate.location == access.location && candidate.base == access.base)
                    variable = &candidate;
            }
            if (!variable) {
                m_variables.append(CarriedVariable { access.location, access.base });
                variable = &m_variables.last();
            }
            variable->reads.append(access.id);
            if (ValueID namedBase = memoryAccessOf(m_function, m_function.at(access.id)).base)
                variable->isReadThroughCastInLoop |= loops.belongsTo(useLists.locationOf(namedBase).block, loop);
            variable->isReadInEveryTurn = variable->isReadInEveryTurn.predicted() || ((!m_firstTurnRunsHere || runsInFirstTurn.get(access.point.block.index())) && dominatesAll(access.point.block, latches.span()));
            variable->isReadWheneverLoopIsEntered |= isReachedWheneverLoopIsEntered(access);
        }

        // The closure of a declared function, for one: nobody stores to it while this function runs, or it is stored to once, ahead of every read.
        auto holdsOneValueForEveryRead = [&](const CarriedVariable& variable) {
            const Value& read = m_function.at(variable.reads.first());
            switch (stabilityOf(m_program, read, memoryAccessOf(m_function, read))) {
            case LocationStability::Unknown:
                return false;
            case LocationStability::UnchangedDuringActivation:
                return true;
            case LocationStability::ChangedByOwnStores:
                break;
            }
            if (m_program.at(read.type).primitives & Primitive::Absent)
                return false;
            return std::ranges::any_of(variable.reads, [&](ValueID candidate) {
                BlockID block = useLists.locationOf(candidate).block;
                auto comesBehindRead = [&](BlockID writer) {
                    return writer != block && dominators.dominates(block, writer);
                };
                return std::ranges::all_of(otherWrites, comesBehindRead, &std::pair<BlockID, LocationMasks>::first) && std::ranges::all_of(leavingBlocksThatWrite, comesBehindRead);
            });
        };
        for (CarriedVariable& variable : m_variables) {
            if (holdsOneValueForEveryRead(variable))
                continue;
            for (const VariableAccess& access : accesses) {
                if (!access.stored || overlapOf(m_program, access.location, variable.location) == LocationOverlap::None)
                    continue;
                if (access.location == variable.location && access.base == variable.base)
                    variable.stores.append(access);
                else
                    variable.blocksOfOtherWrites.append(access.point.block);
            }
            for (const auto& [block, writes] : otherWrites) {
                if (writes.overlaps(m_program, variable.location) && !m_context.build.isSabotaged(Sabotage::CarriedVariablesAreWrittenByOthers))
                    variable.blocksOfOtherWrites.append(block);
            }
            variable.isWrittenOnLeavingPath = writesOnLeavingPaths.overlaps(m_program, variable.location);
        }
        // A loop inside this one is where the time goes: without it this loop would turn for nothing.
        auto hasToStay = [&](BlockID block) {
            return dominatesAll(block, latches.span()) || loops.innerMostLoopOf(block) != &loop;
        };
        auto hasTypesOfReads = [&](const CarriedVariable& variable) {
            TypeID typeOfRead = m_function.at(variable.reads.first()).type;
            bool readsAgree = std::ranges::all_of(variable.reads, [&](ValueID read) {
                return m_function.at(read).type == typeOfRead;
            });
            return readsAgree && std::ranges::all_of(variable.stores, [&](const VariableAccess& store) {
                return typeOf(store.stored).isSubsetOf(m_program.at(typeOfRead), m_program);
            });
        };
        // HoistLoopInvariantReads moves a read that is all there is to a variable.
        m_variables.removeAllMatching([&](const CarriedVariable& variable) {
            if (variable.isReadThroughCastInLoop || !variable.isReadInEveryTurn.predicted() || !hasTypesOfReads(variable) || std::ranges::any_of(variable.blocksOfOtherWrites, hasToStay))
                return true;
            return variable.stores.isEmpty() && !variable.isWrittenOnLeavingPath && !isReadBy(variable, Opcode::TypeTest) && !isReadBy(variable, Opcode::GetCapture);
        });
    }

    // Adds the blocks that stay in a loop when `leaves` has left it and that may write a variable which every turn reads, other than by a store of the loop's own. False, and nothing is added: there is no such variable, or the loop no longer turns without them.
    bool letWritersOfVariablesLeave(const NaturalLoop& loop, BitVector& leaves)
    {
        findVariables(loop, leaves);
        if (m_variables.isEmpty())
            return false;
        BitVector writers(m_function.blocks.size());
        for (const CarriedVariable& variable : m_variables) {
            for (BlockID block : variable.blocksOfOtherWrites)
                writers.set(block.index());
        }

        BitVector isSeen(m_function.blocks.size());
        Vector<BlockID, 16> worklist { loop.header() };
        isSeen.set(loop.header().index());
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (const Target& target : m_function.at(block).terminator.targets) {
                if (target.block == loop.header()) {
                    leaves.merge(writers);
                    return true;
                }
                if (m_stays.get(target.block.index()) && !writers.get(target.block.index()) && !isSeen.set(target.block.index()))
                    worklist.append(target.block);
            }
        }
        return false;
    }

    // The environment of an inlined function comes to stand ahead of the loop once its closure is read there, and with it the variables in it can be carried.
    bool carryAround(BlockID header, BitVector& leaves)
    {
        bool changed = false;
        for (unsigned round = 0; round < maxRoundsPerStep; ++round) {
            const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
            if (!loop || !carryOnce(*loop, leaves))
                break;
            changed = true;
            loop = m_analyses.naturalLoops().headerOf(header);
            if (!loop || !hoistCaptures(*loop))
                break;
        }
        return changed;
    }

    // A variable that the loop only reads, and tests, goes around it as well: so it is tested where it enters.
    bool carryOnce(const NaturalLoop& loop, BitVector& leaves)
    {
        BlockID header = loop.header();
        if (m_function.at(header).isHandlerBlock() || m_function.at(header).frequency.predicted() != BlockFrequency::Normal || m_analyses.isEnteredFromUnreachableBlock(header))
            return false;
        bool isLoopOfProgram = isHeaderOfLoopOfProgram(header);
        std::optional<Edge> singleEntry = m_analyses.naturalLoops().findSingleEntry(loop);
        if (!isLoopOfProgram || !singleEntry || hasEdgeResults(m_function, *singleEntry))
            return false;
        Edge entry = *singleEntry;

        const CFG& cfg = m_analyses.cfg();
        Vector<Edge, 4> edgesBack;
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID block = loop.at(position);
            if (leaves.get(block.index()))
                continue;
            for (Edge edge : cfg.incomingEdges(block)) {
                bool comesBack = leaves.get(edge.from.index());
                if ((comesBack || block == header) && (edge.isHandlerEdge() || (block == header && hasEdgeResults(m_function, edge))))
                    return false;
                if (comesBack)
                    edgesBack.append(edge);
            }
        }

        findVariables(loop, leaves);
        m_variables.removeAllMatching([](const CarriedVariable& variable) {
            return !variable.blocksOfOtherWrites.isEmpty();
        });
        if (m_variables.isEmpty())
            return false;
        if (m_variables.size() > maxCarriedVariablesPerLoop) {
            AOT::stableSort(m_variables, std::ranges::greater { }, [](const CarriedVariable& variable) {
                return variable.reads.size();
            });
            m_variables.shrink(maxCarriedVariablesPerLoop);
        }

        Vector<bool, 8> goesAround;
        for (const CarriedVariable& variable : m_variables)
            goesAround.append(!variable.stores.isEmpty() || variable.isWrittenOnLeavingPath || isReadBy(variable, Opcode::TypeTest));
        Vector<Edge, 4> latchEdges;
        for (Edge edge : cfg.incomingEdges(header)) {
            if (edge != entry)
                latchEdges.append(edge);
        }

        Vector<FactAheadOfLoop, 8> facts;
        for (const CarriedVariable& variable : m_variables)
            facts.append(findFactAheadOfLoop(variable, pointBeforeTerminatorOf(m_function, entry.from)));

        IRBuilder builder = makeBuilder();
        BlockID preheader = entry.from;
        if (m_function.at(preheader).terminator.kind != TerminatorKind::Jump) {
            preheader = builder.splitEdge(entry);
            entry = Edge { preheader, 0 };
        }
        Vector<BlockID, 4> blocksBack;
        if (std::ranges::any_of(m_variables, &CarriedVariable::isWrittenOnLeavingPath)) {
            for (Edge edge : edgesBack) {
                BlockID block = builder.splitEdge(edge);
                blocksBack.append(block);
                leaves.set(block.index());
                for (Edge& latchEdge : latchEdges) {
                    if (latchEdge == edge)
                        latchEdge = Edge { block, 0 };
                }
            }
        }

        Vector<Vector<ValueID, 4>, 8> definitionsOfVariables(m_variables.size());
        for (size_t index = 0; index < m_variables.size(); ++index) {
            const CarriedVariable& variable = m_variables[index];
            Vector<ValueID, 4>& definitions = definitionsOfVariables[index];
            FactAheadOfLoop& fact = facts[index];
            ValueID copiedRead = fact.readWithFact ? fact.readWithFact : variable.reads.first();
            if (fact.enteredFunction) {
                Value entered = IRBuilder::make(Opcode::Entered);
                entered.setFunction(fact.enteredFunction);
                builder.setOrigin(m_function.at(copiedRead).origin());
                fact.fact = builder.insertBefore(pointBeforeTerminatorOf(m_function, preheader), WTF::move(entered));
            }
            auto insertRead = [&](ProgramPoint point) {
                Value read = m_function.at(copiedRead);
                if (fact.isNeeded) {
                    read.setFactOperand(fact.fact);
                    read.setMayPrecedeItsStore(fact.isMissing);
                }
                builder.setOrigin(read.origin());
                return builder.insertBefore(point, WTF::move(read));
            };
            ValueID first = insertRead(pointBeforeTerminatorOf(m_function, preheader));
            if (!goesAround[index]) {
                definitions.append(first);
                continue;
            }
            builder.setOrigin(m_function.at(first).origin());
            ValueID parameter = builder.addParameter(header, m_function.at(first).type);
            m_function.edit(parameter, ValueEdit::Type).setExpected(m_function.at(first).expected());
            builder.appendArgument(entry, first);
            for (Edge edge : latchEdges)
                builder.appendArgument(edge, parameter);
            definitions.append(parameter);
            if (!variable.stores.isEmpty())
                m_variablesWithStores.append({ header, parameter, variable.location, variable.base });
            for (const VariableAccess& store : variable.stores) {
                builder.setOrigin(m_function.at(store.id).origin());
                definitions.append(insertCopy(builder, { store.point.block, store.point.index + 1 }, store.stored));
            }
            if (!variable.isWrittenOnLeavingPath)
                continue;
            for (BlockID block : blocksBack)
                definitions.append(insertRead({ block, 0 }));
        }
        builder.applyInsertions();

        for (size_t index = 0; index < m_variables.size(); ++index) {
            ValueID definition = definitionsOfVariables[index].first();
            for (ValueID read : m_variables[index].reads) {
                if (!facts[index].isMissing || typeOf(read).mayBeUnspecified()) {
                    becomeUpcast(m_function, read, definition);
                    continue;
                }
                Value& value = m_function.edit(read, ValueEdit::Definition);
                ValueID factOfRead = value.factOperand();
                value.becomeAssumeType(definition, Proven::behindFact(value.type));
                value.setFactOperand(factOfRead);
            }
        }
        repair(builder, definitionsOfVariables.span());
        return true;
    }

    struct FactAheadOfLoop {
        bool isNeeded { false };
        bool isMissing { false };
        ValueID fact;
        FunctionID enteredFunction;
        ValueID readWithFact;
    };

    // The variable has its declared type ahead of the loop where a fact that one of the reads names is defined there, where one of them needs none, or where the first turn is sure to get to one of them.
    FactAheadOfLoop findFactAheadOfLoop(const CarriedVariable& variable, ProgramPoint aheadOfLoop)
    {
        // A read of an environment field names no fact: the copy of it ahead of the loop says what the read says.
        bool someReadLeansOnFact = std::ranges::any_of(variable.reads, [&](ValueID id) {
            return m_function.at(id).opcode == Opcode::GetModuleVariable && holdsDeclaredTypeOnlyBehindEntry(m_program, m_function, m_function.at(id));
        });
        if (!someReadLeansOnFact)
            return { };
        for (ValueID id : variable.reads) {
            const Value& read = m_function.at(id);
            if (read.mayPrecedeItsStore())
                continue;
            if (!read.factOperand() || m_analyses.dominates(read.factOperand(), aheadOfLoop))
                return { true, false, read.factOperand(), { }, id };
        }
        for (ValueID id : variable.reads) {
            const Value& read = m_function.at(id);
            if (variable.isReadWheneverLoopIsEntered && !read.mayPrecedeItsStore() && m_function.at(read.factOperand()).opcode == Opcode::Entered)
                return { true, false, { }, m_function.at(read.factOperand()).function(), id };
        }
        return { true, true, { }, { }, { } };
    }

    // The environment that an inlined function reads its variables from is what its closure has captured. With the closure read ahead of the loop, so is the environment.
    bool hoistCaptures(const NaturalLoop& loop)
    {
        std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
        if (!entry || m_function.at(entry->from).terminator.kind != TerminatorKind::Jump)
            return false;
        const UseLists& useLists = m_analyses.useLists();
        const NaturalLoops& loops = m_analyses.naturalLoops();

        Vector<ValueID, 4> hoisted;
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockValues& values = m_function.edit(loop.at(position), BlockEdit::Values).values;
            values.removeAllMatching([&](ValueID id) {
                Value& value = m_function.edit(id);
                if (value.opcode != Opcode::GetCapture)
                    return false;
                ValueID closure = m_analyses.definitionThroughUpcasts(value.operand(0));
                if (loops.belongsTo(useLists.locationOf(closure).block, loop))
                    return false;
                value.setOperand(0, closure);
                hoisted.append(id);
                return true;
            });
        }
        if (hoisted.isEmpty())
            return false;
        m_function.edit(entry->from, BlockEdit::Values).values.appendVector(hoisted);
        m_analyses.didChangeValues();
        return true;
    }

    struct LeadingTest {
        BlockID block;
        std::optional<size_t> parameterOfHeader;
    };

    // A test of what an earlier one of these tests has proven tests the same value.
    Vector<LeadingTest, 4> findLeadingTests(const NaturalLoop& loop)
    {
        const CFG& cfg = m_analyses.cfg();
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const UseLists& useLists = m_analyses.useLists();
        const Block& header = m_function.at(loop.header());
        auto isPassedOnWithType = [&](size_t parameterIndex, TypeID tested) {
            return std::ranges::all_of(cfg.incomingEdges(loop.header()), [&](Edge edge) {
                if (!loops.belongsTo(edge.from, loop))
                    return true;
                if (edge.isHandlerEdge() || parameterIndex >= targetOf(m_function, edge).arguments.size())
                    return false;
                ValueID argument = targetOf(m_function, edge).arguments[parameterIndex];
                return argument == header.parameters[parameterIndex] || typeOf(argument).isSubsetOf(m_program.at(tested), m_program);
            });
        };

        Vector<LeadingTest, 4> tests;
        Vector<ValueID, 4> proven;
        for (BlockID id = loop.header();;) {
            const Block& block = m_function.at(id);
            if (block.handler || block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.conditionOperand() != block.values.last())
                break;
            const Value& test = m_function.at(block.values.last());
            // A fact and the cast that names it go ahead of the loop with the test, behind the tests in front of them.
            auto isCastBehindFact = [&](ValueID value) {
                return m_function.at(value).opcode == Opcode::AssumeType && m_function.at(value).factOperand() && useLists.locationOf(value).block == id;
            };
            bool holdsNothingElse = std::ranges::all_of(block.values.span().first(block.values.size() - 1), [&](ValueID value) {
                Opcode opcode = m_function.at(value).opcode;
                return opcode == Opcode::LoopHint || makesFact(opcode) || isCastBehindFact(value);
            });
            if (test.opcode != Opcode::TypeTest || !holdsNothingElse || useLists.useCount(block.values.last()) != 1)
                break;
            const Target& taken = block.terminator.targets[0];
            const Target& notTaken = block.terminator.targets[1];
            if (!loops.belongsTo(taken.block, loop) || taken.block == loop.header() || !cfg.singleIncomingEdge(taken.block) || loops.belongsTo(notTaken.block, loop))
                break;

            ValueID tested = test.valueOperand();
            if (isCastBehindFact(tested))
                tested = m_function.at(tested).valueOperand();
            std::optional<size_t> parameterOfHeader;
            if (size_t position = proven.find(tested); position != notFound)
                parameterOfHeader = tests[position].parameterOfHeader;
            else if (size_t index = header.parameters.find(tested); index != notFound)
                parameterOfHeader = index;
            else if (loops.belongsTo(useLists.locationOf(tested).block, loop))
                break;
            if (parameterOfHeader && (!isPassedOnWithType(*parameterOfHeader, test.testedType()) || !isNarrowedByTest(header.parameters[*parameterOfHeader], test.testedType())))
                break;
            tests.append({ id, parameterOfHeader });
            proven.append(taken.proofs.isEmpty() ? ValueID() : m_function.at(taken.block).parameters.last());
            id = taken.block;
        }
        return tests;
    }

    bool moveLeadingTestsAheadOf(const NaturalLoop& loop)
    {
        BlockID header = loop.header();
        std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
        if (!entry || hasEdgeResults(m_function, *entry))
            return false;
        Vector<LeadingTest, 4> tests = findLeadingTests(loop);
        if (tests.isEmpty())
            return false;
        Vector<BlockID, 4> region;
        for (const LeadingTest& test : tests)
            region.append(test.block);
        if (RegionCloner::checkCanDuplicate(m_analyses, region.span()) != CloneCheck::CanClone)
            return false;

        IRBuilder builder = makeBuilder();
        RegionCloner cloner(builder, m_function);
        cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
        builder.redirectEdge(*entry, cloner.cloneOf(header), Arguments(targetOf(m_function, *entry).arguments));

        BlockID landing = builder.splitEdge(Edge { cloner.cloneOf(region.last()), 0 });
        Vector<ValueID, 4> provenAheadOfLoop;
        for (size_t position = 0; position < tests.size(); ++position) {
            const Target& taken = m_function.at(tests[position].block).terminator.targets[0];
            if (taken.proofs.isEmpty())
                provenAheadOfLoop.append(ValueID());
            else if (position + 1 == tests.size())
                provenAheadOfLoop.append(m_function.at(landing).parameters.last());
            else
                provenAheadOfLoop.append(cloner.cloneOf(m_function.at(taken.block).parameters.last()));
        }

        Arguments entering;
        for (ValueID parameter : m_function.at(header).parameters)
            entering.append(cloner.cloneOf(parameter));
        for (size_t position = 0; position < tests.size(); ++position) {
            if (tests[position].parameterOfHeader && provenAheadOfLoop[position])
                entering[*tests[position].parameterOfHeader] = provenAheadOfLoop[position];
        }
        builder.setOrigin(takeTerminator(builder, landing).origin());
        builder.emitJump(header, WTF::move(entering));

        for (size_t position = 0; position < tests.size(); ++position) {
            BlockID block = tests[position].block;
            if (tests[position].parameterOfHeader || !provenAheadOfLoop[position]) {
                builder.convertBranchToJump(block, 0);
                continue;
            }
            Terminator branch = takeTerminator(builder, block);
            Arguments arguments = WTF::move(branch.targets[0].arguments);
            for (size_t proof = 0; proof < branch.targets[0].proofs.size(); ++proof)
                arguments.append(provenAheadOfLoop[position]);
            builder.setOrigin(branch.origin());
            builder.emitJump(branch.targets[0].block, WTF::move(arguments));
        }

        SSARepair repair(builder, m_analyses);
        cloner.addVariablesTo(repair, m_analyses);
        builder.emptyUnreachableBlocks();
        m_analyses.didChangeControlFlow();
        repair.run();
        m_analyses.didChangeControlFlow();
        return true;
    }

    // For loops that have their final form, with the types they have in it.
    bool delayStores(std::span<const VariableOfLoop> variables)
    {
        bool changed = false;
        for (size_t first = 0; first < variables.size();) {
            size_t end = first + 1;
            while (end < variables.size() && variables[end].header == variables[first].header)
                ++end;
            if (const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(variables[first].header))
                changed |= delayStoresIn(*loop, variables.subspan(first, end - first));
            first = end;
        }
        return changed;
    }

    // At the head of a turn the parameter is what the variable should hold, behind a store it is what was stored.
    bool delayStoresIn(const NaturalLoop& loop, std::span<const VariableOfLoop> variables)
    {
        struct DelayedVariable {
            VariableOfLoop variable;
            Vector<ProgramPoint, 2> stores { };
            bool isSeenByOthers { false };
        };
        const NaturalLoops& loops = m_analyses.naturalLoops();
        BlockID header = loop.header();
        Vector<DelayedVariable, 4> delayed;
        for (const VariableOfLoop& variable : variables) {
            if (m_function.at(header).parameters.contains(variable.parameter))
                delayed.append({ variable });
        }

        Vector<Edge, 8> leavingEdges;
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID id = loop.at(position);
            const Block& block = m_function.at(id);
            if (block.handler || block.isHandlerBlock())
                return false;
            auto look = [&](Effects effects) {
                if ((effects.mayThrow || effects.mayRunOtherCode()) && !m_context.build.isSabotaged(Sabotage::DelayedStoresPassWhatMayThrow))
                    return false;
                for (DelayedVariable& candidate : delayed)
                    candidate.isSeenByOthers |= effects.reads.overlaps(m_program, candidate.variable.location) || effects.writes.overlaps(m_program, candidate.variable.location);
                return true;
            };
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                ValueID value = block.values[index];
                MemoryAccess access = memoryAccessOf(m_function, m_function.at(value));
                DelayedVariable* stored = nullptr;
                if (access.kind == MemoryAccess::Kind::Write) {
                    ValueID base = access.base ? sameObjectBehindFacts(m_function, access.base) : ValueID();
                    for (DelayedVariable& candidate : delayed) {
                        if (candidate.variable.location == access.location && candidate.variable.base == base)
                            stored = &candidate;
                    }
                }
                if (stored) {
                    stored->stores.append({ id, index });
                    continue;
                }
                Effects effects = m_analyses.effectsOf(value).onAnyEdge();
                if (opcodesThrowingOnlyForLackOfMemory.contains(m_function.at(value).opcode) && !effects.mayThrowByRule())
                    effects.mayThrow = false;
                if (!look(effects))
                    return false;
            }
            if (!look(m_analyses.effectsOfTerminator(id).onAnyEdge()))
                return false;
            for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
                if (!loops.belongsTo(block.terminator.targets[targetIndex].block, loop))
                    leavingEdges.append(Edge { id, targetIndex });
            }
        }
        delayed.removeAllMatching([](const DelayedVariable& candidate) {
            return candidate.isSeenByOthers || candidate.stores.isEmpty();
        });
        if (delayed.isEmpty() || leavingEdges.isEmpty())
            return false;

        IRBuilder builder = makeBuilder();
        Vector<BlockID, 8> blocksOnLeavingEdges;
        for (Edge edge : leavingEdges)
            blocksOnLeavingEdges.append(builder.splitEdge(edge));

        Vector<Vector<ValueID, 4>, 4> definitionsOfVariables(delayed.size());
        Vector<ValueID, 4> delayedStores;
        for (size_t index = 0; index < delayed.size(); ++index) {
            const DelayedVariable& candidate = delayed[index];
            Vector<ValueID, 4>& definitions = definitionsOfVariables[index];
            Value store = m_function.at(m_function.at(candidate.stores.first().block).values[candidate.stores.first().index]);
            ValueID storedBySample = memoryAccessOf(m_function, store).stored;
            builder.setOrigin(store.origin());
            definitions.append(insertCopy(builder, { header, 0 }, candidate.variable.parameter));
            for (ProgramPoint point : candidate.stores) {
                ValueID delayedStore = m_function.at(point.block).values[point.index];
                definitions.append(insertCopy(builder, { point.block, point.index + 1 }, memoryAccessOf(m_function, m_function.at(delayedStore)).stored));
                delayedStores.append(delayedStore);
            }
            for (BlockID block : blocksOnLeavingEdges) {
                if (m_context.build.isSabotaged(Sabotage::DelayedStoresAreLeftOut))
                    break;
                Value storeOnEdge = store;
                for (uint32_t operand = 0; operand < storeOnEdge.numberOfOperands(); ++operand) {
                    if (storeOnEdge.operand(operand) == storedBySample)
                        storeOnEdge.setOperand(operand, definitions.first());
                }
                builder.insertBefore({ block, 0 }, WTF::move(storeOnEdge));
            }
        }
        builder.applyInsertions();
        for (ValueID store : delayedStores)
            becomeUndefined(m_function, store);
        repair(builder, definitionsOfVariables.span());
        return true;
    }
};

// An edge of a hot loop to an unlikely path that runs other code or writes leaves for the same place in a copy of the loop, so the loop keeps the types of its likely paths.
struct LoopPathSeparator : LoopEditor {
    struct SeparationPlan {
        BlockID header;
        Vector<BlockID> region { };
        Vector<Edge> leavingEdges { };
        Vector<BlockID> entriesOfCopy { };
        Vector<TypeID> typesOfCarriedValues { };
        BitVector carriedValuesThatMayBeUnspecified { };
        bool letsVariablesBeCarried { false };
    };

    Vector<TypeID> m_rememberedTypes { };
    BitVector m_isFinishedHeader { };
    BitVector m_isSeparatedHeader { };
    Vector<VariableOfLoop> m_variablesWithStores { };
    bool m_likelyPathCannotLeave { false };
    bool m_pathThatOnlyComputesLeaves { false };
    bool m_wayBackWidensLoopTypes { false };

    enum class PathsThatOnlyCompute : uint8_t { LeaveWhereTheyWidenParameters, Stay };

    PassOutcome run()
    {
        // Inner loops come first. A loop around a separated loop stays as it is: its copy would hold both copies of the inner loop again.
        auto findPlan = [&]() -> std::optional<SeparationPlan> {
            for (BlockID header : headersFromInnermost()) {
                if (m_isFinishedHeader.set(header.index()))
                    continue;
                const NaturalLoop& loop = *m_analyses.naturalLoops().headerOf(header);
                auto planWith = [&](PathsThatOnlyCompute pathsThatOnlyCompute) {
                    m_likelyPathCannotLeave = false;
                    std::optional<SeparationPlan> plan = planFor(loop, true, pathsThatOnlyCompute);
                    if (!plan && m_likelyPathCannotLeave)
                        plan = planFor(loop, false, pathsThatOnlyCompute);
                    return plan;
                };
                m_pathThatOnlyComputesLeaves = false;
                std::optional<SeparationPlan> plan = planWith(PathsThatOnlyCompute::LeaveWhereTheyWidenParameters);
                // Where the loop cannot be copied with an entry on such a path, the paths that run code or write still leave.
                if (!plan && m_pathThatOnlyComputesLeaves)
                    plan = planWith(PathsThatOnlyCompute::Stay);
                if (plan)
                    return plan;
            }
            return std::nullopt;
        };
        unsigned numberOfSeparatedLoops = 0;
        while (std::optional<SeparationPlan> plan = findPlan()) {
            if (m_context.exceeds(LimitID::MaxSeparatedLoopsPerFunction, numberOfSeparatedLoops + 1))
                break;
            separate(*plan);
            ++numberOfSeparatedLoops;
        }
        if (!numberOfSeparatedLoops)
            return PassOutcome::Unchanged;
        if (!m_variablesWithStores.isEmpty()) {
            simplifyCasts(m_context);
            if (VariableCarrier { { m_context } }.delayStores(m_variablesWithStores.span())) {
                m_rememberedTypes.clear();
                retypeValues();
            }
        }
        finishTypedValues(typingInputs());
        m_context.build.statistics().count(StatisticID::LoopsSeparatedFromUnlikelyPaths, numberOfSeparatedLoops);
        return PassOutcome::Changed;
    }

    // A value that was made since the last call has the type it was made with: a clone that of its original, a parameter what its maker gave it.
    void rememberTypes()
    {
        for (size_t index = m_rememberedTypes.size(); index < m_function.values.size(); ++index)
            m_rememberedTypes.append(m_function.values[index].type);
    }

    void restoreRememberedTypes()
    {
        rememberTypes();
        for (size_t index = 0; index < m_rememberedTypes.size(); ++index) {
            if (m_function.values[index].type != m_rememberedTypes[index])
                m_function.edit(ValueID(index), ValueEdit::Type).type = m_rememberedTypes[index];
        }
    }

    // Cutting the code behind a value of type never renumbers blocks and values, so it waits until the last loop is done.
    void retypeValues()
    {
        restoreRememberedTypes();
        typeValuesWithoutCutting(typingInputs());
        m_analyses.didChangeValues();
    }

    Expected<BitVector> findUnlikelyBlocks(const NaturalLoop& loop)
    {
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        Expected<BlockFrequency> frequencyOfLoop = frequencies.classOf(loop.header());
        BitVector isUnlikely(m_function.blocks.size());
        for (unsigned index = 0; index < loop.size(); ++index) {
            if ((frequencies.classOf(loop.at(index)) > frequencyOfLoop).predicted())
                isUnlikely.set(loop.at(index).index());
        }
        return isUnlikely;
    }

    // What may run out of memory calls into the run time, which is no computation, though it goes away with an unused result.
    bool onlyComputes(const Block& block)
    {
        bool valuesOnlyCompute = std::ranges::all_of(block.values, [&](ValueID value) {
            return m_analyses.effectsOf(value).onAnyEdge().isUnobservable();
        });
        return valuesOnlyCompute && effectsOf(m_analyses.effectContext(), m_function, block.terminator).onAnyEdge().isUnobservable();
    }

    // An unlikely block leaves with every unlikely block that leads to it or follows it: a path is in one copy from the edge that enters it to the edge that joins the likely path.
    BitVector findLeavingBlocks(const BitVector& isInLoop, const BitVector& isUnlikely, NOESCAPE const Invocable<bool(BlockID)> auto& leavesByItself)
    {
        BitVector leaves(m_function.blocks.size());
        Vector<BlockID, 16> worklist;
        for (size_t index : isUnlikely) {
            if (leavesByItself(BlockID(index)) && !leaves.set(index))
                worklist.append(BlockID(index));
        }

        auto visit = [&](BlockID neighbor) {
            if (isInLoop.get(neighbor.index()) && isUnlikely.get(neighbor.index()) && !leaves.set(neighbor.index()))
                worklist.append(neighbor);
        };
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (Edge edge : m_analyses.cfg().incomingEdges(block))
                visit(edge.from);
            for (const Target& target : m_function.at(block).terminator.targets)
                visit(target.block);
        }
        return leaves;
    }

    // The types that the parameters of the staying blocks have once the leaving paths pass them nothing.
    Vector<std::optional<Type>> typeParametersWithout(const BitVector& isInLoop, const BitVector& leaves)
    {
        Vector<BlockID, 16> blocksWithParameters;
        Vector<std::optional<Type>> typeWithoutLeavingPaths(m_function.values.size());
        for (size_t index : isInLoop) {
            if (leaves.get(index) || m_function.blocks[index].parameters.isEmpty())
                continue;
            blocksWithParameters.append(BlockID(index));
            for (ValueID parameter : m_function.blocks[index].parameters)
                typeWithoutLeavingPaths[parameter.index()] = Type::never();
        }

        auto typeOnStayingEdge = [&](ValueID value) -> const Type& {
            const std::optional<Type>& narrowed = typeWithoutLeavingPaths[value.index()];
            return narrowed ? *narrowed : typeOf(value);
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (BlockID block : blocksWithParameters) {
                const Block& contents = m_function.at(block);
                for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
                    if (leaves.get(edge.from.index()))
                        continue;
                    const Arguments& arguments = targetOf(m_function, edge).arguments;
                    for (size_t position = 0; position < contents.parameters.size(); ++position) {
                        ValueID parameter = contents.parameters[position];
                        // An edge result has the type of its parameter.
                        const Type& incoming = !edge.isHandlerEdge() && position < arguments.size() ? typeOnStayingEdge(arguments[position]) : typeOf(parameter);
                        Type& known = *typeWithoutLeavingPaths[parameter.index()];
                        if (incoming.isSubsetOf(known, m_program))
                            continue;
                        known = known.unionWith(incoming, m_context.types);
                        changed = true;
                    }
                }
            }
        }
        return typeWithoutLeavingPaths;
    }

    // Whether the parameter is one object that the likely paths make but for what a leaving path passes: by itself the object need not be made.
    bool isAllocationButForLeavingPaths(BlockID block, size_t position, const BitVector& leaves)
    {
        constexpr OpcodeSet allocations = opcodesInterpretedAs(Interpretation::Allocation);
        ValueID passedOnLikelyPaths;
        bool isPassedByLeavingPath = false;
        for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
            const Arguments& arguments = targetOf(m_function, edge).arguments;
            if (edge.isHandlerEdge() || position >= arguments.size())
                return false;
            if (leaves.get(edge.from.index())) {
                isPassedByLeavingPath = true;
                continue;
            }
            if (passedOnLikelyPaths && passedOnLikelyPaths != arguments[position])
                return false;
            passedOnLikelyPaths = arguments[position];
        }
        return isPassedByLeavingPath && passedOnLikelyPaths && allocations.contains(m_function.at(passedOnLikelyPaths).opcode);
    }

    bool leavesAllocationAlone(const BitVector& isInLoop, const BitVector& leaves)
    {
        for (size_t index : isInLoop) {
            if (leaves.get(index))
                continue;
            for (size_t position = 0; position < m_function.blocks[index].parameters.size(); ++position) {
                if (isAllocationButForLeavingPaths(BlockID(index), position, leaves))
                    return true;
            }
        }
        return false;
    }

    // The copy pays where an operation reads a parameter that is wider only for what a leaving path passes.
    bool narrowsParameter(const BitVector& isInLoop, const BitVector& leaves)
    {
        Vector<std::optional<Type>> typeWithoutLeavingPaths = typeParametersWithout(isInLoop, leaves);
        const UseLists& uses = m_analyses.useLists();
        for (size_t index = 0; index < typeWithoutLeavingPaths.size(); ++index) {
            if (!typeWithoutLeavingPaths[index] || typeOf(ValueID(index)).isSubsetOf(*typeWithoutLeavingPaths[index], m_program))
                continue;
            bool isRead = std::ranges::any_of(uses.usesOf(ValueID(index)), [](const Use& use) {
                return use.kind != Use::Kind::EdgeArgument;
            });
            if (isRead)
                return true;
        }
        return false;
    }

    // A test of every turn that asks about what was there ahead of the loop has one answer once nothing that stays in the loop writes what it reads. The back end asks it ahead of the loop where the failing path does not come back into the loop.
    bool leavesTestWithOneAnswer(const BitVector& isInLoop, const BitVector& leaves)
    {
        auto stays = [&](BlockID block) {
            return isInLoop.get(block.index()) && !leaves.get(block.index());
        };
        LocationMasks writes;
        bool runsOtherCode = false;
        auto add = [&](const Effects& effects) {
            runsOtherCode |= effects.mayRunOtherCode();
            writes.merge(effects.writes);
        };
        for (size_t index : isInLoop) {
            if (leaves.get(index))
                continue;
            for (ValueID value : m_function.blocks[index].values)
                add(m_analyses.effectsOf(value).onAnyEdge());
            add(m_analyses.effectsOfTerminator(BlockID(index)).onAnyEdge());
        }
        if (runsOtherCode)
            return false;

        BitVector changesBetweenTurns(m_function.values.size());
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (!stays(block))
                continue;
            const Block& contents = m_function.at(block);
            for (ValueID parameter : contents.parameters)
                changesBetweenTurns.set(parameter.index());
            for (ValueID id : contents.values) {
                Effects effects = m_analyses.effectsOf(id).onAnyEdge();
                bool changes = !effects.isUnobservable() || effects.reads.overlaps(writes);
                m_function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                    changes |= changesBetweenTurns.get(operand.index());
                });
                if (changes)
                    changesBetweenTurns.set(id.index());
            }
            const Terminator& terminator = contents.terminator;
            if (terminator.kind != TerminatorKind::Branch || !contents.values.contains(terminator.conditionOperand()) || changesBetweenTurns.get(terminator.conditionOperand().index()))
                continue;
            if (m_analyses.effectsOf(terminator.conditionOperand()).onAnyEdge().reads.isEmpty())
                continue;
            if (std::ranges::any_of(terminator.targets, [&](const Target& target) { return leaves.get(target.block.index()); }))
                return true;
        }
        return false;
    }

    // A loop that is only expected to run earns its copy when a number gets another representation on the likely paths and a value that is computed from itself in every iteration depends on it: the next iteration waits for that arithmetic. An integer costs the same in either width.
    bool changesRepresentationOfLoopCarriedNumber(BlockID header, const BitVector& isInLoop, const BitVector& leaves)
    {
        const UseLists& uses = m_analyses.useLists();
        auto stays = [&](BlockID block) {
            return isInLoop.get(block.index()) && !leaves.get(block.index());
        };
        auto findDependents = [&](ValueID value) {
            BitVector dependents(m_function.values.size());
            Vector<ValueID, 16> worklist { value };
            auto visit = [&](ValueID dependent) {
                if (!dependents.set(dependent.index()))
                    worklist.append(dependent);
            };
            while (!worklist.isEmpty()) {
                for (const Use& use : uses.usesOf(worklist.takeLast())) {
                    if (!stays(use.block))
                        continue;
                    if (use.kind == Use::Kind::Operand)
                        visit(use.user);
                    else if (use.kind == Use::Kind::EdgeArgument && stays(targetOf(m_function, use.edge()).block))
                        visit(m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex]);
                    else if (use.kind == Use::Kind::TerminatorOperand) {
                        // What a test proves about its operand is that operand.
                        const Terminator& terminator = m_function.at(use.block).terminator;
                        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                            if (!stays(terminator.targets[targetIndex].block))
                                continue;
                            for (ValueID result : m_function.at(terminator.targets[targetIndex].block).parameters.span().last(numberOfEdgeResults(m_function, use.block, targetIndex)))
                                visit(result);
                        }
                    }
                }
            }
            return dependents;
        };

        BitVector isComputedFromItself(m_function.values.size());
        for (ValueID carried : m_function.at(header).parameters) {
            if (findDependents(carried).get(carried.index()))
                isComputedFromItself.set(carried.index());
        }
        if (isComputedFromItself.isEmpty())
            return false;
        Vector<std::optional<Type>> typeWithoutLeavingPaths = typeParametersWithout(isInLoop, leaves);
        for (size_t index = 0; index < typeWithoutLeavingPaths.size(); ++index) {
            const std::optional<Type>& narrowed = typeWithoutLeavingPaths[index];
            const Type& type = typeOf(ValueID(index));
            if (!narrowed || !narrowed->isNumber() || type.isInteger() || defaultRepresentationOf(type) == defaultRepresentationOf(*narrowed))
                continue;
            BitVector dependents = findDependents(ValueID(index));
            dependents.filter(isComputedFromItself);
            if (!dependents.isEmpty())
                return true;
        }
        return false;
    }

    // What the likely paths make of a parameter that gets narrower may get narrower with it.
    BitVector findValuesMadeOfNarrowedParameters(const BitVector& isInLoop, const BitVector& leaves, const Vector<std::optional<Type>>& typeWithoutLeavingPaths)
    {
        BitVector isMadeOfNarrowedParameter(m_function.values.size());
        Vector<ValueID, 16> worklist;
        for (size_t index = 0; index < typeWithoutLeavingPaths.size(); ++index) {
            if (typeWithoutLeavingPaths[index] && !typeOf(ValueID(index)).isSubsetOf(*typeWithoutLeavingPaths[index], m_program))
                worklist.append(ValueID(index));
        }
        auto stays = [&](BlockID block) {
            return isInLoop.get(block.index()) && !leaves.get(block.index());
        };
        const UseLists& uses = m_analyses.useLists();
        while (!worklist.isEmpty()) {
            for (const Use& use : uses.usesOf(worklist.takeLast())) {
                if (!stays(use.block))
                    continue;
                ValueID made;
                if (use.kind == Use::Kind::Operand)
                    made = use.user;
                else if (use.kind == Use::Kind::EdgeArgument && stays(targetOf(m_function, use.edge()).block))
                    made = m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex];
                if (made && !isMadeOfNarrowedParameter.set(made.index()))
                    worklist.append(made);
            }
        }
        return isMadeOfNarrowedParameter;
    }

    // A path that only computes leaves too where a parameter of the likely paths is wider for what it passes, or is an object of theirs but for that.
    BitVector findLeavingBlocks(const NaturalLoop& loop, const BitVector& isInLoop, PathsThatOnlyCompute pathsThatOnlyCompute)
    {
        BitVector isUnlikely = findUnlikelyBlocks(loop).predicted();
        BitVector leaves = findLeavingBlocks(isInLoop, isUnlikely, [&](BlockID block) {
            return !onlyComputes(m_function.at(block));
        });
        if (pathsThatOnlyCompute == PathsThatOnlyCompute::Stay || leaves == isUnlikely)
            return leaves;
        Vector<std::optional<Type>> typeOnLikelyPaths = typeParametersWithout(isInLoop, isUnlikely);
        BitVector mayGetNarrower = findValuesMadeOfNarrowedParameters(isInLoop, isUnlikely, typeOnLikelyPaths);
        BitVector leavesWithPathsThatOnlyCompute = findLeavingBlocks(isInLoop, isUnlikely, [&](BlockID block) {
            if (leaves.get(block.index()))
                return true;
            return std::ranges::any_of(m_function.at(block).terminator.targets, [&](const Target& target) {
                const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
                for (size_t position = 0; position < target.arguments.size(); ++position) {
                    const std::optional<Type>& likely = typeOnLikelyPaths[parameters[position].index()];
                    if (!likely)
                        continue;
                    const std::optional<Type>& passed = typeOnLikelyPaths[target.arguments[position].index()];
                    if (mayGetNarrower.get(parameters[position].index()) || !(passed ? *passed : typeOf(target.arguments[position])).isSubsetOf(*likely, m_program))
                        return true;
                    if (isAllocationButForLeavingPaths(target.block, position, isUnlikely))
                        return true;
                }
                return false;
            });
        });
        m_pathThatOnlyComputesLeaves = leavesWithPathsThatOnlyCompute != leaves;
        return leavesWithPathsThatOnlyCompute;
    }

    std::optional<SeparationPlan> planFor(const NaturalLoop& loop, bool mayCarryVariables, PathsThatOnlyCompute pathsThatOnlyCompute)
    {
        BlockID header = loop.header();
        // What is hot is a share of the program, so other code decides it. A loop that is expected to run is copied too when it is small and the copy shortens its loop-carried arithmetic.
        bool isHot = isHotLoop(m_context, loop).predicted();
        if (!isHot && (m_analyses.blockFrequencies().classOf(header).predicted() != BlockFrequency::Normal || isCold(heatClassOf(m_context, header)).predicted()))
            return std::nullopt;
        if (!isHeaderOfLoopOfProgram(header))
            return std::nullopt;

        const CompactList<ValueID>& parameters = m_function.at(header).parameters;
        SeparationPlan plan { header };
        for (ValueID parameter : parameters)
            plan.typesOfCarriedValues.append(m_function.at(parameter).type);
        BitVector isInLoop(m_function.blocks.size());
        uint64_t bytes = 0;
        uint64_t bytesOfLikelyPaths = 0;
        for (unsigned index = 0; index < loop.size(); ++index) {
            BlockID block = loop.at(index);
            if (m_isSeparatedHeader.get(block.index()))
                return std::nullopt;
            isInLoop.set(block.index());
            plan.region.append(block);
            uint64_t bytesOfBlock = estimatedCodeBytes(m_program, m_function, m_function.at(block));
            bytes += bytesOfBlock;
            if (m_analyses.blockFrequencies().classOf(block).predicted() == BlockFrequency::Normal)
                bytesOfLikelyPaths += bytesOfBlock;
        }
        // The likely paths end up in the program twice, and the longer an iteration is, the less of it waits for one number.
        bool isShort = bytesOfLikelyPaths <= m_context.limit(LimitID::MaxBytesOfSeparatedLoop) / 8;
        if (!isHot && !isShort)
            return std::nullopt;

        BitVector leaves = findLeavingBlocks(loop, isInLoop, pathsThatOnlyCompute);
        plan.letsVariablesBeCarried = isHot && mayCarryVariables && VariableCarrier { { m_context }, false }.letWritersOfVariablesLeave(loop, leaves);
        if (leaves.isEmpty() || leaves.get(header.index()))
            return std::nullopt;
        plan.carriedValuesThatMayBeUnspecified = BitVector(parameters.size());
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (typeOf(parameters[index]).mayBeUnspecified())
                plan.carriedValuesThatMayBeUnspecified.set(index);
        }
        bool movesTest = isHot && std::ranges::any_of(findTestsOfEveryTurn(loop, leaves, plan.carriedValuesThatMayBeUnspecified), [](TypeID tested) {
            return !!tested;
        });
        if (!movesTest && !plan.letsVariablesBeCarried && !(isHot ? narrowsParameter(isInLoop, leaves) || (isShort && (leavesAllocationAlone(isInLoop, leaves) || leavesTestWithOneAnswer(isInLoop, leaves))) : changesRepresentationOfLoopCarriedNumber(header, isInLoop, leaves)))
            return std::nullopt;

        BitVector isEntryOfCopy(m_function.blocks.size());
        for (size_t index : leaves) {
            BlockID block(index);
            for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
                if (edge.isHandlerEdge())
                    return std::nullopt;
                if (leaves.get(edge.from.index()))
                    continue;
                // The copy is entered here from the loop and from its own blocks. A guard speaks of the operands of one test, so two of them do not join.
                uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
                bool passesGuard = std::ranges::any_of(m_function.at(block).parameters.span().last(numberOfResults), [&](ValueID result) {
                    return !typeOf(result).isJavaScriptValue();
                });
                if (passesGuard) {
                    m_likelyPathCannotLeave = plan.letsVariablesBeCarried;
                    return std::nullopt;
                }
                plan.leavingEdges.append(edge);
                if (!isEntryOfCopy.set(index))
                    plan.entriesOfCopy.append(block);
            }
        }
        if (plan.leavingEdges.isEmpty())
            return std::nullopt;
        // The way back enters the copy at its header where a test fails.
        plan.entriesOfCopy.append(header);

        if (m_context.exceeds(LimitID::MaxBytesOfSeparatedLoop, bytes))
            return std::nullopt;
        if (RegionCloner::checkCanDuplicate(m_analyses, plan.region.span(), plan.entriesOfCopy.span()) != CloneCheck::CanClone)
            return std::nullopt;
        return plan;
    }

    void separate(const SeparationPlan& plan)
    {
        m_rememberedTypes.clear();
        IRBuilder builder = makeBuilder();
        RegionCloner cloner(builder, m_function);
        cloner.cloneBlocks(plan.region.span(), [](ValueID, Value&) { });
        rememberTypes();
        // A copy that the loop is entered through runs as often as the loop did.
        for (BlockID block : plan.region) {
            if (!plan.letsVariablesBeCarried)
                m_function.edit(cloner.cloneOf(block), BlockEdit::Rest).frequency = m_function.at(block).frequency.predicted() == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
        }
        for (Edge edge : plan.leavingEdges) {
            const Target& target = targetOf(m_function, edge);
            builder.redirectEdge(edge, cloner.cloneOf(target.block), Arguments(target.arguments));
        }
        // A path that has left is unreachable in the loop.
        SSARepair repair(builder, m_analyses);
        cloner.addVariablesTo(repair, m_analyses);
        builder.emptyUnreachableBlocks();
        m_analyses.didChangeControlFlow();
        retypeValues();

        m_isSeparatedHeader.set(plan.header.index());
        BitVector isInCopy(m_function.blocks.size());
        for (BlockID block : cloner.clonedBlocks())
            isInCopy.set(block.index());
        m_isFinishedHeader.merge(isInCopy);
        if (!addWayBack(builder, plan, cloner.cloneOf(plan.header), isInCopy))
            m_context.build.statistics().count(StatisticID::LoopsWithoutWayBack);
        else if (plan.letsVariablesBeCarried) {
            if (m_wayBackWidensLoopTypes)
                retypeValues();
            carryVariables(builder, plan.header, cloner, isInCopy);
        }

        // A parameter that the repair adds holds what the definitions it joins hold.
        restoreRememberedTypes();
        repair.run();
        m_analyses.didChangeControlFlow();
        retypeValues();
    }

    // A value that goes around the loop and is tested in every turn, with the copy on the failing edge, is tested where it is made instead.
    Vector<TypeID> findTestsOfEveryTurn(const NaturalLoop& loop, const BitVector& leavesLoop, const BitVector& mayBeUnspecified)
    {
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const UseLists& uses = m_analyses.useLists();
        Vector<BlockID, 4> latches;
        for (Edge edge : m_analyses.cfg().incomingEdges(loop.header())) {
            if (loops.belongsTo(edge.from, loop))
                latches.append(edge.from);
        }

        const Block& header = m_function.at(loop.header());
        Vector<TypeID> tested(header.parameters.size());
        for (size_t index = 0; index < header.parameters.size(); ++index) {
            if (mayBeUnspecified.get(index))
                continue;
            for (const Use& use : uses.usesOf(header.parameters[index])) {
                if (use.kind != Use::Kind::Operand || m_function.at(use.user).opcode != Opcode::TypeTest || !loops.belongsTo(use.block, loop))
                    continue;
                const Terminator& terminator = m_function.at(use.block).terminator;
                if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != use.user || !leavesLoop.get(terminator.notTaken().block.index()))
                    continue;
                TypeID testedType = m_function.at(use.user).testedType();
                if (!dominatesAll(use.block, latches.span()) || hasType(header.parameters[index], testedType) || !isNarrowedByTest(header.parameters[index], testedType))
                    continue;
                tested[index] = testedType;
                break;
            }
        }
        return tested;
    }

    // The same for the variables that the loop carries, which are its last parameters. They came out of memory with the type of the variable, so there is more to find.
    Vector<TypeID> findTestsOfVariables(const NaturalLoop& loop, const BitVector& leavesLoop, size_t firstVariable)
    {
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const UseLists& uses = m_analyses.useLists();
        Vector<BlockID, 4> latches;
        for (Edge edge : m_analyses.cfg().incomingEdges(loop.header())) {
            if (loops.belongsTo(edge.from, loop) && !leavesLoop.get(edge.from.index()))
                latches.append(edge.from);
        }

        // What a test has proven, or a cast has passed on, is the same value: the narrowest of the types it is tested for in every turn is what the loop needs.
        const Block& header = m_function.at(loop.header());
        PropertyAccessContext accessContext { m_program, &m_context.inputs.flow };
        Vector<TypeID> tested(header.parameters.size());
        for (size_t index = firstVariable; index < header.parameters.size(); ++index) {
            if (typeOf(header.parameters[index]).mayBeUnspecified())
                continue;
            Vector<ValueID, 8> sameValues { header.parameters[index] };
            for (size_t position = 0; position < sameValues.size() && sameValues.size() < 16; ++position) {
                for (const Use& use : uses.usesOf(sameValues[position])) {
                    if (use.kind != Use::Kind::Operand || !loops.belongsTo(use.block, loop) || leavesLoop.get(use.block.index()))
                        continue;
                    const Value& user = m_function.at(use.user);
                    if (isCastThatProvesNothing(user.opcode) || user.opcode == Opcode::AssumeType) {
                        sameValues.append(use.user);
                        continue;
                    }
                    bool runsInEveryTurn = dominatesAll(use.block, latches.span());
                    auto isNarrower = [&](TypeID type) {
                        return !tested[index] || m_context.types.at(type).isSubsetOf(m_context.types.at(tested[index]), m_program);
                    };
                    if (user.opcode == Opcode::RequireObjectCoercible) {
                        // What comes out of it may have a type that no test can establish: proven contents, a range of integers.
                        const Type& given = typeOf(header.parameters[index]);
                        const Type& wanted = m_context.types.at(user.type);
                        Type testable = testableStoredTypeOf(accessContext, given, wanted, m_context.types);
                        if (!runsInEveryTurn || !given.isJavaScriptValue() || testable.isNever() || !given.castTo(testable, m_context.types).isSubsetOf(wanted, m_program))
                            continue;
                        TypeID testedType = m_context.types.intern(testable);
                        if (!isNarrower(testedType))
                            continue;
                        tested[index] = testedType;
                        sameValues.append(use.user);
                        continue;
                    }
                    if (user.opcode != Opcode::TypeTest || !runsInEveryTurn || !isNarrower(user.testedType()))
                        continue;
                    const Terminator& terminator = m_function.at(use.block).terminator;
                    if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != use.user)
                        continue;
                    BlockID whenFailed = terminator.notTaken().block;
                    if (!leavesLoop.get(whenFailed.index()) && loops.belongsTo(whenFailed, loop))
                        continue;
                    tested[index] = user.testedType();
                    const Target& taken = terminator.taken();
                    if (!taken.proofs.isEmpty() && m_analyses.cfg().singleIncomingEdge(taken.block))
                        sameValues.append(m_function.at(taken.block).parameters.last());
                }
            }
            if (tested[index] && (hasType(header.parameters[index], tested[index]) || !isNarrowedByTest(header.parameters[index], tested[index])))
                tested[index] = TypeID();
        }
        return tested;
    }

    bool passesTests(const Arguments& arguments, std::span<const TypeID> tested) const
    {
        for (size_t index = 0; index < tested.size(); ++index) {
            if (tested[index] && !hasType(arguments[index], tested[index]))
                return false;
        }
        return true;
    }

    // Emits at the end of the builder's block.
    void emitTests(IRBuilder& builder, const Arguments& carried, std::span<const TypeID> tested, BlockID whenPassed, BlockID whenFailed, Expected<BlockFrequency> whenPassedFrequency)
    {
        auto argumentsWhenFailed = [&] {
            Arguments arguments;
            arguments.append(carried.span().first(m_function.at(whenFailed).parameters.size()));
            return arguments;
        };
        Arguments narrowed = carried;
        bool isFirstTest = true;
        for (size_t index = 0; index < tested.size(); ++index) {
            if (!tested[index] || hasType(carried[index], tested[index]))
                continue;
            if (std::exchange(isFirstTest, false))
                builder.emitFailureOfPredictions(Target { whenFailed, argumentsWhenFailed(), { } });
            ValueID test = builder.appendTypeTest(carried[index], tested[index]);
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, whenPassedFrequency, m_function.at(whenFailed).frequency);
            narrowed[index] = branch.provenOnTaken;
            builder.moveToEndOf(branch.notTaken);
            builder.emitJump(whenFailed, argumentsWhenFailed());
            builder.moveToEndOf(branch.taken);
        }
        builder.emitJump(whenPassed, WTF::move(narrowed));
    }

    void testOnEdges(IRBuilder& builder, std::span<const Edge> edges, std::span<const TypeID> tested, BlockID header, BlockID headerOfCopy)
    {
        for (Edge edge : edges) {
            if (passesTests(targetOf(m_function, edge).arguments, tested))
                continue;
            BlockID tests = builder.splitEdge(edge);
            Arguments arguments = WTF::move(takeTerminator(builder, tests).next().arguments);
            emitTests(builder, arguments, tested, header, headerOfCopy, m_function.at(tests).frequency);
        }
    }

    // The values of the copy have no types yet: they get them with the repair. What goes around the copy has the types that it had in the loop before.
    bool addWayBack(IRBuilder& builder, const SeparationPlan& plan, BlockID headerOfCopy, const BitVector& isInCopy)
    {
        TypeArena& types = m_context.types;
        BlockID header = plan.header;
        size_t numberOfParameters = m_function.at(header).parameters.size();
        const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
        if (!loop || m_function.at(headerOfCopy).parameters.size() != numberOfParameters)
            return false;

        Vector<Edge, 4> latchEdgesOfCopy;
        Vector<Edge, 4> latchEdges;
        Vector<Edge, 4> enteringEdges;
        const CFG& cfg = m_analyses.cfg();
        for (Edge edge : cfg.incomingEdges(headerOfCopy)) {
            if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != numberOfParameters)
                return false;
            if (isInCopy.get(edge.from.index()))
                latchEdgesOfCopy.append(edge);
        }
        if (latchEdgesOfCopy.isEmpty())
            return false;
        for (Edge edge : cfg.incomingEdges(header)) {
            if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != numberOfParameters)
                return false;
            (m_analyses.naturalLoops().belongsTo(edge.from, *loop) ? latchEdges : enteringEdges).append(edge);
        }

        // A type that a test cannot establish (proven contents, a range of integers) keeps the copy from coming back.
        Vector<TypeID> testedEveryTurn = findTestsOfEveryTurn(*loop, isInCopy, plan.carriedValuesThatMayBeUnspecified);
        Vector<TypeID> testedTypes = testedEveryTurn;
        Vector<TypeID> typesTakenFromCopy(numberOfParameters);
        for (size_t index = 0; index < numberOfParameters; ++index) {
            const Type& given = types.at(plan.typesOfCarriedValues[index]);
            const Type& typeInLoop = typeOf(m_function.at(header).parameters[index]);
            if (testedTypes[index] || given.isSubsetOf(typeInLoop, m_program) || plan.carriedValuesThatMayBeUnspecified.get(index))
                continue;
            if (!given.isJavaScriptValue() || !typeInLoop.isJavaScriptValue())
                return false;
            // A test for literal strings reads the characters of a string that is none of the program's own. What the copy carries may be any string, so the loop takes any.
            Type wanted = typeInLoop.withLiteralStringsWidened();
            Type testable = testableStoredTypeOf(PropertyAccessContext { m_program, &m_context.inputs.flow }, given, wanted, types);
            if (testable.isNever() || !given.castTo(testable, types).isSubsetOf(wanted, m_program))
                return false;
            testedTypes[index] = types.intern(testable);
            if (typeInLoop.hasLiteralStrings())
                typesTakenFromCopy[index] = types.intern(wanted);
        }
        // The loop has the types of what entered it before there was a way back.
        m_wayBackWidensLoopTypes = false;
        for (size_t index = 0; index < numberOfParameters; ++index) {
            const Type& given = types.at(plan.typesOfCarriedValues[index]);
            const Type& typeInLoop = typeOf(m_function.at(header).parameters[index]);
            m_wayBackWidensLoopTypes |= !(testedTypes[index] ? given.castTo(types.at(testedTypes[index]), types) : given).isSubsetOf(typeInLoop, m_program);
        }

        builder.setOrigin(m_function.at(header).terminator.origin());
        Expected<BlockFrequency> frequencyOfLoop = m_function.at(header).frequency;

        // Everything that enters the loop comes through one block, which is the header of the cycle through the copy. So the loop stays a loop of its own, with one entering edge.
        BlockID entry = builder.addBlock(frequencyOfLoop);
        BlockID tests = builder.addBlock(m_function.at(headerOfCopy).frequency);
        Arguments parametersOfEntry;
        Arguments carried;
        for (size_t index = 0; index < numberOfParameters; ++index) {
            TypeID typeInLoop = m_function.at(m_function.at(header).parameters[index]).type;
            if (TypeID tested = testedEveryTurn[index])
                typeInLoop = types.intern(types.at(typeInLoop).castTo(types.at(tested), types));
            else if (plan.carriedValuesThatMayBeUnspecified.get(index))
                typeInLoop = types.unionOf(typeInLoop, plan.typesOfCarriedValues[index]);
            else if (TypeID taken = typesTakenFromCopy[index])
                typeInLoop = taken;
            parametersOfEntry.append(builder.addParameter(entry, typeInLoop));
            carried.append(builder.addParameter(tests, plan.typesOfCarriedValues[index]));
        }
        bool entersThroughTests = false;
        for (Edge edge : enteringEdges) {
            Arguments arguments(targetOf(m_function, edge).arguments);
            if (plan.letsVariablesBeCarried) {
                builder.redirectEdge(edge, headerOfCopy, WTF::move(arguments));
                continue;
            }
            bool passes = passesTests(arguments, testedEveryTurn.span());
            entersThroughTests |= !passes;
            builder.redirectEdge(edge, passes ? entry : tests, WTF::move(arguments));
        }
        if (entersThroughTests)
            m_function.edit(tests, BlockEdit::Rest).frequency = frequencyOfLoop;
        for (Edge edge : latchEdgesOfCopy)
            builder.redirectEdge(edge, tests, Arguments(targetOf(m_function, edge).arguments));
        testOnEdges(builder, latchEdges.span(), testedEveryTurn.span(), header, headerOfCopy);

        builder.moveToEndOf(entry);
        builder.emitJump(header, WTF::move(parametersOfEntry));
        builder.moveToEndOf(tests);
        emitTests(builder, carried, testedTypes.span(), entry, headerOfCopy, m_function.at(tests).frequency);
        m_analyses.didChangeControlFlow();

        for (BlockID block : { entry, tests })
            m_isFinishedHeader.set(block.index());
        return true;
    }

    // The loop has one entering edge, from the way back. What is read ahead of the loop is read there, and tested there for what every turn tested it for.
    void carryVariables(IRBuilder& builder, BlockID header, const RegionCloner& copy, const BitVector& isInCopy)
    {
        BlockID headerOfCopy = copy.cloneOf(header);
        VariableCarrier carrier { { m_context }, false, &copy };
        BitVector leaves;
        bool changed = carrier.carryAround(header, leaves);
        m_variablesWithStores.appendVector(carrier.m_variablesWithStores);
        const NaturalLoop* loop = changed ? m_analyses.naturalLoops().headerOf(header) : nullptr;
        if (!loop)
            return;
        Vector<TypeID> tested = findTestsOfVariables(*loop, isInCopy, m_function.at(headerOfCopy).parameters.size());
        Vector<Edge, 4> edges;
        for (Edge edge : m_analyses.cfg().incomingEdges(header)) {
            if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != tested.size())
                return;
            edges.append(edge);
        }
        builder.setOrigin(m_function.at(header).terminator.origin());
        testOnEdges(builder, edges.span(), tested.span(), header, headerOfCopy);
        m_analyses.didChangeControlFlow();
    }
};

} // anonymous namespace

PassOutcome separateUnlikelyLoopPaths(FunctionPassContext& context)
{
    if (!context.function.hasBody() || !context.analyses.naturalLoops().numLoops())
        return PassOutcome::Unchanged;
    PassOutcome outcome = LoopPathSeparator { { context } }.run();
    outcome |= VariableCarrier { { context } }.run();
    // SimplifyCasts runs inside this pass, and what it says it has kept holds for the function as it left it.
    context.keptAnalyses = { };
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
