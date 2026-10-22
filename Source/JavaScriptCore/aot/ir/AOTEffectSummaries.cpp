/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummaries.h"

#if ENABLE(AOT)

#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTEffectSummariesInternal.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(EffectSummaries);
WTF_MAKE_TZONE_ALLOCATED_IMPL(InternedLocationMasks);

static uint32_t hashOf(const LocationMasks& masks)
{
    uint64_t hash = masks.heaps().toRaw();
    for (AbstractHeap heap : indexedHeapsInOrder) {
        hash = (hash ^ masks.maskOf(heap)) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    }
    hash *= 0xbf58476d1ce4e5b9ull;
    return static_cast<uint32_t>(hash >> 32);
}

InternedLocationMasks::Slots::Slots(size_t size)
    : idsPlusOne(size)
{
    for (auto& slot : idsPlusOne)
        slot.store(0, std::memory_order_relaxed);
}

InternedLocationMasks::InternedLocationMasks()
{
    ID idOfEmptyMasks = intern(LocationMasks());
    RELEASE_ASSERT(idOfEmptyMasks == empty);
}

InternedLocationMasks::~InternedLocationMasks()
{
    static_assert(std::is_trivially_destructible_v<LocationMasks>);
    for (auto& chunk : m_chunks) {
        if (LocationMasks* storage = chunk.load(std::memory_order_relaxed))
            fastFree(storage);
    }
}

std::optional<InternedLocationMasks::ID> InternedLocationMasks::find(const Slots& slots, uint32_t hash, const LocationMasks& masks) const
{
    size_t mask = slots.idsPlusOne.size() - 1;
    for (size_t index = hash & mask; uint32_t idPlusOne = slots.idsPlusOne[index].load(std::memory_order_acquire); index = (index + 1) & mask) {
        if (at(idPlusOne - 1) == masks)
            return idPlusOne - 1;
    }
    return std::nullopt;
}

void InternedLocationMasks::insert(Slots& slots, uint32_t hash, ID id)
{
    size_t mask = slots.idsPlusOne.size() - 1;
    size_t index = hash & mask;
    while (slots.idsPlusOne[index].load(std::memory_order_relaxed))
        index = (index + 1) & mask;
    slots.idsPlusOne[index].store(id + 1, std::memory_order_release);
}

InternedLocationMasks::Slots& InternedLocationMasks::grow(Shard& shard)
{
    constexpr size_t initialSize = 16;
    assertIsHeld(shard.lock);
    auto larger = makeUnique<Slots>(shard.allSlots.isEmpty() ? initialSize : shard.allSlots.last()->idsPlusOne.size() * 2);
    if (!shard.allSlots.isEmpty()) {
        for (auto& slot : shard.allSlots.last()->idsPlusOne) {
            if (uint32_t idPlusOne = slot.load(std::memory_order_relaxed))
                insert(*larger, hashOf(at(idPlusOne - 1)), idPlusOne - 1);
        }
    }
    shard.allSlots.append(WTF::move(larger));
    shard.slots.store(shard.allSlots.last().get(), std::memory_order_release);
    return *shard.allSlots.last();
}

InternedLocationMasks::ID InternedLocationMasks::append(const LocationMasks& masks)
{
    ID id = m_size.fetch_add(1, std::memory_order_relaxed);
    RELEASE_ASSERT(id < 1u << CompactEffects::idBits);
    Place place = placeOf(id);
    LocationMasks* storage = m_chunks[place.chunk].load(std::memory_order_acquire);
    if (!storage) {
        Locker locker { m_chunkLock };
        storage = m_chunks[place.chunk].load(std::memory_order_relaxed);
        if (!storage) {
            storage = static_cast<LocationMasks*>(fastMalloc(sizeOfChunk(place.chunk) * sizeof(LocationMasks)));
            m_chunks[place.chunk].store(storage, std::memory_order_release);
        }
    }
    new (NotNull, &storage[place.offset]) LocationMasks(masks);
    return id;
}

