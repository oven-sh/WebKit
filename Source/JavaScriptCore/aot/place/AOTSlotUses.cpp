/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTExactBuffer.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTGuardFactDetails.h"
#include "AOTKnownClosures.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <functional>
#include <tuple>
#include <wtf/Atomics.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet environmentFieldOpcodes { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
static constexpr OpcodeSet storesByName { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet allocationOpcodes { Opcode::NewObject, Opcode::MaterializeEnvironment };

namespace {

struct ObjectUnderConstruction {
    uint32_t parameterIndex { 0 };
    LayoutID layout;
};

struct ScanInputs {
    const SlotPlanContext& context;
    BitVector zeroEncodingCandidates;
    BitVector runsBeforeEntry;
    Vector<Vector<LayoutID>> layoutsNamedBelow;
    Vector<Vector<ObjectUnderConstruction, 1>> objectsUnderConstruction;
};

struct StoreByNameUse {
    auto place() const { return std::tuple(key, receivers); }

    PropertyKeyID key;
    LayoutSetID receivers;
    SiteHeat heat { 0 };
};

struct StoreOfUndefined {
    auto place() const { return std::tuple(key, receivers); }

    PropertyKeyID key;
    LayoutSetID receivers;
    uint32_t sites { 0 };
};

struct FillBesideCreation {
    LayoutID layout;
    FunctionID maker;
    PropertyKeyID key;
    bool storesUndefined { false };
    SiteHeat heat { 0 };
};

struct SourceOfResult {
    LayoutID layout;
    FunctionID maker;
};

struct KeysStoredTogether {
    auto place() const { return std::tuple(first, second, receivers); }

    PropertyKeyID first;
    PropertyKeyID second;
    LayoutSetID receivers;
    SiteWeight weight { 0 };
};

struct SlotUseCreation {
    LayoutID layout;
    SiteHeat heat { 0 };
    Expected<bool> isInLoop { false };
};

// `{ ...defaults, ...given }`, `const { secret, ...visible } = record`.
struct CopyIntoNewObject {
    LayoutID target;
    LayoutSetID sources;
    std::span<const PropertyKeyID> excludedKeys;
    SiteHeat heat { 0 };
};

struct SlotUseFreshObject {
    LayoutID layout;
    FunctionID maker;
    ValueID allocation;
    BlockID block;
};

// Heat has no upper limit and is added up in the order of the functions: sums of floating-point numbers depend on their order.
struct FunctionUses {
    FunctionID function;
    Vector<SlotUseCreation> creations;
    Vector<std::pair<LayoutID, SiteHeat>> placesThatMake;
    Vector<FunctionID> functionsCalledByName;
    Vector<FunctionID> functionsCalledByNameInLoops;
    Vector<std::pair<SlotRow, SiteHeat>> fills;
    Vector<FillBesideCreation> fillsBesideCreation;
    Vector<CopyIntoNewObject> copiesIntoNewObjects;
    Vector<SourceOfResult, 1> sourcesOfResult;
    Vector<SlotAccessUse> accesses;
    Vector<StoreByNameUse> storesByName;
    Vector<FunctionID> functionsWhoseNewClosuresAreStoredInto;
    Vector<FunctionID> functionsWithLentClosures;
    Vector<StoreOfUndefined> storesOfUndefined;
    Vector<KeysStoredTogether> keysStoredTogether;
    Vector<VariableID> variablesNamed;
    Vector<VariableID> variablesWithUncheckedReads;
};

} // anonymous namespace

static void add(uint32_t& sum, uint32_t more)
{
    if (more)
        WTF::atomicExchangeAdd(&sum, more, std::memory_order_relaxed);
}

static void addSaturated(SiteWeight& sum, SiteWeight more)
{
    for (;;) {
        SiteWeight previous = WTF::atomicLoad(&sum, std::memory_order_relaxed);
        if (WTF::atomicCompareExchangeWeakRelaxed(&sum, previous, saturatedSum(previous, more)))
            return;
    }
}

static void setBit(FastBitVector& bits, size_t index)
{
    if (!bits.at(index))
        bits.atomicSetAndCheck(index, true);
}

template<typename T, size_t inlineCapacity>
static void foldEntriesOfOnePlace(Vector<T, inlineCapacity>& sorted, const auto& placeOf, const auto& addTo)
{
    size_t count = 0;
    for (const T& entry : sorted) {
        if (count && placeOf(sorted[count - 1]) == placeOf(entry))
            addTo(sorted[count - 1], entry);
        else
            sorted[count++] = entry;
    }
    sorted.shrink(count);
}

template<typename T>
static Vector<T> listsOfAll(std::span<const FunctionUses> functions, Vector<T> FunctionUses::* list)
{
    size_t count = 0;
    for (const FunctionUses& function : functions)
        count += (function.*list).size();
    Vector<T> all;
    all.reserveInitialCapacity(count);
    for (const FunctionUses& function : functions)
        all.appendVector(function.*list);
    return all;
}

static ValueID thisParameterOf(const Function& function)
{
    std::optional<unsigned> index = function.thisParameterIndex();
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    return index && *index < parameters.size() ? parameters[*index] : ValueID();
}

static BitVector computeRunsBeforeEntry(const SlotPlanContext& context)
{
    const Program& program = context.program;
    BitVector runsBeforeEntry;
    runsBeforeEntry.ensureSize(program.functions.size());
    Vector<FunctionID> functionsToVisit;
    auto reach = [&](FunctionID function) {
        if (function && !runsBeforeEntry.quickSet(function.index()))
            functionsToVisit.append(function);
    };
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        const Function& function = program.functions[i];
        bool isModuleBody = function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
        if ((function.mayBeCalledByHost() && !isModuleBody) || isInRuntimeModule(program, function))
            reach(FunctionID(i));
    }
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions)
        reach(entry.function);
    while (!functionsToVisit.isEmpty()) {
        for (FunctionID callee : context.inputs.callGraph.calleesOf(functionsToVisit.takeLast()))
            reach(callee);
    }
    return runsBeforeEntry;
}

static Vector<Vector<LayoutID>> findLayoutsNamedBelow(const SlotPlanContext& context)
{
    const Program& program = context.program;
    Vector<Vector<LayoutID>> below(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!context.isCompiledObject(LayoutID(index)))
            continue;
        LayoutID above = program.layouts[index].base;
        for (size_t steps = 0; above && steps < program.layouts.size(); ++steps, above = program.at(above).base)
            below[above.index()].append(LayoutID(index));
    }
    return below;
}

