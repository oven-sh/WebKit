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
#include "AOTBuiltinUses.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTDominators.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTMemoryLocation.h"
#include "AOTMethodDispatch.h"
#include "AOTNaturalLoops.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTProgramPasses.h"
#include "AOTRegionCloner.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTSplitUnions.h"
#include "AOTThreshold.h"
#include "AOTTypeRelations.h"
#include "AOTUseEffectHistory.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVirtualObject.h"
#include "AOTWrittenFunction.h"
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

static constexpr OpcodeSet opcodesThatMaySuspend = [] {
    OpcodeSet result;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (maySuspend(opcode))
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
        // The block whose edges bring the value: the read's own, or one above it on a way without joins.
        BlockID join;
        Vector<std::pair<Edge, ValueID>, 4> incoming;
    };

    // A value that may clear bits of classes that it does not access, or throw under a handler. A walk visits these and the accesses of its group.
    struct ValueThatMatters {
        uint32_t index;
        // Of the heaps with accesses: those that it may write, and for a store the one that it writes.
        AbstractHeapSet writes;
        bool maySuspend;
        bool mayThrowToHandler;
    };

    // Of the group that is being solved, but for the accesses and the values that matter.
    struct StateOfBlock {
        IndexRange accesses;
        IndexRange valuesThatMatter;
        bool hasListedValuesThatMatter { false };
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
    // Positions in m_accesses, ascending: the accesses that define or look in the group.
    Vector<uint32_t> m_accessesOfGroup { };
    AbstractHeapSet m_heapsOfGroup { };
    Vector<ValueThatMatters> m_valuesThatMatter { };
    // A class with more bits than a group may hold is solved in parts, from its first bits on: the group is one part, and these are all the bits of the class. Empty otherwise.
    IndexRange m_bitsOfClassInParts { };
    // A read that was replaced by a value looks no more and defines no more: whatever its bit would reach, the bit of that value reaches, it comes first, and its type is as narrow.
    BitVector m_isReplacedByValue { };
    BitVector m_isReplacedByUndefined { };
    // A new read of the location is as informative as the value that the bit stands for. Empty if nothing suspends and nothing is caught.
    BitVector m_isAsWellReadAgain { };
    // By handler block: in each block under the handler, the last call.
    UncheckedKeyHashMap<uint32_t, Vector<ProgramPoint, 4>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lastCallsUnderHandler { };
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
    Vector<BlockID> m_joinsAbove { };

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
        AOT::sortInLine(order, { }, [&](uint32_t index) {
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

    void findBitsAsWellReadAgain()
    {
        if (!m_analyses.opcodesOfValues().intersects(opcodesThatMaySuspend) && !m_cfg.hasHandlers())
            return;
        m_isAsWellReadAgain.ensureSize(m_bits.size());
        for (const AccessClass& accessClass : m_classes) {
            const Type* typeOfRead = nullptr;
            for (uint32_t bit = accessClass.bits.first; bit < accessClass.bits.end; ++bit) {
                const Access& access = accessOf(bit);
                if (access.isRead() && (!typeOfRead || !typeOf(access.id).isSubsetOf(*typeOfRead, m_program)))
                    typeOfRead = &typeOf(access.id);
            }
            for (uint32_t bit = accessClass.bits.first; bit < accessClass.bits.end; ++bit) {
                const Access& access = accessOf(bit);
                if (!access.isAbsent && typeOfRead->isSubsetOf(typeOf(access.valueAfter()), m_program))
                    m_isAsWellReadAgain.quickSet(bit);
            }
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
        else if (namesNoObjectMadeHere(a) && namesNoObjectMadeHere(b) && !m_analyses.opcodesOfValues().contains(Opcode::GetHeapFrameWord))
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

    // A value kept across a suspension lives in the frame, which is saved and restored. Reading the location again is cheaper, unless more is known about the value than a read shows.
    void appendClearsOfSuspension(std::span<const Word> row, ActionList& actions)
    {
        for (size_t bit = findBit(row, 0); bit < m_group.size(); bit = findBit(row, bit + 1)) {
            uint32_t bitInAll = m_group.first + static_cast<uint32_t>(bit);
            if (m_isAsWellReadAgain.quickGet(bitInAll))
                appendClear(actions, { bitInAll, bitInAll + 1 });
        }
    }

    void appendClearsOfEffects(ProgramPoint point, std::span<const Word> row, ActionList& actions)
    {
        ValueID id = m_function.at(point.block).values[point.index];
        const Value& value = m_function.at(id);
        if (maySuspend(value.opcode))
            appendClearsOfSuspension(row, actions);
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

    std::span<const ValueThatMatters> valuesThatMatterIn(BlockID blockID)
    {
        StateOfBlock& state = m_states[blockID.index()];
        if (!std::exchange(state.hasListedValuesThatMatter, true)) {
            const Block& block = m_function.at(blockID);
            std::span<const Access> accesses = m_accesses.span().subspan(state.accesses.first, state.accesses.size());
            state.valuesThatMatter.first = m_valuesThatMatter.size();
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                Opcode opcode = m_function.at(id).opcode;
                ValueThatMatters value { index, { }, false, block.handler && m_analyses.effectsOf(id).mayThrow() };
                if (accesses.empty() || accesses.front().id != id) {
                    value.maySuspend = maySuspend(opcode);
                    if (m_opcodesThatMayWrite.contains(opcode)) {
                        const EffectsOnEdges& effects = m_analyses.effectsOf(id);
                        value.writes = (effects.normal.writes.heaps() | effects.handler.writes.heaps()) & accessedHeapSet;
                    }
                } else {
                    if (accesses.front().stored && accesses.front().isOperationOnMemory())
                        value.writes = accesses.front().location.heap();
                    while (!accesses.empty() && accesses.front().id == id)
                        accesses = accesses.subspan(1);
                }
                if (value.writes || value.maySuspend || value.mayThrowToHandler || m_checksScreens)
                    m_valuesThatMatter.append(value);
            }
            state.valuesThatMatter.end = m_valuesThatMatter.size();
        }
        return m_valuesThatMatter.span().subspan(state.valuesThatMatter.first, state.valuesThatMatter.size());
    }

    // Goes through the block with the rows and hands each action to the functor in front of carrying it out on all of them.
    // A function may have one block of a million values and hundreds of groups, so a walk passes over what does nothing to its group.
    void walk(BlockID blockID, std::span<Row* const> rows, bool watchesThrows, NOESCAPE const Invocable<void(const Action&)> auto& functor)
    {
        const Block& block = m_function.at(blockID);
        IndexRange accessesOfBlock = m_states[blockID.index()].accesses;
        std::span<const uint32_t> accesses = std::ranges::subrange(std::ranges::lower_bound(m_accessesOfGroup, accessesOfBlock.first), std::ranges::lower_bound(m_accessesOfGroup, accessesOfBlock.end));
        std::span<const ValueThatMatters> others = valuesThatMatterIn(blockID);
        auto doesNothing = [&](const ValueThatMatters& value) {
            return !value.writes.containsAny(m_heapsOfGroup) && !value.maySuspend && !(watchesThrows && value.mayThrowToHandler);
        };
        Row unionOfRows(FillWith { }, m_numberOfWords, Word { 0 });
        ActionList actions;
        while (true) {
            std::span<const Word> live = rows[0]->span();
            if (rows.size() > 1) {
                std::ranges::fill(unionOfRows, 0);
                for (Row* row : rows) {
                    for (uint32_t word = 0; word < m_numberOfWords; ++word)
                        unionOfRows[word] |= (*row)[word];
                }
                live = unionOfRows.span();
            }
            uint32_t indexOfAccess = accesses.empty() ? std::numeric_limits<uint32_t>::max() : m_accesses[accesses.front()].point.index;
            if (isEmpty(live) && !watchesThrows)
                others = others.subspan(std::ranges::lower_bound(others, indexOfAccess, { }, &ValueThatMatters::index) - others.begin());
            bool isPassedOver = false;
            while (!others.empty() && others.front().index < indexOfAccess && doesNothing(others.front())) {
                if (m_checksScreens) [[unlikely]] {
                    isPassedOver = true;
                    break;
                }
                others = others.subspan(1);
            }
            uint32_t index = others.empty() ? indexOfAccess : std::min(indexOfAccess, others.front().index);
            if (index == std::numeric_limits<uint32_t>::max())
                break;
            if (!others.empty() && others.front().index == index)
                others = others.subspan(1);

            ValueID id = block.values[index];
            std::span<const Access> accessesOfValue;
            if (index == indexOfAccess) {
                uint32_t first = accesses.front();
                while (first > accessesOfBlock.first && m_accesses[first - 1].id == id)
                    --first;
                uint32_t end = accesses.front() + 1;
                while (end < accessesOfBlock.end && m_accesses[end].id == id)
                    ++end;
                accessesOfValue = m_accesses.span().subspan(first, end - first);
                while (!accesses.empty() && accesses.front() < end)
                    accesses = accesses.subspan(1);
            } else {
                auto found = std::ranges::equal_range(m_accesses.span().subspan(accessesOfBlock.first, accessesOfBlock.size()), index, { }, [](const Access& access) { return access.point.index; });
                accessesOfValue = { found.begin(), found.end() };
            }

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
            RELEASE_ASSERT(!isPassedOver || actions.isEmpty());
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

        m_accessesOfGroup.shrink(0);
        m_heapsOfGroup = { };
        auto addAccessOf = [&](uint32_t bit) {
            if (!m_isReplacedByValue.quickGet(bit))
                m_accessesOfGroup.append(m_bits[bit].access);
        };
        for (uint32_t bit = group.first; bit < group.end; ++bit) {
            addAccessOf(bit);
            m_heapsOfGroup.add(accessOf(bit).location.heap());
        }
        for (uint32_t bit = bitsOfClassInParts.first; bit < bitsOfClassInParts.end; ++bit) {
            if (!group.contains(bit) && accessOf(bit).isRead())
                addAccessOf(bit);
        }
        AOT::sort(m_accessesOfGroup);

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
            BlockID join = joinAbove(read.point.block);
            std::span<const Edge> edges = m_cfg.incomingEdges(join);
            const Vector<ValueID, 4>& values = m_valuesOnEdges.find(bit)->value;
            Merge merge { read.id, read.point, join, { } };
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

    std::span<const ProgramPoint> lastCallsUnder(BlockID handler)
    {
        return m_lastCallsUnderHandler.ensure(handler.index(), [&] {
            Vector<ProgramPoint, 4> calls;
            for (BlockID id : m_cfg.reversePostOrder()) {
                const Block& block = m_function.at(id);
                if (!block.handler || block.handler->block != handler)
                    continue;
                for (uint32_t index = block.values.size(); index--;) {
                    if (effectRuleOf(m_function.at(block.values[index]).opcode) == EffectRule::Call) {
                        calls.append({ id, index });
                        break;
                    }
                }
            }
            return calls;
        }).iterator->value.span();
    }

    bool isLiveAfter(ValueID value, ProgramPoint point)
    {
        const UseLists& useLists = m_analyses.useLists();
        for (const Use& use : useLists.usesOf(value)) {
            ProgramPoint pointOfUse = useLists.pointOf(use);
            if (pointOfUse.block == point.block && pointOfUse.index > point.index)
                return true;
        }
        bool isLive = false;
        forEachOutgoingEdge(m_function, point.block, [&](Edge edge, const Target& target) {
            isLive |= !edge.isHandlerEdge() && m_analyses.liveness().isLiveAtHead(value, target.block);
        });
        return isLive;
    }

    // The handler would be the only reader of the value behind some call under it. For that reader the value would stay in the frame or in a saved register
    // across the call, and it stays there after the handler's last read, when the program may have dropped the object. The handler is cold: it reads the location again.
    bool isKeptAcrossCallForHandlerAlone(const Access& source, const Access& read)
    {
        if (!m_cfg.hasHandlers() || source.isAbsent || !m_isAsWellReadAgain.quickGet(*source.bit) || !mayBeReference(typeOf(source.valueAfter())))
            return false;
        ValueID value = source.valueAfter();
        for (BlockID block = read.point.block; block != source.point.block && block != m_cfg.root(); block = m_analyses.dominators().idom(block)) {
            if (!m_function.at(block).isHandlerBlock() || m_analyses.liveness().isLiveAtHead(value, block))
                continue;
            if (std::ranges::any_of(lastCallsUnder(block), [&](ProgramPoint call) { return !isLiveAfter(value, call); }))
                return true;
        }
        return false;
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

    // By block: the block itself where it is a join or has no way in that comes first, else what its one predecessor has.
    BlockID joinAbove(BlockID block)
    {
        if (m_joinsAbove.isEmpty()) {
            m_joinsAbove.grow(m_function.blocks.size());
            for (BlockID id : m_cfg.reversePostOrder()) {
                std::span<const Edge> edges = m_cfg.incomingEdges(id);
                bool hasOneWayIn = id != m_cfg.root() && edges.size() == 1 && !edges[0].isHandlerEdge() && !m_function.at(id).isHandlerBlock();
                m_joinsAbove[id.index()] = hasOneWayIn ? m_joinsAbove[edges[0].from.index()] : id;
            }
        }
        return m_joinsAbove[block.index()];
    }

    // What is available on one way into the block need not be so at its head, and the actions of the block were made for the row at its head. So the block is gone through anew.
    // `if (size < 16) size++; else head++;`, a test or two, and then a read of `size`: each way into the join brings the value, and nothing between the join and the read writes it. So the parameter stands at the join.
    void findMerges(BlockID readBlock, std::span<const uint32_t> bitsOfReads)
    {
        BlockID block = joinAbove(readBlock);
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
        if (block != readBlock) {
            bool isBroughtByEveryEdge = std::ranges::any_of(bitsOfReads, [&](uint32_t bit) {
                const Type& readType = typeOf(accessOf(bit).id);
                auto bringsIt = [&](const Row& row) {
                    return findAvailableAccess(bit, row.span(), [&](const Access& access) {
                        return !access.isAbsent && canForward(access, readType);
                    });
                };
                // What one part does not hold on an edge, another part may.
                return isPartOfClass() ? std::ranges::any_of(available, bringsIt) : std::ranges::all_of(available, bringsIt);
            });
            if (!isBroughtByEveryEdge)
                return;
        }
        Vector<BlockID, 4> way;
        for (BlockID next = readBlock; next != block; next = m_cfg.incomingEdges(next)[0].from)
            way.append(next);
        way.append(block);
        Vector<Row*, 4> rows;
        for (Row& row : available)
            rows.append(&row);
        for (BlockID blockOfWay : way | std::views::reverse) walk(blockOfWay, rows.span(), false, [&](const Action& action) {
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
            Merge merge { read.id, read.point, block, { } };
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
        AOT::sortInLine(blocks, { }, [&](BlockID block) {
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
                        return access.isAbsent ? readsAbsentAsUndefined : canForward(access, readType) && !isKeptAcrossCallForHandlerAlone(access, read);
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
            ValueID parameter = builder.addParameter(merge.join, m_function.at(merge.read).type);
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
        findBitsAsWellReadAgain();
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

// Two values compare the same however often they are asked, and a string keeps its length, whatever runs in between: neither question runs code of the program.
static PassOutcome dropRepeatedStrictComparisons(FunctionPassContext& context)
{
    static constexpr OpcodeSet questions { Opcode::StrictEq, Opcode::GetStringLength };
    if (!context.analyses.opcodesOfValues().intersects(questions))
        return PassOutcome::Unchanged;

    struct Comparison {
        uint64_t operands;
        ValueID result;
        BlockID block;
        // The comparison of the same operands above this one in the dominator tree.
        std::optional<uint32_t> outer;
    };
    struct Repeated {
        ProgramPoint point;
        ValueID first;
    };
    struct NarrowingBranch {
        ValueID test;
        BlockID block;
    };
    Function& function = context.function;
    size_t numberOfComparisons = 0;
    for (const Block& block : function.blocks) {
        numberOfComparisons += std::ranges::count_if(block.values, [&](ValueID id) {
            return questions.contains(function.at(id).opcode);
        });
    }
    if (numberOfComparisons < 2)
        return PassOutcome::Unchanged;

    // A branch hands operands of its test to its arms, and names them by their positions in the test.
    Vector<NarrowingBranch, 8> narrowingBranches;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Terminator& terminator = function.at(BlockID(blockIndex)).terminator;
        if (!terminator.isConditionalBranch() || !questions.contains(function.at(terminator.conditionOperand()).opcode))
            continue;
        if (!terminator.taken().proofs.isEmpty() || !terminator.notTaken().proofs.isEmpty())
            narrowingBranches.append({ terminator.conditionOperand(), BlockID(blockIndex) });
    }
    auto indexOfTest = [](const NarrowingBranch& branch) { return branch.test.index(); };
    AOT::sort(narrowingBranches, { }, indexOfTest);
    // An arm was typed by what its own test says of the operand that it takes: the first test has to say as much of the same operand.
    auto saysAsMuchToArms = [&](ValueID first, ValueID repeatedTest, bool namesOperandsTheOtherWayRound) {
        TypingContext typing { context.program, function, context.types };
        return std::ranges::all_of(std::ranges::equal_range(narrowingBranches, repeatedTest.index(), { }, indexOfTest), [&](const NarrowingBranch& branch) {
            const Terminator& terminator = function.at(branch.block).terminator;
            for (EdgeSide side : { EdgeSide::Taken, EdgeSide::NotTaken }) {
                const Target& target = side == EdgeSide::Taken ? terminator.taken() : terminator.notTaken();
                for (uint32_t index = 0; index < target.proofs.size(); ++index) {
                    uint32_t operandIndex = namesOperandsTheOtherWayRound ? 1 - target.proofs[index] : target.proofs[index];
                    ValueID parameter = function.at(target.block).parameters[target.arguments.size() + index];
                    if (!narrowOperandByTest(typing, function.at(first), operandIndex, side).isSubsetOf(context.program.typeOf(function, parameter), context.program))
                        return false;
                }
            }
            return true;
        });
    };

    BitVector keptReads = context.build.validates() ? findReadsKeptForValidation(function) : BitVector();
    const Dominators& dominators = context.analyses.dominators();
    Vector<BlockID, 4> branchesOnTestsTheOtherWayRound;
    Vector<Comparison, 16> made;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> innermost;
    Vector<size_t, 16> numberMadeAtEntry;
    Vector<Repeated, 8> repeated;
    ValueSubstitution substitution(function);
    dominators.walkTree([&](BlockID block) {
        numberMadeAtEntry.append(made.size());
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (!questions.contains(value.opcode) || keptReads.get(values[index].index()))
                continue;
            // No value has the last index, so a length is no comparison.
            uint64_t left = sameObjectThroughCasts(function, value.operand(0)).valueInFrontOfFacts().index();
            uint64_t right = value.opcode == Opcode::StrictEq ? sameObjectThroughCasts(function, value.rightOperand()).valueInFrontOfFacts().index() : std::numeric_limits<uint32_t>::max();
            uint64_t operands = std::min(left, right) << 32 | std::max(left, right);
            auto found = innermost.find(operands);
            std::optional<uint32_t> outer = found == innermost.end() ? std::nullopt : std::optional(found->value);
            // What throws in the middle of a block goes to its handler, which the end of the block does not dominate.
            std::optional<uint32_t> earlier = outer;
            while (earlier && made[*earlier].block != block && !dominators.terminatorDominates(made[*earlier].block, block))
                earlier = made[*earlier].outer;
            // Behind a test the same question may have a narrower answer, and whoever takes it was typed by that.
            bool namesOperandsTheOtherWayRound = earlier && sameObjectThroughCasts(function, function.at(made[*earlier].result).operand(0)).valueInFrontOfFacts().index() != left;
            if (earlier && context.program.typeOf(function, made[*earlier].result).isSubsetOf(context.program.typeOf(function, values[index]), context.program)
                && saysAsMuchToArms(made[*earlier].result, values[index], namesOperandsTheOtherWayRound)) {
                if (namesOperandsTheOtherWayRound) {
                    for (const NarrowingBranch& branch : std::ranges::equal_range(narrowingBranches, values[index].index(), { }, indexOfTest))
                        branchesOnTestsTheOtherWayRound.append(branch.block);
                }
                substitution.replace(values[index], made[*earlier].result);
                repeated.append({ { block, index }, made[*earlier].result });
                continue;
            }
            innermost.set(operands, static_cast<uint32_t>(made.size()));
            made.append({ operands, values[index], block, outer });
        }
    }, [&](BlockID) {
        for (size_t numberAtEntry = numberMadeAtEntry.takeLast(); made.size() > numberAtEntry; made.removeLast()) {
            if (made.last().outer)
                innermost.set(made.last().operands, *made.last().outer);
            else
                innermost.remove(made.last().operands);
        }
    });
    if (repeated.isEmpty())
        return PassOutcome::Unchanged;
    substitution.apply(function, &context.analyses);
    for (BlockID block : branchesOnTestsTheOtherWayRound) {
        for (Target& target : function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets) {
            for (uint32_t& operandIndex : target.proofs)
                operandIndex = 1 - operandIndex;
        }
    }
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    if (context.build.validates()) {
        // The repeated comparison stays, and its answer is held against the first one's.
        for (const Repeated& comparison : repeated) {
            auto [block, index] = comparison.point;
            ValueID kept = function.at(block).values[index];
            builder.setOrigin(function.at(kept).origin());
            builder.insertBefore({ block, index + 1 }, IRBuilder::make(Opcode::ValidateSameValue, { kept, comparison.first }));
        }
        builder.applyInsertions();
    } else {
        // The walk enters a block once, so the comparisons of one block lie side by side.
        for (size_t next = 0; next < repeated.size();) {
            BlockID block = repeated[next].point.block;
            BitVector erased;
            for (; next < repeated.size() && repeated[next].point.block == block; ++next)
                erased.set(repeated[next].point.index);
            builder.eraseValues(block, erased);
        }
    }
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

// A read at the same place says whether the store runs no code, and what is above the closure has been asked.
static bool isPlainStoreToMemberOfClosure(FunctionPassContext& context, const Value& store)
{
    const Program& program = context.program;
    const Function& function = context.function;
    Value read = IRBuilder::make(Opcode::PropGet, { store.objectOperand() });
    read.setKey(store.key());
    ProvenAccess storage;
    const ProvenAccess& proven = provenAccessOf(program, function, read, storage);
    if (!proven.isComplete || !proven.mayFindStoredValue || program.nameClassOf(store.key()) != NameClass::Ordinary)
        return false;
    return std::ranges::all_of(program.typeOf(function, store.objectOperand()).functions, [&](FunctionID closureFunction) {
        return context.inputs.flow.closuresKeepTheirIntegrity(closureFunction);
    });
}

// The captures of a closure never change: a closure made in this function hands out what it was made with.
static PassOutcome readCapturesOfClosuresJustMade(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::GetCapture) || !context.analyses.opcodesOfValues().contains(Opcode::NewClosure))
        return PassOutcome::Unchanged;
    const Program& program = context.program;
    Function& function = context.function;
    Vector<ProgramPoint, 4> forwarded;
    ValueSubstitution substitution(function);
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& value = function.at(id);
            if (value.opcode != Opcode::GetCapture)
                continue;
            const Value& closure = function.at(sameObjectThroughCasts(function, value.closureOperand()).valueInFrontOfFacts());
            if (closure.opcode != Opcode::NewClosure || closure.function() != value.function() || value.index() >= closure.capturesOperands().size())
                continue;
            ValueID captured = closure.capturesOperands()[value.index()];
            if (!program.typeOf(function, captured).isSubsetOf(program.typeOf(function, id), program))
                continue;
            substitution.replace(id, captured);
            forwarded.append({ block, index });
        }
    }
    if (forwarded.isEmpty())
        return PassOutcome::Unchanged;
    substitution.apply(function, &context.analyses);
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (size_t next = 0; next < forwarded.size();) {
        BlockID block = forwarded[next].block;
        BitVector erased;
        for (; next < forwarded.size() && forwarded[next].block == block; ++next)
            erased.set(forwarded[next].index);
        builder.eraseValues(block, erased);
    }
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

// `count.n = count.n + 1; return count.n`. A read of a closure's member that runs no code finds what the store behind which it stands has stored, where the store is sure to take: nothing above the closure can have the name, and the closure stays extensible and writable.
static PassOutcome forwardStoresToMembersOfClosures(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::PropSet))
        return PassOutcome::Unchanged;

    struct Stored {
        ValueID receiver;
        PropertyKeyID key;
        ValueID value;
    };
    using StoredValues = Vector<Stored, 2>;
    struct Forwarded {
        ProgramPoint point;
        ValueID value;
    };
    const Program& program = context.program;
    Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    BitVector keptReads = context.build.validates() ? findReadsKeptForValidation(function) : BitVector();
    auto rootOf = [&](ValueID id) {
        return sameObjectThroughCasts(function, id).valueInFrontOfFacts();
    };
    Vector<StoredValues> atEnd(function.blocks.size());
    Vector<Forwarded, 4> forwarded;
    ValueSubstitution substitution(function);
    for (BlockID block : cfg.reversePostOrder()) {
        StoredValues stored;
        std::span<const Edge> edges = cfg.incomingEdges(block);
        if (edges.size() == 1 && !edges[0].isHandlerEdge() && block != cfg.root())
            stored = atEnd[edges[0].from.index()];
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& value = function.at(id);
            if (value.opcode == Opcode::PropGet && !value.isFreeName() && !stored.isEmpty() && !keptReads.get(id.index())) {
                ValueID receiver = rootOf(value.objectOperand());
                auto found = std::ranges::find_if(stored, [&](const Stored& entry) { return entry.receiver == receiver && entry.key == value.key(); });
                if (found != stored.end() && program.typeOf(function, found->value).isSubsetOf(program.typeOf(function, id), program)) {
                    substitution.replace(id, found->value);
                    forwarded.append({ { block, index }, found->value });
                    continue;
                }
            }
            if (value.opcode == Opcode::PropSet && !value.isFreeName()) {
                if (isPlainStoreToMemberOfClosure(context, value)) {
                    // On a closure the store touches no other key.
                    if (program.typeOf(function, value.objectOperand()).functions.isEmpty())
                        stored.shrink(0);
                    stored.removeAllMatching([&](const Stored& entry) { return entry.key == value.key(); });
                    stored.append({ rootOf(value.objectOperand()), value.key(), value.valueOperand() });
                    continue;
                }
            }
            Effects effects = context.analyses.effectsOf(id).onAnyEdge();
            if (effects.mayRunOtherCode() || !effects.writes.isEmpty())
                stored.shrink(0);
        }
        if (!context.analyses.effectsOfTerminator(block).onAnyEdge().writes.isEmpty())
            stored.shrink(0);
        atEnd[block.index()] = WTF::move(stored);
    }
    if (forwarded.isEmpty())
        return PassOutcome::Unchanged;
    substitution.apply(function, &context.analyses);
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (size_t next = 0; next < forwarded.size();) {
        BlockID block = forwarded[next].point.block;
        BitVector erased;
        for (; next < forwarded.size() && forwarded[next].point.block == block; ++next)
            erased.set(forwarded[next].point.index);
        builder.eraseValues(block, erased);
    }
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

namespace {

// An operand's key may be any 32 bits (-1 is all ones) and the smaller key is the upper half, so only a larger upper half is free to mark a bucket.
struct SumOperandsHashTraits : WTF::GenericHashTraits<uint64_t> {
    static constexpr bool emptyValueIsZero = false;
    static uint64_t emptyValue() { return 1ull << 32; }
    static void constructDeletedValue(uint64_t& slot) { slot = 2ull << 32; }
    static bool isDeletedValue(uint64_t value) { return value == 2ull << 32; }
};

} // anonymous namespace

// `list[k + 1]` three times in one iteration. The sum of two integers is the same however often it is computed. Once the sums are one value, what was asked of the first is known of the others:
// that the element is there, and what it holds.
static PassOutcome dropRepeatedIntegerSums(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::Add))
        return PassOutcome::Unchanged;

    struct Sum {
        uint64_t operands;
        ValueID result;
        BlockID block;
        // The sum of the same operands above this one in the dominator tree.
        std::optional<uint32_t> outer;
    };
    struct Repeated {
        ProgramPoint point;
        ValueID first;
    };
    const Program& program = context.program;
    Function& function = context.function;
    size_t numberOfSums = 0;
    for (const Block& block : function.blocks) {
        numberOfSums += std::ranges::count_if(block.values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::Add;
        });
    }
    if (numberOfSums < 2)
        return PassOutcome::Unchanged;

    // A constant counts by its value: each use may have a constant of its own.
    constexpr uint32_t isConstant = 1u << 31;
    auto keyOf = [&](ValueID operand) -> std::optional<uint32_t> {
        if (!program.typeOf(function, operand).isInteger())
            return std::nullopt;
        KnownValue known = knownValueOf(program, function, operand);
        if (known.kind() == KnownValue::Kind::Integer) {
            int64_t integer = known.asInteger();
            if (integer < -(1 << 30) || integer >= 1 << 30)
                return std::nullopt;
            return isConstant | (static_cast<uint32_t>(integer) & ~isConstant);
        }
        return sameObjectThroughCasts(function, operand).valueInFrontOfFacts().index();
    };

    BitVector keptReads = context.build.validates() ? findReadsKeptForValidation(function) : BitVector();
    const Dominators& dominators = context.analyses.dominators();
    Vector<Sum, 16> made;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, SumOperandsHashTraits> innermost;
    Vector<size_t, 16> numberMadeAtEntry;
    Vector<Repeated, 8> repeated;
    ValueSubstitution substitution(function);
    dominators.walkTree([&](BlockID block) {
        numberMadeAtEntry.append(made.size());
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::Add || keptReads.get(values[index].index()))
                continue;
            std::optional<uint32_t> left = keyOf(value.leftOperand());
            std::optional<uint32_t> right = left ? keyOf(value.rightOperand()) : std::nullopt;
            if (!right)
                continue;
            uint64_t operands = static_cast<uint64_t>(std::min(*left, *right)) << 32 | std::max(*left, *right);
            auto found = innermost.find(operands);
            std::optional<uint32_t> outer = found == innermost.end() ? std::nullopt : std::optional(found->value);
            // What throws in the middle of a block goes to its handler, which the end of the block does not dominate.
            std::optional<uint32_t> earlier = outer;
            while (earlier && made[*earlier].block != block && !dominators.terminatorDominates(made[*earlier].block, block))
                earlier = made[*earlier].outer;
            // Behind a test an operand may be known more closely, and its sum with it.
            if (earlier && function.at(made[*earlier].result).type == value.type && function.at(made[*earlier].result).expected().predicted() == value.expected().predicted()) {
                substitution.replace(values[index], made[*earlier].result);
                repeated.append({ { block, index }, made[*earlier].result });
                continue;
            }
            innermost.set(operands, static_cast<uint32_t>(made.size()));
            made.append({ operands, values[index], block, outer });
        }
    }, [&](BlockID) {
        for (size_t numberAtEntry = numberMadeAtEntry.takeLast(); made.size() > numberAtEntry; made.removeLast()) {
            if (made.last().outer)
                innermost.set(made.last().operands, *made.last().outer);
            else
                innermost.remove(made.last().operands);
        }
    });
    if (repeated.isEmpty())
        return PassOutcome::Unchanged;
    substitution.apply(function, &context.analyses);
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    if (context.build.validates()) {
        // The repeated sum stays, and its answer is held against the first one's.
        for (const Repeated& sum : repeated) {
            auto [block, index] = sum.point;
            ValueID kept = function.at(block).values[index];
            builder.setOrigin(function.at(kept).origin());
            builder.insertBefore({ block, index + 1 }, IRBuilder::make(Opcode::ValidateSameValue, { kept, sum.first }));
        }
        builder.applyInsertions();
    } else {
        // The walk enters a block once, so the sums of one block lie side by side.
        for (size_t next = 0; next < repeated.size();) {
            BlockID block = repeated[next].point.block;
            BitVector erased;
            for (; next < repeated.size() && repeated[next].point.block == block; ++next)
                erased.set(repeated[next].point.index);
            builder.eraseValues(block, erased);
        }
    }
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

namespace {

struct RepeatedRead {
    Opcode opcode;
    AbstractHeapSet heapsRead;
    bool (*mayBeDropped)(FunctionPassContext&, ValueID, const Value&);
    std::pair<ValueID, ValueID> (*operandsOf)(const Value&);
    bool (*hasSamePayload)(const Value& earlier, const Value& read);
};

} // anonymous namespace

static PassOutcome dropRepeatedReadsOnStraightWays(FunctionPassContext& context, const RepeatedRead& kind)
{
    AOT_THRESHOLD(unsigned, maxValuesBehindRepeatedRead, 64);
    if (!context.analyses.opcodesOfValues().contains(kind.opcode))
        return PassOutcome::Unchanged;

    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const CFG& cfg = analyses.cfg();
    auto identitiesOf = [&](const Value& read) {
        auto [first, second] = kind.operandsOf(read);
        return std::pair { analyses.identityOf(first), analyses.identityOf(second) };
    };
    auto findEarlierRead = [&](BlockID block, uint32_t index, const Value& read) -> ValueID {
        std::pair<ValueID, ValueID> operands = identitiesOf(read);
        unsigned budget = maxValuesBehindRepeatedRead;
        for (;;) {
            std::span<const ValueID> values = function.at(block).values.span();
            while (index--) {
                if (!budget--)
                    return { };
                const Value& earlier = function.at(values[index]);
                Effects effects = analyses.effectsOf(values[index]).onAnyEdge();
                if (effects.mayRunOtherCode() || effects.writes.containsAny(kind.heapsRead))
                    return { };
                // The first read may know more of what it found: behind a test of its position it finds an element, where the second one says an element or undefined.
                bool answersSecondRead = earlier.type == read.type || context.program.at(earlier.type).isSubsetOf(context.program.at(read.type), context.program);
                if (earlier.opcode == kind.opcode && answersSecondRead && kind.hasSamePayload(earlier, read) && identitiesOf(earlier) == operands)
                    return values[index];
            }
            std::span<const BlockID> predecessors = cfg.predecessors(block);
            if (predecessors.size() != 1 || function.at(block).isHandlerBlock())
                return { };
            block = predecessors.front();
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Jump && !terminator.isConditionalBranch())
                return { };
            index = function.at(block).values.size();
        }
    };

    struct Repeated {
        ProgramPoint point;
        ValueID first;
    };
    BitVector keptReads = context.build.validates() ? findReadsKeptForValidation(function) : BitVector();
    HashMap<ValueID, ValueID> firstOfRepeated;
    Vector<Repeated, 8> repeated;
    ValueSubstitution substitution(function);
    for (BlockID block : cfg.reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != kind.opcode || keptReads.get(values[index].index()) || !kind.mayBeDropped(context, values[index], value))
                continue;
            ValueID first = findEarlierRead(block, index, value);
            if (!first)
                continue;
            if (auto found = firstOfRepeated.find(first); found != firstOfRepeated.end())
                first = found->value;
            firstOfRepeated.add(values[index], first);
            substitution.replace(values[index], first);
            repeated.append({ { block, index }, first });
        }
    }
    if (repeated.isEmpty())
        return PassOutcome::Unchanged;
    substitution.apply(function, &analyses);
    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    if (context.build.validates()) {
        // The repeated read stays, and its answer is held against the first one's.
        for (const Repeated& read : repeated) {
            auto [block, index] = read.point;
            ValueID kept = function.at(block).values[index];
            builder.setOrigin(function.at(kept).origin());
            builder.insertBefore({ block, index + 1 }, IRBuilder::make(Opcode::ValidateSameValue, { kept, read.first }));
        }
        builder.applyInsertions();
    } else {
        // The walk enters a block once, so the reads of one block lie side by side.
        for (size_t next = 0; next < repeated.size();) {
            BlockID block = repeated[next].point.block;
            BitVector erased;
            for (; next < repeated.size() && repeated[next].point.block == block; ++next)
                erased.set(repeated[next].point.index);
            builder.eraseValues(block, erased);
        }
    }
    analyses.didChangeValues();
    return PassOutcome::Changed;
}