InternedLocationMasks::ID InternedLocationMasks::intern(const LocationMasks& masks)
{
    uint32_t hash = hashOf(masks);
    Shard& shard = m_shards[hash >> (32 - shardBits)];
    if (const Slots* slots = shard.slots.load(std::memory_order_acquire)) {
        if (std::optional<ID> known = find(*slots, hash, masks))
            return *known;
    }

    Locker locker { shard.lock };
    Slots* slots = shard.slots.load(std::memory_order_relaxed);
    if (slots) {
        if (std::optional<ID> known = find(*slots, hash, masks))
            return *known;
    }
    if (!slots || (shard.count + 1) * 2 > slots->idsPlusOne.size())
        slots = &grow(shard);
    ID id = append(masks);
    insert(*slots, hash, id);
    ++shard.count;
    return id;
}

CompactEffects InternedLocationMasks::compress(const Effects& effects)
{
    uint8_t flags = (effects.mayThrow ? CompactEffects::MayThrow : 0)
        | (effects.mayThrow.byRule() ? CompactEffects::MayThrowByRule : 0)
        | (effects.mayCollect ? CompactEffects::MayCollect : 0)
        | (effects.mayCallUnknownCode ? CompactEffects::MayCallUnknownCode : 0)
        | (effects.maySuspend ? CompactEffects::MaySuspend : 0)
        | (effects.mayBeTerminated ? CompactEffects::MayBeTerminated : 0);
    return CompactEffects(intern(effects.reads), intern(effects.writes), flags);
}

static ThrowEffect throwEffectOf(uint8_t flags)
{
    if (!(flags & CompactEffects::MayThrow))
        return false;
    return flags & CompactEffects::MayThrowByRule ? ThrowEffect(true) : ThrowEffect::onlyForLackOfMemory();
}

Effects InternedLocationMasks::expand(CompactEffects effects) const
{
    uint8_t flags = effects.flags();
    return {
        .reads = at(effects.reads()),
        .writes = at(effects.writes()),
        .mayThrow = throwEffectOf(flags),
        .mayCollect = !!(flags & CompactEffects::MayCollect),
        .mayCallUnknownCode = !!(flags & CompactEffects::MayCallUnknownCode),
        .maySuspend = !!(flags & CompactEffects::MaySuspend),
        .mayBeTerminated = !!(flags & CompactEffects::MayBeTerminated),
    };
}

void InternedLocationMasks::mergeInto(Effects& effects, CompactEffects other) const
{
    uint8_t flags = other.flags();
    if (other.reads() != empty)
        effects.reads.merge(at(other.reads()));
    if (other.writes() != empty)
        effects.writes.merge(at(other.writes()));
    effects.mayThrow |= throwEffectOf(flags);
    effects.mayCollect |= !!(flags & CompactEffects::MayCollect);
    effects.mayCallUnknownCode |= !!(flags & CompactEffects::MayCallUnknownCode);
    effects.maySuspend |= !!(flags & CompactEffects::MaySuspend);
    effects.mayBeTerminated |= !!(flags & CompactEffects::MayBeTerminated);
}

InternedLocationMasks::ID InternedLocationMasks::merge(ID a, ID b)
{
    if (a == b || b == empty)
        return a;
    if (a == empty)
        return b;
    LocationMasks merged = at(a);
    return merged.merge(at(b)) ? intern(merged) : a;
}

void StoredFunctionEffects::computeEffectsOnCall(InternedLocationMasks& masks)
{
    auto withParameters = [&](CompactEffects effects) {
        return CompactEffects(masks.merge(effects.reads(), throughParameters.reads()), masks.merge(effects.writes(), throughParameters.writes()), effects.flags());
    };
    normalOnCall = withParameters(normal);
    handlerOnCall = normal.mayThrow() ? withParameters(handler) : handler;
}

static constexpr unsigned bitsOfRetainingOperand = 30;
static_assert(static_cast<unsigned>(RetainedIn::UnknownMemory) < 4 && static_cast<unsigned>(RetainedIn::Result) < 4 && static_cast<unsigned>(RetainedIn::Operand) < 4);