static void forEachObjectHandedOver(const SlotPlanContext& context, FunctionID id, std::span<const ObjectUnderConstruction> handedOver, NOESCAPE const Invocable<void(FunctionID, ObjectUnderConstruction)> auto& handOver)
{
    const Function& function = context.program.at(id);
    FunctionAnalyses analyses({ context.build, context.program, &context.inputs.flow, nullptr, id }, function);
    LayoutID constructed = function.constructedClass;
    Vector<ValueID, 4> visitedParameters;
    auto follow = [&](auto& self, ValueID value) -> LayoutID {
        ValueID root = definitionThroughSameValues(function, value);
        const Value& definition = function.at(root);
        if (definition.opcode == Opcode::NewObject)
            return constructed && function.kind == FunctionKind::BaseConstructor && definition.layout() == constructed ? constructed : LayoutID();
        if (definition.opcode != Opcode::Parameter || visitedParameters.contains(root))
            return { };
        visitedParameters.append(root);

        ValueLocation location = analyses.useLists().locationOf(root);
        if (location.block == entryBlock) {
            if (constructed && function.thisParameterIndex() == location.index)
                return constructed;
            auto found = std::ranges::find(handedOver, location.index, &ObjectUnderConstruction::parameterIndex);
            return found == handedOver.end() ? LayoutID() : found->layout;
        }
        LayoutID layout;
        for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(function, edge, location.index);
            LayoutID layoutOnEdge = passed ? self(self, passed) : LayoutID();
            if (!layoutOnEdge || (layout && layout != layoutOnEdge))
                return { };
            layout = layoutOnEdge;
        }
        return layout;
    };
    auto forEachObjectAmong = [&](std::span<const ValueID> values, const auto& functor) {
        for (uint32_t index = 0; index < values.size(); ++index) {
            visitedParameters.shrink(0);
            if (LayoutID layout = follow(follow, values[index]))
                functor(ObjectUnderConstruction { index, layout });
        }
    };

    Vector<std::pair<FunctionID, ObjectUnderConstruction>, 8> closures;
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    double frequencyOfEntry = frequencies.frequencyOf(entryBlock).predicted();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        bool runsNearlyAlways = frequencies.frequencyOf(block).predicted() * 2 > frequencyOfEntry;
        for (ValueID valueID : function.at(block).values) {
            const Value& value = function.at(valueID);
            if (value.opcode == Opcode::NewClosure) {
                forEachObjectAmong(value.capturesOperands(), [&](ObjectUnderConstruction object) {
                    closures.append({ value.function(), object });
                });
                continue;
            }
            if (value.opcode != Opcode::CallDirect || value.isConstruction() || !runsNearlyAlways)
                continue;
            forEachObjectAmong(value.argumentsOperands(), [&](ObjectUnderConstruction object) {
                handOver(value.function(), object);
            });
            for (auto& [closure, object] : closures) {
                if (closure == value.function())
                    handOver(closure, object);
            }
        }
    }
}

static Vector<Vector<ObjectUnderConstruction, 1>> findObjectsUnderConstruction(const SlotPlanContext& context)
{
    using HandOver = std::pair<FunctionID, ObjectUnderConstruction>;
    const Program& program = context.program;
    auto callsDirectly = [&](FunctionID function) {
        return std::ranges::any_of(program.at(function).values, [](const Value& value) {
            return value.opcode == Opcode::CallDirect && !value.isConstruction();
        });
    };

    Vector<Vector<ObjectUnderConstruction, 1>> objects(program.functions.size());
    Vector<FunctionID> functionsToVisit;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].constructedClass && program.functions[index].hasBody() && callsDirectly(FunctionID(index)))
            functionsToVisit.append(FunctionID(index));
    }
    while (!functionsToVisit.isEmpty()) {
        Vector<HandOver> handOvers = collectInParallel<HandOver>(context.build.parallelLoops(), functionsToVisit.size(), [&](unsigned index, Vector<HandOver>& collected) {
            FunctionID function = functionsToVisit[index];
            forEachObjectHandedOver(context, function, objects[function.index()].span(), [&](FunctionID callee, ObjectUnderConstruction object) {
                collected.append({ callee, object });
            });
        });
        functionsToVisit.shrink(0);
        for (auto& [calleeID, object] : handOvers) {
            Vector<ObjectUnderConstruction, 1>& known = objects[calleeID.index()];
            const Function& callee = program.at(calleeID);
            if (!callee.hasBody() || callee.constructedClass || std::ranges::contains(known, object.parameterIndex, &ObjectUnderConstruction::parameterIndex))
                continue;
            known.append(object);
            if (!functionsToVisit.contains(calleeID) && callsDirectly(calleeID))
                functionsToVisit.append(calleeID);
        }
    }
    return objects;
}

// Equal accesses are added up in the order of their heat, so that the sum does not depend on who found which.
static bool accessIsOrderedBefore(const SlotAccessUse& a, const SlotAccessUse& b)
{
    return a.access != b.access ? a.access < b.access : (a.fillHeat < b.fillHeat).predicted();
}

static void foldEqualAccesses(Vector<SlotAccessUse>& sorted)
{
    foldEntriesOfOnePlace(sorted, [](const SlotAccessUse& use) { return use.access; }, [](SlotAccessUse& folded, const SlotAccessUse& use) {
        folded.sites += use.sites;
        folded.weight = saturatedSum(folded.weight, use.weight);
        folded.fillHeat += use.fillHeat;
        folded.storesWithoutAllocation += use.storesWithoutAllocation;
        folded.storesIntoFilledSlots += use.storesIntoFilledSlots;
        folded.storesIntoNewObjects += use.storesIntoNewObjects;
    });
}

namespace {

class SlotUseScanner {
public:
    SlotUseScanner(const ScanInputs& inputs, SlotUses& shared, FunctionID id)
        : m_inputs(inputs)
        , m_shared(shared)
        , m_program(inputs.context.program)
        , m_rows(inputs.context.rows)
        , m_id(id)
        , m_function(m_program.at(id))
        , m_thisParameter(thisParameterOf(m_function))
        , m_analyses({ inputs.context.build, m_program, &inputs.context.inputs.flow, nullptr, id }, m_function)
        , m_weights(m_program, inputs.context.inputs.heat, id, m_analyses)
    {
    }