// `list[k]` behind `list[k]` on a straight way, with nothing between them that writes elements or runs code of the program: `if (list[k] > list[k + 1]) { swap }`. The second takes what the first found.
static PassOutcome dropRepeatedElementReads(FunctionPassContext& context)
{
    return dropRepeatedReadsOnStraightWays(context, { Opcode::GetArrayElement, { AbstractHeap::Elements, AbstractHeap::ArrayStorage }, [](FunctionPassContext& context, ValueID id, const Value&) {
        return !context.analyses.effectsOf(id).onAnyEdge().mayExitSideways();
    }, [](const Value& read) {
        return std::pair { read.arrayOperand(), read.positionOperand() };
    }, [](const Value& earlier, const Value& read) {
        return earlier.arrayStorage() == read.arrayStorage() && earlier.readsHoleAsNaN() == read.readsHoleAsNaN();
    } });
}

static PassOutcome dropRepeatedPrivateReads(FunctionPassContext& context)
{
    return dropRepeatedReadsOnStraightWays(context, { Opcode::PrivateGet, { AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::ShapeWords }, [](FunctionPassContext& context, ValueID, const Value& read) {
        const Field& field = context.program.at(read.layout()).fields[read.field()];
        return field.form.isEmpty() && !field.flags.contains(FieldFlags::IsBrand);
    }, [](const Value& read) {
        return std::pair { read.objectOperand(), read.tokenOperand() };
    }, [](const Value& earlier, const Value& read) {
        return earlier.layout() == read.layout() && earlier.field() == read.field();
    } });
}