StoredParameterSummary StoredParameterSummary::create(CompactEffects memory, const OperandSummary& operand)
{
    RELEASE_ASSERT(operand.retainingOperand < 1u << bitsOfRetainingOperand);
    return { memory, operand.uses.toRaw(), operand.retainingOperand | static_cast<uint32_t>(operand.retainedIn) << bitsOfRetainingOperand };
}

OperandSummary StoredParameterSummary::operand() const
{
    return { OperandUses::fromRaw(uses), static_cast<RetainedIn>(retainedIn >> bitsOfRetainingOperand), retainedIn & ((1u << bitsOfRetainingOperand) - 1) };
}

const ParameterSummary& FunctionSummary::parameter(uint32_t entryParameter) const
{
    static constexpr ParameterSummary unknownParameter { };
    return entryParameter < parameters.size() ? parameters[entryParameter] : unknownParameter;
}

EffectsOnEdges FunctionSummary::onCall() const
{
    EffectsOnEdges effects = elsewhere;
    effects.normal.reads.merge(throughParameters.reads);
    effects.normal.writes.merge(throughParameters.writes);
    if (elsewhere.mayThrow()) {
        effects.handler.reads.merge(throughParameters.reads);
        effects.handler.writes.merge(throughParameters.writes);
    }
    return effects;
}

FunctionSummary FunctionSummary::withoutParameters(const BitVector& removedEntryParameters) const
{
    Vector<uint32_t> newIndexes(parameters.size());
    uint32_t numberOfRemainingParameters = 0;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        newIndexes[index] = numberOfRemainingParameters;
        numberOfRemainingParameters += !removedEntryParameters.get(index);
    }

    FunctionSummary reduced { .elsewhere = elsewhere, .throughParameters = throughParameters, .unknownCodeWitness = unknownCodeWitness };
    reduced.parameters.reserveInitialCapacity(numberOfRemainingParameters);
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (removedEntryParameters.get(index))
            continue;
        ParameterSummary parameter = parameters[index];
        OperandSummary& operand = parameter.operand;
        if (operand.retainedIn == RetainedIn::Operand) {
            bool hasRetainingOperand = operand.retainingOperand < parameters.size() && !removedEntryParameters.get(operand.retainingOperand);
            operand.retainedIn = hasRetainingOperand ? RetainedIn::Operand : RetainedIn::UnknownMemory;
            operand.retainingOperand = hasRetainingOperand ? newIndexes[operand.retainingOperand] : 0;
        }
        reduced.parameters.append(WTF::move(parameter));
    }
    return reduced;
}

EffectsOnEdges FunctionSummaryView::elsewhere() const
{
    if (m_summary)
        return m_summary->elsewhere;
    return { m_masks->expand(m_effects->normal), m_masks->expand(m_effects->handler) };
}

OperandMemory FunctionSummaryView::throughParameters() const
{
    return m_summary ? m_summary->throughParameters : m_masks->expandMemory(m_effects->throughParameters);
}

OperandSummary FunctionSummaryView::operand(uint32_t entryParameter) const
{
    if (m_summary)
        return m_summary->parameter(entryParameter).operand;
    return entryParameter < m_parameters.size() ? m_parameters[entryParameter].operand() : OperandSummary();
}

OperandMemory FunctionSummaryView::memory(uint32_t entryParameter) const
{
    if (m_summary)
        return m_summary->parameter(entryParameter).memory;
    return entryParameter < m_parameters.size() ? m_masks->expandMemory(m_parameters[entryParameter].memory) : OperandMemory();
}

EffectsOnEdges FunctionSummaryView::onCall() const
{
    if (m_summary)
        return m_summary->onCall();
    return { m_masks->expand(m_effects->normalOnCall), m_masks->expand(m_effects->handlerOnCall) };
}

FunctionSummary FunctionSummaryView::expanded() const
{
    if (m_summary)
        return *m_summary;
    FunctionSummary summary { .elsewhere = elsewhere(), .throughParameters = throughParameters(), .unknownCodeWitness = m_witness };
    summary.parameters = Vector<ParameterSummary>(m_parameters.size(), [&](size_t index) {
        return ParameterSummary { m_parameters[index].operand(), m_masks->expandMemory(m_parameters[index].memory) };
    });
    return summary;
}

