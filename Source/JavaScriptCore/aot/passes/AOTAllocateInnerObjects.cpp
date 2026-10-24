/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocateInnerObjects.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTCallGraph.h"
#include "AOTConvention.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRProgram.h"
#include "AOTSiteWeight.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

bool mayShareCell(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::NewObject || value.storage() != AllocationStorage::Heap || value.prototypeOperand())
        return false;
    // What Object.create makes is handed to the run time before it is whole, for what lies above it.
    if (program.at(value.layout()).has(LayoutFlags::IsOfCreatedObject))
        return false;
    LayoutKind kind = program.at(value.layout()).kind;
    return kind == LayoutKind::Record || kind == LayoutKind::Instance;
}

// A short literal without holes. Its cell and its first storage get room in the cell of the object that lists it.
AOT_THRESHOLD(unsigned, maxLengthOfInnerArray, 7);

bool isArrayThatMayBeInnerCell(const Value& value)
{
    return value.opcode == Opcode::NewArray && value.storage() == AllocationStorage::Heap && value.holes().empty() && !value.capacityOperand() && value.elementsOperands().size() <= maxLengthOfInnerArray;
}

bool hasAllocationThatMayShareCell(const Program& program, FunctionID function)
{
    return anyListedValue(program, function, { Opcode::NewObject }, [&](ValueID, const Value& value) {
        return mayShareCell(program, value);
    });
}

// Each time the object is made at most one of the blocks may run: they lie in the arms of a branch, and none of them in a loop that the object is made outside of.
bool runsAtMostOneOf(const CFG& cfg, BlockID blockOfObject, std::span<const BlockID> blocks)
{
    BitVector isListed(cfg.numNodes());
    for (BlockID block : blocks) {
        if (isListed.set(block.index()))
            return false;
    }
    BitVector untilMadeAgain(cfg.numNodes());
    untilMadeAgain.set(blockOfObject.index());
    for (BlockID block : blocks) {
        for (BlockID successor : cfg.successors(block)) {
            if (successor == blockOfObject)
                continue;
            BitVector reached = cfg.blocksReachableFrom(successor, untilMadeAgain);
            reached.filter(isListed);
            if (!reached.isEmpty())
                return false;
        }
    }
    return true;
}

// A result of a function that is, at every return, an object that this activation has made and that no allocation of the function has listed on the way there.
struct FreshResult {
    FunctionID function;
    uint32_t index { 0 };
    LayoutID layout;
    Vector<ValueID, 2> objects;
};

class FreshResultFinder {
public:
    FreshResultFinder(const Program& program, FunctionID id)
        : m_program(program)
        , m_id(id)
        , m_function(program.at(id))
    {
    }

    void run(Vector<FreshResult>&);

private:
    struct Return {
        BlockID block;
        ValueID object;
    };
    struct Candidate {
        bool isRefuted { false };
        LayoutID layout;
        Vector<ValueID, 2> objects;
        Vector<Return, 2> returns;
    };

    bool returnsAllocationThatMayShareCell() const;
    void addReturn(BlockID, uint32_t index, ValueID);
    bool isListedOnlyOnOtherPaths(const CFG&, const Candidate&) const;

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    Vector<Candidate, 4> m_candidates;
};

// In some block, reachable or not. Where this is false, addReturn() finds no object.
bool FreshResultFinder::returnsAllocationThatMayShareCell() const
{
    auto mayShare = [&](ValueID returned) {
        return mayShareCell(m_program, m_function.at(sameObjectThroughCasts(m_function, returned)));
    };
    for (const Block& block : m_function.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = block.terminator.valueOperand();
        if (!returned)
            continue;
        const Value& value = m_function.at(returned);
        if (value.opcode != Opcode::Results) {
            if (mayShare(returned))
                return true;
            continue;
        }
        for (ValueID result : value.valuesOperands()) {
            if (mayShare(result))
                return true;
        }
    }
    return false;
}

void FreshResultFinder::addReturn(BlockID block, uint32_t index, ValueID returned)
{
    Candidate& candidate = m_candidates[index];
    ValueID object = sameObjectThroughCasts(m_function, returned);
    const Value& value = m_function.at(object);
    if (value.opcode == Opcode::Unspecified)
        return;
    if (!mayShareCell(m_program, value) || (candidate.layout && candidate.layout != value.layout())) {
        candidate.isRefuted = true;
        return;
    }
    candidate.layout = value.layout();
    if (!candidate.objects.contains(object))
        candidate.objects.append(object);
    candidate.returns.append({ block, object });
}