    FunctionUses run()
    {
        m_uses.function = m_id;
        LayoutID constructed = m_function.constructedClass;
        if (constructed && m_program.at(constructed).constructor == m_id && m_function.kind != FunctionKind::BaseConstructor)
            m_uses.creations.append({ constructed, m_weights.heatOf(entryBlock), false });
        findSourcesOfResult();
        for (unsigned i = 0; i < m_function.blocks.size(); ++i) {
            for (ValueID id : m_function.blocks[i].values)
                scanValue(BlockID(i), m_function.at(id));
            scanNames(m_function.blocks[i].terminator);
        }
        if (m_readsZeroEncodingCandidate)
            findUncheckedVariableReads(m_analyses, m_inputs.zeroEncodingCandidates, m_uses.variablesWithUncheckedReads);
        AOT::sort(m_uses.accesses, accessIsOrderedBefore);
        foldEqualAccesses(m_uses.accesses);
        AOT::sort(m_uses.variablesNamed);
        removeRepeatedElements(m_uses.variablesNamed);
        return WTF::move(m_uses);
    }

private:
    SlotRowUse& rowUseOf(LayoutID layout, FieldIndex field) { return m_shared.rows[m_rows.rowOf(layout, field)]; }
    ValueID rootOf(ValueID id) const { return definitionThroughSameValues(m_function, id); }

    bool isUndefinedConstant(ValueID id) const
    {
        auto constant = constantThroughCasts(m_program, m_function, id);
        return constant && m_function.at(*constant).opcode == Opcode::Undefined;
    }

    BlockID blockOf(ValueID id)
    {
        if (m_blockOfValue.isEmpty()) {
            m_blockOfValue.fill(BlockID(), m_function.values.size());
            for (unsigned index = 0; index < m_function.blocks.size(); ++index) {
                const Block& block = m_function.blocks[index];
                for (ValueID parameter : block.parameters)
                    m_blockOfValue[parameter.index()] = BlockID(index);
                for (ValueID value : block.values)
                    m_blockOfValue[value.index()] = BlockID(index);
            }
        }
        return m_blockOfValue[id.index()];
    }

    void forEachClassConstructedBy(const Value& construct, const Invocable<void(LayoutID)> auto& functor) const
    {
        CalleeSetID callees = construct.callees();
        if (!callees)
            return;
        for (FunctionID callee : m_program.at(callees).seen().predicted().functions) {
            if (LayoutID constructed = m_program.at(callee).constructedClass)
                functor(constructed);
        }
    }

    bool collectFreshObjects(ValueID value, BlockID join, Vector<ValueID, 4>& visitedParameters, Vector<SlotUseFreshObject, 4>& objects)
    {
        ValueID root = rootOf(value);
        const Value& definition = m_function.at(root);
        auto addObjectOf = [&](LayoutID layout, FunctionID maker) {
            objects.append({ layout, maker, root, join ? join : blockOf(root) });
        };
        auto addObjectHandedIn = [&](LayoutID layout) {
            objects.append({ layout, { }, { }, join ? join : entryBlock });
            return true;
        };
        switch (definition.opcode) {
        case Opcode::NewObject:
            addObjectOf(definition.layout(), { });
            return true;
        case Opcode::CallDirect:
            if (!definition.isConstruction())
                addObjectOf({ }, definition.function());
            else if (LayoutID constructed = m_program.at(definition.function()).constructedClass)
                addObjectOf(constructed, { });
            else
                return false;
            return true;
        case Opcode::Construct: {
            // The list may lack callees. What they make is not seen here, and the estimate does without it.
            size_t numberOfOtherObjects = objects.size();
            forEachClassConstructedBy(definition, [&](LayoutID constructed) {
                addObjectOf(constructed, { });
            });
            return objects.size() != numberOfOtherObjects;
        }
        default:
            break;
        }
        if (LayoutID constructed = m_function.constructedClass; constructed && root == m_thisParameter)
            return addObjectHandedIn(constructed);
        if (definition.opcode != Opcode::Parameter)
            return false;
        if (visitedParameters.contains(root))
            return true;
        visitedParameters.append(root);

        BlockID block = blockOf(root);
        if (!block)
            return false;
        std::span<const ValueID> parameters = m_function.at(block).parameters.span();
        auto parameterIndex = static_cast<uint32_t>(std::ranges::find(parameters, root) - parameters.begin());
        if (block == entryBlock) {
            std::span<const ObjectUnderConstruction> handedOver = m_inputs.objectsUnderConstruction[m_id.index()].span();
            auto found = std::ranges::find(handedOver, parameterIndex, &ObjectUnderConstruction::parameterIndex);
            return found != handedOver.end() && addObjectHandedIn(found->layout);
        }
        std::span<const Edge> edges = m_analyses.cfg().incomingEdges(block);
        if (!join && edges.size() > 1)
            join = block;
        bool areAllFresh = true;
        for (Edge edge : edges) {
            ValueID passed = valuePassedOn(m_function, edge, parameterIndex);
            areAllFresh &= passed && collectFreshObjects(passed, join, visitedParameters, objects);
        }
        return areAllFresh;
    }

    void findSourcesOfResult()
    {
        if (m_function.results.size() != 1 || !m_program.at(m_function.results[0].expected().predicted()).layouts)
            return;
        Vector<SlotUseFreshObject, 4> objects;
        for (const Block& block : m_function.blocks) {
            if (block.terminator.kind == TerminatorKind::ReturnCall)
                return;
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            Vector<ValueID, 4> visitedParameters;
            ValueID returned = block.terminator.valueOperand();
            if (!returned || !collectFreshObjects(returned, { }, visitedParameters, objects))
                return;
        }
        for (const SlotUseFreshObject& object : objects)
            m_uses.sourcesOfResult.append({ object.layout, object.maker });
    }

    void countAllocation(LayoutID layout, BlockID block)
    {
        add(m_shared.layouts[layout.index()].allocationSites, 1);
        m_uses.creations.append({ layout, m_weights.heatOf(block), m_weights.isInLoop(block) });
    }

    void countInitialField(LayoutID layout, FieldIndex field, bool isUndefined, BlockID block)
    {
        SlotRowUse& use = rowUseOf(layout, field);
        add(use.initializingSites, 1);
        if (isUndefined)
            return;
        add(use.sitesInitializingWithValue, 1);
        m_uses.fills.append({ m_rows.rowOf(layout, field), m_weights.heatOf(block) });
    }