static StoredFunctionEffects compress(InternedLocationMasks& masks, const FunctionSummary& summary)
{
    StoredFunctionEffects effects { masks.compress(summary.elsewhere.normal), masks.compress(summary.elsewhere.handler), masks.compress(summary.throughParameters), { }, { } };
    effects.computeEffectsOnCall(masks);
    return effects;
}

EffectSummaries::EffectSummaries(Ref<InternedLocationMasks>&& masks, Contents contents)
    : m_masks(WTF::move(masks))
    , m_contents(contents)
    , m_effectsOfUnknownFunction(compress(*m_masks, FunctionSummary()))
{
}

EffectSummaries::EffectSummaries(std::span<const FunctionSummary> summaries, const Effects& unknownCode)
    : EffectSummaries(InternedLocationMasks::create(), Contents::Any)
{
    m_unknownCode = unknownCode;
    store(summaries);
}

EffectSummaries::EffectSummaries(const EffectSummaries& known, FunctionID firstAdded, std::span<const FunctionSummary> added)
    : EffectSummaries(known.m_masks ? Ref { *known.m_masks } : InternedLocationMasks::create(), Contents::Any)
{
    RELEASE_ASSERT(firstAdded && !known.contains(firstAdded));
    m_unknownCode = known.unknownCode();
    m_known = &known;
    m_firstStored = firstAdded.index();
    store(added);
}

void EffectSummaries::store(std::span<const FunctionSummary> summaries)
{
    bool hasWitnesses = std::ranges::any_of(summaries, [](const FunctionSummary& summary) {
        return !!summary.unknownCodeWitness;
    });
    grow(summaries.size());
    if (hasWitnesses)
        m_witnesses.grow(summaries.size());
    for (unsigned index = 0; index < summaries.size(); ++index) {
        setNumberOfParameters(index, summaries[index].parameters.size());
        set(index, summaries[index]);
    }
}

void EffectSummaries::clear()
{
    m_effects.clear();
    m_parameterRanges.clear();
    m_parameters.clear();
    m_witnesses.clear();
}

void EffectSummaries::grow(unsigned size, const ParallelLoops* loops)
{
    RELEASE_ASSERT(size >= m_parameterRanges.size());
    if (size == m_parameterRanges.size())
        return;
    if (loops) {
        if (m_contents == Contents::Any)
            reserveCapacityInParallel(*loops, m_effects, size);
        reserveCapacityInParallel(*loops, m_parameterRanges, size);
    }
    if (m_contents == Contents::Any)
        m_effects.insertFill(m_effects.size(), m_effectsOfUnknownFunction, size - m_effects.size());
    m_parameterRanges.grow(size);
    if (!m_witnesses.isEmpty())
        m_witnesses.grow(size);
}

void EffectSummaries::setNumberOfParameters(unsigned index, uint32_t numberOfParameters)
{
    StoredParameterRange& range = m_parameterRanges[index];
    if (range.size < numberOfParameters) {
        range.first = m_parameters.size();
        m_parameters.grow(m_parameters.size() + numberOfParameters);
    }
    range.size = numberOfParameters;
}

void EffectSummaries::set(unsigned index, const StoredFunctionEffects& effects, UnknownCodeWitness witness)
{
    RELEASE_ASSERT(m_contents == Contents::Any);
    m_effects[index] = effects;
    if (!m_witnesses.isEmpty())
        m_witnesses[index] = witness;
    else
        ASSERT(!witness);
}