bool FreshResultFinder::isListedOnlyOnOtherPaths(const CFG& cfg, const Candidate& candidate) const
{
    for (ValueID object : candidate.objects) {
        BlockID blockOfObject;
        Vector<BlockID, 4> holders;
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (id == object)
                    blockOfObject = block;
                const Value& value = m_function.at(id);
                if (value.opcode != Opcode::NewObject)
                    continue;
                for (ValueID listed : value.valuesOperands()) {
                    if (sameObjectThroughCasts(m_function, listed) == object)
                        holders.append(block);
                }
            }
        }
        for (const Return& returned : candidate.returns) {
            if (returned.object == object)
                holders.append(returned.block);
        }
        if (!blockOfObject || !runsAtMostOneOf(cfg, blockOfObject, holders.span()))
            return false;
    }
    return true;
}

void FreshResultFinder::run(Vector<FreshResult>& found)
{
    if (m_function.results.isEmpty() || !hasAllocationThatMayShareCell(m_program, m_id) || !returnsAllocationThatMayShareCell())
        return;
    m_candidates.grow(m_function.results.size());
    CFG cfg(m_function);
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return;
        if (terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = terminator.valueOperand();
        if (!returned)
            return;
        const Value& value = m_function.at(returned);
        if (value.opcode != Opcode::Results) {
            if (m_candidates.size() != 1)
                return;
            addReturn(block, 0, returned);
            continue;
        }
        auto values = value.valuesOperands();
        if (values.size() != m_candidates.size())
            return;
        // An object that is two results at once would have two holders.
        for (uint32_t index = 0; index < values.size(); ++index) {
            for (uint32_t other = 0; other < index; ++other) {
                if (sameObjectThroughCasts(m_function, values[index]) == sameObjectThroughCasts(m_function, values[other]) && m_function.at(sameObjectThroughCasts(m_function, values[index])).opcode == Opcode::NewObject)
                    return;
            }
            addReturn(block, index, values[index]);
        }
    }
    for (uint32_t index = 0; index < m_candidates.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        if (candidate.isRefuted || candidate.objects.isEmpty() || !isListedOnlyOnOtherPaths(cfg, candidate))
            continue;
        found.append({ m_id, index, candidate.layout, WTF::move(candidate.objects) });
    }
}

class FreshResults {
public:
    explicit FreshResults(Vector<FreshResult>&& results)
        : m_results(WTF::move(results))
    {
        for (const FreshResult& result : m_results)
            m_functions.set(result.function.index());
    }

    bool hasResultOf(FunctionID function) const { return m_functions.get(function.index()); }

    // They are in the order of the functions, and of the results of each.
    const FreshResult* find(FunctionID function, uint32_t index) const
    {
        auto keyOf = [](const FreshResult& result) {
            return std::pair { result.function.index(), result.index };
        };
        auto position = std::ranges::lower_bound(m_results, std::pair { function.index(), index }, { }, keyOf);
        return position != m_results.end() && keyOf(*position) == std::pair { function.index(), index } ? &*position : nullptr;
    }

private:
    Vector<FreshResult> m_results;
    BitVector m_functions;
};

// How often a function with fresh results is taken to be called from one place. The heat of the caller says it: that of the callee is from before calls of it were inlined.
struct CallOfFreshResults {
    FunctionID callee;
    double weight { 0 };
};

// A field that the allocation fills with an object that was made for this allocation alone, by the same function or by one that it has called.
struct FreshField {
    uint32_t field { 0 };
    ValueID object;
    // None for an array, which has a capacity.
    LayoutID layout;
    uint32_t arrayCapacity { 0 };
    const FreshResult* result { nullptr };
    // How often the allocation is taken to run, for an object that the function makes itself.
    double weightOfAllocation { 0 };
};

struct InnerObjectBirth {
    FunctionID function;
    ValueID value;
    BlockID block;
    LayoutID layout;
    // How often it is taken to run, where it has found a fresh field.
    double weight { 0 };
    Vector<FreshField, 1> freshFields;
};

class BirthFinder {
public:
    BirthFinder(const Program& program, const FlowResults& flow, const CallGraph& callGraph, const Heat& heat, const FreshResults& freshResults, BuildContext& build, FunctionID id)
        : m_program(program)
        , m_flow(flow)
        , m_callGraph(callGraph)
        , m_heat(heat)
        , m_freshResults(freshResults)
        , m_build(build)
        , m_id(id)
        , m_function(program.at(id))
    {
    }

