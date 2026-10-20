/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeferClosuresInFields.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::GetSlot::object == OperandIndex::GetDeferredClosureSlot::object && OperandIndex::GetSlot::guard == OperandIndex::GetDeferredClosureSlot::guard);
static_assert(OperandIndex::GetSlotOrUndefined::object == OperandIndex::GetDeferredClosureSlot::object && OperandIndex::GetSlotOrUndefined::guard == OperandIndex::GetDeferredClosureSlot::guard);

namespace {

using Row = uint32_t;

class FieldRows {
public:
    explicit FieldRows(const Program& program)
    {
        m_firstRows.reserveInitialCapacity(program.layouts.size() + 1);
        Row next = 0;
        for (const Layout& layout : program.layouts) {
            m_firstRows.append(next);
            next += layout.fields.size();
        }
        m_firstRows.append(next);
    }

    Row rowOf(LayoutID layout, uint32_t field) const { return m_firstRows[layout.index()] + field; }
    size_t count() const { return m_firstRows.last(); }

private:
    Vector<Row> m_firstRows;
};

enum class Refusal : uint8_t {
    None,
    NotGivenByEveryAllocation,
    NotGivenUnderPrototypeOfAnotherClass,
    GivenClosuresOfSeveralFunctions,
    StoredElsewhere,
    ReadTogetherWithFieldThatMayBeEmpty,
};
constexpr size_t numberOfRefusals = 6;

struct RowState {
    bool mayDefer() const { return function && refusal == Refusal::None; }

    FunctionID function;
    uint32_t numberOfGivingAllocations { 0 };
    Refusal refusal { Refusal::None };
};

// What an allocation lists for a field, or what a store gives the field before anything else can see the object.
struct GivenValue {
    uint32_t field { 0 };
    ValueID store;
    ValueID value;
    // The closure behind the value, where it was made for this field alone and the run time could make it.
    ValueID closure;
    bool canBeListed { false };
};

struct Creation {
    FunctionID function;
    BlockID block;
    ValueID object;
    LayoutID layout;
    Vector<GivenValue> values;
};

struct SlotAccess {
    FunctionID function;
    ValueID value;

    friend bool operator==(const SlotAccess&, const SlotAccess&) = default;
    friend auto operator<=>(const SlotAccess&, const SlotAccess&) = default;
};

struct FunctionEdits {
    FunctionID function;
    std::span<const Creation> creations;
    std::span<const SlotAccess> reads;
};

struct EditCounts {
    uint64_t closures { 0 };
    uint64_t stores { 0 };
};

} // anonymous namespace

static ASCIILiteral nameOf(Refusal refusal)
{
    switch (refusal) {
    case Refusal::None:
        return "defer"_s;
    case Refusal::NotGivenByEveryAllocation:
        return "are not given by every allocation"_s;
    case Refusal::NotGivenUnderPrototypeOfAnotherClass:
        return "are given by every allocation but those whose prototype may select another class"_s;
    case Refusal::GivenClosuresOfSeveralFunctions:
        return "are given closures of several functions"_s;
    case Refusal::StoredElsewhere:
        return "are stored elsewhere"_s;
    case Refusal::ReadTogetherWithFieldThatMayBeEmpty:
        return "are read together with a field that may be empty"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The rows of a class and of the layouts below it share a slot, and the run time allocates the object of a subclass and of a function called with new.
static BitVector findLayoutsThatMayDefer(const Program& program)
{
    BitVector isBase(program.layouts.size());
    for (const Layout& layout : program.layouts) {
        if (layout.flow.baseState == BaseState::Layout)
            isBase.quickSet(layout.flow.base.index());
    }

    BitVector result(program.layouts.size());
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance)
            continue;
        if (layout.flow.allocation == AllocationState::NeverAllocated || isBase.quickGet(index))
            continue;
        if (layout.flow.baseState == BaseState::Layout || layout.flow.baseState == BaseState::BuiltinClass)
            continue;
        if (layout.constructor && program.at(layout.constructor).kind != FunctionKind::BaseConstructor)
            continue;
        result.quickSet(index);
    }
    return result;
}