void EffectSummaries::set(unsigned index, const FunctionSummary& summary)
{
    std::span<StoredParameterSummary> parameters = parametersOf(index);
    RELEASE_ASSERT(parameters.size() == summary.parameters.size());
    if (m_contents == Contents::SayNothingOfBodies) {
        RELEASE_ASSERT(summary.elsewhere == m_summaryOfUnknownFunction.elsewhere && summary.throughParameters == m_summaryOfUnknownFunction.throughParameters && !summary.unknownCodeWitness);
        for (size_t parameter = 0; parameter < parameters.size(); ++parameter) {
            RELEASE_ASSERT(summary.parameters[parameter].memory == m_summaryOfUnknownFunction.throughParameters);
            parameters[parameter] = StoredParameterSummary::create(m_effectsOfUnknownFunction.throughParameters, summary.parameters[parameter].operand);
        }
        return;
    }
    for (size_t parameter = 0; parameter < parameters.size(); ++parameter)
        parameters[parameter] = StoredParameterSummary::create(m_masks->compress(summary.parameters[parameter].memory), summary.parameters[parameter].operand);
    set(index, compress(*m_masks, summary), summary.unknownCodeWitness);
}

FunctionSummaryView EffectSummaries::of(FunctionID id) const
{
    if (stores(id)) {
        unsigned index = id.index() - m_firstStored;
        StoredParameterRange range = m_parameterRanges[index];
        return FunctionSummaryView(*m_masks, m_effects.isEmpty() ? m_effectsOfUnknownFunction : m_effects[index], m_parameters.subspan(range.first, range.size), m_witnesses.isEmpty() ? UnknownCodeWitness() : m_witnesses[index]);
    }
    return m_known ? m_known->of(id) : FunctionSummaryView(m_summaryOfUnknownFunction);
}

bool EffectSummaries::contains(FunctionID id) const
{
    return stores(id) || (m_known && m_known->contains(id));
}

bool describeByRole(const Program& program, const Function& function, EffectDescription& description)
{
    const BuiltinTable& table = program.builtins();
    switch (function.role) {
    case FunctionRole::Serializer: {
        LayoutID layout = function.creator.as<CreatorRecord::Serializer>().layout;
        if (!layout)
            return false;
        // Its body tests for this layout and does nothing to an object of another one, of the same declared type or not.
        ScratchTypeArena scratch(program.types);
        describeOverload(program, table.overloadIDsOf(Builtins::JSONStringify).at(0), Type::forLayout(layout, scratch), description);
        for (Opcode row : { Opcode::JSONHasStackSpace, Opcode::StringBuilderReserve, Opcode::StringBuilderCommit, Opcode::StringBuilderCheckpoint, Opcode::StringBuilderRollBack, Opcode::StringBuilderAppend, Opcode::JSONAppendString })
            description.addOwn(declaredBoundOf(row));
        return true;
    }
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
        describeOverload(program, table.overloadIDsOf(Builtins::JSONParse).at(0), program.at(TypeTable::string), description);
        for (Opcode row : { Opcode::JSONHasStackSpace, Opcode::JSONParseValue })
            description.addOwn(declaredBoundOf(row));
        return true;
    case FunctionRole::ChainStep:
        description.addFunction(function.derivation.source);
        description.addOwn(declaredBoundOf(Opcode::NewArray));
        return true;
    case FunctionRole::BoundFunction:
        description.addFunction(function.creator.as<CreatorRecord::BoundFunction>().target);
        return true;
    default:
        return false;
    }
}

// A built-in reads whatever its operand reaches, and the table need not say so: nothing moves a store across a call of it.
static FunctionSummary summarizeByRole(const EffectContext& context, const Function& function, const EffectDescription& description)
{
    EffectsOnEdges effects = resolveEffects(context, description);
    effects.normal.reads.merge(LocationMasks { perObjectHeaps });
    if (effects.mayThrow())
        effects.handler.reads.merge(LocationMasks { perObjectHeaps });

    ParameterSummary parameter;
    parameter.memory.writes = effects.onAnyEdge().writes.restrictedTo(perObjectHeaps);
    Vector<ParameterSummary> parameters(function.numberOfEntryParameters(), [&](size_t) {
        return parameter;
    });
    return createFunctionSummary(effects.normal, effects.handler, WTF::move(parameters), { });
}