    void run(Vector<InnerObjectBirth>&, Vector<CallOfFreshResults>&);

private:
    struct FreshObject {
        // Of the operation that makes the object: an allocation, or a call.
        BlockID block;
        // Two reads of one result of one call are one object.
        ValueID firstName;
        const FreshResult* result { nullptr };
    };
    struct Holder {
        uint32_t birth { 0 };
        uint32_t freshField { 0 };
    };

    bool callsFunctionWithFreshResults() const;
    void findCalls(const CFG&, const FunctionSiteWeights&, Vector<CallOfFreshResults>&) const;
    void findFreshObjects(const CFG&);
    void findBirths(FunctionAnalyses&, const FunctionSiteWeights&);
    bool runsNoCodeBetween(FunctionAnalyses&, BlockID, ValueID first, ValueID last) const;
    void removeFieldsOfObjectsHeldTwice(const CFG&);

    const Program& m_program;
    const FlowResults& m_flow;
    const CallGraph& m_callGraph;
    const Heat& m_heat;
    const FreshResults& m_freshResults;
    BuildContext& m_build;
    FunctionID m_id;
    const Function& m_function;
    Vector<InnerObjectBirth> m_births;
    Vector<FreshObject> m_freshObjects;
};

void BirthFinder::run(Vector<InnerObjectBirth>& births, Vector<CallOfFreshResults>& calls)
{
    bool hasAllocation = hasAllocationThatMayShareCell(m_program, m_id);
    bool hasCall = callsFunctionWithFreshResults();
    if (!hasAllocation && !hasCall)
        return;
    FunctionAnalyses analyses({ m_build, m_program, &m_flow, nullptr, m_id }, m_function);
    const CFG& cfg = analyses.cfg();
    FunctionSiteWeights weights(m_program, m_heat, m_id, analyses);
    if (hasCall)
        findCalls(cfg, weights, calls);
    if (!hasAllocation)
        return;
    findFreshObjects(cfg);
    findBirths(analyses, weights);
    removeFieldsOfObjectsHeldTwice(cfg);
    births.appendVector(WTF::move(m_births));
}

bool BirthFinder::callsFunctionWithFreshResults() const
{
    return std::ranges::any_of(m_callGraph.calleesOf(m_id), [&](FunctionID callee) {
        return m_freshResults.hasResultOf(callee);
    });
}

void BirthFinder::findCalls(const CFG& cfg, const FunctionSiteWeights& weights, Vector<CallOfFreshResults>& calls) const
{
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::CallDirect && m_freshResults.hasResultOf(value.function()))
                calls.append({ value.function(), weights.heatOf(block) });
        }
    }
}

void BirthFinder::findFreshObjects(const CFG& cfg)
{
    struct ReadResult {
        ValueID call;
        uint32_t index;
        ValueID firstName;
    };
    Vector<ReadResult, 8> readResults;
    Vector<BlockID> blockOfCall(m_function.values.size());
    m_freshObjects.grow(m_function.values.size());
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            FreshObject& fresh = m_freshObjects[id.index()];
            switch (value.opcode) {
            case Opcode::NewObject:
                if (mayShareCell(m_program, value))
                    fresh = { block, id, nullptr };
                break;
            case Opcode::NewArray:
                if (isArrayThatMayBeInnerCell(value))
                    fresh = { block, id, nullptr };
                break;
            case Opcode::CallDirect:
                blockOfCall[id.index()] = block;
                if (m_program.at(value.function()).results.size() != 1)
                    break;
                if (const FreshResult* result = m_freshResults.find(value.function(), 0))
                    fresh = { block, id, result };
                break;
            case Opcode::ResultAt: {
                ValueID call = value.resultsOperand();
                if (m_function.at(call).opcode != Opcode::CallDirect || !blockOfCall[call.index()])
                    break;
                const FreshResult* result = m_freshResults.find(m_function.at(call).function(), value.index());
                if (!result)
                    break;
                size_t position = readResults.findIf([&](const ReadResult& read) {
                    return read.call == call && read.index == value.index();
                });
                if (position == notFound) {
                    position = readResults.size();
                    readResults.append({ call, value.index(), id });
                }
                fresh = { blockOfCall[call.index()], readResults[position].firstName, result };
                break;
            }
            default:
                break;
            }
        }
    }
}