    void scanCallByName(BlockID block, const Value& call)
    {
        FunctionID callee = call.function();
        if (LayoutID constructed = m_program.at(callee).constructedClass; constructed && call.isConstruction())
            m_uses.placesThatMake.append({ constructed, m_weights.heatOf(block) });
        bool isClassConstructor = m_function.kind == FunctionKind::BaseConstructor || m_function.kind == FunctionKind::DerivedConstructor;
        if (callee == m_id && call.isConstruction() && !isClassConstructor)
            return;
        (m_weights.isInLoop(block).predicted() ? m_uses.functionsCalledByNameInLoops : m_uses.functionsCalledByName).append(callee);
    }

    void scanSlotAccess(BlockID block, const Value& value)
    {
        SlotAccessUse use { { value.key(), value.receivers(), isSlotStore(value.opcode) }, 1, m_weights.weightOf(block) };
        if (std::optional<uint8_t> storedOperand = storedOf(value.opcode)) {
            if (isUndefinedConstant(value.operand(*storedOperand)))
                m_uses.storesOfUndefined.append({ value.key(), value.receivers(), 1 });
            else
                use.fillHeat = m_weights.heatOf(block);
            // PutSlot declares no allocation, so it can neither create nor grow an Extension. Behind a proof that the slot is filled it finds the Extension that the creating store left.
            bool cannotAllocate = value.opcode == Opcode::PutSlot;
            use.storesIntoFilledSlots = cannotAllocate && guardProvesSlotFilled(m_analyses, value);
            use.storesWithoutAllocation = cannotAllocate && !use.storesIntoFilledSlots;
            ValueID objectID = rootOf(value.objectOperand());
            const Value& object = m_function.at(objectID);
            std::span<const LayoutID> receivers = m_program.at(value.receivers()).completeMembers();
            use.storesIntoNewObjects = object.opcode == Opcode::NewObject && receivers.size() == 1 && receivers[0] == object.layout();
            noteStoreBehindStore(objectID, value, use.weight);
        }
        m_uses.accesses.append(use);
    }

    // In the order of the blocks. The rarer of the two stores says how often both run.
    void noteStoreBehindStore(ValueID object, const Value& store, SiteWeight weight)
    {
        LastStore current { store.key(), store.receivers(), weight };
        auto result = m_lastStoreInto.add(object.index(), current);
        if (result.isNewEntry)
            return;
        LastStore last = std::exchange(result.iterator->value, current);
        if (last.receivers == current.receivers && last.key != current.key)
            m_uses.keysStoredTogether.append({ std::min(last.key, current.key), std::max(last.key, current.key), current.receivers, lesserOf(last.weight, weight) });
    }

    void scanStoreByName(BlockID block, const Value& store)
    {
        std::optional<uint8_t> receiver = receiverOf(store.opcode);
        ValueID object = receiver ? rootOf(store.operand(*receiver)) : ValueID();
        if (object && m_function.at(object).opcode == Opcode::NewClosure)
            m_uses.functionsWhoseNewClosuresAreStoredInto.append(m_function.at(object).function());
        LayoutSetID receivers = store.receivers();
        if (!receivers)
            return;
        std::optional<uint8_t> stored = storedOf(store.opcode);
        if (stored && isUndefinedConstant(store.operand(*stored)))
            m_uses.storesOfUndefined.append({ store.key(), receivers, 1 });
        else
            m_uses.storesByName.append({ store.key(), receivers, m_weights.heatOf(block) });
        if (object)
            noteStoreBehindStore(object, store, m_weights.weightOf(block));
    }

    void scanStoreIntoFreshObject(BlockID block, const Value& store)
    {
        std::optional<uint8_t> receiver = receiverOf(store.opcode);
        std::optional<uint8_t> stored = storedOf(store.opcode);
        if (!receiver || !stored || !Value::opcodesWithKey.contains(store.opcode) || !store.key())
            return;

        Vector<ValueID, 4> visitedParameters;
        Vector<SlotUseFreshObject, 4> objects;
        collectFreshObjects(store.operand(*receiver), { }, visitedParameters, objects);
        if (objects.isEmpty())
            return;

        bool storesUndefined = isUndefinedConstant(store.operand(*stored));
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        double frequencyOfStore = frequencies.frequencyOf(block).predicted();
        auto countStoreInto = [&](LayoutID layout) {
            std::optional<uint32_t> field = m_program.at(layout).findField(store.key());
            if (!field)
                return;
            SlotRowUse& use = rowUseOf(layout, *field);
            add(storesUndefined ? use.storesOfUndefined : use.storesOfConstructorWithValue, 1);
        };
        for (SlotUseFreshObject object : objects) {
            if (!storesUndefined && frequencyOfStore * 2 <= frequencies.frequencyOf(object.block).predicted())
                continue;
            if (object.allocation) {
                if (storesUndefined || m_fieldsFilledBesideAllocation.add(static_cast<uint64_t>(object.allocation.index()) << 32 | store.key().index()).isNewEntry)
                    m_uses.fillsBesideCreation.append({ object.layout, object.maker, store.key(), storesUndefined, m_weights.heatOf(blockOf(object.allocation)) });
                continue;
            }
            countStoreInto(object.layout);
            for (LayoutID below : m_inputs.layoutsNamedBelow[object.layout.index()])
                countStoreInto(below);
        }
    }

    void scanCopy(BlockID block, const Value& copy)
    {
        const Value& target = m_function.at(rootOf(copy.targetOperand()));
        if (target.opcode != Opcode::NewObject || !m_inputs.context.isCompiledObject(target.layout()))
            return;
        m_uses.copiesIntoNewObjects.append({ target.layout(), m_program.expectedOrTypeOf(m_function, copy.sourceOperand()).predicted().layouts, copy.excludedKeys(), m_weights.heatOf(block) });
    }

    void scanAllocation(BlockID block, const Value& allocation)
    {
        LayoutID layout = allocation.layout();
        countAllocation(layout, block);
        if (allocation.opcode == Opcode::NewObject && m_function.constructedClass != layout)
            m_uses.placesThatMake.append({ layout, m_weights.heatOf(block) });
        if (allocation.opcode == Opcode::MaterializeEnvironment)
            return;
        auto fields = allocation.fields();
        auto initialValues = allocation.variadicOperands();
        for (uint32_t i = 0; i < fields.size(); ++i)
            countInitialField(layout, fields[i], isUndefinedConstant(initialValues[i]), block);
    }