static bool mayDeferClosure(const Program& program, FieldReference reference)
{
    constexpr OptionSet<FieldFlags> flagsOfOtherFields { FieldFlags::IsPrivate, FieldFlags::IsImmutable, FieldFlags::IsBrand, FieldFlags::IsHidden, FieldFlags::IsInherited, FieldFlags::IsRemovedAsUnread };
    const Field& field = fieldAt(program, reference);
    return field.key && !field.flags.containsAny(flagsOfOtherFields) && !program.isIndexLike(field.key) && !isRemovedField(program, reference);
}

static bool canBeMadeByRunTime(const Program& program, const Value& closure)
{
    const Function& callee = program.at(closure.function());
    if (callee.constructedClass || callee.constructorForm() != ConstructorForm::NotConstructor || callee.captures.size() > 1)
        return false;
    return closure.storage() == AllocationStorage::Heap;
}

static bool createsValueWithoutRunningCode(const Value& value)
{
    if (isRematerializable(value) || isCastWithoutTest(value.opcode) || value.opcode == Opcode::AllGuards)
        return true;
    return interpretedOf(value.opcode) == Interpretation::Allocation && declaredBoundOf(value.opcode).isRemovableWhenUnused();
}

static void countAllocation(std::span<uint32_t> allocationsOfLayouts, LayoutID layout)
{
    WTF::atomicExchangeAdd(&allocationsOfLayouts[layout.index()], 1u, std::memory_order_relaxed);
}

namespace {

class CreationFinder {
public:
    CreationFinder(const Program& program, BuildContext& build, const BitVector& layoutsThatMayDefer, FunctionID id)
        : m_program(program)
        , m_build(build)
        , m_layoutsThatMayDefer(layoutsThatMayDefer)
        , m_id(id)
        , m_function(program.at(id))
    {
    }

    void run(std::span<uint32_t> allocationsOfLayouts, std::span<uint32_t> allocationsThatMayGiveOtherObjects, Vector<Creation>& found)
    {
        bool isEnabled = m_build.isEnabled(PassID::DeferClosuresInFields, m_id);
        for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
            const BlockValues& values = m_function.blocks[blockIndex].values;
            for (size_t index = 0; index < values.size(); ++index) {
                const Value& value = m_function.at(values[index]);
                if ((value.opcode != Opcode::NewObject && value.opcode != Opcode::CloneAs) || !m_layoutsThatMayDefer.quickGet(value.layout().index()))
                    continue;
                countAllocation(allocationsOfLayouts, value.layout());
                if (mayAllocateOtherLayout(m_program, value)) {
                    countAllocation(allocationsThatMayGiveOtherObjects, value.layout());
                    continue;
                }
                if (!isEnabled || value.opcode != Opcode::NewObject)
                    continue;
                Creation creation = describeCreation(BlockID(blockIndex), index);
                bool givesClosure = creation.values.containsIf([](const GivenValue& given) {
                    return !!given.closure;
                });
                if (givesClosure)
                    found.append(WTF::move(creation));
            }
        }
    }

private:
    const UseLists& useLists()
    {
        if (!m_analyses)
            m_analyses.emplace(AnalysisContext { m_build, m_program, nullptr, nullptr, m_id }, m_function);
        return m_analyses->useLists();
    }

    void beginStretch(ValueID object)
    {
        m_isInStretch.ensureSize(m_function.values.size());
        m_isObject.ensureSize(m_function.values.size());
        m_canMove.ensureSize(m_function.values.size());
        for (ValueID id : m_stretch) {
            m_isInStretch.quickClear(id.index());
            m_isObject.quickClear(id.index());
            m_canMove.quickClear(id.index());
        }
        m_stretch.shrink(0);
        m_stretch.append(object);
        m_isInStretch.quickSet(object.index());
        m_isObject.quickSet(object.index());
    }