// `shape.radius + shape.radius`, each asked of the tables of the run time alone: while nothing is written and no code runs they answer the same, and so does the test whether they did.
static PassOutcome dropRepeatedReadsFromTables(FunctionPassContext& context)
{
    PassOutcome outcome = dropRepeatedReadsOnStraightWays(context, { Opcode::PropGet, { AbstractHeap::NamedProperties, AbstractHeap::ObjectFields, AbstractHeap::ShapeWords, AbstractHeap::PrototypeLinks, AbstractHeap::RuntimeState }, [](FunctionPassContext&, ValueID, const Value& read) {
        return read.asksOnlyTables();
    }, [](const Value& read) {
        return std::pair { read.objectOperand(), read.objectOperand() };
    }, [](const Value& earlier, const Value& read) {
        return earlier.asksOnlyTables() && earlier.key() == read.key();
    } });
    if (outcome == PassOutcome::Unchanged)
        return outcome;
    return outcome | dropRepeatedReadsOnStraightWays(context, { Opcode::DidReadAbove, { }, [](FunctionPassContext&, ValueID, const Value&) {
        return true;
    }, [](const Value& test) {
        return std::pair { test.valueOperand(), test.valueOperand() };
    }, [](const Value&, const Value&) {
        return true;
    } });
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

// Closures and the environments they share, made here, that only hold each other and are stored to: nothing can tell whether they were made.
static PassOutcome removeClosuresNothingReads(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::NewClosure) || !context.analyses.opcodesOfValues().contains(Opcode::PropSet))
        return PassOutcome::Unchanged;
    Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    BitVector candidates(function.values.size());
    Vector<ValueID, 8> made;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            // A closure with one object for its function hands that object to whoever reads it from elsewhere, a method above all: what is stored on it stays.
            // A frame of a function that may be renamed shows the name of its object when the stack is read.
            auto isFreshClosure = [&] {
                if (value.isMadeOnDemand() || namesClosureOfTable(context.program, value))
                    return false;
                FunctionID callee = value.function();
                return !context.inputs.flow.closuresMayBeRenamed(callee) && !context.program.at(callee).has(FunctionCapabilities::RecordsCallee) && !context.program.at(writtenFunctionOf(context.program, callee)).has(FunctionCapabilities::RecordsCallee);
            };
            bool isFresh = value.opcode == Opcode::NewObject || (value.opcode == Opcode::NewClosure && isFreshClosure());
            if (isFresh) {
                candidates.quickSet(id.index());
                made.append(id);
            }
        }
    }
    auto isOnlyHeldOrStoredTo = [&](ValueID id) {
        return std::ranges::all_of(useLists.usesOf(id), [&](const Use& use) {
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = function.at(use.user);
            switch (user.opcode) {
            case Opcode::NewClosure:
                return candidates.quickGet(use.user.index());
            case Opcode::PutEnvironmentField:
                return candidates.quickGet(user.environmentOperand().index());
            case Opcode::PropSet:
                return function.at(id).opcode == Opcode::NewClosure || use.operandIndex == OperandIndex::PropSet::value
                    ? candidates.quickGet(user.objectOperand().index()) && function.at(user.objectOperand()).opcode == Opcode::NewClosure && !user.isFreeName() && isPlainStoreToMemberOfClosure(context, user)
                    : false;
            default:
                return false;
            }
        });
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (ValueID id : made) {
            if (candidates.quickGet(id.index()) && !isOnlyHeldOrStoredTo(id)) {
                candidates.quickClear(id.index());
                changed = true;
            }
        }
    }
    if (!std::ranges::any_of(made, [&](ValueID id) { return candidates.quickGet(id.index()) && function.at(id).opcode == Opcode::NewClosure; }))
        return PassOutcome::Unchanged;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        BitVector erased;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            bool isErased = candidates.quickGet(values[index].index());
            if (value.opcode == Opcode::PutEnvironmentField)
                isErased = candidates.quickGet(value.environmentOperand().index());
            if (value.opcode == Opcode::PropSet)
                isErased = candidates.quickGet(value.objectOperand().index());
            if (isErased)
                erased.set(index);
        }
        if (!erased.isEmpty())
            builder.eraseValues(block, erased);
    }
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

static PassOutcome eliminateReadsOnce(FunctionPassContext& context)
{
    // First: reads and tests under one sum are then reads and tests under one key.
    PassOutcome outcome = dropRepeatedIntegerSums(context);
    outcome |= dropRepeatedElementReads(context);
    outcome |= dropRepeatedPrivateReads(context);
    if (context.analyses.opcodesOfValues().intersects(opcodesReadingAccessedHeaps))
        outcome |= RedundantReadElimination { context }.run();
    outcome |= dropRepeatedMapSets(context);
    outcome |= dropRepeatedBrandTests(context);
    outcome |= dropRepeatedStrictComparisons(context);
    outcome |= dropRepeatedInitializations(context);
    outcome |= forwardStoresToMembersOfClosures(context);
    return outcome;
}

PassOutcome eliminateRedundantReads(FunctionPassContext& context)
{
    PassOutcome outcome = eliminateReadsOnce(context);
    // What a read forwards may be a closure just made, and what a capture hands out may be read again. A second run of this pass must find nothing.
    while (true) {
        PassOutcome closures = readCapturesOfClosuresJustMade(context);
        closures |= removeClosuresNothingReads(context);
        if (closures == PassOutcome::Unchanged)
            return outcome;
        outcome = PassOutcome::Changed;
        eliminateReadsOnce(context);
    }
}

namespace {

constexpr AbstractHeapSet carriedHeaps = heapsOfVariables | AbstractHeapSet { AbstractHeap::ObjectFields };
// The results of these tests hold until something writes what they read.
constexpr OpcodeSet objectTestsHoistedBeforeLoops { Opcode::HasBirthFieldsFilled };
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

    // The copy is entered in the middle of a turn. What the loop has made by then joins what the copy makes there: a parameter for each such value that is live.
    // Entries times live values is all that grows faster than the loop here: a loop that carries n sums, each with a path that leaves, would get n x n parameters.
    // So a loop of any size is copied as long as the joins stay in proportion to the copy. Of 6,000 loops of real programs the most has 1.3 joins for a value copied, half have 0.12.
    bool joinsMoreThanItCopies(std::span<const BlockID> region, std::span<const BlockID> entriesOfCopy)
    {
        AOT_THRESHOLD(size_t, joinsForEachValueCopied, 2);
        BitVector isInLoop(m_function.blocks.size());
        size_t mostValuesJoined = 0;
        for (BlockID block : region) {
            isInLoop.set(block.index());
            mostValuesJoined += m_function.at(block).parameters.size() + m_function.at(block).values.size();
        }
        mostValuesJoined *= joinsForEachValueCopied;
        const Liveness& liveness = m_analyses.liveness();
        const UseLists& uses = m_analyses.useLists();
        size_t numberOfValuesJoined = 0;
        for (BlockID entry : entriesOfCopy) {
            for (ValueID live : liveness.liveAtHead(entry)) {
                if (isInLoop.get(uses.blockOf(live).index()) && ++numberOfValuesJoined > mostValuesJoined)
                    return true;
            }
        }
        return false;
    }

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
        AOT::stableSortInLine(sorted, { }, &NaturalLoop::size);
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
        Expected<bool> isReadInEveryFullIteration { false };
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
        bool tookHeadsOutOfLoops = false;
        for (BlockID header : headersFromInnermost()) {
            const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
            if (loop && isHeaderOfLoopOfProgram(header) && beginLaterTurnsBehindUnchangingHead(*loop))
                tookHeadsOutOfLoops = true;
        }
        if (tookHeadsOutOfLoops) {
            removeTrivialParameters(m_context);
            retype();
        }
        bool hoistedObjectTests = false;
        if (m_analyses.opcodesOfValues().intersects(objectTestsHoistedBeforeLoops)) {
            for (BlockID header : headersFromInnermost()) {
                const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
                if (loop && isHeaderOfLoopOfProgram(header) && moveLeadingTestsAheadOf(*loop, LeadingTests::OfObjects))
                    hoistedObjectTests = true;
            }
            // The loop's accesses name the value proven before the loop, not a parameter that is only ever passed that value. Nothing is remembered by number from here on.
            if (hoistedObjectTests) {
                removeTrivialParameters(m_context);
                retype();
            }
        }
        BitVector isHeaderOfChangedLoop;
        for (BlockID header : headersFromInnermost()) {
            BitVector leaves;
            if (carryAround(header, leaves))
                isHeaderOfChangedLoop.set(header.index());
        }
        if (isHeaderOfChangedLoop.isEmpty())
            return hoistedObjectTests || tookHeadsOutOfLoops ? PassOutcome::Changed : PassOutcome::Unchanged;

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

    // An object's slot is a variable if what the access relies on is established before the loop.
    bool isCarriedAccess(const NaturalLoop& loop, const MemoryAccess& access, const Value& value)
    {
        if (access.location.heap() != AbstractHeap::ObjectFields)
            return true;
        if (value.opcode != Opcode::GetSlot && value.opcode != Opcode::PutSlot)
            return false;
        ValueID guard = value.guardOperand();
        if (!guard)
            return value.level() == KnowledgeLevel::Proven;
        return !m_analyses.naturalLoops().belongsTo(m_analyses.useLists().locationOf(guard).block, loop);
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
                if (stays && access && carriedHeaps.contains(access.location.heap()) && !access.location.isEntireHeap() && isCarriedAccess(loop, access, m_function.at(id))) {
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
        // The same if the loop does not exit at one of its own tests. A loop that exits immediately has read the variable for nothing, which costs a load.
        BitVector runsInFirstFullIteration(m_function.blocks.size());
        bool mayHaveEnded = false;
        // A path that is seldom taken does not count above. That a variable has been stored may hang on the very test that such a path begins with.
        std::optional<ProgramPoint> firstWayOut;
        BitVector beginsBeforeAnyWayOut(m_function.blocks.size());
        for (BlockID block = loop.header(); m_firstTurnRunsHere && !runsInFirstFullIteration.set(block.index());) {
            if (!mayHaveEnded)
                runsInFirstTurn.set(block.index());
            const BlockValues& values = m_function.at(block).values;
            if (!firstWayOut)
                beginsBeforeAnyWayOut.set(block.index());
            for (uint32_t index = 0; index < values.size() && !firstWayOut; ++index) {
                if (mayLeaveOrStoreToVariable(m_analyses.effectsOf(values[index]).onAnyEdge()))
                    firstWayOut = ProgramPoint { block, index };
            }
            BlockID next;
            bool forks = false;
            for (const Target& target : m_function.at(block).terminator.targets) {
                if (m_stays.get(target.block.index())) {
                    forks |= !!next;
                    next = target.block;
                    continue;
                }
                if (!firstWayOut)
                    firstWayOut = pointBeforeTerminatorOf(m_function, block);
                if (!loops.belongsTo(target.block, loop))
                    mayHaveEnded |= (m_function.at(target.block).frequency <= m_function.at(loop.header()).frequency).predicted();
            }
            if (forks || !next)
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
            variable->isReadInEveryFullIteration = variable->isReadInEveryFullIteration.predicted() || (runsInFirstFullIteration.get(access.point.block.index()) && dominatesAll(access.point.block, latches.span()));
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
        // A variable that is carried in a register but still stored in every iteration gains little, and storing a string means creating it. So after a test that may exit the loop it is only carried if its stores leave the loop too.
        std::optional<bool> storesCanLeave;
        auto isWorthPreloading = [&](const CarriedVariable& variable) {
            if (variable.isReadInEveryTurn.predicted())
                return true;
            if (!variable.isReadInEveryFullIteration.predicted() || variable.stores.isEmpty() || !variable.blocksOfOtherWrites.isEmpty() || variable.isWrittenOnLeavingPath)
                return false;
            if (!storesCanLeave)
                storesCanLeave = leaves.isEmpty() && noCodeObservesVariablesIn(loop);
            return *storesCanLeave;
        };
        // A slot that the loop only reads is hoisted by other code. An integer in a slot is incremented in place, in one instruction with its test.
        auto doesNotBenefitFromRegister = [&](const CarriedVariable& variable) {
            return variable.location.heap() == AbstractHeap::ObjectFields && (variable.stores.isEmpty() || typeOf(variable.reads.first()).isInteger());
        };
        m_variables.removeAllMatching([&](const CarriedVariable& variable) {
            if (variable.isReadThroughCastInLoop || doesNotBenefitFromRegister(variable) || !isWorthPreloading(variable) || !hasTypesOfReads(variable) || std::ranges::any_of(variable.blocksOfOtherWrites, hasToStay))
                return true;
            return variable.stores.isEmpty() && !variable.isWrittenOnLeavingPath && !isReadBy(variable, Opcode::TypeTest) && !isReadBy(variable, Opcode::GetCapture);
        });
    }

    // Adds the blocks that stay in a loop when `leaves` has left it and that may write a variable which every turn reads, other than by a store of the loop's own. False, and nothing is added: there is no such variable, or the loop no longer turns without them.
    enum class WhatPays : bool { Reads, Stores };
    bool letWritersOfVariablesLeave(const NaturalLoop& loop, BitVector& leaves, WhatPays whatPays)
    {
        findVariables(loop, leaves);
        if (m_variables.isEmpty())
            return false;
        if (whatPays == WhatPays::Stores && std::ranges::all_of(m_variables, [](const CarriedVariable& variable) { return variable.stores.isEmpty(); }))
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
            const Value& read = m_function.at(id);
            return read.opcode == Opcode::GetModuleVariable && (read.factOperand() || holdsDeclaredTypeOnlyBehindEntry(m_program, m_function, read));
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

    // `for (let i = 0; i < queue.length; i++)` where the queue is an array or an object with a layout: every turn begins by telling the two apart and reads the length of the one it has found,
    // and the two ways join in front of the comparison. Nothing in the loop writes what they read, so every turn finds what the first one found.
    struct UnchangingHead {
        Vector<BlockID, 8> blocks { };
        BlockID join { };
        // For each parameter of the join, the parameter of the header that every way passes for it. None: what is passed is the same in every turn.
        Vector<std::optional<size_t>, 4> parametersOfHeaderPassedOn { };
    };

    // Nothing if the loop runs other code.
    std::optional<LocationMasks> findWritesOf(const NaturalLoop& loop)
    {
        LocationMasks writes;
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID block = loop.at(position);
            Effects ofTerminator = m_analyses.effectsOfTerminator(block).onAnyEdge();
            if (ofTerminator.mayRunOtherCode())
                return std::nullopt;
            writes.merge(ofTerminator.writes);
            for (ValueID value : m_function.at(block).values) {
                Effects effects = m_analyses.effectsOf(value).onAnyEdge();
                // A store to a slot leaves the field in its slot. Any other store to a field may give it a value that the slot cannot hold.
                bool mayTakeFieldOutOfSlot = m_function.at(value).opcode != Opcode::PutSlot && !effects.writes.restrictedTo({ AbstractHeap::ObjectFields }).isEmpty();
                if (mayTakeFieldOutOfSlot || effects.mayRunOtherCode())
                    return std::nullopt;
                writes.merge(effects.writes);
            }
        }
        return writes;
    }

    // The blocks from the header on that only ask what has one answer, as far as all their ways through the loop lead to one block.
    std::optional<UnchangingHead> findUnchangingHead(const NaturalLoop& loop)
    {
        const CFG& cfg = m_analyses.cfg();
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const UseLists& useLists = m_analyses.useLists();
        BlockID header = loop.header();
        const CompactList<ValueID>& parametersOfHeader = m_function.at(header).parameters;

        BitVector staysTheSame(m_function.values.size());
        auto isSameInEveryTurn = [&](ValueID value) {
            return staysTheSame.get(value.index()) || !loops.belongsTo(useLists.locationOf(value).block, loop);
        };
        std::optional<LocationMasks> writes;
        auto onlyAsks = [&](BlockID id) {
            const Block& block = m_function.at(id);
            const Terminator& terminator = block.terminator;
            if (block.handler || block.isHandlerBlock() || loops.innerMostLoopOf(id) != &loop || (terminator.kind != TerminatorKind::Jump && terminator.kind != TerminatorKind::Branch))
                return false;
            for (ValueID value : block.values) {
                const Value& operation = m_function.at(value);
                if (operation.opcode == Opcode::LoopHint && id == header)
                    continue;
                bool hasSameOperands = true;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    hasSameOperands &= isSameInEveryTurn(operand);
                });
                Effects effects = m_analyses.effectsOf(value).onAnyEdge();
                if (!hasSameOperands || !effects.isUnobservable() || !effects.writes.isEmpty())
                    return false;
                if (!effects.reads.isEmpty()) {
                    if (!writes && !(writes = findWritesOf(loop)))
                        return false;
                    if (effects.reads.overlaps(*writes))
                        return false;
                }
                staysTheSame.set(value.index());
            }
            if (terminator.kind == TerminatorKind::Branch && !isSameInEveryTurn(terminator.conditionOperand()))
                return false;
            return std::ranges::none_of(terminator.targets, [&](const Target& target) {
                return target.block == header;
            });
        };
        auto waysOnThroughLoop = [&](BlockID id) -> size_t {
            return std::ranges::count_if(m_function.at(id).terminator.targets, [&](const Target& target) {
                return loops.belongsTo(target.block, loop);
            });
        };

        if (!onlyAsks(header))
            return std::nullopt;
        UnchangingHead head { { header } };
        BitVector isInHead(m_function.blocks.size());
        isInHead.set(header.index());
        size_t openWays = waysOnThroughLoop(header);
        // A head with one way through the loop is a row of leading tests, which moveLeadingTestsAheadOf() and the back end take.
        bool forks = openWays > 1;
        size_t numberOfBlocksAheadOfJoin = 0;

        for (BlockID id : cfg.reversePostOrder()) {
            if (id == header || !loops.belongsTo(id, loop))
                continue;
            const Block& block = m_function.at(id);
            std::span<const Edge> waysIn = cfg.incomingEdges(id);
            bool isEnteredFromHeadAlone = std::ranges::all_of(waysIn, [&](Edge edge) {
                return !edge.isHandlerEdge() && isInHead.get(edge.from.index());
            });
            if (!isEnteredFromHeadAlone)
                break;

            Vector<std::optional<size_t>, 4> passedOn(block.parameters.size());
            bool canBeJoin = !block.handler && !block.isHandlerBlock();
            bool getsTheSameInEveryTurn = true;
            for (size_t position = 0; position < block.parameters.size(); ++position) {
                size_t numberOfWaysThatPassParameter = 0;
                for (Edge edge : waysIn) {
                    const Arguments& arguments = targetOf(m_function, edge).arguments;
                    // What a test of the head has proven.
                    if (position >= arguments.size() || isSameInEveryTurn(arguments[position]))
                        continue;
                    size_t index = parametersOfHeader.find(arguments[position]);
                    canBeJoin &= index != notFound && (!passedOn[position] || *passedOn[position] == index);
                    passedOn[position] = index;
                    ++numberOfWaysThatPassParameter;
                }
                if (!numberOfWaysThatPassParameter)
                    continue;
                getsTheSameInEveryTurn = false;
                canBeJoin &= numberOfWaysThatPassParameter == waysIn.size();
            }
            if (canBeJoin && forks && waysIn.size() == openWays) {
                head.join = id;
                head.parametersOfHeaderPassedOn = WTF::move(passedOn);
                numberOfBlocksAheadOfJoin = head.blocks.size();
            }
            if (!getsTheSameInEveryTurn)
                break;
            for (ValueID parameter : block.parameters)
                staysTheSame.set(parameter.index());
            if (!onlyAsks(id))
                break;
            head.blocks.append(id);
            isInHead.set(id.index());
            size_t waysOn = waysOnThroughLoop(id);
            forks |= waysOn > 1;
            openWays = openWays - waysIn.size() + waysOn;
        }
        if (!head.join)
            return std::nullopt;
        head.blocks.shrink(numberOfBlocksAheadOfJoin);
        return head;
    }