// Allocations aside, which run no code of the program.
bool BirthFinder::runsNoCodeBetween(FunctionAnalyses& analyses, BlockID block, ValueID first, ValueID last) const
{
    constexpr OpcodeSet allocations { Opcode::NewObject, Opcode::NewArray, Opcode::NewClosure };
    bool isBetween = false;
    for (ValueID id : m_function.at(block).values) {
        if (id == last)
            return isBetween;
        if (isBetween && !allocations.contains(m_function.at(id).opcode)) {
            EffectsOnEdges effects = analyses.effectsOf(id);
            if (effects.normal.mayCollect || effects.normal.mayRunOtherCode() || effects.handler.mayCollect || effects.handler.mayRunOtherCode())
                return false;
        }
        isBetween |= id == first;
    }
    return false;
}

void BirthFinder::findBirths(FunctionAnalyses& analyses, const FunctionSiteWeights& weights)
{
    const CFG& cfg = analyses.cfg();
    bool logsDecisions = m_build.options().logging.dumpedPass == PassID::AllocateInnerObjects;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (!mayShareCell(m_program, value))
                continue;
            InnerObjectBirth birth { m_id, id, block, value.layout(), 0, { } };
            auto fields = value.fields();
            auto values = value.valuesOperands();
            for (uint32_t index = 0; index < fields.size(); ++index) {
                const FreshObject& fresh = m_freshObjects[sameObjectThroughCasts(m_function, values[index]).index()];
                if (!fresh.block)
                    continue;
                // The operation of the inner object allocates the outer one, as the class of the outer one wants it. So the class has to be defined by then: nothing may define it in between.
                bool isConstructed = m_program.at(value.layout()).kind == LayoutKind::Instance;
                if (isConstructed && (fresh.result || fresh.block != block || !runsNoCodeBetween(analyses, block, fresh.firstName, id)))
                    continue;
                const Value& made = m_function.at(fresh.firstName);
                if (made.opcode == Opcode::NewArray) {
                    birth.freshFields.append({ fields[index], fresh.firstName, { }, arrayLiteralVectorLength(made.elementsOperands().size()), nullptr, weights.heatOf(fresh.block) });
                    continue;
                }
                LayoutID layout = fresh.result ? fresh.result->layout : made.layout();
                if (layout != value.layout())
                    birth.freshFields.append({ fields[index], fresh.firstName, layout, 0, fresh.result, weights.heatOf(fresh.block) });
            }
            // Only the log adds up the weights of births that offer no field.
            if (!birth.freshFields.isEmpty() || logsDecisions)
                birth.weight = weights.heatOf(block);
            m_births.append(WTF::move(birth));
        }
    }
}

// The object has room in front of it for one outer object.
void BirthFinder::removeFieldsOfObjectsHeldTwice(const CFG& cfg)
{
    struct Held {
        ValueID object;
        LayoutID outerLayout;
        uint32_t field;
        Holder holder;
    };
    Vector<Held, 8> held;
    for (uint32_t birth = 0; birth < m_births.size(); ++birth) {
        for (uint32_t index = 0; index < m_births[birth].freshFields.size(); ++index) {
            const FreshField& fresh = m_births[birth].freshFields[index];
            held.append({ fresh.object, m_births[birth].layout, fresh.field, { birth, index } });
        }
    }
    if (held.isEmpty())
        return;
    auto keyOf = [](const Held& entry) {
        return std::tuple { entry.object.index(), entry.outerLayout.index(), entry.field };
    };
    AOT::stableSort(held, { }, keyOf);

    for (size_t begin = 0; begin < held.size();) {
        size_t end = begin + 1;
        while (end < held.size() && keyOf(held[end]) == keyOf(held[begin]))
            ++end;
        Vector<BlockID, 4> blocks;
        for (size_t index = begin; index < end; ++index)
            blocks.append(m_births[held[index].holder.birth].block);
        if (!runsAtMostOneOf(cfg, m_freshObjects[held[begin].object.index()].block, blocks.span())) {
            for (size_t index = begin; index < end; ++index)
                m_births[held[index].holder.birth].freshFields[held[index].holder.freshField].object = ValueID();
        }
        begin = end;
    }
    for (InnerObjectBirth& birth : m_births) {
        birth.freshFields.removeAllMatching([](const FreshField& fresh) {
            return !fresh.object;
        });
    }
}