    void scanData(BlockID block, const Value& value)
    {
        forEachDataNodeInPreorder(m_program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
            if (const DataNode::String* string = node.tryAs<DataNode::String>())
                setBit(m_shared.stringsNamed, string->value.index());
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members)
                setBit(m_shared.keysNamed, member.key.index());
            if (!object->layout)
                return;
            countAllocation(object->layout, block);
            for (const DataMember& member : object->members) {
                if (std::optional<uint32_t> field = m_program.at(object->layout).findField(member.key))
                    countInitialField(object->layout, *field, member.value.is<DataNode::Undefined>(), block);
            }
        });
    }

    void scanNames(const auto& valueOrTerminator)
    {
        forEachReference(valueOrTerminator, WTF::makeVisitor([&](PropertyKeyID key) {
            setBit(m_shared.keysNamed, key.index());
        }, [&](StringID string) {
            setBit(m_shared.stringsNamed, string.index());
        }, [&](VariableID variable) {
            m_uses.variablesNamed.append(variable);
        }, [](auto) { }));
    }

    void noteKeyDefinedByDescriptor(const Type& key)
    {
        ConstantKey constant = constantKeyOf(m_program, key);
        std::optional<PropertyKeyID> known = constant.kind == ConstantKeyKind::Symbol ? std::optional(constant.symbol)
            : constant.kind == ConstantKeyKind::Text ? m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = constant.text }) : std::nullopt;
        if (known)
            setBit(m_shared.keysDefinedByDescriptor, known->index());
    }

    // The names under which the text gives some object an accessor, or a property by a descriptor.
    void scanDefinitionsByDescriptor(const Value& value)
    {
        std::span<const ValueID> arguments = value.argumentsOperands();
        auto typeOfArgument = [&](const std::optional<BuiltinOperand>& operand) -> const Type* {
            if (!operand || operand->kind() != BuiltinOperand::Kind::Argument || operand->argumentIndex() >= arguments.size())
                return nullptr;
            return &m_program.typeOf(m_function, arguments[operand->argumentIndex()]);
        };
        for (const BuiltinPropertyAction& action : m_program.builtins().at(value.overload()).flow.propertyActions) {
            if (action.kind == BuiltinPropertyActionKind::TakesDescriptor && action.key.kind == BuiltinKeyKind::Operand) {
                if (const Type* key = typeOfArgument(action.key.operand))
                    noteKeyDefinedByDescriptor(*key);
            } else if (action.kind == BuiltinPropertyActionKind::TakesDescriptorMap && action.value.kind == BuiltinPlaceKind::Operand) {
                const Type* map = typeOfArgument(action.value.operand);
                if (!map || !map->layouts)
                    continue;
                for (LayoutID layout : m_program.at(map->layouts).written()) {
                    for (const Field& field : m_program.at(layout).fields) {
                        if (field.key)
                            setBit(m_shared.keysDefinedByDescriptor, field.key.index());
                    }
                }
            }
        }
    }

    void scanValue(BlockID block, const Value& value)
    {
        Opcode opcode = value.opcode;
        if (Value::opcodesWithFieldAccess.contains(opcode))
            scanSlotAccess(block, value);
        else if (siteCachesOf(opcode)) {
            SiteWeight weight = m_weights.weightOf(block);
            forEachPropertyKey(value, [&](PropertyKeyID key) {
                addSaturated(m_shared.keyWeights[key.index()], weight);
            });
        }
        if (storesByName.contains(opcode))
            scanStoreByName(block, value);
        if (storesByName.contains(opcode) || isSlotStore(opcode))
            scanStoreIntoFreshObject(block, value);
        if (environmentFieldOpcodes.contains(opcode))
            add(rowUseOf(value.layout(), value.field()).environmentUses, 1);
        if (opcode == Opcode::PrivateDefine) {
            add(rowUseOf(value.layout(), value.field()).privateDefinitionsByName, 1);
            m_uses.fills.append({ m_rows.rowOf(value.layout(), value.field()), m_weights.heatOf(block) });
        }
        if (allocationOpcodes.contains(opcode))
            scanAllocation(block, value);
        if (opcode == Opcode::CopyDataProperties)
            scanCopy(block, value);
        if ((opcode == Opcode::DefineGetter || opcode == Opcode::DefineSetter) && value.key())
            setBit(m_shared.keysDefinedByDescriptor, value.key().index());
        if (opcode == Opcode::CallBuiltin)
            scanDefinitionsByDescriptor(value);
        if (opcode == Opcode::NewClosure && value.isLent())
            m_uses.functionsWithLentClosures.append(value.function());
        if (opcode == Opcode::CallDirect)
            scanCallByName(block, value);
        if (opcode == Opcode::Construct) {
            forEachClassConstructedBy(value, [&](LayoutID constructed) {
                m_uses.placesThatMake.append({ constructed, m_weights.heatOf(block) });
            });
        }
        if (Value::opcodesWithData.contains(opcode))
            scanData(block, value);
        if (opcode == Opcode::GetModuleVariable && m_inputs.zeroEncodingCandidates.quickGet(value.variable().index()))
            m_readsZeroEncodingCandidate = true;
        scanNames(value);
    }

    struct LastStore {
        PropertyKeyID key;
        LayoutSetID receivers;
        SiteWeight weight { 0 };
    };

    const ScanInputs& m_inputs;
    SlotUses& m_shared;
    const Program& m_program;
    const SlotRows& m_rows;
    FunctionID m_id;
    const Function& m_function;
    ValueID m_thisParameter;
    FunctionAnalyses m_analyses;
    FunctionSiteWeights m_weights;
    FunctionUses m_uses;
    Vector<BlockID> m_blockOfValue;
    UncheckedKeyHashMap<uint32_t, LastStore, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lastStoreInto;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_fieldsFilledBesideAllocation;
    bool m_readsZeroEncodingCandidate { false };
};

} // anonymous namespace

static Vector<StoreByNameUse> mergeStoresByName(const ParallelLoops& loops, std::span<const FunctionUses> functions)
{
    Vector<StoreByNameUse> stores = listsOfAll(functions, &FunctionUses::storesByName);
    parallelSort(loops, stores.mutableSpan(), [](const StoreByNameUse& a, const StoreByNameUse& b) {
        return std::tuple(a.key, a.receivers, a.heat.predicted()) < std::tuple(b.key, b.receivers, b.heat.predicted());
    });
    foldEntriesOfOnePlace(stores, std::mem_fn(&StoreByNameUse::place), [](StoreByNameUse& folded, const StoreByNameUse& store) {
        folded.heat += store.heat;
    });
    return stores;
}

