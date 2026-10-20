/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDecidedBranches.h"
#include "AOTEffects.h"
#include "AOTEffectsOfOperands.h"
#include "AOTForwardDataflow.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTMemoryLocation.h"
#include "AOTTypePropagation.h"
#include "AOTUseEffectHistory.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr std::array accessedHeaps { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields, AbstractHeap::ObjectFields, AbstractHeap::ShapeEpoch };

constexpr AbstractHeapSet accessedHeapSet = [] {
    AbstractHeapSet heaps;
    for (AbstractHeap heap : accessedHeaps)
        heaps.add(heap);
    return heaps;
}();

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
        findActions();
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
        ObjectID allocation;
    };

    struct LocationClasses {
        bool isFixed;
        MemoryLocation location;
        uint64_t mask;
        IndexRange classes;
    };

    struct Action {
        enum class Kind : uint8_t { Defines, Looks, Clears };

        Kind kind;
        IndexRange bits;
    };

    using ActionList = Vector<Action, 8>;

    struct OperandWrites {
        ValueID operand;
        LocationMasks writes;
    };

    struct Writer {
        ValueID id;
        ProgramPoint after;
        const EffectsOnEdges& effects;
        LocationMasks beyond;
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
        BitVector available;
    };

    struct AvailabilityProblem {
        using State = BitVector;

        State stateAtEntry() const { return BitVector(group.size()); }

        void execute(State& state, const DataflowStep& step) const
        {
            for (const Action& action : pass.actionsIn(pass.m_actionsOfValue[step.id.index()]))
                carryOut(action, group, state);
        }

        void executeThrow(State& state, const DataflowStep& step) const
        {
            for (const Action& action : pass.actionsIn(pass.m_actionsOfValue[step.id.index()])) {
                if (action.kind == Action::Kind::Clears)
                    carryOut(action, group, state);
            }
        }

        void executeTerminator(State& state, const DataflowTerminatorStep& step) const
        {
            for (const Action& action : pass.actionsIn(pass.m_actionsOfTerminator[step.block.index()]))
                carryOut(action, group, state);
        }

        void enterEdge(State&, Edge) const { }

        bool merge(State& into, const State& from) const
        {
            size_t previousCount = into.bitCount();
            into.filter(from);
            return into.bitCount() != previousCount;
        }

        const RedundantReadElimination& pass;
        IndexRange group;
    };

    static size_t positionInAccessedHeaps(AbstractHeap heap)
    {
        auto found = std::ranges::find(accessedHeaps, heap);
        RELEASE_ASSERT(found != accessedHeaps.end());
        return static_cast<size_t>(found - accessedHeaps.begin());
    }

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    const Access& accessOf(size_t bit) const { return m_accesses[m_bits[bit].access]; }
    IndexRange bitsOf(IndexRange classes) const { return { m_classes[classes.first].bits.first, m_classes[classes.end - 1].bits.end }; }
    IndexRange classesIn(IndexRange locations) const { return { m_locations[locations.first].classes.first, m_locations[locations.end - 1].classes.end }; }
    std::span<const Action> actionsIn(IndexRange range) const { return m_actions.span().subspan(range.first, range.size()); }
    bool hasClobberableClasses() const { return !std::ranges::all_of(m_clobberableLocations, &IndexRange::isEmpty); }

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
        if (check.opcode != Opcode::CheckInitialized)
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

    std::optional<Access> findInexactAccess(ValueID id, ProgramPoint point) const
    {
        if (m_function.at(id).opcode == Opcode::GetShapeEpoch)
            return Access { .location = MemoryLocation(AbstractHeap::ShapeEpoch), .id = id, .block = point.block, .index = point.index };
        return findSettlingCheck(id, point);
    }

    std::optional<Access> findAccess(ValueID id, ProgramPoint point) const
    {
        const Value& value = m_function.at(id);
        MemoryAccess access = memoryAccessOf(m_function, value);
        switch (access.kind) {
        case MemoryAccess::Kind::NotExact:
            return findInexactAccess(id, point);
        case MemoryAccess::Kind::Read:
        case MemoryAccess::Kind::Write:
            break;
        }
        if (!accessedHeapSet.contains(access.location.heap()))
            return std::nullopt;

        bool isStable = stabilityOf(m_program, value, access) != LocationStability::Unknown;
        bool settles = !isStable && isSettling(access.location);
        return Access {
            .location = access.location,
            .base = access.base ? m_analyses.sameObjectThroughCasts(access.base) : ValueID(),
            .id = id,
            .stored = access.stored,
            .block = point.block,
            .index = point.index,
            .isFixed = isStable || settles,
            .mayPrecedeStore = settles && access.kind == MemoryAccess::Kind::Read,
        };
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
        forEachLocationAndBase(order.mutableSpan(), [&](std::span<uint32_t> members) {
            setStabilityAtAllocation(members);
        });
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
        m_classes.append(AccessClass { { firstBit, static_cast<uint32_t>(m_bits.size()) }, first.base, locationIndex, ObjectID() });
        if (first.isFixed)
            return;

        IndexRange& locations = m_clobberableLocations[positionInAccessedHeaps(first.location.heap())];
        if (locations.isEmpty())
            locations.first = locationIndex;
        locations.end = locationIndex + 1;
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

    std::span<const uint32_t> trackedClassesIn(IndexRange classes) const
    {
        return { std::ranges::lower_bound(m_trackedClasses, classes.first), std::ranges::lower_bound(m_trackedClasses, classes.end) };
    }

    ObjectID trackedAllocationOf(ValueID base)
    {
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

    void findTrackedClasses()
    {
        for (uint32_t classIndex = 0; classIndex < m_classes.size(); ++classIndex) {
            AccessClass& accessClass = m_classes[classIndex];
            if (!accessClass.base)
                continue;
            accessClass.allocation = trackedAllocationOf(accessClass.base);
            if (accessClass.allocation)
                m_trackedClasses.append(classIndex);
        }
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

    bool areDistinctObjects(const AccessClass& accessClass, ValueID storeBase, ObjectID uncapturedStoreAllocation, ProgramPoint afterStore)
    {
        const EscapeAnalysis& escape = escapeAnalysis();
        if (escape.gaveUp())
            return false;
        auto mayBe = [&](ValueID value, ObjectID object) {
            return escape.pointsTo(value).objects.contains(object);
        };
        if (uncapturedStoreAllocation && !mayBe(accessClass.base, uncapturedStoreAllocation))
            return true;
        if (accessClass.allocation && !mayBe(storeBase, accessClass.allocation) && isUncapturedBefore(accessClass.allocation, afterStore))
            return true;
        return escape.alias(accessClass.base, storeBase) == AliasResult::NoAlias;
    }

    static void appendClear(ActionList& actions, IndexRange bits)
    {
        if (bits.isEmpty())
            return;
        if (!actions.isEmpty() && actions.last().kind == Action::Kind::Clears) {
            IndexRange& previous = actions.last().bits;
            if (bits.first >= previous.first && bits.first <= previous.end) {
                previous.end = std::max(previous.end, bits.end);
                return;
            }
        }
        actions.append(Action { Action::Kind::Clears, bits });
    }

    static void appendClearsExcept(ActionList& actions, IndexRange bits, std::span<const IndexRange> survivors)
    {
        for (IndexRange survivor : survivors) {
            appendClear(actions, { bits.first, survivor.first });
            bits.first = survivor.end;
        }
        appendClear(actions, bits);
    }

    void appendClearsIn(IndexRange classes, std::ranges::input_range auto candidates, ActionList& actions, NOESCAPE const Invocable<bool(const AccessClass&)> auto& survives)
    {
        Vector<IndexRange, 8> survivors;
        for (uint32_t classIndex : candidates) {
            const AccessClass& accessClass = m_classes[classIndex];
            if (survives(accessClass))
                survivors.append(accessClass.bits);
        }
        appendClearsExcept(actions, bitsOf(classes), survivors.span());
    }

    void appendClearsOfStore(const Access& store, ActionList& actions)
    {
        std::array sections { classesAt(true, store.location), classesAt(false, store.location) };
        if (std::ranges::all_of(sections, &IndexRange::isEmpty))
            return;

        ProgramPoint afterStore { store.block, store.index + 1 };
        ObjectID allocation = store.base ? uncapturedAllocationOf(store.base, afterStore) : ObjectID();
        for (IndexRange classes : sections) {
            if (classes.isEmpty())
                continue;
            appendClearsIn(classes, std::views::iota(classes.first, classes.end), actions, [&](const AccessClass& accessClass) {
                return store.base && accessClass.base != store.base && areDistinctObjects(accessClass, store.base, allocation, afterStore);
            });
        }
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

    bool isUnaffected(const AccessClass& accessClass, Writer& writer)
    {
        if (!accessClass.base || escapeAnalysis().gaveUp())
            return false;
        const LocationClasses& entry = m_locations[accessClass.location];
        AbstractHeap heap = entry.location.heap();
        if (writer.beyond.overlaps(heap, entry.mask) && (!accessClass.allocation || !isUncapturedBefore(accessClass.allocation, writer.after)))
            return false;
        return std::ranges::none_of(operandWritesOf(writer), [&](const OperandWrites& written) {
            return written.writes.overlaps(heap, entry.mask) && escapeAnalysis().alias(written.operand, accessClass.base) != AliasResult::NoAlias;
        });
    }

    void appendClearsOfWrites(const LocationMasks& writes, const LocationMasks& beyond, ActionList& actions, NOESCAPE const Invocable<bool(const AccessClass&)> auto& survives)
    {
        for (size_t position = 0; position < accessedHeaps.size(); ++position) {
            AbstractHeap heap = accessedHeaps[position];
            IndexRange locations = m_clobberableLocations[position];
            if (locations.isEmpty() || !writes.contains(heap))
                continue;
            if (writes.containsEntire(heap) && beyond.containsEntire(heap)) {
                IndexRange classes = classesIn(locations);
                appendClearsIn(classes, trackedClassesIn(classes), actions, survives);
                continue;
            }
            for (const LocationClasses& entry : m_locations.span().subspan(locations.first, locations.size())) {
                if (writes.overlaps(heap, entry.mask))
                    appendClearsIn(entry.classes, std::views::iota(entry.classes.first, entry.classes.end), actions, survives);
            }
        }
    }

    void appendClearsOfEffects(ProgramPoint point, ActionList& actions)
    {
        if (!hasClobberableClasses())
            return;
        ValueID id = m_function.at(point.block).values[point.index];
        const EffectsOnEdges& effects = m_analyses.effectsOf(id);
        LocationMasks writes = effects.onAnyEdge().writes;
        if (!writes.containsAny(accessedHeapSet))
            return;

        Writer writer {
            .id = id,
            .after = { point.block, point.index + 1 },
            .effects = effects,
            .beyond = effectsBeyondOperands(m_analyses.effectContext(), m_function, m_function.at(id), effects).onAnyEdge().writes,
            .operandWrites = std::nullopt,
        };
        appendClearsOfWrites(writes, writer.beyond, actions, [&](const AccessClass& accessClass) {
            return isUnaffected(accessClass, writer);
        });
    }

    void appendClearsOfTerminator(BlockID block, ActionList& actions)
    {
        if (!hasClobberableClasses())
            return;
        LocationMasks writes = m_analyses.effectsOfTerminator(block).onAnyEdge().writes;
        appendClearsOfWrites(writes, writes, actions, [](const AccessClass&) {
            return false;
        });
    }

    void appendClears(ProgramPoint point, std::span<const Access> accesses, ActionList& actions)
    {
        if (accesses.empty()) {
            appendClearsOfEffects(point, actions);
            return;
        }
        const Access& access = accesses.front();
        if (access.stored && !access.isAtAllocation() && !access.isSettling)
            appendClearsOfStore(access, actions);
    }

    static void appendDefinitions(std::span<const Access> accesses, ActionList& actions)
    {
        for (const Access& access : accesses) {
            if (!access.bit)
                continue;
            Action::Kind kind = access.mayPrecedeStore ? Action::Kind::Looks : Action::Kind::Defines;
            actions.append(Action { kind, { *access.bit, *access.bit + 1 } });
        }
    }

    IndexRange recordActions(ActionList& actions)
    {
        uint32_t first = static_cast<uint32_t>(m_actions.size());
        m_actions.append(actions.span());
        actions.shrink(0);
        return { first, static_cast<uint32_t>(m_actions.size()) };
    }

    void findActions()
    {
        findTrackedClasses();
        m_actionsOfValue.grow(m_function.values.size());
        m_actionsOfTerminator.grow(m_function.blocks.size());
        ActionList actions;
        size_t nextAccess = 0;
        for (BlockID blockID : m_cfg.reversePostOrder()) {
            const Block& block = m_function.at(blockID);
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                size_t firstAccess = nextAccess;
                while (nextAccess < m_accesses.size() && m_accesses[nextAccess].id == id)
                    ++nextAccess;
                std::span<const Access> accesses = m_accesses.span().subspan(firstAccess, nextAccess - firstAccess);
                appendClears(ProgramPoint { blockID, index }, accesses, actions);
                appendDefinitions(accesses, actions);
                m_actionsOfValue[id.index()] = recordActions(actions);
            }
            appendClearsOfTerminator(blockID, actions);
            m_actionsOfTerminator[blockID.index()] = recordActions(actions);
        }
    }

    IndexRange actionsOfValuesIn(BlockID block) const
    {
        const BlockValues& values = m_function.at(block).values;
        if (values.isEmpty())
            return { };
        return { m_actionsOfValue[values.first().index()].first, m_actionsOfValue[values.last().index()].end };
    }

    static void carryOut(const Action& action, IndexRange group, BitVector& available)
    {
        IndexRange bits = action.bits.clippedTo(group);
        if (bits.isEmpty())
            return;
        size_t first = bits.first - group.first;
        size_t end = bits.end - group.first;
        switch (action.kind) {
        case Action::Kind::Defines:
            available.quickSet(first);
            return;
        case Action::Kind::Looks:
            return;
        case Action::Kind::Clears:
            for (size_t bit = available.findBit(first, true); bit < end; bit = available.findBit(bit + 1, true))
                available.quickClear(bit);
            return;
        }
    }

    void solve(IndexRange group)
    {
        AvailabilityProblem problem { *this, group };
        m_heads = m_analyses.solveForward(problem);
    }

    void solveGroups()
    {
        uint64_t maxBitsPerGroup = m_context.limit(LimitID::MaxReadsPerSolve);
        bool hasSeveralGroups = false;
        IndexRange group;
        for (const AccessClass& accessClass : m_classes) {
            if (!group.isEmpty() && accessClass.bits.end - group.first > maxBitsPerGroup) {
                solve(group);
                findReplacements(group);
                group.first = accessClass.bits.first;
                hasSeveralGroups = true;
            }
            group.end = accessClass.bits.end;
        }
        solve(group);
        findReplacements(group);
        if (hasSeveralGroups)
            m_context.gaveUp(LimitID::MaxReadsPerSolve);
    }

    bool canForward(const Access& source, const Type& readType) const
    {
        if (source.mayBeDeferredClosure)
            return false;
        return typeOf(source.valueAfter()).isSubsetOf(readType, m_program);
    }

    std::optional<uint32_t> findAvailableAccess(uint32_t readBit, const BitVector& available, IndexRange group, NOESCAPE const Invocable<bool(const Access&)> auto& isSuitable) const
    {
        IndexRange bits = m_classes[m_bits[readBit].accessClass].bits;
        for (size_t bit = group.first + available.findBit(bits.first - group.first, true); bit < bits.end; bit = group.first + available.findBit(bit + 1 - group.first, true)) {
            if (bit != readBit && isSuitable(accessOf(bit)))
                return static_cast<uint32_t>(bit);
        }
        return std::nullopt;
    }

    std::optional<Replacement> findReplacement(uint32_t bit, const BitVector& available, IndexRange group) const
    {
        const Access& read = accessOf(bit);
        if (!read.isRead())
            return std::nullopt;
        const Type& readType = typeOf(read.id);
        bool readsAbsentAsUndefined = m_function.at(read.id).opcode == Opcode::GetSlotOrUndefined && (readType.primitives & Primitive::Undefined);
        std::optional<uint32_t> source = findAvailableAccess(bit, available, group, [&](const Access& access) {
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

    Vector<IncomingState, 4> computeIncomingStates(AvailabilityProblem& problem, const Access& read, std::span<const Action> earlierActions)
    {
        Vector<IncomingState, 4> states;
        if (!canMergeInto(read.block, read.id))
            return states;
        for (Edge edge : m_cfg.incomingEdges(read.block)) {
            const Block& predecessor = m_function.at(edge.from);
            BitVector available = *m_heads[edge.from.index()];
            replayBlock(m_analyses, problem, available, edge.from, static_cast<uint32_t>(predecessor.values.size()));
            problem.executeTerminator(available, DataflowTerminatorStep { edge.from, predecessor.terminator, m_analyses.effectsOfTerminator(edge.from) });
            for (const Action& action : earlierActions)
                carryOut(action, problem.group, available);
            states.append(IncomingState { edge, WTF::move(available) });
        }
        return states;
    }

    bool isMergeableRead(uint32_t bit) const
    {
        const Access& access = accessOf(bit);
        return access.isRead() && typeOf(access.id).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    }

    void tryMerge(uint32_t bit, std::span<const IncomingState> incomingStates, IndexRange group)
    {
        if (incomingStates.empty())
            return;
        const Access& read = accessOf(bit);
        const Type& readType = typeOf(read.id);
        Merge merge { read.block, read.id, read.index, { } };
        for (const IncomingState& state : incomingStates) {
            std::optional<uint32_t> source = findAvailableAccess(bit, state.available, group, [&](const Access& access) {
                return !access.isAbsent && canForward(access, readType);
            });
            if (!source)
                return;
            merge.incoming.append(Merge::Incoming { state.edge, accessOf(*source).valueAfter() });
        }
        m_merges.append(WTF::move(merge));
    }

    void findReplacements(IndexRange group)
    {
        AvailabilityProblem problem { *this, group };
        for (BlockID blockID : m_cfg.reversePostOrder()) {
            const std::optional<BitVector>& head = m_heads[blockID.index()];
            IndexRange actions = actionsOfValuesIn(blockID);
            if (!head || actions.isEmpty())
                continue;
            BitVector available = *head;
            std::optional<Vector<IncomingState, 4>> incomingStates;
            for (uint32_t actionIndex = actions.first; actionIndex < actions.end; ++actionIndex) {
                const Action& action = m_actions[actionIndex];
                if (action.kind != Action::Kind::Clears && group.contains(action.bits.first)) {
                    uint32_t bit = action.bits.first;
                    if (std::optional<Replacement> replacement = findReplacement(bit, available, group))
                        m_replacements.append(*replacement);
                    else if (isMergeableRead(bit)) {
                        if (!incomingStates)
                            incomingStates = computeIncomingStates(problem, accessOf(bit), actionsIn({ actions.first, actionIndex }));
                        tryMerge(bit, incomingStates->span(), group);
                    }
                }
                carryOut(action, group, available);
                if (!incomingStates)
                    continue;
                for (IncomingState& state : *incomingStates)
                    carryOut(action, group, state.available);
            }
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
            m_function.at(parameter).setPrediction(m_function.at(merge.read).prediction());
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
        narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build }, replacingValues.span());
        return PassOutcome::Changed;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    const EscapeAnalysis* m_escape { nullptr };
    std::optional<UseEffectHistory> m_captureHistory;
    Vector<Access> m_accesses;
    Vector<Bit> m_bits;
    Vector<AccessClass> m_classes;
    Vector<LocationClasses> m_locations;
    std::array<IndexRange, accessedHeaps.size()> m_clobberableLocations { };
    Vector<uint32_t> m_trackedClasses;
    Vector<Action> m_actions;
    Vector<IndexRange> m_actionsOfValue;
    Vector<IndexRange> m_actionsOfTerminator;
    Vector<std::optional<BitVector>> m_heads;
    Vector<Replacement> m_replacements;
    Vector<Merge> m_merges;
};

} // anonymous namespace

PassOutcome eliminateRedundantReads(FunctionPassContext& context)
{
    return RedundantReadElimination(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