CalleeSummary::CalleeSummary(const EffectContext& context, FunctionID id)
    : m_computed(context.summaries.of(id))
{
    if (!id || context.summaries.contains(id))
        return;
    const Function& function = context.program.at(id);
    EffectDescription description;
    if (describeByRole(context.program, function, description))
        m_summaryByRole = summarizeByRole(context, function, description);
}

static void dumpParameter(PrintStream& out, unsigned index, const ParameterSummary& parameter)
{
    out.print(" p"_s, index, ": "_s);
    if (parameter == ParameterSummary()) {
        out.print("?"_s);
        return;
    }
    out.print("["_s);
    CommaPrinter comma;
    for (OperandUse use : allMembersOf<OperandUse>) {
        if (parameter.operand.uses.contains(use))
            out.print(comma, nameOf(use));
    }
    out.print("]"_s);
    if (parameter.operand.uses.contains(OperandUse::Retains)) {
        switch (parameter.operand.retainedIn) {
        case RetainedIn::UnknownMemory:
            break;
        case RetainedIn::Result:
            out.print(" in result"_s);
            break;
        case RetainedIn::Operand:
            out.print(" in p"_s, parameter.operand.retainingOperand);
            break;
        }
    }
    if (!parameter.memory.reads.isEmpty())
        out.print(" reads("_s, parameter.memory.reads, ")"_s);
    if (!parameter.memory.writes.isEmpty())
        out.print(" writes("_s, parameter.memory.writes, ")"_s);
}

void EffectSummaries::dump(PrintStream& out, const Program& program) const
{
    out.println("unknown code: "_s, m_unknownCode);
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!program.functions[i].hasBody())
            continue;
        FunctionSummary summary = of(FunctionID(i)).expanded();
        out.print("f"_s, i, " return: "_s, summary.elsewhere.normal, " throw: "_s, summary.elsewhere.handler);
        for (unsigned parameter = 0; parameter < summary.parameters.size(); ++parameter)
            dumpParameter(out, parameter, summary.parameters[parameter]);
        if (summary.unknownCodeWitness) {
            out.print(" unknown code from f"_s, summary.unknownCodeWitness.function.index());
            if (summary.unknownCodeWitness.value)
                out.print(" v"_s, summary.unknownCodeWitness.value.index());
        }
        out.println();
    }
}

// An opaque value may refer to a function, a built-in class may have instances that are called, and the table has its own rule for environments.
static bool isNeverCalled(const Type& type)
{
    return type.isJavaScriptValue() && !type.mayBeFunction() && type.classes.isEmpty();
}

uint64_t parametersWhoseContentsAreCalled(const Program& program, FunctionID id, const BorrowedParameters& borrowedParameters)
{
    static_assert(maxBorrowedParameters <= 64);
    if (!borrowedParameters.wordOf(id))
        return 0;

    const Function& function = program.at(id);
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    uint32_t numberOfTrackedParameters = std::min<uint32_t>(parameters.size(), maxBorrowedParameters);
    uint64_t result = 0;
    for (uint32_t parameter = 0; parameter < numberOfTrackedParameters; ++parameter) {
        if (!borrowedParameters.isBorrowed(id, parameter) || !borrowedParameters.mayBeCalled(id, parameter))
            continue;
        if (isNeverCalled(program.typeOf(function, parameters[parameter])))
            result |= 1ull << parameter;
    }
    return result;
}

EffectSummaries createSummariesFromFlow(const Program& program, const FlowResults& flow, BuildContext& build)
{
    return createSummariesFromFlow(program, flow, computeBorrowedParameters(program, build), build);
}

// Flow names each use, also of what is read through the parameter, as the program was when flow looked at it. The table knows fewer uses, of the program as it is.
// A use of flow that says that what is read through the parameter leaves comes with RetainsContents.
static OperandSummary withoutUsesThatTableExcludes(OperandUses usesByFlow, const BorrowedParameters& borrowedParameters, FunctionID id, uint32_t borrowedParameter)
{
    OperandUses uses = usesByFlow - OperandUses { OperandUse::Retains, OperandUse::Returns, OperandUse::PassesToUserCode };
    if (borrowedParameters.borrowsContents(id, borrowedParameter))
        uses.remove(OperandUse::RetainsContents);
    if (!borrowedParameters.mayBeCalled(id, borrowedParameter))
        uses.remove(callingUses);
    return { uses, RetainedIn::UnknownMemory, 0 };
}