    // The head stays where it is and runs once, as the beginning of the first turn. Every later turn begins where the ways through the head join, with what the first turn has found. Nothing is copied.
    bool beginLaterTurnsBehindUnchangingHead(const NaturalLoop& loop)
    {
        BlockID header = loop.header();
        Vector<Edge, 4> latches;
        for (Edge edge : m_analyses.cfg().incomingEdges(header)) {
            if (!m_analyses.naturalLoops().belongsTo(edge.from, loop))
                continue;
            if (edge.isHandlerEdge() || hasEdgeResults(m_function, edge))
                return false;
            latches.append(edge);
        }
        std::optional<UnchangingHead> head = findUnchangingHead(loop);
        if (!head)
            return false;
        BlockID join = head->join;
        Vector<Edge, 4> waysToJoin;
        waysToJoin.append(m_analyses.cfg().incomingEdges(join));
        Vector<ValueID, 4> parametersOfHeader;
        parametersOfHeader.append(m_function.at(header).parameters.span());
        size_t numberOfAnswers = m_function.at(join).parameters.size();

        IRBuilder builder = makeBuilder();
        builder.setOrigin(m_function.at(header).terminator.origin());
        BlockID landing = builder.addBlock(m_function.at(join).frequency);
        Arguments entering;
        for (size_t position = 0; position < numberOfAnswers; ++position) {
            const Value& parameter = m_function.at(m_function.at(join).parameters[position]);
            TypeID type = parameter.type;
            ExpectedType expected = parameter.expected();
            ValueID found = builder.addParameter(landing, type);
            m_function.edit(found, ValueEdit::Type).setExpected(expected);
            entering.append(found);
        }
        for (Edge edge : waysToJoin) {
            BlockID way = builder.splitEdge(edge);
            Terminator jump = takeTerminator(builder, way);
            builder.setOrigin(jump.origin());
            builder.emitJump(landing, WTF::move(jump.targets[0].arguments));
        }

        builder.setOrigin(m_function.at(header).terminator.origin());
        Vector<Vector<ValueID, 4>, 4> definitionsOfVariables;
        constexpr uint32_t noEdgeResults = 0;
        for (ValueID parameter : parametersOfHeader) {
            TypeID type = m_function.at(parameter).type;
            ExpectedType expected = m_function.at(parameter).expected();
            ValueID carried = builder.addParameter(join, type, noEdgeResults);
            m_function.edit(carried, ValueEdit::Type).setExpected(expected);
            definitionsOfVariables.append({ parameter, carried });
            entering.append(parameter);
        }
        builder.moveToEndOf(landing);
        builder.emitJump(join, WTF::move(entering));
        for (Edge latch : latches) {
            Arguments handedOn(targetOf(m_function, latch).arguments);
            Arguments arguments;
            for (size_t position = 0; position < numberOfAnswers; ++position) {
                std::optional<size_t> passedOn = head->parametersOfHeaderPassedOn[position];
                arguments.append(passedOn ? handedOn[*passedOn] : m_function.at(join).parameters[position]);
            }
            arguments.append(handedOn.span());
            builder.redirectEdge(latch, join, WTF::move(arguments));
        }

        // A way out of the head may come back to it, through the copy of a separated loop: that cycle keeps its mark.
        for (ValueID value : m_function.at(header).values) {
            if (m_function.at(value).opcode != Opcode::LoopHint)
                continue;
            builder.setOrigin(m_function.at(value).origin());
            builder.insertBefore({ join, 0 }, Value(m_function.at(value)));
        }
        builder.applyInsertions();
        repair(builder, definitionsOfVariables.span());
        return true;
    }

    struct LeadingTest {
        BlockID block;
        std::optional<size_t> parameterOfHeader;
        // The block only computes and decides whether the loop continues. It is also evaluated before the loop, and again in the first iteration with the same result.
        bool staysInLoop { false };
    };
    enum class LeadingTests : uint8_t { OfTypes, OfObjects };