    bool isObject(ValueID id) const { return id && m_isObject.quickGet(id.index()); }
    bool isAvailableAtAllocation(ValueID id) const { return !m_isInStretch.quickGet(id.index()) || m_canMove.quickGet(id.index()); }

    void addToStretch(ValueID id, const Value& value)
    {
        bool usesOnlyObject = true;
        bool canMove = true;
        value.forEachChild([&](ValueID operand) {
            usesOnlyObject &= isObject(operand);
            canMove &= isAvailableAtAllocation(operand);
        });
        m_stretch.append(id);
        m_isInStretch.quickSet(id.index());
        if (usesOnlyObject && (isCastWithoutTest(value.opcode) || value.opcode == Opcode::AllGuards))
            m_isObject.quickSet(id.index());
        if (canMove)
            m_canMove.quickSet(id.index());
    }

    // One closure that several objects share is one function object, so the closure has to be made as often as the object.
    ValueID findClosureMadeFor(BlockID block, ValueID given)
    {
        ValueID id = definitionThroughUpcasts(m_function, given);
        const Value& closure = m_function.at(id);
        if (closure.opcode != Opcode::NewClosure || !canBeMadeByRunTime(m_program, closure))
            return { };

        const UseLists& uses = useLists();
        for (ValueID cast = given; cast != id; cast = m_function.at(cast).operand(0)) {
            if (uses.useCount(cast) != 1)
                return { };
        }
        if (uses.useCount(id) != 1)
            return { };
        ValueLocation location = uses.locationOf(id);
        if (location.placement != ValuePlacement::InBlock || location.block != block)
            return { };
        if (closure.isMadeOnDemand())
            return id;

        std::span<const ValueID> captures = closure.capturesOperands();
        if (captures.size() != m_program.at(closure.function()).captures.size())
            return { };
        return captures.empty() || isObject(captures[0]) ? id : ValueID();
    }

    std::optional<uint32_t> fieldStoredBy(const Value& store, LayoutID layout) const
    {
        if (store.level() != KnowledgeLevel::Proven || (store.guardOperand() && !isObject(store.guardOperand())))
            return std::nullopt;
        const LayoutSet& receivers = m_program.at(store.receivers());
        if (!receivers.isComplete || receivers.members.size() != 1 || receivers.members[0] != layout)
            return std::nullopt;
        return m_program.at(layout).findField(store.key());
    }

    Creation describeCreation(BlockID blockID, size_t allocationIndex)
    {
        const BlockValues& values = m_function.at(blockID).values;
        ValueID object = values[allocationIndex];
        const Value& allocation = m_function.at(object);
        LayoutID layout = allocation.layout();
        Creation creation { m_id, blockID, object, layout, { } };
        beginStretch(object);

        auto closureFor = [&](uint32_t field, ValueID given) {
            return mayDeferClosure(m_program, { layout, field }) ? findClosureMadeFor(blockID, given) : ValueID();
        };

        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> listedValues = allocation.valuesOperands();
        for (size_t index = 0; index < fields.size(); ++index)
            creation.values.append({ fields[index], { }, listedValues[index], closureFor(fields[index], listedValues[index]), true });

        for (size_t index = allocationIndex + 1; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& value = m_function.at(id);
            if (value.opcode != Opcode::PutSlot && value.opcode != Opcode::PutSlotOrAdd) {
                if (!createsValueWithoutRunningCode(value))
                    break;
                addToStretch(id, value);
                continue;
            }
            std::optional<uint32_t> field = isObject(value.objectOperand()) ? fieldStoredBy(value, layout) : std::nullopt;
            if (!field)
                break;
            ValueID stored = value.valueOperand();
            creation.values.append({ *field, id, stored, closureFor(*field, stored), isAvailableAtAllocation(stored) });
        }
        return creation;
    }