// The table has one answer for a call of a parameter and a call of what is read through it.
// The escape analysis takes a call of a cell that is no closure for a call of unknown code that is handed the cell.
// A call of a value that is no function throws and keeps nothing, so of such a parameter the answer says that the values it holds may be called: they count as retained.
FunctionSummary createSummaryFromFlow(const Program& program, const FlowResults& flow, const BorrowedParameters& borrowedParameters, BuildContext& build, FunctionID id)
{
    bool asksTable = build.isEnabled(FlowResultKind::OperandSummaries, id.index());
    uint64_t contentsAreCalled = parametersWhoseContentsAreCalled(program, id, borrowedParameters);
    FunctionSummary summary;
    summary.parameters = Vector<ParameterSummary>(program.at(id).numberOfEntryParameters());
    for (unsigned parameter = 0; parameter < summary.parameters.size(); ++parameter) {
        OperandSummary& operand = summary.parameters[parameter].operand;
        operand = publishedParameterSummary(flow, id, parameter);
        if (!asksTable || !borrowedParameters.isBorrowed(id, parameter))
            continue;
        operand = operand.uses == OperandUses::all() ? borrowedParameters.summaryOf(id, parameter) : withoutUsesThatTableExcludes(operand.uses, borrowedParameters, id, parameter);
        if (contentsAreCalled >> parameter & 1) {
            operand.uses.remove(callingUses);
            operand.uses.add(OperandUse::RetainsContents);
        }
    }
    return summary;
}

EffectSummaries createSummariesFromFlow(const Program& program, const FlowResults& flow, const BorrowedParameters& borrowedParameters, BuildContext& build)
{
    unsigned numberOfFunctions = program.functions.size();
    EffectSummaries summaries(InternedLocationMasks::create(), EffectSummaries::Contents::SayNothingOfBodies);
    summaries.grow(numberOfFunctions, &build.parallelLoops());
    Vector<uint32_t> numbersOfParameters(numberOfFunctions);
    forEachIndexInParallel(build.parallelLoops(), numberOfFunctions, [&](unsigned index) {
        numbersOfParameters[index] = program.functions[index].numberOfEntryParameters();
    });
    summaries.setParameterCounts([&](unsigned index) {
        return numbersOfParameters[index];
    }, &build.parallelLoops());
    forEachIndexInParallel(build.parallelLoops(), numberOfFunctions, [&](unsigned index) {
        summaries.set(index, createSummaryFromFlow(program, flow, borrowedParameters, build, FunctionID(index)));
    });
    return summaries;
}

EffectSummaries computeEffectSummaries(const Program& program, const FlowResults& flow, BuildContext& build)
{
    BodySummaries nothingKept;
    EffectSummaries summaries;
    EffectSummarizer(program, flow, build, nothingKept, EffectSummarizer::Witnesses::Found).update(summaries);
    return summaries;
}

void updateEffectSummaries(const Program& program, const FlowResults& flow, BuildContext& build, BodySummaries& kept, EffectSummaries& summaries, BodyReferences* references)
{
    EffectSummarizer summarizer(program, flow, build, kept, EffectSummarizer::Witnesses::Omitted);
    summarizer.giveReferencesTo(references);
    summarizer.update(summaries);
}