    bool noWriteClobbersReadsOf(const NaturalLoop& loop, ValueID test)
    {
        LocationMasks reads = m_analyses.effectsOf(test).onAnyEdge().reads;
        auto preservesIt = [&](const Effects& effects) {
            return !effects.mayRunOtherCode() && !effects.writes.overlaps(reads);
        };
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID block = loop.at(position);
            if (!preservesIt(m_analyses.effectsOfTerminator(block).onAnyEdge()))
                return false;
            for (ValueID value : m_function.at(block).values) {
                Effects effects = m_analyses.effectsOf(value).onAnyEdge();
                // A store to a slot leaves the field in its slot. Any other store to a field may give it a value that the slot cannot hold.
                bool mayTakeFieldOutOfSlot = m_function.at(value).opcode != Opcode::PutSlot && !effects.writes.restrictedTo({ AbstractHeap::ObjectFields }).isEmpty();
                if (mayTakeFieldOutOfSlot || !preservesIt(effects))
                    return false;
            }
        }
        return true;
    }

    // A test of what an earlier one of these tests has proven tests the same value.
    Vector<LeadingTest, 4> findLeadingTests(const NaturalLoop& loop, LeadingTests wanted)
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
            auto onlyComputes = [&] {
                return std::ranges::all_of(block.values, [&](ValueID value) {
                    return m_function.at(value).opcode == Opcode::LoopHint || m_analyses.effectsOf(value).onAnyEdge().isPure();
                });
            };
            // Where a test has gone ahead of the loop already, its block goes on to the next one.
            if (!block.handler && block.terminator.kind == TerminatorKind::Jump) {
                BlockID next = block.terminator.next().block;
                if (!onlyComputes() || !loops.belongsTo(next, loop) || next == loop.header() || !cfg.singleIncomingEdge(next))
                    break;
                tests.append({ id, std::nullopt, true });
                proven.append(ValueID());
                id = next;
                continue;
            }
            if (block.handler || block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.conditionOperand() != block.values.last())
                break;
            const Value& test = m_function.at(block.values.last());
            // A fact and the cast that names it go ahead of the loop with the test, behind the tests in front of them.
            auto isCastBehindFact = [&](ValueID value) {
                return m_function.at(value).opcode == Opcode::AssumeType && m_function.at(value).factOperand() && useLists.locationOf(value).block == id;
            };
            // What only computes is computed ahead of the loop as well, for nothing.
            bool holdsNothingElse = std::ranges::all_of(block.values.span().first(block.values.size() - 1), [&](ValueID value) {
                Opcode opcode = m_function.at(value).opcode;
                return opcode == Opcode::LoopHint || makesFact(opcode) || isCastBehindFact(value) || m_analyses.effectsOf(value).onAnyEdge().isPure();
            });
            auto staysAndLeadsOn = [&] {
                const Target& first = block.terminator.targets[0];
                const Target& second = block.terminator.targets[1];
                if (!onlyComputes() || loops.belongsTo(first.block, loop) == loops.belongsTo(second.block, loop))
                    return false;
                BlockID next = loops.belongsTo(first.block, loop) ? first.block : second.block;
                if (next == loop.header() || !cfg.singleIncomingEdge(next))
                    return false;
                tests.append({ id, std::nullopt, true });
                proven.append(ValueID());
                id = next;
                return true;
            };
            if (wanted == LeadingTests::OfObjects ? !objectTestsHoistedBeforeLoops.contains(test.opcode) : test.opcode != Opcode::TypeTest) {
                if (!staysAndLeadsOn())
                    break;
                continue;
            }
            if (test.opcode != (wanted == LeadingTests::OfTypes ? Opcode::TypeTest : test.opcode) || !holdsNothingElse || useLists.useCount(block.values.last()) != 1)
                break;
            const Target& taken = block.terminator.targets[0];
            const Target& notTaken = block.terminator.targets[1];
            if (!loops.belongsTo(taken.block, loop) || taken.block == loop.header() || !cfg.singleIncomingEdge(taken.block) || loops.belongsTo(notTaken.block, loop))
                break;
            if (wanted == LeadingTests::OfObjects) {
                bool testsOnlyLoopInvariantValues = true;
                test.forEachOperand([&](ValueID operand, uint32_t) {
                    testsOnlyLoopInvariantValues &= !loops.belongsTo(useLists.locationOf(operand).block, loop);
                });
                if (!testsOnlyLoopInvariantValues || taken.proofs.isEmpty() || !noWriteClobbersReadsOf(loop, block.values.last()))
                    break;
                tests.append({ id, std::nullopt });
                proven.append(m_function.at(taken.block).parameters.last());
                id = taken.block;
                continue;
            }

            ValueID tested = test.valueOperand();
            if (isCastBehindFact(tested))
                tested = m_function.at(tested).valueOperand();
            std::optional<size_t> parameterOfHeader;
            if (size_t position = proven.find(tested); position != notFound)
                parameterOfHeader = tests[position].parameterOfHeader;
            else if (size_t index = header.parameters.find(tested); index != notFound)
                parameterOfHeader = index;
            else if (loops.belongsTo(useLists.locationOf(tested).block, loop)) {
                if (!staysAndLeadsOn())
                    break;
                continue;
            }
            // A counter that is not yet known to stay an int32 is tested in every turn. Such a test decides whether the loop goes on, like the test that ends it.
            if (parameterOfHeader && (!isPassedOnWithType(*parameterOfHeader, test.testedType()) || !isNarrowedByTest(header.parameters[*parameterOfHeader], test.testedType()))) {
                if (!staysAndLeadsOn())
                    break;
                continue;
            }
            tests.append({ id, parameterOfHeader });
            proven.append(taken.proofs.isEmpty() ? ValueID() : m_function.at(taken.block).parameters.last());
            id = taken.block;
        }
        while (!tests.isEmpty() && tests.last().staysInLoop)
            tests.removeLast();
        return tests;
    }

    bool moveLeadingTestsAheadOf(const NaturalLoop& loop, LeadingTests wanted = LeadingTests::OfTypes)
    {
        BlockID header = loop.header();
        std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
        if (!entry || hasEdgeResults(m_function, *entry))
            return false;
        Vector<LeadingTest, 4> tests = findLeadingTests(loop, wanted);
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
            if (taken.proofs.isEmpty() || tests[position].staysInLoop)
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
            if (tests[position].staysInLoop)
                continue;
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

    // An operation that only throws when out of memory lets the stores wait: a handler makes them where it throws.
    Effects effectsOnDelayedStores(ValueID value)
    {
        Effects effects = m_analyses.effectsOf(value).onAnyEdge();
        if (opcodesThrowingOnlyForLackOfMemory.contains(m_function.at(value).opcode) && !effects.mayThrowByRule())
            effects.mayThrow = false;
        return effects;
    }

    bool letsStoresWait(const Effects& effects) const
    {
        return (!effects.mayThrow && !effects.mayRunOtherCode()) || m_context.build.isSabotaged(Sabotage::DelayedStoresPassWhatMayThrow);
    }

    // The loop is only left through its edges, and nothing in it runs code.
    bool noCodeObservesVariablesIn(const NaturalLoop& loop)
    {
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID id = loop.at(position);
            const Block& block = m_function.at(id);
            if (block.handler || block.isHandlerBlock() || !letsStoresWait(m_analyses.effectsOfTerminator(id).onAnyEdge()))
                return false;
            for (ValueID value : block.values) {
                if (!letsStoresWait(effectsOnDelayedStores(value)))
                    return false;
            }
        }
        return true;
    }

    // At the head of a turn the parameter is what the variable should hold, behind a store it is what was stored.
    bool delayStoresIn(const NaturalLoop& loop, std::span<const VariableOfLoop> variables)
    {
        struct DelayedVariable {
            VariableOfLoop variable;
            Vector<ProgramPoint, 2> stores { };
            bool isSeenByOthers { false };
            TypeID typeOfField { };
        };
        const NaturalLoops& loops = m_analyses.naturalLoops();
        BlockID header = loop.header();
        Vector<DelayedVariable, 4> delayed;
        for (const VariableOfLoop& variable : variables) {
            if (m_function.at(header).parameters.contains(variable.parameter))
                delayed.append({ variable });
        }

        Vector<Edge, 8> leavingEdges;
        Vector<ProgramPoint, 4> pointsThatMayRunOutOfMemory;
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID id = loop.at(position);
            const Block& block = m_function.at(id);
            if (block.handler || block.isHandlerBlock())
                return false;
            auto look = [&](Effects effects) {
                if (!letsStoresWait(effects))
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
                if (!look(effectsOnDelayedStores(value)))
                    return false;
                // A poll throws what an allocation has left owed. Under a handler the back end no longer takes it for the poll of a loop.
                Effects effects = m_analyses.effectsOf(value).onAnyEdge();
                if (effects.mayThrow && !effects.mayThrowByRule() && m_function.at(value).opcode != Opcode::LoopHint)
                    pointsThatMayRunOutOfMemory.append({ id, index });
            }
            if (!look(m_analyses.effectsOfTerminator(id).onAnyEdge()))
                return false;
            for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
                if (!loops.belongsTo(block.terminator.targets[targetIndex].block, loop))
                    leavingEdges.append(Edge { id, targetIndex });
            }
        }
        bool overlooksOthers = m_context.build.isSabotaged(Sabotage::DelayedStoresAreSeenByOthers);
        delayed.removeAllMatching([&](const DelayedVariable& candidate) {
            if (candidate.stores.isEmpty())
                return true;
            if (candidate.isSeenByOthers)
                m_context.build.statistics().count(StatisticID::StoresLeftInLoopsForOthers);
            return candidate.isSeenByOthers && !overlooksOthers;
        });

        auto storeAt = [&](ProgramPoint point) -> const Value& {
            return m_function.at(m_function.at(point.block).values[point.index]);
        };
        // The store on a leaving edge is a copy of the first store. What that names beside the stored value has to be there on every way out: a cast of the object, or a fact,
        // that is made on one path through the loop is not.
        auto isThereOnEveryWayOut = [&](ValueID operand) {
            return std::ranges::all_of(leavingEdges, [&](Edge edge) {
                return m_analyses.dominates(operand, pointBeforeTerminatorOf(m_function, edge.from));
            }) && std::ranges::all_of(pointsThatMayRunOutOfMemory, [&](ProgramPoint point) {
                return m_analyses.dominates(operand, point);
            });
        };
        auto canBeCopiedToEveryWayOut = [&](ProgramPoint point) {
            const Value& store = storeAt(point);
            uint32_t storedOperandIndex = memoryAccessOf(m_function, store).storedOperandIndex;
            for (uint32_t index = 0; index < store.numberOfOperands(); ++index) {
                ValueID operand = store.operand(index);
                if (index != storedOperandIndex && operand && !isThereOnEveryWayOut(operand))
                    return false;
            }
            return true;
        };
        delayed.removeAllMatching([&](DelayedVariable& candidate) {
            auto found = std::ranges::find_if(candidate.stores, canBeCopiedToEveryWayOut);
            if (found == candidate.stores.end())
                return true;
            std::swap(candidate.stores.first(), *found);
            return false;
        });

        // It stores any definition of the variable. Each definition needs the type that the first store requires.
        // Only accesses by slot can differ in that type: each names its own set of layouts.
        TypingContext typing { m_program, m_function, m_context.types };
        delayed.removeAllMatching([&](DelayedVariable& candidate) {
            const Value& first = storeAt(candidate.stores.first());
            if (!Value::opcodesWithReceivers.contains(first.opcode))
                return false;
            MemoryAccess access = memoryAccessOf(m_function, first);
            TypeID required = requiredTypeOfOperand(typing, first, access.storedOperandIndex);
            bool otherStoreIsWider = std::ranges::any_of(candidate.stores, [&](ProgramPoint point) {
                return !hasType(memoryAccessOf(m_function, storeAt(point)).stored, required);
            });
            if (otherStoreIsWider)
                return true;
            if (hasType(candidate.variable.parameter, required))
                return false;
            // The read that fills the variable allows more layouts than the store. What it finds has the type of the one field that the store names if the object is proven to have that layout in the whole loop.
            bool namesOneField = first.level() == KnowledgeLevel::Proven && m_program.at(first.receivers()).completeMembers().value().size() == 1;
            candidate.typeOfField = required;
            return !namesOneField || !hasType(candidate.variable.base, m_function.at(access.base).type);
        });
        if (delayed.isEmpty() || leavingEdges.isEmpty())
            return false;

        IRBuilder builder = makeBuilder();
        // An error for lack of memory can be caught, and whoever catches it reads the variables. So each operation that may throw one stands alone in a block,
        // under a handler that makes the stores with what the variables hold there and throws the error again. The header keeps no handler.
        bool eachStandsAlone = std::ranges::all_of(pointsThatMayRunOutOfMemory, [&](ProgramPoint point) {
            return point.block != header && m_function.at(point.block).values.size() == 1;
        });
        if (!eachStandsAlone) {
            // Last to first: what stands in front of a split keeps its place.
            for (ProgramPoint point : pointsThatMayRunOutOfMemory | std::views::reverse) {
                Vector<uint32_t, 2> indexes;
                if (point.index || point.block == header)
                    indexes.append(point.index);
                if (point.index + 1 < m_function.at(point.block).values.size())
                    indexes.append(point.index + 1);
                if (!indexes.isEmpty())
                    builder.splitBlock(point.block, indexes.span());
            }
            m_analyses.didChangeControlFlow();
            if (const NaturalLoop* sameLoop = m_analyses.naturalLoops().headerOf(header))
                delayStoresIn(*sameLoop, variables);
            return true;
        }

        Vector<ProgramPoint, 8> pointsOfStores;
        for (Edge edge : leavingEdges)
            pointsOfStores.append({ builder.splitEdge(edge), 0 });
        for (ProgramPoint point : pointsThatMayRunOutOfMemory) {
            builder.setOrigin(m_function.at(m_function.at(point.block).values.first()).origin());
            builder.setHandler(std::nullopt);
            BlockID failed = builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
            m_function.edit(point.block, { BlockEdit::Edges, BlockEdit::Arguments }).handler = Target { failed, { }, { } };
            builder.moveToEndOf(failed);
            builder.emitRethrow(builder.append(Opcode::CatchException, { }));
            pointsOfStores.append({ failed, 1 });
        }

        Vector<Vector<ValueID, 4>, 4> definitionsOfVariables(delayed.size());
        Vector<ValueID, 4> delayedStores;
        for (size_t index = 0; index < delayed.size(); ++index) {
            const DelayedVariable& candidate = delayed[index];
            Vector<ValueID, 4>& definitions = definitionsOfVariables[index];
            Value store = storeAt(candidate.stores.first());
            uint32_t storedOperandIndex = memoryAccessOf(m_function, store).storedOperandIndex;
            builder.setOrigin(store.origin());
            if (candidate.typeOfField) {
                Value cast = IRBuilder::make(Opcode::AssumeType, { candidate.variable.parameter });
                cast.setProvenType(Proven::byPosition(candidate.typeOfField));
                definitions.append(builder.insertBefore({ header, 0 }, WTF::move(cast)));
            } else
                definitions.append(insertCopy(builder, { header, 0 }, candidate.variable.parameter));
            for (ProgramPoint point : candidate.stores) {
                ValueID delayedStore = m_function.at(point.block).values[point.index];
                definitions.append(insertCopy(builder, { point.block, point.index + 1 }, memoryAccessOf(m_function, m_function.at(delayedStore)).stored));
                delayedStores.append(delayedStore);
            }
            for (ProgramPoint point : pointsOfStores) {
                if (m_context.build.isSabotaged(Sabotage::DelayedStoresAreLeftOut))
                    break;
                Value storeOnEdge = store;
                storeOnEdge.setOperand(storedOperandIndex, definitions.first());
                builder.insertBefore(point, WTF::move(storeOnEdge));
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
        Vector<TypeID, 4> typesOfCounters { };
        BitVector carriedValuesThatMayBeUnspecified { };
        bool letsVariablesBeCarried { false };
        bool onlyLeavesTestWithOneAnswer { false };
        bool standsInForLoopInside { false };
        BitVector emptyPathsThatStay { };
    };
    struct WayBack {
        Vector<Edge, 4> latchEdgesOfCopy { };
        Vector<Edge, 4> latchEdges { };
        Vector<Edge, 4> enteringEdges { };
        Vector<TypeID> testedEveryTurn { };
        Vector<TypeID> testedTypes { };
        Vector<TypeID> typesTakenFromCopy { };
        bool widensLoopTypes { false };
    };

    // What is asked about the values of one loop lies in tables of one entry a value of the function. They live as long as the pass and are empty between two loops:
    // a loop fills the entries of its own values and empties them again, so a function of a thousand loops is not walked a thousand times.
    struct TypesOfParameters {
        WTF_MAKE_NONCOPYABLE(TypesOfParameters);
        explicit TypesOfParameters(Vector<std::optional<Type>>& table)
            : m_table(table)
        {
        }
        TypesOfParameters(TypesOfParameters&& other)
            : parameters(WTF::move(other.parameters))
            , m_table(other.m_table)
        {
        }
        ~TypesOfParameters()
        {
            for (ValueID parameter : parameters)
                m_table[parameter.index()] = std::nullopt;
        }
        const std::optional<Type>& operator[](size_t index) const LIFETIME_BOUND { return m_table[index]; }
        Type& add(ValueID parameter, Type&& type) LIFETIME_BOUND
        {
            parameters.append(parameter);
            return m_table[parameter.index()].emplace(WTF::move(type));
        }
        Type& at(ValueID parameter) LIFETIME_BOUND { return *m_table[parameter.index()]; }

        Vector<ValueID, 16> parameters { };

    private:
        Vector<std::optional<Type>>& m_table;
    };
    struct MarkedValues {
        WTF_MAKE_NONCOPYABLE(MarkedValues);
        explicit MarkedValues(BitVector& table)
            : m_table(table)
        {
        }
        MarkedValues(MarkedValues&& other)
            : m_marked(WTF::move(other.m_marked))
            , m_table(other.m_table)
        {
        }
        ~MarkedValues()
        {
            for (uint32_t index : m_marked)
                m_table.quickClear(index);
        }
        bool get(size_t index) const { return m_table.get(index); }
        bool set(size_t index)
        {
            if (m_table.set(index))
                return true;
            m_marked.append(static_cast<uint32_t>(index));
            return false;
        }

    private:
        Vector<uint32_t, 16> m_marked { };
        BitVector& m_table;
    };

    Vector<std::optional<Type>> m_typesOfParameters { };
    BitVector m_markedValues { };
    uint64_t m_entriesRead { 0 };
    Vector<TypeID> m_rememberedTypes { };
    BitVector m_isFinishedHeader { };
    BitVector m_isSeparatedHeader { };
    Vector<VariableOfLoop> m_variablesWithStores { };
    bool m_likelyPathCannotLeave { false };
    bool m_pathThatOnlyComputesLeaves { false };

    enum class PathsThatOnlyCompute : uint8_t { LeaveWhereTheyWidenParameters, Stay };

    PassOutcome run()
    {
        auto findPlanOf = [&](const NaturalLoop& loop) {
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
            return plan;
        };
        // The test leaves the loop in the back end only where its failing path does not come back, and the copy of this loop comes back each turn. The copy of the loop around it comes back behind all the turns of this one.
        auto findPlanOfLoopAround = [&](const SeparationPlan& plan) -> std::optional<SeparationPlan> {
            const NaturalLoop* around = m_analyses.naturalLoops().innerMostOuterLoop(*m_analyses.naturalLoops().headerOf(plan.header));
            if (!plan.onlyLeavesTestWithOneAnswer || !around || m_isFinishedHeader.get(around->header().index()))
                return std::nullopt;
            m_likelyPathCannotLeave = false;
            constexpr bool paysForLoopInside = true;
            std::optional<SeparationPlan> planAround = planFor(*around, true, PathsThatOnlyCompute::LeaveWhereTheyWidenParameters, paysForLoopInside);
            if (!planAround && m_likelyPathCannotLeave)
                planAround = planFor(*around, false, PathsThatOnlyCompute::LeaveWhereTheyWidenParameters, paysForLoopInside);
            if (planAround)
                m_isFinishedHeader.set(around->header().index());
            return planAround;
        };
        // Inner loops come first. A loop around a separated loop stays as it is: its copy would hold both copies of the inner loop again.
        // A round plans every loop that it can and then copies them all. What a copy makes stale is made anew for the whole function: the types, the loops, who uses what. That happens a few times
        // a round and not a few times a loop, so a function of a thousand loops is not walked a thousand times. The loops of a round share no block: a loop inside one of them waits for the next round.
        auto planRound = [&] {
            Vector<SeparationPlan> plans;
            BitVector isCopiedInThisRound(m_function.blocks.size());
            for (BlockID header : headersFromInnermost()) {
                if (isCopiedInThisRound.get(header.index()) || m_isFinishedHeader.set(header.index()))
                    continue;
                std::optional<SeparationPlan> plan = findPlanOf(*m_analyses.naturalLoops().headerOf(header));
                if (!plan)
                    continue;
                if (std::optional<SeparationPlan> planAround = findPlanOfLoopAround(*plan))
                    plan = WTF::move(planAround);
                m_isSeparatedHeader.set(plan->header.index());
                for (BlockID block : plan->region)
                    isCopiedInThisRound.set(block.index());
                plans.append(WTF::move(*plan));
            }
            return plans;
        };
        unsigned numberOfSeparatedLoops = 0;
        for (Vector<SeparationPlan> plans = planRound(); !plans.isEmpty(); plans = planRound()) {
            separate(plans.span());
            numberOfSeparatedLoops += plans.size();
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
    TypesOfParameters typeParametersWithout(const BitVector& isInLoop, const BitVector& leaves)
    {
        Vector<BlockID, 16> blocksWithParameters;
        if (m_typesOfParameters.size() < m_function.values.size())
            m_typesOfParameters.grow(m_function.values.size());
        TypesOfParameters typeWithoutLeavingPaths(m_typesOfParameters);
        for (size_t index : isInLoop) {
            if (leaves.get(index) || m_function.blocks[index].parameters.isEmpty())
                continue;
            blocksWithParameters.append(BlockID(index));
            for (ValueID parameter : m_function.blocks[index].parameters)
                typeWithoutLeavingPaths.add(parameter, Type::never());
        }
        m_entriesRead += typeWithoutLeavingPaths.parameters.size();

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
                        Type& known = typeWithoutLeavingPaths.at(parameter);
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

    uint64_t numberOfAllocationsLeftAlone(const BitVector& isInLoop, const BitVector& leaves)
    {
        uint64_t count = 0;
        for (size_t index : isInLoop) {
            if (leaves.get(index))
                continue;
            for (size_t position = 0; position < m_function.blocks[index].parameters.size(); ++position)
                count += isAllocationButForLeavingPaths(BlockID(index), position, leaves);
        }
        return count;
    }

    ValueID findObjectPassedOnEveryEdge(ValueID parameter)
    {
        Vector<ValueID, 8> parameters { parameter };
        ValueID object;
        for (size_t next = 0; next < parameters.size(); ++next) {
            ValueLocation location = m_analyses.useLists().locationOf(parameters[next]);
            if (location.placement != ValuePlacement::Parameter || location.block == m_analyses.cfg().root())
                return { };
            for (Edge edge : m_analyses.cfg().incomingEdges(location.block)) {
                if (edge.isHandlerEdge())
                    return { };
                IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
                if (incoming.kind != IncomingValue::Kind::Argument || !incoming.argument)
                    return { };
                ValueID source = m_analyses.sameObjectThroughCasts(incoming.argument).valueInFrontOfFacts();
                if (m_function.at(source).opcode == Opcode::Parameter) {
                    if (!parameters.contains(source))
                        parameters.append(source);
                    continue;
                }
                if (object && object != source)
                    return { };
                object = source;
            }
        }
        return object;
    }

    // The likely paths make an object in every turn, and they and a leaving path each name it by a cast of their own. Where the two meet, the likely paths read it through a parameter, and what they have read of it ahead is read again.
    bool leavesObjectUnderOneName(const BitVector& isInLoop, const BitVector& leaves)
    {
        const UseLists& uses = m_analyses.useLists();
        for (size_t index : isInLoop) {
            if (leaves.get(index) || m_function.blocks[index].parameters.isEmpty())
                continue;
            bool isJoinedByLeavingPath = std::ranges::any_of(m_analyses.cfg().incomingEdges(BlockID(index)), [&](Edge edge) {
                return leaves.get(edge.from.index());
            });
            if (!isJoinedByLeavingPath)
                continue;
            for (ValueID parameter : m_function.blocks[index].parameters) {
                bool isRead = typeOf(parameter).isObject() && std::ranges::any_of(uses.usesOf(parameter), [](const Use& use) {
                    return use.kind != Use::Kind::EdgeArgument;
                });
                ValueID object = isRead ? findObjectPassedOnEveryEdge(parameter) : ValueID();
                if (!object || resultRuleOf(m_function.at(object).opcode) != ResultRule::Allocation)
                    continue;
                BlockID madeIn = uses.blockOf(object);
                if (madeIn && isInLoop.get(madeIn.index()) && !leaves.get(madeIn.index()))
                    return true;
            }
        }
        return false;
    }

    bool leavesReadsUnwritten(const BitVector& isInLoop, const BitVector& leaves)
    {
        LocationMasks readsOfLikelyPaths;
        LocationMasks writesOfLikelyPaths;
        LocationMasks writesOfLeavingPaths;
        for (size_t index : isInLoop) {
            auto add = [&](const Effects& effects) {
                (leaves.get(index) ? writesOfLeavingPaths : writesOfLikelyPaths).merge(effects.mayRunOtherCode() ? LocationMasks::top() : effects.writes);
                if (!leaves.get(index))
                    readsOfLikelyPaths.merge(effects.reads);
            };
            for (ValueID value : m_function.blocks[index].values)
                add(m_analyses.effectsOf(value).onAnyEdge());
            add(m_analyses.effectsOfTerminator(BlockID(index)).onAnyEdge());
        }
        return !writesOfLeavingPaths.intersectedWith(readsOfLikelyPaths).isSubsetOf(writesOfLikelyPaths);
    }

    bool leavesObjectToLikelyPaths(const BitVector& isInLoop, const BitVector& leaves)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        for (unsigned index = 0; index < escape.numberOfObjects(); ++index) {
            const TrackedObject& object = escape.at(ObjectID(index));
            if (object.kind != ObjectKind::Allocation)
                continue;
            BlockID madeIn = m_analyses.useLists().blockOf(object.definition);
            if (!madeIn || !isInLoop.get(madeIn.index()) || leaves.get(madeIn.index()))
                continue;
            bool isUsedOnLeavingPath = false;
            bool escapesOnLikelyPath = false;
            for (const ObjectUse& use : escape.usesOf(ObjectID(index))) {
                if (leaves.get(use.use.block.index()))
                    isUsedOnLeavingPath = true;
                else
                    escapesOnLikelyPath |= use.isEscape;
            }
            if (isUsedOnLeavingPath && !escapesOnLikelyPath)
                return true;
        }
        return false;
    }

    // The copy pays where an operation reads a parameter that is wider only for what a leaving path passes.
    bool narrowsParameter(const BitVector& isInLoop, const BitVector& leaves)
    {
        TypesOfParameters typeWithoutLeavingPaths = typeParametersWithout(isInLoop, leaves);
        const UseLists& uses = m_analyses.useLists();
        m_entriesRead += typeWithoutLeavingPaths.parameters.size();
        for (ValueID parameter : typeWithoutLeavingPaths.parameters) {
            if (typeOf(parameter).isSubsetOf(typeWithoutLeavingPaths.at(parameter), m_program))
                continue;
            bool isRead = std::ranges::any_of(uses.usesOf(parameter), [](const Use& use) {
                return use.kind != Use::Kind::EdgeArgument;
            });
            if (isRead)
                return true;
        }
        return false;
    }

    // Reads that the tables of the run time answer call nothing, so the turns that they answer run without a call once the others have left.
    bool leavesReadsFromTablesAlone(const BitVector& leaves)
    {
        for (size_t index : leaves) {
            for (Edge edge : m_analyses.cfg().incomingEdges(BlockID(index))) {
                const Terminator& terminator = m_function.at(edge.from).terminator;
                if (leaves.get(edge.from.index()) || edge.isHandlerEdge() || !terminator.isConditionalBranch())
                    continue;
                const Value& test = m_function.at(terminator.conditionOperand());
                if (test.opcode != Opcode::DidReadAbove)
                    continue;
                const Value& read = m_function.at(test.valueOperand());
                if (read.opcode == Opcode::PropGet && read.asksOnlyTables())
                    return true;
            }
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

        MarkedValues changesBetweenTurns(m_markedValues);
        Vector<BlockID, 16> stayingBlocks;
        for (size_t index : isInLoop) {
            if (!leaves.get(index) && m_analyses.cfg().isReachable(BlockID(index)))
                stayingBlocks.append(BlockID(index));
        }
        std::ranges::sort(stayingBlocks, { }, [&](BlockID block) {
            return m_analyses.cfg().reversePostOrderIndex(block);
        });
        m_entriesRead += stayingBlocks.size();
        for (BlockID block : stayingBlocks) {
            const Block& contents = m_function.at(block);
            // What a test with one answer has proven is what it tested: `queue` as an object with a layout, behind the test that tells such an object from an array.
            std::optional<Edge> onlyWayIn = m_analyses.cfg().singleIncomingEdge(block);
            bool followsTestWithOneAnswer = onlyWayIn && !onlyWayIn->isHandlerEdge() && stays(onlyWayIn->from) && m_function.at(onlyWayIn->from).terminator.kind == TerminatorKind::Branch
                && !changesBetweenTurns.get(m_function.at(onlyWayIn->from).terminator.conditionOperand().index());
            for (size_t position = 0; position < contents.parameters.size(); ++position) {
                if (!followsTestWithOneAnswer || position < targetOf(m_function, *onlyWayIn).arguments.size())
                    changesBetweenTurns.set(contents.parameters[position].index());
            }
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
            // So has a test of the type of a value that was made ahead of the loop. Whether a number is an integer is asked where a counter is made one.
            const Value& test = m_function.at(terminator.conditionOperand());
            bool asksForTypeOfValueFromAheadOfLoop = test.opcode == Opcode::TypeTest
                && !isInLoop.get(m_analyses.useLists().blockOf(m_analyses.identityOf(test.operand(0))).index())
                && !m_program.at(test.testedType()).isSubsetOf(m_program.at(TypeTable::number), m_program);
            if (!asksForTypeOfValueFromAheadOfLoop && m_analyses.effectsOf(terminator.conditionOperand()).onAnyEdge().reads.isEmpty())
                continue;
            if (std::ranges::any_of(terminator.targets, [&](const Target& target) { return leaves.get(target.block.index()); }))
                return true;
        }
        return false;
    }

    // What the likely paths make of a parameter that gets narrower may get narrower with it.
    MarkedValues findValuesMadeOfNarrowedParameters(const BitVector& isInLoop, const BitVector& leaves, const TypesOfParameters& typeWithoutLeavingPaths)
    {
        MarkedValues isMadeOfNarrowedParameter(m_markedValues);
        Vector<ValueID, 16> worklist;
        m_entriesRead += typeWithoutLeavingPaths.parameters.size();
        for (ValueID parameter : typeWithoutLeavingPaths.parameters) {
            if (!typeOf(parameter).isSubsetOf(*typeWithoutLeavingPaths[parameter.index()], m_program))
                worklist.append(parameter);
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

    bool holdsOnlyConstants(const Block& block)
    {
        return std::ranges::all_of(block.values, [&](ValueID value) {
            return isRematerializable(m_function.at(value));
        });
    }

    // The loop is typed by the rules of its operations as if no unlikely path passed anything on: what the likely paths make of a narrower parameter is as narrow as it will be.
    BitVector findBlocksThatWidenParameters(const BitVector& isInLoop, const BitVector& isUnlikely, const BitVector& askedBlocks)
    {
        struct PassedValue {
            Edge edge;
            uint32_t position;
        };
        BitVector widensParameter(m_function.blocks.size());
        Vector<PassedValue, 8> passedValues;
        Vector<ValueID, 16> typedValues;
        for (size_t index : askedBlocks) {
            const Terminator& terminator = m_function.blocks[index].terminator;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                const Target& target = terminator.targets[targetIndex];
                if (!isInLoop.get(target.block.index()) || isUnlikely.get(target.block.index()))
                    continue;
                const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
                for (uint32_t position = 0; position < parameters.size(); ++position) {
                    if (position < target.arguments.size()) {
                        if (isAllocationButForLeavingPaths(target.block, position, isUnlikely))
                            widensParameter.set(index);
                        typedValues.append(target.arguments[position]);
                    }
                    passedValues.append({ Edge { BlockID(index), targetIndex }, position });
                    typedValues.append(parameters[position]);
                }
            }
            // What a test proves on an edge is typed from the operands of the test.
            terminator.forEachOperand([&](ValueID test, uint32_t) {
                typedValues.append(test);
                m_function.at(test).forEachOperand([&](ValueID operand, uint32_t) {
                    typedValues.append(operand);
                });
            });
        }

        BitVector isValueOfLoop(m_function.values.size());
        for (size_t index : isInLoop) {
            for (ValueID parameter : m_function.blocks[index].parameters)
                isValueOfLoop.set(parameter.index());
            for (ValueID value : m_function.blocks[index].values)
                isValueOfLoop.set(value.index());
        }
        TypePropagationInputs inputs = typingInputs();
        inputs.blocksThatPassNothing = &isUnlikely;
        TypePropagation propagation(inputs);
        propagation.restartSourcesOf(typedValues.span(), isValueOfLoop);
        propagation.solveTypes();
        for (const PassedValue& passed : passedValues) {
            const Target& target = targetOf(m_function, passed.edge);
            const Type& typeOnLikelyPaths = typeOf(m_function.at(target.block).parameters[passed.position]);
            if (!typeOnLikelyPaths.isJavaScriptValue())
                continue;
            TypeID passedType = passed.position < target.arguments.size()
                ? m_function.at(target.arguments[passed.position]).type
                : typeOnEdge(m_program, m_function, { passed.edge.from, passed.edge.targetIndex, passed.position - static_cast<uint32_t>(target.arguments.size()) }, m_context.types);
            if (!m_context.types.at(passedType).isSubsetOf(typeOnLikelyPaths, m_program))
                widensParameter.set(passed.edge.from.index());
        }
        propagation.restoreTypes();
        return widensParameter;
    }

    // A path that only computes leaves too where a parameter of the likely paths is wider for what it passes, or is an object of theirs but for that.
    BitVector findLeavingBlocks(const NaturalLoop& loop, const BitVector& isInLoop, PathsThatOnlyCompute pathsThatOnlyCompute, BitVector& emptyPathsThatStay)
    {
        BitVector isUnlikely = findUnlikelyBlocks(loop).predicted();
        BitVector leaves = findLeavingBlocks(isInLoop, isUnlikely, [&](BlockID block) {
            return !onlyComputes(m_function.at(block));
        });
        if (pathsThatOnlyCompute == PathsThatOnlyCompute::Stay || leaves == isUnlikely)
            return leaves;
        TypesOfParameters typeOnLikelyPaths = typeParametersWithout(isInLoop, isUnlikely);
        MarkedValues mayGetNarrower = findValuesMadeOfNarrowedParameters(isInLoop, isUnlikely, typeOnLikelyPaths);
        auto mayWidenParameter = [&](BlockID block) {
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
        };
        // What has left comes back at the end of a turn of this loop: for a path of a loop inside, behind all the turns that the inner loop has left.
        // Such a path without an operation takes nothing out of the inner loop, so it leaves only where the types say that a parameter is wider for it.
        BitVector mayWiden(m_function.blocks.size());
        BitVector emptyPaths(m_function.blocks.size());
        for (size_t index : isUnlikely) {
            if (leaves.get(index) || !mayWidenParameter(BlockID(index)))
                continue;
            mayWiden.set(index);
            if (m_analyses.naturalLoops().innerMostLoopOf(BlockID(index)) != &loop && holdsOnlyConstants(m_function.blocks[index]))
                emptyPaths.set(index);
        }
        if (!emptyPaths.isEmpty()) {
            emptyPaths.exclude(findBlocksThatWidenParameters(isInLoop, isUnlikely, emptyPaths));
            mayWiden.exclude(emptyPaths);
        }
        BitVector leavesWithPathsThatOnlyCompute = findLeavingBlocks(isInLoop, isUnlikely, [&](BlockID block) {
            return leaves.get(block.index()) || mayWiden.get(block.index());
        });
        emptyPathsThatStay = WTF::move(emptyPaths);
        emptyPathsThatStay.exclude(leavesWithPathsThatOnlyCompute);
        m_pathThatOnlyComputesLeaves = leavesWithPathsThatOnlyCompute != leaves;
        return leavesWithPathsThatOnlyCompute;
    }

    // A counter that goes up from an integer is never below it. Tested for no more than 32 bits where it comes back from the copy, it would lose that in the loop as well.
    Vector<TypeID, 4> typesOfCountersFromStartOn(const NaturalLoop& loop)
    {
        const CompactList<ValueID>& parameters = m_function.at(loop.header()).parameters;
        auto mayBeCounter = [&](ValueID parameter) {
            return typeOf(parameter).isNumberWithoutFraction() && !typeOf(parameter).isInteger();
        };
        if (std::ranges::none_of(parameters, mayBeCounter))
            return { };
        Vector<TypeID, 4> result(parameters.size());
        for (const InductionVariable& counter : m_analyses.inductionVariables().of(loop).variables) {
            if (!mayBeCounter(counter.parameter) || counter.step.kind != InductionStepKind::Constant || !counter.start || !typeOf(counter.start).isInt32())
                continue;
            const Range& start = typeOf(counter.start).integers;
            result[counter.parameterIndex] = m_context.types.intern(Type::integer(counter.direction == InductionDirection::Increasing ? Range { start.minimum, INT32_MAX } : Range { INT32_MIN, start.maximum }));
        }
        return result;
    }

    std::optional<SeparationPlan> planFor(const NaturalLoop& loop, bool mayCarryVariables, PathsThatOnlyCompute pathsThatOnlyCompute, bool paysForLoopInside = false)
    {
        BlockID header = loop.header();
        if (!isExpectedToRun(m_context, loop).predicted())
            return std::nullopt;
        if (!isHeaderOfLoopOfProgram(header))
            return std::nullopt;

        const CompactList<ValueID>& parameters = m_function.at(header).parameters;
        SeparationPlan plan { header };
        for (ValueID parameter : parameters)
            plan.typesOfCarriedValues.append(m_function.at(parameter).type);
        plan.typesOfCounters = typesOfCountersFromStartOn(loop);
        BitVector isInLoop(m_function.blocks.size());
        uint64_t bytesOfLikelyPaths = 0;
        bool runsLoopInEveryTurn = false;
        for (unsigned index = 0; index < loop.size(); ++index) {
            BlockID block = loop.at(index);
            if (m_isSeparatedHeader.get(block.index()))
                return std::nullopt;
            isInLoop.set(block.index());
            plan.region.append(block);
            if (m_analyses.blockFrequencies().classOf(block).predicted() == BlockFrequency::Normal) {
                bytesOfLikelyPaths += estimatedCodeBytes(m_program, m_function, m_function.at(block));
                runsLoopInEveryTurn |= m_analyses.naturalLoops().innerMostLoopOf(block) != &loop;
            }
        }
        plan.standsInForLoopInside = paysForLoopInside;
        // The likely paths end up in the program twice, and the longer an iteration is, the less of it waits for one number.
        AOT_THRESHOLD(uint64_t, bytesOfShortTurn, 512);
        bool isShort = bytesOfLikelyPaths <= bytesOfShortTurn;

        BitVector leaves = findLeavingBlocks(loop, isInLoop, pathsThatOnlyCompute, plan.emptyPathsThatStay);
        // A loop that carries variables is entered through its copy, which is likely code then: it runs the whole first turn, with every turn of a loop inside. A turn that runs a loop
        // of its own does not wait for one read of a variable, so there the variables have to be stored as well.
        plan.letsVariablesBeCarried = mayCarryVariables && VariableCarrier { { m_context }, false }.letWritersOfVariablesLeave(loop, leaves, runsLoopInEveryTurn ? VariableCarrier::WhatPays::Stores : VariableCarrier::WhatPays::Reads);
        if (leaves.isEmpty() || leaves.get(header.index()))
            return std::nullopt;
        plan.carriedValuesThatMayBeUnspecified = BitVector(parameters.size());
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (typeOf(parameters[index]).mayBeUnspecified())
                plan.carriedValuesThatMayBeUnspecified.set(index);
        }
        Vector<TypeID> testsOfEveryTurn = findTestsOfEveryTurn(loop, leaves, plan.carriedValuesThatMayBeUnspecified);
        bool movesTest = std::ranges::any_of(testsOfEveryTurn, [](TypeID tested) {
            return !!tested;
        });
        bool paysToMoveTest = movesTest && (paysForLoopInside || !runsLoopInEveryTurn || !onlyChoosesBetweenFormsOfLoopInside(loop, isInLoop, leaves, testsOfEveryTurn.span()));
        bool paysByItself = paysToMoveTest || plan.letsVariablesBeCarried || narrowsParameter(isInLoop, leaves) || bytesOfLikelyPaths <= bytesOfShortTurn * numberOfAllocationsLeftAlone(isInLoop, leaves) || leavesReadsFromTablesAlone(leaves);
        bool onlyLeavesObjectUnderOneName = !paysByItself && leavesObjectUnderOneName(isInLoop, leaves);
        paysByItself |= onlyLeavesObjectUnderOneName;
        if (!paysByItself && !(isShort && (paysForLoopInside || leavesTestWithOneAnswer(isInLoop, leaves))))
            return std::nullopt;
        plan.onlyLeavesTestWithOneAnswer = !paysByItself && !movesTest;
        // The back end takes a loop with rare paths that come back for one of few turns.
        if (onlyLeavesObjectUnderOneName)
            plan.emptyPathsThatStay.clearAll();

        // Many tests may fail into one block that holds nothing and goes on into a path that leaves. Entered from the loop and from its copy, the copy of that block would join every value that is
        // alive there, and each of its edges would carry them all: tests times values. The block stays, and the one edge behind it leaves in place of theirs.
        AOT_THRESHOLD(size_t, fewestEdgesGathered, 16);
        BitVector staysToGatherEdges(m_function.blocks.size());
        for (size_t index : leaves) {
            const Block& block = m_function.blocks[index];
            std::span<const Edge> incoming = m_analyses.cfg().incomingEdges(BlockID(index));
            if (incoming.size() < fewestEdgesGathered || !block.values.isEmpty() || !block.parameters.isEmpty() || block.isHandlerBlock() || block.handler || block.terminator.kind != TerminatorKind::Jump)
                continue;
            const Target& next = block.terminator.next();
            if (!next.proofs.isEmpty() || !leaves.get(next.block.index()))
                continue;
            bool isEnteredOnlyFromPathsThatStay = std::ranges::all_of(incoming, [&](Edge edge) {
                return !edge.isHandlerEdge() && !leaves.get(edge.from.index()) && targetOf(m_function, edge).proofs.isEmpty();
            });
            if (isEnteredOnlyFromPathsThatStay)
                staysToGatherEdges.set(index);
        }

        BitVector isEntryOfCopy(m_function.blocks.size());
        for (size_t index : leaves) {
            BlockID block(index);
            if (staysToGatherEdges.get(index))
                continue;
            for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
                if (edge.isHandlerEdge())
                    return std::nullopt;
                if (leaves.get(edge.from.index()) && !staysToGatherEdges.get(edge.from.index()))
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

        if (joinsMoreThanItCopies(plan.region.span(), plan.entriesOfCopy.span()))
            return std::nullopt;
        if (RegionCloner::checkCanDuplicate(m_analyses, plan.region.span(), plan.entriesOfCopy.span()) != CloneCheck::CanClone)
            return std::nullopt;
        return plan;
    }

    // The loops share no block.
    void separate(std::span<const SeparationPlan> plans)
    {
        m_rememberedTypes.clear();
        IRBuilder builder = makeBuilder();
        Vector<std::unique_ptr<RegionCloner>> cloners;
        for (const SeparationPlan& plan : plans) {
            cloners.append(makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, m_function));
            cloners.last()->cloneBlocks(plan.region.span(), [](ValueID, Value&) { });
        }
        rememberTypes();
        for (size_t index = 0; index < plans.size(); ++index) {
            const SeparationPlan& plan = plans[index];
            const RegionCloner& cloner = *cloners[index];
            // A copy that the loop is entered through runs as often as the loop did.
            for (BlockID block : plan.region) {
                if (!plan.letsVariablesBeCarried)
                    m_function.edit(cloner.cloneOf(block), BlockEdit::Rest).frequency = m_function.at(block).frequency.predicted() == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
            }
            for (Edge edge : plan.leavingEdges) {
                const Target& target = targetOf(m_function, edge);
                builder.redirectEdge(edge, cloner.cloneOf(target.block), Arguments(target.arguments));
            }
            // They are as far out of the way of the likely paths as the paths that have left.
            for (size_t block : plan.emptyPathsThatStay)
                m_function.edit(BlockID(block), BlockEdit::Rest).frequency = BlockFrequency::Rare;
        }
        // A path that has left is unreachable in the loop.
        SSARepair repair(builder, m_analyses);
        for (const std::unique_ptr<RegionCloner>& cloner : cloners)
            cloner->addVariablesTo(repair, m_analyses);
        builder.emptyUnreachableBlocks();
        m_analyses.didChangeControlFlow();
        retypeValues();

        auto blocksOf = [&](const RegionCloner& cloner) {
            BitVector isInCopy(m_function.blocks.size());
            for (BlockID block : cloner.clonedBlocks())
                isInCopy.set(block.index());
            return isInCopy;
        };
        // Every way back is planned before the first is built: building one asks nothing about the function as a whole, and changes no edge that another is planned with.
        Vector<std::optional<WayBack>> waysBack;
        for (size_t index = 0; index < plans.size(); ++index) {
            BitVector isInCopy = blocksOf(*cloners[index]);
            m_isFinishedHeader.merge(isInCopy);
            waysBack.append(planWayBack(plans[index], cloners[index]->cloneOf(plans[index].header), isInCopy));
        }
        bool widensLoopThatCarriesVariables = false;
        for (size_t index = 0; index < plans.size(); ++index) {
            if (!waysBack[index]) {
                m_context.build.statistics().count(StatisticID::LoopsWithoutWayBack);
                continue;
            }
            addWayBack(builder, plans[index], cloners[index]->cloneOf(plans[index].header), *waysBack[index]);
            widensLoopThatCarriesVariables |= plans[index].letsVariablesBeCarried && waysBack[index]->widensLoopTypes;
        }
        m_analyses.didChangeControlFlow();
        if (widensLoopThatCarriesVariables)
            retypeValues();
        for (size_t index = 0; index < plans.size(); ++index) {
            if (waysBack[index] && plans[index].letsVariablesBeCarried)
                carryVariables(builder, plans[index].header, *cloners[index], blocksOf(*cloners[index]));
        }

        // A parameter that the repair adds holds what the definitions it joins hold.
        restoreRememberedTypes();
        repair.run();
        m_analyses.didChangeControlFlow();
        retypeValues();
    }

    // `for (let n = next(); n > 0; n = next()) for (let i = 0; i < n; i++) ..`: the loop inside stands there twice, for an int32 and, rare, for any other number, behind one test of `n`.
    // A copy of the loop around them would hold both again, and the test would stand on every edge into that loop in place of once in front of the two. A turn that runs a loop does not wait for one test.
    bool onlyChoosesBetweenFormsOfLoopInside(const NaturalLoop& loop, const BitVector& isInLoop, const BitVector& leavesLoop, std::span<const TypeID> testsOfEveryTurn)
    {
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const Dominators& dominators = m_analyses.dominators();
        const Block& header = m_function.at(loop.header());
        Vector<BlockID, 2> whereTestsFail;
        for (size_t index = 0; index < header.parameters.size(); ++index) {
            if (!testsOfEveryTurn[index])
                continue;
            bool runsLoopWhenFailed = false;
            for (const Use& use : m_analyses.useLists().usesOf(header.parameters[index])) {
                if (use.kind != Use::Kind::Operand || m_function.at(use.user).opcode != Opcode::TypeTest || !loops.belongsTo(use.block, loop))
                    continue;
                const Terminator& terminator = m_function.at(use.block).terminator;
                BlockID whenFailed = terminator.kind == TerminatorKind::Branch && terminator.conditionOperand() == use.user ? terminator.notTaken().block : BlockID();
                if (!whenFailed || !leavesLoop.get(whenFailed.index()))
                    continue;
                // A rare form that comes back in front of the likely one hands it a counter from the middle of a run, which is any number.
                if (loops.innerMostLoopOf(use.block) != &loop)
                    return false;
                whereTestsFail.append(whenFailed);
                for (size_t leaving : leavesLoop)
                    runsLoopWhenFailed |= loops.headerOf(BlockID(leaving)) && dominators.dominates(whenFailed, BlockID(leaving));
            }
            if (!runsLoopWhenFailed)
                return false;
        }
        bool likelyFormHasRarePaths = false;
        for (size_t leaving : leavesLoop) {
            if (m_analyses.blockFrequencies().classOf(BlockID(leaving)).predicted() != BlockFrequency::Rare)
                return false;
            likelyFormHasRarePaths |= std::ranges::none_of(whereTestsFail, [&](BlockID whenFailed) {
                return dominators.dominates(whenFailed, BlockID(leaving));
            });
        }
        if (!likelyFormHasRarePaths)
            return true;
        auto gains = [&](const BitVector& isInGainingLoop, bool runsLoopInEveryTurn) {
            BitVector leavesGainingLoop = leavesLoop;
            leavesGainingLoop.filter(isInGainingLoop);
            if (leavesGainingLoop.isEmpty())
                return false;
            return !runsLoopInEveryTurn || numberOfAllocationsLeftAlone(isInGainingLoop, leavesGainingLoop) || leavesObjectToLikelyPaths(isInGainingLoop, leavesGainingLoop) || leavesReadsUnwritten(isInGainingLoop, leavesGainingLoop);
        };
        if (gains(isInLoop, true))
            return false;
        for (unsigned index = 0; index < loop.size(); ++index) {
            const NaturalLoop* inside = loops.headerOf(loop.at(index));
            if (!inside || inside == &loop || leavesLoop.get(loop.at(index).index()))
                continue;
            bool runsLoopInEveryTurn = false;
            BitVector isInside(m_function.blocks.size());
            for (unsigned position = 0; position < inside->size(); ++position) {
                BlockID block = inside->at(position);
                isInside.set(block.index());
                runsLoopInEveryTurn |= !leavesLoop.get(block.index()) && loops.innerMostLoopOf(block) != inside;
            }
            if (gains(isInside, runsLoopInEveryTurn))
                return false;
        }
        return true;
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
    void emitTests(IRBuilder& builder, const Arguments& carried, std::span<const TypeID> tested, BlockID whenPassed, BlockID whenFailed, Expected<BlockFrequency> whenPassedFrequency, bool nothingElseEntersWhenFailed = false)
    {
        auto argumentsWhenFailed = [&] {
            Arguments arguments;
            arguments.append(carried.span().first(m_function.at(whenFailed).parameters.size()));
            return arguments;
        };
        auto isAsked = [&](size_t index) {
            return tested[index] && !hasType(carried[index], tested[index]);
        };
        size_t numberOfTests = 0;
        for (size_t index = 0; index < tested.size(); ++index)
            numberOfTests += isAsked(index);
        BlockID failed = nothingElseEntersWhenFailed && numberOfTests > 1 ? builder.addBlock(m_function.at(whenFailed).frequency) : BlockID();
        auto targetWhenFailed = [&] {
            return failed ? Target { failed, { }, { } } : Target { whenFailed, argumentsWhenFailed(), { } };
        };
        Arguments narrowed = carried;
        if (numberOfTests)
            builder.emitFailureOfPredictions(targetWhenFailed());
        for (size_t index = 0; index < tested.size(); ++index) {
            if (!isAsked(index))
                continue;
            ValueID test = builder.appendTypeTest(carried[index], tested[index]);
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, whenPassedFrequency, m_function.at(whenFailed).frequency);
            narrowed[index] = branch.provenOnTaken;
            builder.moveToEndOf(branch.notTaken);
            Target target = targetWhenFailed();
            builder.emitJump(target.block, WTF::move(target.arguments));
            builder.moveToEndOf(branch.taken);
        }
        builder.emitJump(whenPassed, WTF::move(narrowed));
        if (failed) {
            builder.moveToEndOf(failed);
            builder.emitJump(whenFailed, argumentsWhenFailed());
        }
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
    std::optional<WayBack> planWayBack(const SeparationPlan& plan, BlockID headerOfCopy, const BitVector& isInCopy)
    {
        TypeArena& types = m_context.types;
        BlockID header = plan.header;
        size_t numberOfParameters = m_function.at(header).parameters.size();
        const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
        if (!loop || m_function.at(headerOfCopy).parameters.size() != numberOfParameters)
            return std::nullopt;

        WayBack wayBack;
        auto& [latchEdgesOfCopy, latchEdges, enteringEdges, testedEveryTurn, testedTypes, typesTakenFromCopy, widensLoopTypes] = wayBack;
        const CFG& cfg = m_analyses.cfg();
        for (Edge edge : cfg.incomingEdges(headerOfCopy)) {
            if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != numberOfParameters)
                return std::nullopt;
            if (isInCopy.get(edge.from.index()))
                latchEdgesOfCopy.append(edge);
        }
        if (latchEdgesOfCopy.isEmpty())
            return std::nullopt;
        for (Edge edge : cfg.incomingEdges(header)) {
            if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != numberOfParameters)
                return std::nullopt;
            (m_analyses.naturalLoops().belongsTo(edge.from, *loop) ? latchEdges : enteringEdges).append(edge);
        }

        // A type that a test cannot establish (proven contents, a range of integers) keeps the copy from coming back.
        // A test that moves to where the value is made stands on every edge into the loop in place of once at its head. A loop that is separated for the loop inside it wants nothing for itself.
        testedEveryTurn = plan.standsInForLoopInside ? Vector<TypeID>(numberOfParameters) : findTestsOfEveryTurn(*loop, isInCopy, plan.carriedValuesThatMayBeUnspecified);
        testedTypes = testedEveryTurn;
        for (size_t index = 0; index < plan.typesOfCounters.size(); ++index) {
            TypeID fromStartOn = plan.typesOfCounters[index];
            if (fromStartOn && testedTypes[index] && types.at(fromStartOn).isSubsetOf(types.at(testedTypes[index]), m_program))
                testedTypes[index] = fromStartOn;
        }
        typesTakenFromCopy = Vector<TypeID>(numberOfParameters);
        for (size_t index = 0; index < numberOfParameters; ++index) {
            const Type& given = types.at(plan.typesOfCarriedValues[index]);
            const Type& typeInLoop = typeOf(m_function.at(header).parameters[index]);
            if (testedTypes[index] || given.isSubsetOf(typeInLoop, m_program) || plan.carriedValuesThatMayBeUnspecified.get(index))
                continue;
            if (!given.isJavaScriptValue() || !typeInLoop.isJavaScriptValue())
                return std::nullopt;
            // A test for literal strings reads the characters of a string that is none of the program's own. What the copy carries may be any string, so the loop takes any.
            Type wanted = typeInLoop.withLiteralStringsWidened();
            Type testable = testableStoredTypeOf(PropertyAccessContext { m_program, &m_context.inputs.flow }, given, wanted, types);
            if (testable.isNever() || !given.castTo(testable, types).isSubsetOf(wanted, m_program))
                return std::nullopt;
            testedTypes[index] = types.intern(testable);
            if (typeInLoop.hasLiteralStrings())
                typesTakenFromCopy[index] = types.intern(wanted);
        }
        // The loop has the types of what entered it before there was a way back.
        for (size_t index = 0; index < numberOfParameters; ++index) {
            const Type& given = types.at(plan.typesOfCarriedValues[index]);
            const Type& typeInLoop = typeOf(m_function.at(header).parameters[index]);
            widensLoopTypes |= !(testedTypes[index] ? given.castTo(types.at(testedTypes[index]), types) : given).isSubsetOf(typeInLoop, m_program);
        }
        return wayBack;
    }

    // Whoever has built the last one says that the control flow has changed.
    void addWayBack(IRBuilder& builder, const SeparationPlan& plan, BlockID headerOfCopy, const WayBack& wayBack)
    {
        TypeArena& types = m_context.types;
        BlockID header = plan.header;
        size_t numberOfParameters = m_function.at(header).parameters.size();
        const auto& [latchEdgesOfCopy, latchEdges, enteringEdges, testedEveryTurn, testedTypes, typesTakenFromCopy, widensLoopTypes] = wayBack;
        UNUSED_VARIABLE(widensLoopTypes);

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
        bool onlyWayBackEntersHeaderOfCopy = !plan.letsVariablesBeCarried && std::ranges::all_of(latchEdges, [&](Edge edge) {
            return passesTests(targetOf(m_function, edge).arguments, testedEveryTurn.span());
        });
        testOnEdges(builder, latchEdges.span(), testedEveryTurn.span(), header, headerOfCopy);
        for (ValueID id : m_function.at(headerOfCopy).values) {
            const Value& test = m_function.at(id);
            if (test.opcode != Opcode::TypeTest)
                continue;
            size_t index = m_function.at(headerOfCopy).parameters.find(test.operand(0));
            if (index != notFound && testedEveryTurn[index] == test.testedType() && testedTypes[index] != test.testedType())
                m_function.edit(id, ValueEdit::Definition).setTestedType(testedTypes[index]);
        }

        builder.moveToEndOf(entry);
        builder.emitJump(header, WTF::move(parametersOfEntry));
        builder.moveToEndOf(tests);
        emitTests(builder, carried, testedTypes.span(), entry, headerOfCopy, m_function.at(tests).frequency, onlyWayBackEntersHeaderOfCopy);

        for (BlockID block : { entry, tests })
            m_isFinishedHeader.set(block.index());
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

// What these answer holds until something writes what they read, and they may be asked at any time.
constexpr OpcodeSet questionsAskedAheadOfLoops { Opcode::TypeTest, Opcode::IsSlotFilled, Opcode::IsNotFrozen, Opcode::AreMembersUnchanged, Opcode::IsClosureOf, Opcode::GetSlot, Opcode::Expect, Opcode::Upcast };
// A lookup also reads the fields that compiled objects have under its own name. A store to a field under another name leaves what it finds.
constexpr AbstractHeapSet heapsThatEveryLookupByNameReads { AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks };

// `for (..) emitter.emit("tick", i)` asks in every turn what no turn changes: where the table of listeners is, what lies under "tick", whether that is the one function. Then it runs the function.
// The questions are asked once, ahead of the loop. Behind the answers with which a turn runs no other code stands the loop without the questions.
// Behind any other answer stands a copy of the loop as it was, which is as likely as the loop was: an emitter with two listeners pays for the questions once and for nothing else.
// A question costs about what the read costs that it stands for, so a loop that is left in its first turn would only lose. The first turn runs as it was, in a copy of its own, and the questions are asked behind it.
struct LoopVersioner : LoopEditor {
    struct Step {
        enum class Kind : uint8_t { Computation, ReadByName, Parameter, Test };
        Kind kind;
        BlockID block;
        ValueID value;
        ValueID passed { };
        uint32_t keptTarget { 0 };
    };
    struct Plan {
        BlockID header;
        Edge entry;
        Vector<BlockID> region { };
        Vector<Step, 16> steps { };
        Vector<Edge, 4> leavingEdges { };
        Vector<Edge, 2> latches { };
    };
    struct Outlook {
        bool staysQuiet { false };
        unsigned questions { 0 };
    };

    BitVector m_isFinishedHeader { };

    PassOutcome run()
    {
        if (!m_analyses.opcodesOfValues().contains(Opcode::PropGet))
            return PassOutcome::Unchanged;
        // Inner loops come first. A round plans every loop that it can and then makes all their versions, so the loops, the edges and who uses what are found anew once a round and not once a loop.
        // The loops of a round share no block: a loop around one of them waits for the next round, where it holds all three copies.
        // `for (..) ..; for (..) ..` where the first loop leaves straight into the head of the second: the copies of the first enter the second too, so it has one entry no longer. It waits as well.
        auto planRound = [&] {
            Vector<Plan> plans;
            BitVector isCopiedInThisRound(m_function.blocks.size());
            BitVector entersLoopOfThisRound(m_function.blocks.size());
            for (BlockID header : headersFromInnermost()) {
                if (m_isFinishedHeader.get(header.index()))
                    continue;
                const NaturalLoop& loop = *m_analyses.naturalLoops().headerOf(header);
                bool waits = false;
                for (unsigned index = 0; index < loop.size() && !waits; ++index)
                    waits = isCopiedInThisRound.get(loop.at(index).index()) || entersLoopOfThisRound.get(loop.at(index).index());
                if (waits)
                    continue;
                std::optional<Plan> plan = planFor(loop);
                if (plan && isCopiedInThisRound.get(plan->entry.from.index()))
                    continue;
                m_isFinishedHeader.set(header.index());
                if (!plan)
                    continue;
                entersLoopOfThisRound.set(plan->entry.from.index());
                for (BlockID block : plan->region)
                    isCopiedInThisRound.set(block.index());
                plans.append(WTF::move(*plan));
            }
            return plans;
        };
        unsigned numberOfLoops = 0;
        // A loop gets two copies, and the loop around it two of all three. All the copies together may not outgrow the function, or what a few small loops come to.
        AOT_THRESHOLD(size_t, valuesThatCopiesOfLoopsMayAlwaysAdd, 4096);
        const size_t mostValuesWithCopies = m_function.values.size() + std::max(m_function.values.size(), valuesThatCopiesOfLoopsMayAlwaysAdd);
        for (Vector<Plan> plans = planRound(); !plans.isEmpty(); plans = planRound()) {
            unsigned numberOfVersions = makeVersions(plans.span(), mostValuesWithCopies);
            numberOfLoops += numberOfVersions;
            if (numberOfVersions < plans.size())
                break;
        }
        if (!numberOfLoops)
            return PassOutcome::Unchanged;
        removeTrivialParameters(m_context);
        VariableCarrier { { m_context } }.retype();
        return PassOutcome::Changed;
    }

    // It may run a getter or a trap, so it is one operation that nothing moves past. Mostly it finds a plain value, and what its site remembers says so without running anything.
    bool isReadOffObjectWithoutLayout(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (value.opcode != Opcode::PropGet || value.isFreeName() || !value.key() || m_program.nameClassOf(value.key()) == NameClass::Length)
            return false;
        if (!m_analyses.effectsOf(id).onAnyEdge().mayRunOtherCode())
            return false;
        const Type& object = typeOf(value.objectOperand());
        if (!object.isJavaScriptValue() || !object.hasObjects())
            return false;
        if (!object.layouts)
            return !value.receivers();
        return object.hasOnlyLayouts() && std::ranges::all_of(m_program.at(object.layouts).completeMembers(), [&](LayoutID layout) {
            return !!builtinBaseClassOf(m_program, layout);
        });
    }

    static bool staysQuiet(const Effects& effects)
    {
        return !effects.mayRunOtherCode() && !effects.writes.containsAny(heapsThatEveryLookupByNameReads);
    }

    std::optional<Plan> planFor(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const CFG& cfg = m_analyses.cfg();
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        BlockID header = loop.header();
        if (!isHeaderOfLoopOfProgram(header) || frequencies.classOf(header).predicted() != BlockFrequency::Normal)
            return std::nullopt;
        if (!isExpectedToRun(m_context, loop).predicted() || !isHot(heatClassOfLoopAt(m_context, header)).predicted())
            return std::nullopt;
        std::optional<Edge> entry = loops.findSingleEntry(loop);
        if (!entry || hasEdgeResults(m_function, *entry))
            return std::nullopt;

        auto isInLoopBehindHeader = [&](BlockID block) {
            return block != header && loops.belongsTo(block, loop);
        };
        auto isUnlikely = [&](BlockID block) {
            return (frequencies.classOf(block) > frequencies.classOf(header)).predicted();
        };
        // A read that seldom runs is not worth a question in front of every entry: it is other code, and its path goes on in the copy.
        auto mayBecomeQuestion = [&](BlockID block, ValueID value) {
            return !isUnlikely(block) && loops.innerMostLoopOf(block) == &loop && isReadOffObjectWithoutLayout(value);
        };

        Plan plan { header, *entry };
        for (Edge edge : cfg.incomingEdges(header)) {
            if (!loops.belongsTo(edge.from, loop))
                continue;
            if (edge.isHandlerEdge() || hasEdgeResults(m_function, edge))
                return std::nullopt;
            plan.latches.append(edge);
        }
        bool readsByName = false;
        for (unsigned position = 0; position < loop.size(); ++position) {
            BlockID id = loop.at(position);
            const Block& block = m_function.at(id);
            if (block.handler || block.isHandlerBlock())
                return std::nullopt;
            plan.region.append(id);
            readsByName |= std::ranges::any_of(block.values, [&](ValueID value) {
                return mayBecomeQuestion(id, value);
            });
        }
        if (!readsByName)
            return std::nullopt;

        // From the end of a turn to its start: whether a turn that gets here can end without running other code, if every question that may be asked ahead is answered the best way, and how many of them that saves.
        Vector<Outlook> outlooks(m_function.blocks.size());
        auto outlookBehind = [&](BlockID target) -> Outlook {
            if (!isInLoopBehindHeader(target))
                return { true, 0 };
            // An unlikely path that runs other code goes on in the copy.
            if (!outlooks[target.index()].staysQuiet && isUnlikely(target))
                return { true, 0 };
            return outlooks[target.index()];
        };
        for (BlockID id : std::views::reverse(cfg.reversePostOrder())) {
            if (!loops.belongsTo(id, loop))
                continue;
            // A loop inside may stand on a way that is not kept: the walk over several listeners, behind the test for one. On a way that is kept it would be copied for nothing.
            if (loops.innerMostLoopOf(id) != &loop)
                continue;
            const Block& block = m_function.at(id);
            Outlook outlook { staysQuiet(m_analyses.effectsOfTerminator(id).onAnyEdge()), 0 };
            for (ValueID value : block.values) {
                if (mayBecomeQuestion(id, value))
                    ++outlook.questions;
                else
                    outlook.staysQuiet &= staysQuiet(m_analyses.effectsOf(value).onAnyEdge());
            }
            bool mayBeAskedAhead = block.terminator.kind == TerminatorKind::Branch && questionsAskedAheadOfLoops.contains(m_function.at(block.terminator.conditionOperand()).opcode);
            Outlook behind { !mayBeAskedAhead, 0 };
            for (const Target& target : block.terminator.targets) {
                if (!mayBeAskedAhead) {
                    Outlook next = outlookBehind(target.block);
                    behind.staysQuiet &= next.staysQuiet;
                    behind.questions = std::max(behind.questions, next.questions);
                    continue;
                }
                if (!isInLoopBehindHeader(target.block) || !outlooks[target.block.index()].staysQuiet)
                    continue;
                Outlook next = outlooks[target.block.index()];
                if (!behind.staysQuiet || next.questions + 1 > behind.questions)
                    behind = { true, next.questions + 1 };
            }
            // Neither way stays in the loop and quiet: it is a test like any other.
            if (mayBeAskedAhead && !behind.staysQuiet) {
                behind = { true, 0 };
                for (const Target& target : block.terminator.targets)
                    behind.staysQuiet &= outlookBehind(target.block).staysQuiet;
            }
            outlook.staysQuiet &= behind.staysQuiet;
            outlook.questions += behind.questions;
            outlooks[id.index()] = outlook;
        }
        if (!outlooks[header.index()].staysQuiet)
            return std::nullopt;

        // What was entered in every turn was entered in the first, which has run by the time the questions are asked. A cast that names a fact holds wherever the fact does.
        auto holdsBehindFirstTurn = [&](BlockID block, const Value& operation) {
            // The first turn has also passed the test of what is read from, or has thrown.
            if (operation.opcode == Opcode::Entered || operation.opcode == Opcode::RequireObjectCoercible) {
                return std::ranges::all_of(plan.latches, [&](Edge latch) {
                    return m_analyses.dominators().dominates(block, latch.from);
                });
            }
            // Flow's word on what a read answers goes with the answer: the questions are asked behind the same tests as the read.
            return operation.opcode == Opcode::AssumeType;
        };
        // What a turn writes is known at the end of the walk. An answer that it changes stays a question of every turn, and the walk starts over without it.
        BitVector changesInLoop(m_function.values.size());
        while (true) {
            std::optional<bool> isSettled = walkAlongKeptWays(loop, plan, outlooks.span(), changesInLoop, mayBecomeQuestion, holdsBehindFirstTurn);
            if (!isSettled)
                return std::nullopt;
            if (*isSettled)
                break;
        }

        Vector<BlockID, 4> entriesOfCopy { header };
        BitVector isEntryOfCopy(m_function.blocks.size());
        isEntryOfCopy.set(header.index());
        for (Edge edge : plan.leavingEdges) {
            if (!isEntryOfCopy.set(targetOf(m_function, edge).block.index()))
                entriesOfCopy.append(targetOf(m_function, edge).block);
        }
        if (joinsMoreThanItCopies(plan.region.span(), entriesOfCopy.span()))
            return std::nullopt;
        if (RegionCloner::checkCanDuplicate(m_analyses, plan.region.span(), entriesOfCopy.span()) != CloneCheck::CanClone)
            return std::nullopt;
        return plan;
    }

    // The name under which a value was read off an object without a layout, seen through the casts, the tests and the joins that the plan has settled.
    PropertyKeyID keyThatWasAskedFor(const Plan& plan, ValueID value)
    {
        while (true) {
            value = m_analyses.identityOf(value);
            const Step* step = std::ranges::find(plan.steps, value, &Step::value);
            if (step == plan.steps.end())
                return { };
            if (step->kind == Step::Kind::ReadByName)
                return m_function.at(value).key();
            if (step->kind != Step::Kind::Parameter)
                return { };
            value = step->passed;
        }
    }

    // `typeof handler === "function"` where a turn stays quiet behind either answer. Flow has seen what the program stores under the name.
    std::optional<uint32_t> findTargetThatFlowPredicts(const Plan& plan, const Terminator& branch)
    {
        const Value& test = m_function.at(branch.conditionOperand());
        if (test.opcode != Opcode::TypeTest || !m_context.inputs.flow.hasResults())
            return std::nullopt;
        PropertyKeyID key = keyThatWasAskedFor(plan, test.operand(OperandIndex::TypeTest::value));
        if (!key)
            return std::nullopt;
        const Type& tested = m_context.types.at(test.testedType());
        std::optional<uint32_t> predicted;
        for (const FlowClosureStoredUnderKey& stored : m_context.inputs.flow.closuresSeenStoredUnder(key).predicted()) {
            Type closure = Type::forFunction(stored.function);
            bool passes = closure.isSubsetOf(tested, m_program);
            if (!passes && closure.overlaps(tested, m_program))
                return std::nullopt;
            uint32_t target = passes ? 0 : 1;
            if (predicted && *predicted != target)
                return std::nullopt;
            predicted = target;
        }
        return predicted;
    }

    // From the start of a turn to its end, along the ways that are kept. Nothing where the loop cannot be told apart, false where an answer has turned out to change in the loop.
    std::optional<bool> walkAlongKeptWays(const NaturalLoop& loop, Plan& plan, std::span<const Outlook> outlooks, BitVector& changesInLoop, const auto& mayBecomeQuestion, const auto& holdsBehindFirstTurn)
    {
        const NaturalLoops& loops = m_analyses.naturalLoops();
        const CFG& cfg = m_analyses.cfg();
        const UseLists& useLists = m_analyses.useLists();
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        BlockID header = loop.header();
        auto isInLoopBehindHeader = [&](BlockID block) {
            return block != header && loops.belongsTo(block, loop);
        };
        auto isUnlikely = [&](BlockID block) {
            return (frequencies.classOf(block) > frequencies.classOf(header)).predicted();
        };
        plan.steps.clear();
        plan.leavingEdges.clear();
        BitVector isReached(m_function.blocks.size());
        BitVector staysTheSame(m_function.values.size());
        Vector<std::optional<uint32_t>> keptTargetOf(m_function.blocks.size());
        LocationMasks writes { };
        isReached.set(header.index());
        auto isSameInEveryTurn = [&](ValueID value) {
            return staysTheSame.get(value.index()) || !loops.belongsTo(useLists.blockOf(value), loop);
        };
        auto chooseTarget = [&](const Terminator& branch) -> std::optional<uint32_t> {
            std::optional<uint32_t> best;
            auto rankOf = [&](uint32_t index) {
                BlockID target = branch.targets[index].block;
                return std::pair { frequencies.classOf(target).predicted(), std::numeric_limits<unsigned>::max() - outlooks[target.index()].questions };
            };
            unsigned numberOfQuietWays = 0;
            for (uint32_t index = 0; index < branch.targets.size(); ++index) {
                BlockID target = branch.targets[index].block;
                if (!isInLoopBehindHeader(target) || !outlooks[target.index()].staysQuiet)
                    continue;
                ++numberOfQuietWays;
                if (!best || rankOf(index) < rankOf(*best))
                    best = index;
            }
            if (numberOfQuietWays > 1) {
                if (std::optional<uint32_t> predicted = findTargetThatFlowPredicts(plan, branch))
                    return predicted;
            }
            return best;
        };
        bool asksSite = false;
        // Likely code that runs other code is left out only where flow has said what is found instead: the one function that the program stores under the name.
        // Without that, the way that is kept is merely the one that does nothing, and an emitter with two listeners would get a loop for no listener.
        bool leavesOutLikelyCode = false;
        bool keepsWhatFlowPredicts = false;
        for (BlockID id : cfg.reversePostOrder()) {
            if (!isReached.get(id.index()))
                continue;
            const Block& block = m_function.at(id);
            if (id != header) {
                std::optional<Edge> only;
                unsigned numberOfWaysIn = 0;
                for (Edge edge : cfg.incomingEdges(id)) {
                    if (!isReached.get(edge.from.index()) || (keptTargetOf[edge.from.index()] && *keptTargetOf[edge.from.index()] != edge.targetIndex))
                        continue;
                    only = edge;
                    ++numberOfWaysIn;
                }
                if (numberOfWaysIn == 1) {
                    const Target& target = targetOf(m_function, *only);
                    for (uint32_t position = 0; position < block.parameters.size(); ++position) {
                        if (position >= target.arguments.size()) {
                            if (keptTargetOf[only->from.index()])
                                staysTheSame.set(block.parameters[position].index());
                            continue;
                        }
                        if (!isSameInEveryTurn(target.arguments[position]))
                            continue;
                        staysTheSame.set(block.parameters[position].index());
                        plan.steps.append({ Step::Kind::Parameter, id, block.parameters[position], target.arguments[position] });
                    }
                }
            }
            for (ValueID value : block.values) {
                const Value& operation = m_function.at(value);
                bool hasSameOperands = true;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    hasSameOperands &= isSameInEveryTurn(operand);
                });
                Effects effects = m_analyses.effectsOf(value).onAnyEdge();
                if (hasSameOperands && mayBecomeQuestion(id, value)) {
                    plan.steps.append({ Step::Kind::ReadByName, id, value });
                    staysTheSame.set(value.index());
                    asksSite = true;
                    continue;
                }
                if (hasSameOperands && !changesInLoop.get(value.index()) && (questionsAskedAheadOfLoops.contains(operation.opcode) || holdsBehindFirstTurn(id, operation)) && (effects.isUnobservable() || operation.opcode == Opcode::RequireObjectCoercible)) {
                    plan.steps.append({ Step::Kind::Computation, id, value });
                    staysTheSame.set(value.index());
                    continue;
                }
                if (!staysQuiet(effects))
                    return std::nullopt;
                writes.merge(effects.writes);
            }
            Effects effects = m_analyses.effectsOfTerminator(id).onAnyEdge();
            if (!staysQuiet(effects))
                return std::nullopt;
            writes.merge(effects.writes);
            const Terminator& terminator = block.terminator;
            if (terminator.kind == TerminatorKind::Branch && staysTheSame.get(terminator.conditionOperand().index())) {
                if (std::optional<uint32_t> kept = chooseTarget(terminator)) {
                    keptTargetOf[id.index()] = kept;
                    BlockID other = terminator.targets[1 - *kept].block;
                    leavesOutLikelyCode |= isInLoopBehindHeader(other) && !isUnlikely(other) && !outlooks[other.index()].staysQuiet;
                    keepsWhatFlowPredicts |= !*kept && m_function.at(terminator.conditionOperand()).opcode == Opcode::IsClosureOf;
                    plan.steps.append({ Step::Kind::Test, id, terminator.conditionOperand(), { }, *kept });
                    isReached.set(terminator.targets[*kept].block.index());
                    continue;
                }
            }
            for (uint32_t index = 0; index < terminator.targets.size(); ++index) {
                BlockID target = terminator.targets[index].block;
                if (!isInLoopBehindHeader(target))
                    continue;
                if (outlooks[target.index()].staysQuiet) {
                    isReached.set(target.index());
                    continue;
                }
                if (!isUnlikely(target))
                    return std::nullopt;
                // The copy is entered there from the loop and from its own blocks. A guard speaks of the operands of one test, so two of them do not join.
                bool passesGuard = std::ranges::any_of(m_function.at(target).parameters.span().last(numberOfEdgeResults(m_function, id, index)), [&](ValueID result) {
                    return !typeOf(result).isJavaScriptValue();
                });
                if (passesGuard)
                    return std::nullopt;
                plan.leavingEdges.append(Edge { id, index });
            }
        }
        if (!asksSite || (leavesOutLikelyCode && !keepsWhatFlowPredicts))
            return std::nullopt;
        bool isSettled = true;
        for (const Step& step : plan.steps) {
            if (step.kind == Step::Kind::ReadByName && writes.overlaps(m_program, MemoryLocation::objectField(m_function.at(step.value).key())))
                return std::nullopt;
            if (step.kind == Step::Kind::Computation && m_analyses.effectsOf(step.value).onAnyEdge().reads.without(bookkeepingHeaps).overlaps(writes)) {
                changesInLoop.set(step.value.index());
                isSettled = false;
            }
        }
        return isSettled;
    }

    // The loops share no block. Answers how many of them got a version.
    unsigned makeVersions(std::span<const Plan> plans, size_t mostValuesWithCopies)
    {
        IRBuilder builder = makeBuilder();
        struct Copies {
            std::unique_ptr<RegionCloner> whole;
            std::unique_ptr<RegionCloner> firstTurn;
        };
        Vector<Copies> copies;
        for (const Plan& plan : plans) {
            if (m_function.values.size() > mostValuesWithCopies)
                break;
            copies.append({ makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, m_function), makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, m_function) });
            for (RegionCloner* cloner : { copies.last().whole.get(), copies.last().firstTurn.get() }) {
                cloner->cloneBlocks(plan.region.span(), [](ValueID, Value&) { });
                for (BlockID block : cloner->clonedBlocks())
                    m_isFinishedHeader.set(block.index());
            }
            for (Edge edge : plan.leavingEdges) {
                const Target& target = targetOf(m_function, edge);
                builder.redirectEdge(edge, copies.last().whole->cloneOf(target.block), Arguments(target.arguments));
            }
        }
        unsigned numberOfVersions = copies.size();
        if (!numberOfVersions)
            return 0;
        SSARepair repair(builder, m_analyses);
        for (const Copies& made : copies) {
            made.whole->addVariablesTo(repair, m_analyses);
            made.firstTurn->addVariablesTo(repair, m_analyses);
        }
        for (unsigned index = 0; index < numberOfVersions; ++index)
            askAheadOfLoop(builder, plans[index], *copies[index].whole, *copies[index].firstTurn);
        builder.emptyUnreachableBlocks();
        m_analyses.didChangeControlFlow();
        repair.run();
        m_analyses.didChangeControlFlow();
        m_context.build.statistics().count(StatisticID::LoopsEnteredByAnswers, numberOfVersions);
        return numberOfVersions;
    }

    // It asks nothing about the function as a whole.
    void askAheadOfLoop(IRBuilder& builder, const Plan& plan, const RegionCloner& cloner, const RegionCloner& firstTurn)
    {
        BlockID headerOfCopy = cloner.cloneOf(plan.header);
        builder.redirectEdge(plan.entry, firstTurn.cloneOf(plan.header), Arguments(targetOf(m_function, plan.entry).arguments));
        Expected<BlockFrequency> frequency = m_function.at(plan.header).frequency;
        BlockID current = builder.addBlock(frequency);
        Arguments entering;
        for (ValueID parameter : m_function.at(plan.header).parameters)
            entering.append(builder.addParameter(current, m_function.at(parameter).type));
        for (Edge latch : plan.latches) {
            Edge endOfFirstTurn { firstTurn.cloneOf(latch.from), latch.targetIndex };
            builder.redirectEdge(endOfFirstTurn, current, Arguments(targetOf(m_function, endOfFirstTurn).arguments));
        }

        // A branch both of whose sides leave the loop stays in it, and its edges go on handing over what its test proves: the condition stays that test.
        BitVector conditionsThatStay(m_function.values.size());
        for (BlockID block : plan.region) {
            const Terminator& terminator = m_function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch || (terminator.taken().proofs.isEmpty() && terminator.notTaken().proofs.isEmpty()))
                continue;
            bool becomesJump = std::ranges::any_of(plan.steps, [&](const Step& step) {
                return step.kind == Step::Kind::Test && step.block == block;
            });
            if (!becomesJump)
                conditionsThatStay.set(terminator.conditionOperand().index());
        }

        HashMap<ValueID, ValueID> askedAhead;
        auto aheadOfLoop = [&](ValueID value) {
            return value ? askedAhead.getOptional(value).value_or(value) : value;
        };
        struct Answer {
            ValueID object;
            PropertyKeyID key;
            ValueID value;
        };
        Vector<Answer, 4> answers;
        Type valueOrNothing = Type::anything();
        valueOrNothing.primitives |= Primitive::Absent;
        TypeID typeOfAnswer = m_context.types.intern(valueOrNothing);
        Vector<BlockID, 8> otherAnswers;

        for (const Step& step : plan.steps) {
            switch (step.kind) {
            case Step::Kind::Parameter:
                askedAhead.set(step.value, aheadOfLoop(step.passed));
                break;
            case Step::Kind::Computation: {
                Value copy(m_function.at(step.value));
                copy.forEachOperand([&](ValueID& operand, uint32_t) {
                    operand = aheadOfLoop(operand);
                });
                builder.moveToEndOf(current);
                builder.setOrigin(m_function.at(step.value).origin());
                ValueID asked = builder.append(WTF::move(copy));
                askedAhead.set(step.value, asked);
                if (typeOf(step.value).isJavaScriptValue() && !conditionsThatStay.get(step.value.index()))
                    becomeUpcast(m_function, step.value, asked);
                break;
            }
            case Step::Kind::ReadByName: {
                ValueID object = aheadOfLoop(m_function.at(step.value).objectOperand());
                PropertyKeyID key = m_function.at(step.value).key();
                ValueID found;
                for (const Answer& answer : answers) {
                    if (answer.object == object && answer.key == key)
                        found = answer.value;
                }
                if (!found) {
                    builder.moveToEndOf(current);
                    builder.setOrigin(m_function.at(step.value).origin());
                    Value question = IRBuilder::make(Opcode::GetCachedValue, { object });
                    question.setKey(key);
                    question.setValueType(typeOfAnswer);
                    ValueID answer = builder.append(WTF::move(question));
                    IRBuilder::ProvenBranch knows = builder.emitBranchWithProof(builder.appendTypeTest(answer, Type::anything()), OperandIndex::TypeTest::value, frequency, frequency);
                    builder.moveToEndOf(knows.notTaken);
                    builder.emitJump(headerOfCopy, Arguments(entering));
                    otherAnswers.append(knows.notTaken);
                    current = knows.taken;
                    found = knows.provenOnTaken;
                    if (m_function.at(step.value).type != TypeTable::anything) {
                        builder.moveToEndOf(current);
                        Value cast = IRBuilder::make(Opcode::AssumeType, { found });
                        cast.setProvenType(Proven::byPosition(m_function.at(step.value).type));
                        found = builder.append(WTF::move(cast));
                    }
                    answers.append({ object, key, found });
                }
                askedAhead.set(step.value, found);
                becomeUpcast(m_function, step.value, found);
                break;
            }
            case Step::Kind::Test: {
                Terminator branch = takeTerminator(builder, step.block);
                Target kept = WTF::move(branch.targets[step.keptTarget]);
                builder.moveToEndOf(current);
                builder.setOrigin(branch.origin());
                BlockID next = builder.addBlock(frequency);
                otherAnswers.append(builder.addBlock(frequency));
                Target passes { next, { }, { } };
                Target fails { otherAnswers.last(), { }, { } };
                if (step.keptTarget)
                    std::swap(passes, fails);
                builder.emitBranch(aheadOfLoop(step.value), WTF::move(passes), WTF::move(fails));
                builder.moveToEndOf(otherAnswers.last());
                builder.emitJump(headerOfCopy, Arguments(entering));
                const CompactList<ValueID>& parameters = m_function.at(kept.block).parameters;
                size_t firstResult = parameters.size() - kept.proofs.size();
                Arguments arguments = WTF::move(kept.arguments);
                for (size_t index = 0; index < kept.proofs.size(); ++index) {
                    ValueID proven = builder.addEdgeResult(Edge { current, step.keptTarget }, kept.proofs[index]);
                    askedAhead.set(parameters[firstResult + index], proven);
                    arguments.append(proven);
                }
                builder.moveToEndOf(step.block);
                builder.emitJump(kept.block, WTF::move(arguments));
                current = next;
                break;
            }
            }
        }
        builder.moveToEndOf(current);
        builder.setOrigin(m_function.at(plan.header).terminator.origin());
        builder.emitJump(plan.header, WTF::move(entering));
        // All the questions together choose between two loops that are as likely as each other. Each as an even chance of its own would leave the loop behind them next to nothing.
        if (!otherAnswers.isEmpty())
            otherAnswers.removeLast();
        for (BlockID block : otherAnswers)
            m_function.edit(block, BlockEdit::Rest).frequency = rarerOf(frequency, BlockFrequency::Unlikely);
    }
};