    const Program& m_program;
    BuildContext& m_build;
    const BitVector& m_layoutsThatMayDefer;
    FunctionID m_id;
    const Function& m_function;
    std::optional<FunctionAnalyses> m_analyses;
    Vector<ValueID, 32> m_stretch;
    BitVector m_isInStretch;
    BitVector m_isObject;
    BitVector m_canMove;
};

class Decision {
public:
    Decision(const Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
        , m_rows(program)
    {
    }

    void run();

    bool defersAnything() const { return m_numberOfDeferringRows; }
    uint64_t numberOfDeferringRows() const { return m_numberOfDeferringRows; }
    std::span<const Creation> creations() const LIFETIME_BOUND { return m_creations.span(); }
    std::span<const SlotAccess> reads() const LIFETIME_BOUND { return m_reads.span(); }
    std::span<const FieldReference> candidates() const LIFETIME_BOUND { return m_candidates.span(); }

    const RowState& stateOf(LayoutID layout, uint32_t field) const LIFETIME_BOUND { return m_states[m_rows.rowOf(layout, field)]; }
    bool defers(const Creation&, const GivenValue&) const;
    bool namesDeferringRow(const Value& access) const;
    Vector<bool, 16> chooseListedValues(const Creation&) const;

    void dump(PrintStream&) const;

private:
    void findCreations();
    void findCandidates();
    void findAccesses();
    void refuse(Row, Refusal);
    void refuseRowsNamedBy(const Value& access, Refusal);
    void countGivingAllocations();
    bool refuseRowsNotGivenByEveryAllocation();
    bool refuseRowsReadWithEmptyFields();

    void forEachCandidateNamedBy(const Value& access, const Invocable<void(Row)> auto& functor) const
    {
        for (LayoutID member : m_program.at(access.receivers()).members) {
            std::optional<uint32_t> field = m_program.at(member).findField(access.key());
            if (field && m_states[m_rows.rowOf(member, *field)].function)
                functor(m_rows.rowOf(member, *field));
        }
    }

    const Program& m_program;
    BuildContext& m_build;
    FieldRows m_rows;
    Vector<uint32_t> m_allocationsOfLayouts;
    Vector<uint32_t> m_allocationsThatMayGiveOtherObjects;
    Vector<Creation> m_creations;
    Vector<RowState> m_states;
    Vector<FieldReference> m_candidates;
    BitVector m_keysOfCandidates;
    Vector<SlotAccess> m_otherStores;
    Vector<SlotAccess> m_reads;
    uint64_t m_numberOfDeferringRows { 0 };
};

void Decision::findCreations()
{
    BitVector layoutsThatMayDefer = findLayoutsThatMayDefer(m_program);
    m_allocationsOfLayouts.fill(0, m_program.layouts.size());
    m_allocationsThatMayGiveOtherObjects.fill(0, m_program.layouts.size());
    std::span<uint32_t> allocationsOfLayouts = m_allocationsOfLayouts.mutableSpan();
    std::span<uint32_t> allocationsThatMayGiveOtherObjects = m_allocationsThatMayGiveOtherObjects.mutableSpan();
    const ParallelLoops& loops = m_build.parallelLoops();
    forEachIndexInParallel(loops, m_program.data.size(), [&](unsigned index) {
        forEachDataNodeInPreorder(m_program.data[index].root, [&](uint32_t, const DataNode& node) {
            if (const DataNode::Object* object = node.tryAs<DataNode::Object>())
                countAllocation(allocationsOfLayouts, object->layout);
        });
    });
    m_creations = collectInParallel<Creation>(loops, m_program.functions.size(), largeFunctions(m_program, loops).span(), [&](unsigned index, Vector<Creation>& found) {
        if (m_program.functions[index].hasBody())
            CreationFinder(m_program, m_build, layoutsThatMayDefer, FunctionID(index)).run(allocationsOfLayouts, allocationsThatMayGiveOtherObjects, found);
    });
}

void Decision::refuse(Row row, Refusal refusal)
{
    if (m_states[row].refusal == Refusal::None)
        m_states[row].refusal = refusal;
}

void Decision::findCandidates()
{
    m_states.fill({ }, m_rows.count());
    m_keysOfCandidates.ensureSize(m_program.types.numberOfPropertyKeys());
    for (const Creation& creation : m_creations) {
        const Function& function = m_program.at(creation.function);
        for (const GivenValue& given : creation.values) {
            if (!given.closure)
                continue;
            Row row = m_rows.rowOf(creation.layout, given.field);
            FunctionID closureFunction = function.at(given.closure).function();
            if (!m_states[row].function) {
                m_states[row].function = closureFunction;
                m_candidates.append({ creation.layout, given.field });
                m_keysOfCandidates.quickSet(fieldAt(m_program, { creation.layout, given.field }).key.index());
            } else if (m_states[row].function != closureFunction)
                refuse(row, Refusal::GivenClosuresOfSeveralFunctions);
        }
    }
}

void Decision::findAccesses()
{
    Vector<SlotAccess> storesOfCreations;
    for (const Creation& creation : m_creations) {
        for (const GivenValue& given : creation.values) {
            if (given.store)
                storesOfCreations.append({ creation.function, given.store });
        }
    }
    std::ranges::sort(storesOfCreations);

    const ParallelLoops& loops = m_build.parallelLoops();
    Vector<SlotAccess> accesses = collectInParallel<SlotAccess>(loops, m_program.functions.size(), largeFunctions(m_program, loops).span(), [&](unsigned index, Vector<SlotAccess>& found) {
        const Function& function = m_program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (!Value::opcodesWithFieldAccess.contains(value.opcode) || value.opcode == Opcode::IsSlotFilled || value.opcode == Opcode::GetDeferredClosureSlot)
                    continue;
                if (TypeTable::isProvisional(value.key()) || !m_keysOfCandidates.quickGet(value.key().index()))
                    continue;
                bool namesCandidate = false;
                forEachCandidateNamedBy(value, [&](Row) {
                    namesCandidate = true;
                });
                if (namesCandidate)
                    found.append({ FunctionID(index), id });
            }
        }
    });

    for (const SlotAccess& access : accesses) {
        if (!storedOf(m_program.at(access.function).at(access.value).opcode))
            m_reads.append(access);
        else if (!std::ranges::binary_search(storesOfCreations, access))
            m_otherStores.append(access);
    }
}