// Which outer layout the allocation of an inner object makes room for.
class Promises {
public:
    bool canPromise(const InnerObjectBirth& birth, const FreshField& fresh) const
    {
        bool result = true;
        forEachAllocation(birth, fresh, [&](FunctionID function, ValueID object) {
            auto promised = m_outerLayouts.find(keyOf(function, object));
            result &= promised == m_outerLayouts.end() || promised->value == birth.layout.index();
        });
        return result;
    }

    void promise(const InnerObjectBirth& birth, const FreshField& fresh)
    {
        forEachAllocation(birth, fresh, [&](FunctionID function, ValueID object) {
            m_outerLayouts.add(keyOf(function, object), birth.layout.index());
        });
    }

    // One number for all the allocations that make the object of the field.
    static uint64_t firstAllocation(const InnerObjectBirth& birth, const FreshField& fresh)
    {
        return fresh.result ? keyOf(fresh.result->function, fresh.result->objects.first()) : keyOf(birth.function, fresh.object);
    }

    static void forEachAllocation(const InnerObjectBirth& birth, const FreshField& fresh, const Invocable<void(FunctionID, ValueID)> auto& functor)
    {
        if (!fresh.result) {
            functor(birth.function, fresh.object);
            return;
        }
        for (ValueID object : fresh.result->objects)
            functor(fresh.result->function, object);
    }

private:
    static uint64_t keyOf(FunctionID function, ValueID object) { return (static_cast<uint64_t>(function.index()) << 32 | object.index()) + 1; }

    UncheckedKeyHashMap<uint64_t, uint32_t> m_outerLayouts;
};

const FreshField* fieldFilledAtBirth(const Program& program, const Promises& promises, const InnerObjectBirth& birth, const InnerObject& inner)
{
    const Value& value = program.at(birth.function).at(birth.value);
    if (value.allocationClass() != inner.outerAllocationClass || !std::ranges::equal(value.fields(), inner.outerFields))
        return nullptr;
    for (const FreshField& fresh : birth.freshFields) {
        if (fresh.field == inner.field && fresh.layout == inner.layout && fresh.arrayCapacity == inner.arrayCapacity)
            return promises.canPromise(birth, fresh) ? &fresh : nullptr;
    }
    return nullptr;
}

// The total weight of the births that fieldFilledAtBirth() accepts, for every inner object that a birth of the layout could propose.
class InnerObjectWeights {
public:
    InnerObjectWeights(const Program&, const Promises&, std::span<const InnerObjectBirth>);

    double weightOf(size_t birthIndex, const FreshField& candidate) const
    {
        uint32_t group = m_birthGroups[birthIndex];
        for (const WeightedField& field : m_fields.subspan(m_groupOffsets[group], m_groupOffsets[group + 1] - m_groupOffsets[group])) {
            if (field.matches(candidate))
                return field.weight;
        }
        return 0;
    }

private:
    struct WeightedField {
        bool matches(const FreshField& fresh) const { return field == fresh.field && layout == fresh.layout && arrayCapacity == fresh.arrayCapacity; }

        uint32_t field { 0 };
        LayoutID layout;
        uint32_t arrayCapacity { 0 };
        double weight { 0 };
    };

    void addBirth(const Promises&, const InnerObjectBirth&, size_t groupOffset);

    // Births with the same allocation class and the same field list are one group.
    Vector<uint32_t, 16> m_birthGroups;
    Vector<uint32_t, 16> m_groupOffsets;
    Vector<WeightedField, 16> m_fields;
};