// `JSON.parse(text)[i].kind` in a loop that is expected to run. Nothing with a layout of the program is seen to arrive, and the program stores nothing under the name, so whatever has it was made elsewhere, and no test of a layout can stand before the read.
// What the tables of the run time hold for the Structure of the object is read without a call. A read that they do not answer is unlikely and runs code, so it leaves the loop with the other unlikely paths.
struct ReaderFromTables : LoopEditor {
    bool isCalled(ValueID read)
    {
        constexpr OpcodeSet calls { Opcode::Call, Opcode::CallList, Opcode::CallForwardVarargs, Opcode::Construct, Opcode::ConstructList };
        return std::ranges::any_of(m_analyses.useLists().usesOf(read), [&](const Use& use) {
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = m_function.at(use.user);
            if (user.opcode == Opcode::IsClosureOf || (calls.contains(user.opcode) && !use.operandIndex))
                return true;
            return user.opcode == Opcode::TypeTest && !m_context.types.at(user.testedType()).primitives && m_context.types.at(user.testedType()).hasOnlyKnownCallees();
        });
    }

    // `encoder.encodeInto(text, bytes).written`: once the unlikely paths have left the loop, SinkAllocations hands the field over without the object.
    bool mayBeReplacedByItsFields(BlockID block, ValueID object)
    {
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        BitVector isListed(m_function.values.size());
        Vector<ValueID, 8> sources;
        auto add = [&](ValueID value) {
            ValueID source = m_analyses.identityOf(value);
            if (!isListed.set(source.index()))
                sources.append(source);
        };
        add(object);
        bool hasMaker = false;
        for (size_t next = 0; next < sources.size(); ++next) {
            ValueID source = sources[next];
            if (m_function.at(source).opcode != Opcode::Parameter) {
                if (describeVirtualObject(m_analyses, source).support != VirtualObjectSupport::Supported)
                    return false;
                hasMaker = true;
                continue;
            }
            ValueLocation location = m_analyses.useLists().locationOf(source);
            if (location.placement != ValuePlacement::Parameter || location.block == m_analyses.cfg().root())
                return false;
            for (Edge edge : m_analyses.cfg().incomingEdges(location.block)) {
                if (edge.isHandlerEdge())
                    return false;
                if ((frequencies.classOf(edge.from) > frequencies.classOf(block)).predicted())
                    continue;
                IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
                if (incoming.kind == IncomingValue::Kind::Argument) {
                    if (!incoming.argument)
                        return false;
                    add(incoming.argument);
                    continue;
                }
                std::optional<EdgeResult> result = tryGetEdgeResult(m_function, incoming.result);
                if (!result || !result->subject)
                    return false;
                add(result->subject);
            }
        }
        return hasMaker;
    }