void Decision::refuseRowsNamedBy(const Value& access, Refusal refusal)
{
    forEachCandidateNamedBy(access, [&](Row row) {
        refuse(row, refusal);
    });
}

bool Decision::defers(const Creation& creation, const GivenValue& given) const
{
    return given.closure && stateOf(creation.layout, given.field).mayDefer();
}

// The order in which an allocation lists its fields is the order of their keys, and a store that stays creates its field behind all of them.
Vector<bool, 16> Decision::chooseListedValues(const Creation& creation) const
{
    Vector<bool, 16> isListed(FillWith { }, creation.values.size(), false);
    Vector<uint32_t, 16> listedFields;
    Vector<uint32_t, 4> fieldsStoredLater;
    bool canListMoreFields = true;
    for (size_t index = 0; index < creation.values.size(); ++index) {
        const GivenValue& given = creation.values[index];
        bool hasField = listedFields.contains(given.field);
        bool canBeListed = !given.store
            || ((given.canBeListed || defers(creation, given)) && !fieldsStoredLater.contains(given.field) && (hasField || canListMoreFields));
        if (!canBeListed) {
            fieldsStoredLater.append(given.field);
            canListMoreFields &= hasField;
            continue;
        }
        isListed[index] = true;
        if (!hasField)
            listedFields.append(given.field);
    }
    return isListed;
}