InnerObjectWeights::InnerObjectWeights(const Program& program, const Promises& promises, std::span<const InnerObjectBirth> births)
    : m_birthGroups(births.size())
{
    auto allocationAt = [&](uint32_t index) -> const Value& {
        return program.at(births[index].function).at(births[index].value);
    };
    Vector<uint32_t, 16> sortedIndices(births.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(sortedIndices, [&](uint32_t a, uint32_t b) {
        const Value& first = allocationAt(a);
        const Value& second = allocationAt(b);
        if (first.allocationClass() != second.allocationClass())
            return first.allocationClass() < second.allocationClass();
        if (!std::ranges::equal(first.fields(), second.fields()))
            return std::ranges::lexicographical_compare(first.fields(), second.fields());
        return a < b;
    });
    for (size_t begin = 0; begin < sortedIndices.size();) {
        const Value& first = allocationAt(sortedIndices[begin]);
        size_t end = begin + 1;
        while (end < sortedIndices.size() && allocationAt(sortedIndices[end]).allocationClass() == first.allocationClass() && std::ranges::equal(allocationAt(sortedIndices[end]).fields(), first.fields()))
            ++end;
        size_t groupOffset = m_fields.size();
        // Weights are floating-point sums, so the births of a group are added in list order.
        for (uint32_t index : sortedIndices.subspan(begin, end - begin)) {
            m_birthGroups[index] = m_groupOffsets.size();
            addBirth(promises, births[index], groupOffset);
        }
        m_groupOffsets.append(groupOffset);
        begin = end;
    }
    m_groupOffsets.append(m_fields.size());
}

void InnerObjectWeights::addBirth(const Promises& promises, const InnerObjectBirth& birth, size_t groupOffset)
{
    for (size_t index = 0; index < birth.freshFields.size(); ++index) {
        const FreshField& fresh = birth.freshFields[index];
        WeightedField field { fresh.field, fresh.layout, fresh.arrayCapacity, 0 };
        bool isDuplicate = std::ranges::any_of(birth.freshFields.subspan(0, index), [&](const FreshField& earlier) {
            return field.matches(earlier);
        });
        if (isDuplicate || !promises.canPromise(birth, fresh))
            continue;
        size_t position = groupOffset;
        while (position < m_fields.size() && !m_fields[position].matches(fresh))
            ++position;
        if (position == m_fields.size())
            m_fields.append(field);
        m_fields[position].weight += birth.weight;
    }
}

// The layouts of which something but a NewObject on the heap may make an object, or into whose objects something copies the fields of another.
BitVector findLayoutsMadeOtherwise(const Program& program, BuildContext& build)
{
    constexpr OpcodeSet namesLayoutWithoutMakingObjects {
        Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::MaterializeEnvironment, Opcode::NewClass, Opcode::GetClassPrototype, Opcode::DefineMethod, Opcode::FinishClass,
        Opcode::NewClassToken, Opcode::GetConstructPrototype, Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas, Opcode::HasKeyOrder
    };
    OpcodeSet mayMakeObjects;
    Value::opcodesWithLayout.forEach([&](Opcode opcode) {
        if (!namesLayoutWithoutMakingObjects.contains(opcode))
            mayMakeObjects.add(opcode);
    });
    OpcodeSet mayMakeOrCopyObjects = mayMakeObjects | OpcodeSet { Opcode::CopyObject, Opcode::AssumeType };
    Vector<uint32_t> found = collectInParallel<uint32_t>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& layouts) {
        const Function& function = program.functions[index];
        // A cast narrows a copy only in a function that makes one.
        bool mayCopy = mayHoldValueWith(program, FunctionID(index), { Opcode::CopyObject });
        forEachListedValue(program, FunctionID(index), mayCopy ? mayMakeOrCopyObjects : mayMakeObjects, [&](BlockID, uint32_t, ValueID id, const Value& value) {
            // The copy of a record has the layout of the record, a cell of its own, and in its fields what the fields of the record hold.
            bool narrowsCopy = value.opcode == Opcode::AssumeType && function.at(value.valueOperand()).opcode == Opcode::CopyObject;
            if (value.opcode == Opcode::CopyObject || narrowsCopy) {
                if (LayoutSetID copied = program.typeOf(function, narrowsCopy ? id : value.sourceOperand()).layouts) {
                    for (LayoutID layout : program.at(copied).members)
                        layouts.append(layout.index());
                }
                return;
            }
            if (!Value::opcodesWithLayout.contains(value.opcode) || namesLayoutWithoutMakingObjects.contains(value.opcode) || mayShareCell(program, value))
                return;
            layouts.append(value.layout().index());
        });
    });
    BitVector result(program.layouts.size());
    for (uint32_t layout : found)
        result.set(layout);
    for (const ExpectedResult& expected : program.expectedResults) {
        for (LayoutID layout : expected.layouts)
            result.set(layout.index());
    }
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            if (node.kind() == DataNode::Kind::Object && node.as<DataNode::Object>().layout)
                result.set(node.as<DataNode::Object>().layout.index());
        });
    }
    return result;
}