    bool isAskedOfTables(BlockID block, uint32_t index)
    {
        ValueID id = m_function.at(block).values[index];
        const Value& read = m_function.at(id);
        if (read.opcode != Opcode::PropGet || read.isFreeName() || read.isForTypeOf() || read.asksOnlyTables() || read.isTailCall)
            return false;
        if (m_program.at(read.key()).kind != PropertyKeyKind::Text || m_program.nameClassOf(read.key()) != NameClass::Ordinary)
            return false;
        if (read.receivers() && !arrivesWithoutLayoutOfProgram(m_context, block, read.objectOperand()))
            return false;
        // The form in line is four times the call. In a large program some layout has nearly every name, so without this nearly every read of parsed data in a loop takes it.
        if (!m_program.layoutsWithName(read.key()).empty() && !paysToLeaveDataFromOutsideUntested(m_context, block))
            return false;
        // Of an object that the program has made something is known, and a method lies above the object, where the site of the read answers as well.
        if (!typeOf(read.objectOperand()).anyObject || isCalled(id))
            return false;
        // `bytes.byteLength`, where bytes is expected to be a Uint8Array: a test for the class finds the getter, which the tables do not run.
        BuiltinUseInputs inputs { m_program, m_function, m_context.types, m_context.inputs.flow, m_analyses.effectContext(), &m_analyses, { } };
        if (planBuiltinUse(inputs, id, ProgramPoint { block, index }))
            return false;
        // What the program itself puts under a name onto an object of one of its layouts lies beside the fields, where the tables need a handler.
        if (m_program.mayBeStoredThroughUnknownReceiver(read.key()) || m_program.isDefinedOnObjectsWithoutKnownLayout(read.key()))
            return false;
        const NaturalLoop* loop = m_analyses.naturalLoops().innerMostLoopOf(block);
        if (!loop || !isHeaderOfLoopOfProgram(loop->header()) || !isExpectedToRun(m_context, *loop).predicted())
            return false;
        // One object for all the turns is asked once, ahead of the loop: LoopVersioner.
        BlockID madeIn = m_analyses.useLists().blockOf(m_analyses.identityOf(read.objectOperand()));
        return madeIn && m_analyses.naturalLoops().belongsTo(madeIn, *loop) && !mayBeReplacedByItsFields(block, read.objectOperand());
    }