FunctionSummary createFunctionSummary(Effects onReturn, Effects onThrow, Vector<ParameterSummary>&& parameters, UnknownCodeWitness witness)
{
    onReturn = onReturn.withoutSuspension();
    onThrow = onThrow.withoutSuspension();

    ThrowEffect mayThrow = onReturn.mayThrow;
    mayThrow |= onThrow.mayThrow;
    bool mayCollect = onReturn.mayCollect || onThrow.mayCollect;
    bool mayBeTerminated = onReturn.mayBeTerminated || onThrow.mayBeTerminated;
    for (Effects* effects : { &onReturn, &onThrow }) {
        effects->mayThrow = mayThrow;
        effects->mayCollect = mayCollect;
        effects->mayBeTerminated = mayBeTerminated;
    }
    if (!mayThrow)
        onThrow = Effects::none();

    FunctionSummary summary;
    summary.elsewhere = { onReturn, onThrow };
    summary.throughParameters = OperandMemory::none();
    for (const ParameterSummary& parameter : parameters)
        summary.throughParameters.merge(parameter.memory);
    summary.parameters = WTF::move(parameters);
    if (onReturn.mayCallUnknownCode || onThrow.mayCallUnknownCode)
        summary.unknownCodeWitness = witness;
    return summary;
}

static Effects effectsOfMemory(const OperandMemory& memory)
{
    Effects effects = Effects::none();
    effects.reads = memory.reads;
    effects.writes = memory.writes;
    return effects;
}

static Effects valueOf(const EffectContext& context, const SummaryNode& node)
{
    switch (node.part) {
    case SummaryPart::OnReturn:
        return context.summaries.of(node.function).elsewhere().normal;
    case SummaryPart::OnThrow:
        return context.summaries.of(node.function).elsewhere().handler;
    case SummaryPart::Parameter:
        return effectsOfMemory(context.summaries.of(node.function).memory(node.parameter));
    case SummaryPart::UnknownCode:
        return context.summaries.unknownCode();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Effects valueOf(const BodySummary& body, const SummaryNode& node)
{
    switch (node.part) {
    case SummaryPart::OnReturn:
        return body.onReturn;
    case SummaryPart::OnThrow:
        return body.onThrow;
    case SummaryPart::Parameter:
        return effectsOfMemory(body.parameters[node.parameter].memory);
    case SummaryPart::UnknownCode:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mergeInto(BodySummary& body, const SummaryNode& target, const Effects& source)
{
    switch (target.part) {
    case SummaryPart::OnReturn:
        return body.onReturn.merge(source);
    case SummaryPart::OnThrow:
        return body.onThrow.merge(source);
    case SummaryPart::Parameter: {
        OperandMemory& memory = body.parameters[target.parameter].memory;
        bool changed = memory.reads.merge(source.reads.restrictedTo(perObjectHeaps));
        changed |= memory.writes.merge(source.writes.restrictedTo(perObjectHeaps));
        return changed;
    }
    case SummaryPart::UnknownCode:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnknownCodeWitness witnessOf(const EffectContext& context, FunctionID callee)
{
    UnknownCodeWitness witness = context.summaries.of(callee).unknownCodeWitness();
    return witness ? witness : UnknownCodeWitness { callee, ValueID() };
}

static bool isRecursiveCall(const SummaryDependency& ownDependency)
{
    return ownDependency.source.part == SummaryPart::OnReturn;
}

FunctionSummary summarizeBody(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses)
{
    ASSERT(id);
    BodySummary body = summarizeBodyLocally(context, id, analyses);

    Vector<SummaryDependency, 4> ownDependencies;
    for (const SummaryDependency& dependency : body.dependencies) {
        ASSERT(dependency.target.function == id);
        if (dependency.source.function == id) {
            ownDependencies.append(dependency);
            continue;
        }
        Effects source = valueOf(context, dependency.source);
        mergeInto(body, dependency.target, source);
        if (source.mayCallUnknownCode && !body.unknownCodeWitness)
            body.unknownCodeWitness = witnessOf(context, dependency.source.function);
    }

    for (const SummaryDependency& dependency : ownDependencies) {
        if (isRecursiveCall(dependency))
            body.onReturn.mayThrow = true;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const SummaryDependency& dependency : ownDependencies)
            changed |= mergeInto(body, dependency.target, valueOf(body, dependency.source));
    }

    return createFunctionSummary(body.onReturn, body.onThrow, WTF::move(body.parameters), body.unknownCodeWitness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