void Decision::countGivingAllocations()
{
    for (FieldReference candidate : m_candidates)
        m_states[m_rows.rowOf(candidate.layout, candidate.field)].numberOfGivingAllocations = 0;
    for (const Creation& creation : m_creations) {
        Vector<bool, 16> isListed = chooseListedValues(creation);
        Vector<uint32_t, 16> fieldsSeen;
        for (size_t index = creation.values.size(); index--;) {
            const GivenValue& given = creation.values[index];
            if (fieldsSeen.contains(given.field))
                continue;
            fieldsSeen.append(given.field);
            if (isListed[index] && defers(creation, given))
                ++m_states[m_rows.rowOf(creation.layout, given.field)].numberOfGivingAllocations;
        }
    }
}

bool Decision::refuseRowsNotGivenByEveryAllocation()
{
    bool didRefuse = false;
    for (FieldReference candidate : m_candidates) {
        RowState& state = m_states[m_rows.rowOf(candidate.layout, candidate.field)];
        uint32_t numberOfAllocations = m_allocationsOfLayouts[candidate.layout.index()];
        if (!state.mayDefer() || state.numberOfGivingAllocations == numberOfAllocations)
            continue;
        bool isGivenByAllOthers = state.numberOfGivingAllocations + m_allocationsThatMayGiveOtherObjects[candidate.layout.index()] == numberOfAllocations;
        state.refusal = isGivenByAllOthers ? Refusal::NotGivenUnderPrototypeOfAnotherClass : Refusal::NotGivenByEveryAllocation;
        didRefuse = true;
    }
    return didRefuse;
}

// The read of a slot that defers its closure relies on a filled slot in every receiver.
bool Decision::refuseRowsReadWithEmptyFields()
{
    bool didRefuse = false;
    for (const SlotAccess& access : m_reads) {
        const Value& read = m_program.at(access.function).at(access.value);
        if (read.opcode != Opcode::GetSlotOrUndefined || !namesDeferringRow(read))
            continue;
        bool isFilledInEveryReceiver = std::ranges::all_of(m_program.at(read.receivers()).members, [&](LayoutID member) {
            std::optional<uint32_t> field = m_program.at(member).findField(read.key());
            return field && (stateOf(member, *field).mayDefer() || flowOf(m_program, { member, *field }).occupancy == Occupancy::AlwaysFilled);
        });
        if (isFilledInEveryReceiver)
            continue;
        refuseRowsNamedBy(read, Refusal::ReadTogetherWithFieldThatMayBeEmpty);
        didRefuse = true;
    }
    return didRefuse;
}

bool Decision::namesDeferringRow(const Value& access) const
{
    bool result = false;
    forEachCandidateNamedBy(access, [&](Row row) {
        result |= m_states[row].mayDefer();
    });
    return result;
}

void Decision::run()
{
    findCreations();
    if (m_creations.isEmpty())
        return;
    findCandidates();
    findAccesses();
    for (const SlotAccess& access : m_otherStores)
        refuseRowsNamedBy(m_program.at(access.function).at(access.value), Refusal::StoredElsewhere);

    bool didRefuse = true;
    while (didRefuse) {
        countGivingAllocations();
        didRefuse = refuseRowsNotGivenByEveryAllocation();
        didRefuse |= refuseRowsReadWithEmptyFields();
    }
    for (FieldReference candidate : m_candidates)
        m_numberOfDeferringRows += stateOf(candidate.layout, candidate.field).mayDefer();
}

void Decision::dump(PrintStream& out) const
{
    std::array<uint64_t, numberOfRefusals> counts { };
    for (FieldReference candidate : m_candidates)
        ++counts[static_cast<size_t>(stateOf(candidate.layout, candidate.field).refusal)];
    for (size_t index = 0; index < numberOfRefusals; ++index)
        out.println("fields given closures that could be deferred: ", counts[index], " ", nameOf(static_cast<Refusal>(index)));
}

class FunctionRewriter {
public:
    FunctionRewriter(const Decision& decision, Function& function)
        : m_decision(decision)
        , m_function(function)
    {
    }