    PassOutcome run()
    {
        Vector<ValueSite> sites;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (m_analyses.blockFrequencies().classOf(block).predicted() != BlockFrequency::Normal)
                continue;
            const BlockValues& values = m_function.at(block).values;
            // What stands in front of a site stays where it is when the block is cut there.
            for (uint32_t index = values.size(); index--;) {
                if (isAskedOfTables(block, index))
                    sites.append(ValueSite { block, index, values[index] });
            }
        }
        if (sites.isEmpty())
            return PassOutcome::Unchanged;
        IRBuilder builder = makeBuilder();
        for (const ValueSite& site : sites) {
            SiteExpansion expansion(m_context, builder);
            expansion.begin(site);
            Value read = expansion.original();
            read.setAsksOnlyTables(true);
            ValueID answer = expansion.append(WTF::move(read));
            ValueID didRead = expansion.append(IRBuilder::make(Opcode::DidReadAbove, { answer }));
            BlockID hasAnswer = builder.addBlock(expansion.frequency());
            BlockID hasNoAnswer = builder.addBlock(BlockFrequency::Unlikely);
            builder.emitBranch(didRead, Target { hasAnswer, { }, { } }, Target { hasNoAnswer, { }, { } });
            builder.moveToEndOf(hasAnswer);
            expansion.finishArm(answer);
            builder.moveToEndOf(hasNoAnswer);
            expansion.finishArm(expansion.appendOriginal());
            expansion.end();
        }
        m_analyses.didChangeControlFlow();
        return PassOutcome::Changed;
    }
};

} // anonymous namespace

PassOutcome separateUnlikelyLoopPaths(FunctionPassContext& context)
{
    if (!context.function.hasBody() || !context.analyses.naturalLoops().numLoops())
        return PassOutcome::Unchanged;
    PassOutcome outcome = testNumbersThatBoundLoopsAroundLoops(context);
    outcome |= ReaderFromTables { { context } }.run();
    LoopPathSeparator separator { { context } };
    outcome |= separator.run();
    context.build.statistics().count(StatisticID::EntriesReadToSeparateLoops, separator.m_entriesRead);
    outcome |= dropRepeatedReadsFromTables(context);
    outcome |= LoopVersioner { { context } }.run();
    outcome |= VariableCarrier { { context } }.run();
    // SimplifyCasts runs inside this pass, and what it says it has kept holds for the function as it left it.
    context.keptAnalyses = { };
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