// Nothing puts a value into the field but the operation that allocates the object, and nobody whom the compiler does not see holds such an object.
bool isStoredOnlyAtBirth(const Program& program, FieldReference field)
{
    const FieldFlow& flow = flowOf(program, field);
    const LayoutFlow& layout = program.at(field.layout).flow;
    return flow.location.writers == WriterState::AllKnown && flow.location.stores == StoreCount::AtMostOnce && flow.location.initialization == InitializationState::SetAtCreation
        && flow.stability == everyFlagOf<FieldStability>() && layout.impossibleMutations.containsAll({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript }) && layout.observedUses.isEmpty();
}

} // anonymous namespace

void allocateInnerObjects(ProgramEditor& editor, const FlowResults& flow, const CallGraph& callGraph, const Heat& heat, BuildContext& build)
{
    const Program& program = editor.program();
    auto isEnabled = [&](unsigned index) {
        return program.functions[index].hasBody() && build.isEnabled(PassID::AllocateInnerObjects, FunctionID(index));
    };
    FreshResults freshResults(collectInParallel<FreshResult>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FreshResult>& found) {
        if (isEnabled(index))
            FreshResultFinder(program, FunctionID(index)).run(found);
    }));
    struct Found {
        Vector<InnerObjectBirth> births;
        Vector<CallOfFreshResults> calls;
    };
    Vector<Found> foundInFunctions = collectInParallel<Found>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Found>& found) {
        if (!program.functions[index].hasBody())
            return;
        Found inFunction;
        BirthFinder(program, flow, callGraph, heat, freshResults, build, FunctionID(index)).run(inFunction.births, inFunction.calls);
        if (!isEnabled(index))
            inFunction.births.clear();
        if (!inFunction.births.isEmpty() || !inFunction.calls.isEmpty())
            found.append(WTF::move(inFunction));
    });
    // A stable counting sort by layout.
    Vector<uint32_t> positions(FillWith { }, program.layouts.size() + 1, 0);
    for (const Found& found : foundInFunctions) {
        for (const InnerObjectBirth& birth : found.births)
            ++positions[birth.layout.index() + 1];
    }
    for (size_t layout = 0; layout < program.layouts.size(); ++layout)
        positions[layout + 1] += positions[layout];
    Vector<InnerObjectBirth> births(positions.last());
    struct CallsOfFunction {
        unsigned number { 0 };
        double weight { 0 };
    };
    UncheckedKeyHashMap<uint32_t, CallsOfFunction> callsOfFunctions;
    for (Found& found : foundInFunctions) {
        for (InnerObjectBirth& birth : found.births)
            births[positions[birth.layout.index()]++] = WTF::move(birth);
        for (const CallOfFreshResults& call : found.calls) {
            CallsOfFunction& calls = callsOfFunctions.add(call.callee.index() + 1, CallsOfFunction { }).iterator->value;
            ++calls.number;
            calls.weight += call.weight;
        }
    }
    // Every call of the function makes the object, so every call has to be one that was seen.
    auto weightOfObject = [&](const FreshField& fresh) {
        if (!fresh.result)
            return fresh.weightOfAllocation;
        FunctionID callee = fresh.result->function;
        CallsOfFunction calls = callsOfFunctions.get(callee.index() + 1);
        bool everyCallWasSeen = callGraph.entrySet(callee) == EntrySet::DirectCallers && callGraph.numberOfCallSites(callee) == calls.number;
        return everyCallWasSeen ? calls.weight : std::numeric_limits<double>::infinity();
    };

    // An allocation that has no fresh object to offer asks for a cell without the room. A layout is outer or inner, not both, and has one inner object: the one that is made most often.
    bool logsDecisions = build.options().logging.dumpedPass == PassID::AllocateInnerObjects;
    BitVector isOuter(program.layouts.size());
    BitVector isInner(program.layouts.size());
    Promises promises;
    std::optional<BitVector> layoutsMadeOtherwise;
    uint64_t numberOfInnerObjects = 0;
    uint64_t numberOfInnerArrays = 0;
    uint64_t numberAlwaysInPlace = 0;
    for (size_t begin = 0; begin < births.size();) {
        size_t end = begin + 1;
        while (end < births.size() && births[end].layout == births[begin].layout)
            ++end;
        std::span<const InnerObjectBirth> ofLayout = births.span().subspan(begin, end - begin);
        begin = end;

        LayoutID layout = ofLayout.front().layout;
        if (isInner.get(layout.index()))
            continue;
        bool hasFreshField = std::ranges::any_of(ofLayout, [](const InnerObjectBirth& birth) {
            return !birth.freshFields.isEmpty();
        });
        if (!hasFreshField)
            continue;
        // All the allocations that fill the room are born alike: they list the same fields. The likeliest of those that offer a field says which.
        InnerObjectWeights weights(program, promises, ofLayout);
        std::optional<InnerObject> best;
        double weightOfBest = 0;
        for (size_t model = 0; model < ofLayout.size(); ++model) {
            for (const FreshField& candidate : ofLayout[model].freshFields) {
                if (candidate.layout && isOuter.get(candidate.layout.index()))
                    continue;
                double weight = weights.weightOf(model, candidate);
                if (weight > weightOfBest) {
                    const Value& value = program.at(ofLayout[model].function).at(ofLayout[model].value);
                    best = InnerObject { candidate.field, candidate.layout, candidate.arrayCapacity, Vector<uint32_t>(value.fields()), value.allocationClass(), false };
                    weightOfBest = weight;
                }
            }
        }
        if (!best)
            continue;
        if (logsDecisions) [[unlikely]] {
            double weightOfAll = 0;
            for (const InnerObjectBirth& birth : ofLayout)
                weightOfAll += birth.weight;
            dataLogLn("inner objects: L", layout.index(), " field ", best->field, best->layout ? " L" : " array of capacity ", best->layout ? best->layout.index() : best->arrayCapacity, ": ", ofLayout.size(), " allocations of weight ", weightOfAll, ", with the inner object ", weightOfBest);
        }

        // The room in front of an inner object is lost where nothing takes it: on a path without an outer object, in a caller that makes none.
        UncheckedKeyHashMap<uint64_t, double> weightTaken;
        for (const InnerObjectBirth& birth : ofLayout) {
            if (const FreshField* fresh = fieldFilledAtBirth(program, promises, birth, *best))
                weightTaken.add(Promises::firstAllocation(birth, *fresh), 0).iterator->value += birth.weight;
        }
        auto isTakenMostly = [&](const InnerObjectBirth& birth, const FreshField& fresh) {
            constexpr double mostOf = 7.0 / 8;
            return weightTaken.get(Promises::firstAllocation(birth, fresh)) >= mostOf * weightOfObject(fresh);
        };

        bool hasInnerObject = false;
        best->isAlwaysInPlace = isStoredOnlyAtBirth(program, { layout, best->field });
        for (const InnerObjectBirth& birth : ofLayout) {
            const FreshField* fresh = fieldFilledAtBirth(program, promises, birth, *best);
            if (!fresh || !isTakenMostly(birth, *fresh)) {
                if (logsDecisions && fresh) [[unlikely]]
                    dataLogLn("inner objects: f", birth.function.index(), " v", birth.value.index(), " takes no cell: the inner object is made ", weightOfObject(*fresh), " times, taken ", weightTaken.get(Promises::firstAllocation(birth, *fresh)));
                best->isAlwaysInPlace = false;
                continue;
            }
            hasInnerObject = true;
            promises.promise(birth, *fresh);
            Promises::forEachAllocation(birth, *fresh, [&](FunctionID function, ValueID object) {
                Value& allocation = editor.atForLowering(function).edit(object);
                (allocation.opcode == Opcode::NewArray ? numberOfInnerArrays : numberOfInnerObjects) += !allocation.outerLayout();
                allocation.setOuterLayout(layout);
            });
            editor.atForLowering(birth.function).edit(birth.value, ValueEdit::Definition).setTakesCellOfInnerObject(true);
        }
        if (!hasInnerObject)
            continue;
        isOuter.set(layout.index());
        if (best->layout)
            isInner.set(best->layout.index());
        if (best->isAlwaysInPlace) {
            if (!layoutsMadeOtherwise)
                layoutsMadeOtherwise = findLayoutsMadeOtherwise(program, build);
            best->isAlwaysInPlace = !layoutsMadeOtherwise->get(layout.index());
        }
        numberAlwaysInPlace += best->isAlwaysInPlace;
        editor.setInnerObjects(layout, { WTF::move(*best) });
    }
    build.statistics().count(StatisticID::InnerObjectsAlwaysInPlace, numberAlwaysInPlace);
    build.statistics().count(StatisticID::InnerObjectsAllocated, numberOfInnerObjects);
    build.statistics().count(StatisticID::InnerArraysAllocated, numberOfInnerArrays);
    build.destroyLater(WTF::move(births));
    build.destroyLater(WTF::move(foundInFunctions));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