static Vector<StoreOfUndefined> mergeStoresOfUndefined(const ParallelLoops& loops, std::span<const FunctionUses> functions)
{
    Vector<StoreOfUndefined> stores = listsOfAll(functions, &FunctionUses::storesOfUndefined);
    parallelSort(loops, stores.mutableSpan(), [](const StoreOfUndefined& a, const StoreOfUndefined& b) {
        return a.place() < b.place();
    });
    foldEntriesOfOnePlace(stores, std::mem_fn(&StoreOfUndefined::place), [](StoreOfUndefined& folded, const StoreOfUndefined& store) {
        folded.sites += store.sites;
    });
    return stores;
}

static void findRowsStoredTogether(const SlotPlanContext& context, std::span<const FunctionUses> functions, Vector<RowsStoredTogether>& rows)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    auto addWeight = [](auto& folded, const auto& stored) {
        folded.weight = saturatedSum(folded.weight, stored.weight);
    };
    Vector<KeysStoredTogether> keys = listsOfAll(functions, &FunctionUses::keysStoredTogether);
    parallelSort(loops, keys.mutableSpan(), [](const KeysStoredTogether& a, const KeysStoredTogether& b) {
        return a.place() < b.place();
    });
    foldEntriesOfOnePlace(keys, std::mem_fn(&KeysStoredTogether::place), addWeight);

    for (const KeysStoredTogether& stored : keys) {
        context.forEachHolder(stored.first, stored.receivers, [&](LayoutID member, SlotRow first) {
            if (std::optional<uint32_t> second = program.at(member).findField(stored.second))
                rows.append({ first, context.rows.rowOf(member, *second), stored.weight });
        });
    }
    auto placeOf = [](const RowsStoredTogether& stored) {
        return std::pair(stored.first, stored.second);
    };
    parallelSort(loops, rows.mutableSpan(), [&](const RowsStoredTogether& a, const RowsStoredTogether& b) {
        return placeOf(a) < placeOf(b);
    });
    foldEntriesOfOnePlace(rows, placeOf, addWeight);
    parallelSort(loops, rows.mutableSpan(), [&](const RowsStoredTogether& a, const RowsStoredTogether& b) {
        return !a.weight.isSamePredictionAs(b.weight) ? (a.weight > b.weight).predicted() : placeOf(a) < placeOf(b);
    });
}

static void markVariables(const BitVector& runsBeforeEntry, std::span<const FunctionUses> functions, std::span<VariableUse> variables)
{
    for (const FunctionUses& function : functions) {
        bool isBeforeEntry = runsBeforeEntry.quickGet(function.function.index());
        for (VariableID variable : function.variablesNamed) {
            variables[variable.index()].isNamedByCode = true;
            variables[variable.index()].isUsedBeforeProgramEntry |= isBeforeEntry;
        }
        for (VariableID variable : function.variablesWithUncheckedReads)
            variables[variable.index()].hasUncheckedRead = true;
    }
}

static void markFunctionsWhoseNewClosuresAreStoredInto(std::span<const FunctionUses> functions, BitVector& marked)
{
    for (const FunctionUses& function : functions) {
        for (FunctionID closure : function.functionsWhoseNewClosuresAreStoredInto)
            marked.set(closure.index());
    }
    for (const FunctionUses& function : functions) {
        for (FunctionID closure : function.functionsWithLentClosures)
            marked.clear(closure.index());
    }
}

static void distributeFillHeat(const SlotPlanContext& context, SlotUses& uses, PropertyKeyID key, LayoutSetID receivers, SiteHeat heat)
{
    if (!heat.predicted())
        return;
    Vector<std::pair<SlotRow, SiteHeat>, 8> holders;
    SiteHeat made = 0;
    context.forEachHolder(key, receivers, [&](LayoutID member, SlotRow row) {
        holders.append({ row, uses.layouts[member.index()].creationHeat });
        made += holders.last().second;
    });
    if (!made.predicted())
        return;
    for (auto [row, madeOfHolder] : holders)
        uses.rows[row].fillHeat += heat * (madeOfHolder / made);
}

static Vector<const FunctionUses*> usesByFunction(const Program& program, std::span<const FunctionUses> functions)
{
    Vector<const FunctionUses*> usesOf(FillWith { }, program.functions.size(), nullptr);
    for (const FunctionUses& uses : functions)
        usesOf[uses.function.index()] = &uses;
    return usesOf;
}

static Vector<LayoutID> findLayoutsOfFreshResults(const Program& program, std::span<const FunctionUses> functions)
{
    enum class State : uint8_t { Unknown, Visiting, Known };
    Vector<LayoutID> layouts(program.functions.size());
    Vector<State> states(FillWith { }, program.functions.size(), State::Known);
    Vector<const FunctionUses*> usesOf = usesByFunction(program, functions);
    for (const FunctionUses& uses : functions) {
        if (!uses.sourcesOfResult.isEmpty())
            states[uses.function.index()] = State::Unknown;
    }
    auto resolve = [&](auto& self, FunctionID function) -> LayoutID {
        State& state = states[function.index()];
        if (state != State::Unknown)
            return layouts[function.index()];
        state = State::Visiting;
        LayoutID result;
        bool isOneLayout = true;
        for (const SourceOfResult& source : usesOf[function.index()]->sourcesOfResult) {
            LayoutID layout = source.layout ? source.layout : self(self, source.maker);
            isOneLayout &= layout && (!result || result == layout);
            result = layout;
        }
        state = State::Known;
        return layouts[function.index()] = isOneLayout ? result : LayoutID();
    };
    for (const FunctionUses& uses : functions)
        resolve(resolve, uses.function);
    return layouts;
}

static BitVector findFunctionsThatRunInLoops(const SlotPlanContext& context, std::span<const FunctionUses> functions)
{
    unsigned numberOfFunctions = context.program.functions.size();
    Vector<const FunctionUses*> usesOf = usesByFunction(context.program, functions);
    auto calleesOf = [&](unsigned index) {
        return usesOf[index] ? usesOf[index]->functionsCalledByName.span() : std::span<const FunctionID>();
    };

    BitVector runsInLoop(numberOfFunctions);
    Vector<FunctionID> worklist;
    auto add = [&](FunctionID function) {
        if (!runsInLoop.quickSet(function.index()))
            worklist.append(function);
    };
    for (size_t index : computeStronglyConnectedComponents(numberOfFunctions, calleesOf).isInCycle)
        add(FunctionID(index));
    for (const FunctionUses& uses : functions) {
        for (FunctionID callee : uses.functionsCalledByNameInLoops)
            add(callee);
    }
    for (const FlowIndirectCall& call : context.inputs.flow.indirectCalls()) {
        if (call.times.predicted() == BuiltinCallbackTimes::PerElement && call.callee.index() < numberOfFunctions)
            add(call.callee);
    }
    while (!worklist.isEmpty()) {
        for (FunctionID callee : calleesOf(worklist.takeLast().index()))
            add(callee);
    }
    return runsInLoop;
}