    EditCounts run(const FunctionEdits& edits)
    {
        for (const SlotAccess& access : edits.reads) {
            if (m_decision.namesDeferringRow(m_function.at(access.value)))
                becomeReadOfDeferredClosure(m_function.at(access.value));
        }
        for (const Creation& creation : edits.creations)
            rewrite(creation);
        return m_counts;
    }

private:
    static void becomeReadOfDeferredClosure(Value& read)
    {
        Value previous = read;
        read.become(Opcode::GetDeferredClosureSlot);
        read.setObjectOperand(previous.objectOperand());
        read.setGuardOperand(previous.guardOperand());
        read.setKey(previous.key());
        read.setReceivers(previous.receivers());
        read.setLevel(previous.level());
        if (previous.accessSite())
            read.setAccessSite(previous.accessSite());
    }

    void markForMove(ValueID id)
    {
        if (!m_isInStretch.quickGet(id.index()) || m_moves.quickSet(id.index()))
            return;
        m_function.at(id).forEachChild([&](ValueID operand) {
            markForMove(operand);
        });
    }

    void rewrite(const Creation& creation)
    {
        bool defersClosure = std::ranges::any_of(creation.values, [&](const GivenValue& given) {
            return m_decision.defers(creation, given);
        });
        if (!defersClosure)
            return;

        Vector<bool, 16> isListed = m_decision.chooseListedValues(creation);
        Vector<uint32_t, 16> fields;
        Vector<size_t, 16> givenBy;
        size_t numberOfListedStores = 0;
        for (size_t index = 0; index < creation.values.size(); ++index) {
            if (!isListed[index])
                continue;
            numberOfListedStores += !!creation.values[index].store;
            size_t position = fields.find(creation.values[index].field);
            if (position == notFound) {
                fields.append(creation.values[index].field);
                givenBy.append(index);
            } else
                givenBy[position] = index;
        }

        for (size_t index : givenBy) {
            const GivenValue& given = creation.values[index];
            if (!m_decision.defers(creation, given))
                continue;
            Value& closure = m_function.at(given.closure);
            closure.setVariadicOperands({ });
            closure.setIsMadeOnDemand(true);
            ++m_counts.closures;
        }
        if (!numberOfListedStores)
            return;

        BlockValues& values = m_function.at(creation.block).values;
        size_t allocationIndex = values.find(creation.object);
        RELEASE_ASSERT(allocationIndex != notFound);
        ValueID lastListedStore;
        for (size_t index = 0; index < creation.values.size(); ++index) {
            if (isListed[index] && creation.values[index].store)
                lastListedStore = creation.values[index].store;
        }
        size_t end = values.find(lastListedStore) + 1;
        RELEASE_ASSERT(end > allocationIndex);

        m_isInStretch.ensureSize(m_function.values.size());
        m_moves.ensureSize(m_function.values.size());
        m_isRemoved.ensureSize(m_function.values.size());
        for (size_t index = allocationIndex + 1; index < end; ++index)
            m_isInStretch.quickSet(values[index].index());
        for (size_t index = 0; index < creation.values.size(); ++index) {
            if (isListed[index] && creation.values[index].store)
                m_isRemoved.quickSet(creation.values[index].store.index());
        }
        Vector<ValueID, 16> listedValues;
        for (size_t index : givenBy) {
            listedValues.append(creation.values[index].value);
            markForMove(creation.values[index].value);
        }

        Vector<ValueID, 32> reordered;
        for (size_t index = allocationIndex + 1; index < end; ++index) {
            if (m_moves.quickGet(values[index].index()))
                reordered.append(values[index]);
        }
        reordered.append(creation.object);
        for (size_t index = allocationIndex + 1; index < end; ++index) {
            if (!m_moves.quickGet(values[index].index()) && !m_isRemoved.quickGet(values[index].index()))
                reordered.append(values[index]);
        }
        for (size_t index = allocationIndex + 1; index < end; ++index) {
            m_isInStretch.quickClear(values[index].index());
            m_moves.quickClear(values[index].index());
            m_isRemoved.quickClear(values[index].index());
        }
        std::ranges::copy(reordered, values.begin() + allocationIndex);
        values.removeAt(allocationIndex + reordered.size(), end - allocationIndex - reordered.size());

        Value& allocation = m_function.at(creation.object);
        allocation.setFields(fields.span());
        allocation.setVariadicOperands(listedValues.span());
        m_counts.stores += numberOfListedStores;
    }

    const Decision& m_decision;
    Function& m_function;
    BitVector m_isInStretch;
    BitVector m_moves;
    BitVector m_isRemoved;
    EditCounts m_counts;
};

} // anonymous namespace

static Vector<FunctionEdits> groupByFunction(std::span<const Creation> creations, std::span<const SlotAccess> reads)
{
    Vector<FunctionEdits> edits;
    while (!creations.empty() || !reads.empty()) {
        FunctionID function = creations.empty() || (!reads.empty() && reads.front().function < creations.front().function) ? reads.front().function : creations.front().function;
        size_t numberOfCreations = std::ranges::find_if(creations, [&](const Creation& creation) {
            return creation.function != function;
        }) - creations.begin();
        size_t numberOfReads = std::ranges::find_if(reads, [&](const SlotAccess& read) {
            return read.function != function;
        }) - reads.begin();
        edits.append({ function, creations.first(numberOfCreations), reads.first(numberOfReads) });
        creations = creations.subspan(numberOfCreations);
        reads = reads.subspan(numberOfReads);
    }
    return edits;
}

static bool changesFunction(const Program& program, const Decision& decision, const FunctionEdits& edits)
{
    const Function& function = program.at(edits.function);
    bool readsDeferringRow = std::ranges::any_of(edits.reads, [&](const SlotAccess& access) {
        return decision.namesDeferringRow(function.at(access.value));
    });
    return readsDeferringRow || std::ranges::any_of(edits.creations, [&](const Creation& creation) {
        return std::ranges::any_of(creation.values, [&](const GivenValue& given) {
            return decision.defers(creation, given);
        });
    });
}

static void recordDeferrals(ProgramEditor& editor, const Decision& decision)
{
    const Program& program = editor.program();
    for (FieldReference candidate : decision.candidates()) {
        const RowState& state = decision.stateOf(candidate.layout, candidate.field);
        if (!state.mayDefer())
            continue;
        FieldFlow& flow = editor.at(candidate.layout).fields[candidate.field].flow;
        flow.location.deferral = program.at(state.function).captures.isEmpty() ? ClosureDeferral::CapturesNothing : ClosureDeferral::CapturesObject;
        flow.location.deferredFunction = state.function;
        flow.location.initialization = InitializationState::SetAtCreation;
        flow.occupancy = Occupancy::AlwaysFilled;
    }
}

void deferClosuresInFields(ProgramEditor& editor, BuildContext& build)
{
    if (build.recordedOptions().useWideReferences())
        return;

    const Program& program = editor.program();
    Decision decision(program, build);
    decision.run();
    if (build.options().logging.dumpedPass == PassID::DeferClosuresInFields) [[unlikely]]
        decision.dump(WTF::dataFile());
    if (!decision.defersAnything())
        return;

    Vector<FunctionEdits> edits = groupByFunction(decision.creations(), decision.reads());
    Vector<EditCounts> counts = collectInParallel<EditCounts>(build.parallelLoops(), edits.size(), [&](unsigned index, Vector<EditCounts>& collected) {
        if (changesFunction(program, decision, edits[index]))
            collected.append(FunctionRewriter(decision, editor.at(edits[index].function)).run(edits[index]));
    });
    recordDeferrals(editor, decision);

    EditCounts total;
    for (const EditCounts& count : counts) {
        total.closures += count.closures;
        total.stores += count.stores;
    }
    Statistics& statistics = build.statistics();
    statistics.count(StatisticID::FieldsDeferringClosures, decision.numberOfDeferringRows());
    statistics.count(StatisticID::ClosuresDeferredInFields, total.closures);
    statistics.count(StatisticID::StoresListedByAllocations, total.stores);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