static void countStoresOfFactories(const SlotPlanContext& context, std::span<const FunctionUses> functions, SlotUses& uses)
{
    const Program& program = context.program;
    Vector<SiteHeat> heatOfPlacesThatMake(FillWith { }, program.layouts.size(), 0.0);
    Vector<SiteHeat> heatOfPlacesThatFill(FillWith { }, uses.rows.size(), 0.0);
    for (const FunctionUses& function : functions) {
        for (auto [layout, heat] : function.placesThatMake)
            heatOfPlacesThatMake[layout.index()] += heat;
        for (const FillBesideCreation& fill : function.fillsBesideCreation) {
            std::optional<uint32_t> field = fill.layout && !fill.storesUndefined ? program.at(fill.layout).findField(fill.key) : std::nullopt;
            if (field)
                heatOfPlacesThatFill[context.rows.rowOf(fill.layout, *field)] += fill.heat;
        }
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID layout(index);
        if (!heatOfPlacesThatMake[index].predicted() || !context.isCompiledObject(layout))
            continue;
        for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(layout); ++field) {
            SlotRow row = context.rows.rowOf(layout, field);
            if ((heatOfPlacesThatFill[row] * 2 >= heatOfPlacesThatMake[index]).predicted())
                ++uses.rows[row].storesOfConstructorWithValue;
        }
    }
}

static void addUpCreations(const SlotPlanContext& context, std::span<const FunctionUses> functions, SlotUses& uses)
{
    BitVector runsInLoop = findFunctionsThatRunInLoops(context, functions);
    for (const FunctionUses& function : functions) {
        for (const SlotUseCreation& creation : function.creations) {
            LayoutUse& use = uses.layouts[creation.layout.index()];
            use.creationHeat += creation.heat;
            use.isMadeInLoop |= creation.isInLoop.predicted() || runsInLoop.quickGet(function.function.index());
        }
    }
}

static void addUpFillsBesideCreation(const SlotPlanContext& context, std::span<const FunctionUses> functions, SlotUses& uses)
{
    const Program& program = context.program;
    Vector<LayoutID> freshResults = findLayoutsOfFreshResults(program, functions);
    for (const FunctionUses& function : functions) {
        for (const FillBesideCreation& fill : function.fillsBesideCreation) {
            LayoutID layout = fill.layout ? fill.layout : freshResults[fill.maker.index()];
            std::optional<uint32_t> field = layout ? program.at(layout).findField(fill.key) : std::nullopt;
            if (!field)
                continue;
            SlotRowUse& use = uses.rows[context.rows.rowOf(layout, *field)];
            if (fill.storesUndefined)
                ++use.storesOfUndefined;
            else
                use.heatFilledBesideCreation += fill.heat;
        }
    }
}

static bool isOptionalWhereDeclared(const Program& program, FieldReference reference)
{
    if (!fieldAt(program, reference).originLayout)
        return false;
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const Field& field = fieldAt(program, reference);
        if (!field.originLayout || !field.originField)
            break;
        reference = { field.originLayout, *field.originField };
    }
    return !fieldAt(program, reference).flags.contains(FieldFlags::IsRequired);
}

// The run time fills a field of the new object as often as what it copies has a property of that name. Only a field that most of the new objects get counts: what a few of them get is no better known than before.
static void distributeCopies(const SlotPlanContext& context, SlotUses& uses, std::span<const FunctionUses> functions)
{
    AOT_THRESHOLD(size_t, mostSourcesLookedAt, 8);
    AOT_THRESHOLD(unsigned, mostRoundsOverCopies, 4);
    const Program& program = context.program;
    bool hasCopies = std::ranges::any_of(functions, [](const FunctionUses& function) {
        return !function.copiesIntoNewObjects.isEmpty();
    });
    if (!hasCopies)
        return;

    for (const FunctionUses& function : functions) {
        for (const CopyIntoNewObject& copy : function.copiesIntoNewObjects) {
            for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(copy.target); ++field) {
                PropertyKeyID key = program.at(copy.target).fields[field].key;
                if (key && !std::ranges::contains(copy.excludedKeys, key))
                    uses.rows[context.rows.rowOf(copy.target, field)].isFilledBySomeCopy = true;
            }
        }
    }

    // A source may itself be put together from copies.
    BitVector rowsFilledByCopies(uses.rows.size());
    Vector<SiteHeat> heatOfCopies(uses.rows.size());
    for (unsigned round = 0; round < mostRoundsOverCopies; ++round) {
        heatOfCopies.fill(SiteHeat(0.0));
        for (const FunctionUses& function : functions) {
            for (const CopyIntoNewObject& copy : function.copiesIntoNewObjects) {
                auto allSources = copy.sources ? program.at(copy.sources).all() : std::nullopt;
                if (!allSources)
                    continue;
                std::span<const LayoutID> sources = *allSources;
                SiteHeat made = 0;
                for (LayoutID source : sources)
                    made += uses.layouts[source.index()].creationHeat;
                bool knowsSources = made.predicted() && sources.size() <= mostSourcesLookedAt && std::ranges::all_of(sources, [&](LayoutID source) {
                    return context.isCompiledObject(source);
                });
                if (!knowsSources)
                    continue;
                for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(copy.target); ++field) {
                    PropertyKeyID key = program.at(copy.target).fields[field].key;
                    if (!key || std::ranges::contains(copy.excludedKeys, key))
                        continue;
                    SiteHeat filled = 0;
                    for (LayoutID source : sources) {
                        std::optional<uint32_t> fieldOfSource = program.at(source).findField(key);
                        if (!fieldOfSource)
                            continue;
                        SlotRow rowOfSource = context.rows.rowOf(source, *fieldOfSource);
                        SiteHeat madeOfSource = uses.layouts[source.index()].creationHeat;
                        filled += rowsFilledByCopies.quickGet(rowOfSource) ? madeOfSource : lesserOf(madeOfSource, uses.rows[rowOfSource].fillHeat);
                    }
                    heatOfCopies[context.rows.rowOf(copy.target, field)] += copy.heat * (filled / made);
                }
            }
        }
        bool hasChanged = false;
        for (size_t row = 0; row < heatOfCopies.size(); ++row) {
            if (heatOfCopies[row].predicted() && (heatOfCopies[row] >= uses.rows[row].holderHeat * 0.4999).predicted())
                hasChanged |= !rowsFilledByCopies.quickSet(row);
        }
        if (!hasChanged)
            break;
    }
    for (size_t row : rowsFilledByCopies) {
        uses.rows[row].fillHeat += heatOfCopies[row];
        uses.rows[row].heatFilledBesideCreation += heatOfCopies[row];
    }
}

static void distributeToRows(const SlotPlanContext& context, SlotUses& uses, std::span<const FunctionUses> functions, std::span<const StoreByNameUse> stores, std::span<const StoreOfUndefined> storesOfUndefined)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    std::span<SlotRowUse> rows = uses.rows.mutableSpan();
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!context.isCompiledObject(id))
            return;
        const Layout& layout = program.at(id);
        for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(id); ++field) {
            SlotRowUse& use = rows[context.rows.rowOf(id, field)];
            use.holderHeat = uses.layouts[index].creationHeat;
            use.hasHoldersMadeInLoop = uses.layouts[index].isMadeInLoop;
            OptionSet<FieldFlags> flags = layout.fields[field].flags;
            use.isRequiredByDeclaringType = layout.isDerived() && flags.contains(FieldFlags::IsRequired) && !flags.contains(FieldFlags::IsInherited);
            use.isOptionalInTypeOfThis = !flags.contains(FieldFlags::IsInherited) && isOptionalWhereDeclared(program, { id, field });
        }
    });
    // By one thread, in the order of the lists.
    for (const StoreByNameUse& store : stores)
        distributeFillHeat(context, uses, store.key, store.receivers, store.heat);
    for (const SlotAccessUse& use : uses.accesses)
        distributeFillHeat(context, uses, use.access.key, use.access.receivers, use.fillHeat);
    distributeCopies(context, uses, functions);
    forEachIndexInParallel(loops, rows.size(), [&](unsigned row) {
        SlotRowUse& use = rows[row];
        if (use.heatFilledBesideCreation.predicted() && (use.heatFilledBesideCreation >= use.holderHeat * 0.4999).predicted() && (use.heatFilledBesideCreation <= use.holderHeat * 2).predicted())
            ++use.storesOfConstructorWithValue;
    });
    forEachIndexInParallel(loops, storesOfUndefined.size(), [&](unsigned index) {
        const StoreOfUndefined& store = storesOfUndefined[index];
        context.forEachHolder(store.key, store.receivers, [&](LayoutID, SlotRow row) {
            add(rows[row].storesOfUndefined, store.sites);
        });
    });
    forEachIndexInParallel(loops, uses.accesses.size(), [&](unsigned index) {
        const SlotAccessUse& use = uses.accesses[index];
        context.forEachHolder(use.access.key, use.access.receivers, [&](LayoutID, SlotRow row) {
            add(rows[row].accessSites, use.sites);
            addSaturated(rows[row].accessWeight, use.weight);
            add(rows[row].storesWithoutAllocation, use.storesWithoutAllocation);
            add(rows[row].storesIntoFilledSlots, use.storesIntoFilledSlots);
            add(rows[row].storesIntoNewObjects, use.storesIntoNewObjects);
        });
    });
}

SlotUses collectSlotUses(const SlotPlanContext& context)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    SlotUses uses;
    ScanInputs inputs { context, { }, { }, { }, { } };
    runTasksInParallel(loops, {
        [&] { uses.rows.fill({ }, context.rows.count()); },
        [&] {
            uses.layouts.fill({ }, program.layouts.size());
            uses.variables.fill({ }, program.variables.size());
            uses.keyWeights.fill(0, program.types.numberOfPropertyKeys());
            uses.keysNamed = FastBitVector(program.types.numberOfPropertyKeys());
            uses.keysDefinedByDescriptor = FastBitVector(program.types.numberOfPropertyKeys());
            uses.stringsNamed = FastBitVector(program.strings.size());
        },
        [&] { inputs.layoutsNamedBelow = findLayoutsNamedBelow(context); },
        [&] { inputs.objectsUnderConstruction = findObjectsUnderConstruction(context); },
        [&] { inputs.zeroEncodingCandidates = findZeroEncodingCandidates(context); },
        [&] { inputs.runsBeforeEntry = computeRunsBeforeEntry(context); },
    });
    ParallelArray<FunctionUses> usesOfFunctions = collectIntoArrayInParallel<FunctionUses>(loops, program.functions.size(), largeFunctions(program, loops).span(), [&](unsigned index, Vector<FunctionUses>& collected) {
        if (program.functions[index].hasBody())
            collected.append(SlotUseScanner(inputs, uses, FunctionID(index)).run());
    });
    std::span<const FunctionUses> functions = usesOfFunctions.span();
    Vector<StoreByNameUse> stores;
    Vector<StoreOfUndefined> storesOfUndefined;
    runTasksInParallel(loops, {
        [&] {
            uses.accesses = listsOfAll(functions, &FunctionUses::accesses);
            parallelSort(loops, uses.accesses.mutableSpan(), accessIsOrderedBefore);
            foldEqualAccesses(uses.accesses);
            moveToExactBuffer(uses.accesses);
        },
        [&] { stores = mergeStoresByName(loops, functions); },
        [&] { storesOfUndefined = mergeStoresOfUndefined(loops, functions); },
        [&] { findRowsStoredTogether(context, functions, uses.rowsStoredTogether); },
        [&] { markVariables(inputs.runsBeforeEntry, functions, uses.variables.mutableSpan()); },
        [&] { markFunctionsWhoseNewClosuresAreStoredInto(functions, uses.functionsWhoseNewClosuresAreStoredInto); },
        [&] { countStoresOfFactories(context, functions, uses); },
        [&] { addUpCreations(context, functions, uses); },
        [&] {
            for (const FunctionUses& function : functions) {
                for (auto [row, heat] : function.fills)
                    uses.rows[row].fillHeat += heat;
            }
        },
        [&] { addUpFillsBesideCreation(context, functions, uses); },
    });
    distributeToRows(context, uses, functions, stores.span(), storesOfUndefined.span());
    context.build.destroyLater(WTF::move(usesOfFunctions));
    return uses;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
