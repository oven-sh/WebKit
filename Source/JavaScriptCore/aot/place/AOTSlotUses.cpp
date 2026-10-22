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
#include "AOTSchemaWalkers.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>
#include <wtf/Atomics.h>
#include <wtf/HashSet.h>
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet environmentFieldOpcodes { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
static constexpr OpcodeSet storesByName { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet allocationOpcodes { Opcode::NewObject, Opcode::CloneAs, Opcode::MaterializeEnvironment };

namespace {

// A parameter of the entry block in which a function is handed an object that its constructor is still filling.
struct ObjectUnderConstruction {
    uint32_t parameterIndex { 0 };
    LayoutID layout;

    friend bool operator==(const ObjectUnderConstruction&, const ObjectUnderConstruction&) = default;
};

struct ScanInputs {
    const SlotPlanContext& context;
    BitVector zeroEncodingCandidates;
    BitVector runsBeforeEntry;
    // By layout: the compiled objects whose class the source puts below the class of the layout, at any depth.
    Vector<Vector<LayoutID>> layoutsNamedBelow;
    // By function.
    Vector<Vector<ObjectUnderConstruction, 1>> objectsUnderConstruction;
};

struct SharedCounters {
    std::span<SlotRowUse> rows;
    std::span<LayoutUse> layouts;
    std::span<SiteWeight> keyWeights;
};

// It says that the field exists and fills nothing.
struct StoreOfUndefined {
    PropertyKeyID key;
    LayoutSetID receivers;
    uint32_t sites { 0 };
};

// A function stores into a field of nearly every object that it makes at one place, or that it gets from a function that has just made it.
struct FillBesideCreation {
    // Where the function sees the object made.
    LayoutID layout;
    // Otherwise: the function that returned it.
    FunctionID maker;
    PropertyKeyID key;
    bool storesUndefined { false };
    // How often such an object is made there.
    double heat { 0 };
};

// Where the object that a function returns comes from.
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

struct Creation {
    LayoutID layout;
    double heat { 0 };
    bool isInLoop { false };
};

struct FunctionUses {
    FunctionID function;
    // In heat without an upper limit, which is added up in the order of the functions: sums of floating-point numbers depend on their order.
    Vector<Creation> creations;
    // One entry for each place outside the constructor of a layout that makes an object of it: an allocation, or a construction by name. With how often it runs.
    Vector<std::pair<LayoutID, double>> placesThatMake;
    Vector<FunctionID> functionsCalledByName;
    Vector<FunctionID> functionsCalledByNameInLoops;
    Vector<std::pair<SlotRow, double>> fills;
    Vector<FillBesideCreation> fillsBesideCreation;
    // Empty where the function may return something else than an object made during the call.
    Vector<SourceOfResult, 1> sourcesOfResult;
    Vector<SlotAccessUse> accesses;
    Vector<StoreByNameUse> storesByName;
    Vector<FunctionID> functionsWhoseNewClosuresAreStoredInto;
    Vector<FunctionID> functionsWithLentClosures;
    Vector<StoreOfUndefined> storesOfUndefined;
    Vector<KeysStoredTogether> keysStoredTogether;
    Vector<VariableID> variablesNamed;
    Vector<VariableID> variablesWithUncheckedReads;
    Vector<VariableID> variablesUsedBeforeProgramEntry;
};

struct Site {
    BlockID block;
    const Value& value;
};

// An object that a function is the first to fill.
struct FreshObject {
    // None for what a call returns whose callee does not say by itself what it makes.
    LayoutID layout;
    FunctionID maker;
    // The allocation or the call. None for the object that the function was called to construct.
    ValueID allocation;
    // It runs once for each such object on its way to the store.
    BlockID block;
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

// The host enters a module body through a closure, and creating a closure outside the runtime modules fills the statics.
static bool isModuleBodyOrItsEntrypoint(const Function& function)
{
    return function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
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
        if ((function.mayBeCalledByHost() && !isModuleBodyOrItsEntrypoint(function)) || isInRuntimeModule(program, function))
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

namespace {

// `constructor() { this.update = options => { this.options = options; }; this.update({}); }`, `constructor() { this.reset(); }`: what a constructor calls at nearly every activation
// with the object as receiver, as argument, or in a closure over it that the constructor has made, goes on filling the object.
class ConstructionFollower {
public:
    ConstructionFollower(const SlotPlanContext& context, FunctionID id, std::span<const ObjectUnderConstruction> handedOver)
        : m_program(context.program)
        , m_function(m_program.at(id))
        , m_handedOver(handedOver)
        , m_analyses({ context.build, m_program, &context.inputs.flow, nullptr, id }, m_function)
    {
    }

    void run(NOESCAPE const Invocable<void(FunctionID, ObjectUnderConstruction)> auto& handOver)
    {
        struct ClosureOverObject {
            FunctionID function;
            ObjectUnderConstruction object;
        };
        Vector<ClosureOverObject, 8> closures;
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        double frequencyOfEntry = frequencies.frequencyOf(entryBlock);
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            bool runsNearlyAlways = frequencies.frequencyOf(block) * 2 > frequencyOfEntry;
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (value.opcode == Opcode::NewClosure) {
                    std::span<const ValueID> captures = value.capturesOperands();
                    for (uint32_t index = 0; index < captures.size(); ++index) {
                        if (LayoutID layout = layoutUnderConstruction(captures[index]))
                            closures.append({ value.function(), { index, layout } });
                    }
                    continue;
                }
                if (value.opcode != Opcode::CallDirect || value.isConstruction() || !runsNearlyAlways)
                    continue;
                std::span<const ValueID> arguments = value.argumentsOperands();
                for (uint32_t index = 0; index < arguments.size(); ++index) {
                    if (LayoutID layout = layoutUnderConstruction(arguments[index]))
                        handOver(value.function(), { index, layout });
                }
                for (const ClosureOverObject& closure : closures) {
                    if (closure.function == value.function())
                        handOver(closure.function, closure.object);
                }
            }
        }
    }

private:
    LayoutID layoutUnderConstruction(ValueID value)
    {
        Vector<ValueID, 4> visitedParameters;
        return layoutUnderConstruction(value, visitedParameters);
    }

    // A test that passes hands the object on as a parameter of the block behind it.
    LayoutID layoutUnderConstruction(ValueID value, Vector<ValueID, 4>& visitedParameters)
    {
        ValueID root = definitionThroughSameValues(m_function, value);
        const Value& definition = m_function.at(root);
        LayoutID constructed = m_function.constructedClass;
        if (definition.opcode == Opcode::NewObject)
            return constructed && m_function.kind == FunctionKind::BaseConstructor && definition.layout() == constructed ? constructed : LayoutID();
        if (definition.opcode != Opcode::Parameter || visitedParameters.contains(root))
            return { };
        visitedParameters.append(root);

        ValueLocation location = m_analyses.useLists().locationOf(root);
        if (location.block == entryBlock) {
            if (constructed && m_function.thisParameterIndex() == location.index)
                return constructed;
            auto found = std::ranges::find(m_handedOver, location.index, &ObjectUnderConstruction::parameterIndex);
            return found == m_handedOver.end() ? LayoutID() : found->layout;
        }
        LayoutID layout;
        for (Edge edge : m_analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(m_function, edge, location.index);
            LayoutID layoutOnEdge = passed ? layoutUnderConstruction(passed, visitedParameters) : LayoutID();
            if (!layoutOnEdge || (layout && layout != layoutOnEdge))
                return { };
            layout = layoutOnEdge;
        }
        return layout;
    }

    const Program& m_program;
    const Function& m_function;
    std::span<const ObjectUnderConstruction> m_handedOver;
    FunctionAnalyses m_analyses;
};

} // anonymous namespace

static Vector<Vector<ObjectUnderConstruction, 1>> findObjectsUnderConstruction(const SlotPlanContext& context)
{
    struct HandOver {
        FunctionID callee;
        ObjectUnderConstruction object;
    };
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
            ConstructionFollower(context, function, objects[function.index()].span()).run([&](FunctionID callee, ObjectUnderConstruction object) {
                collected.append({ callee, object });
            });
        });
        functionsToVisit.shrink(0);
        for (const HandOver& handOver : handOvers) {
            Vector<ObjectUnderConstruction, 1>& known = objects[handOver.callee.index()];
            const Function& callee = program.at(handOver.callee);
            if (!callee.hasBody() || callee.constructedClass || std::ranges::contains(known, handOver.object.parameterIndex, &ObjectUnderConstruction::parameterIndex))
                continue;
            known.append(handOver.object);
            if (!functionsToVisit.contains(handOver.callee) && callsDirectly(handOver.callee))
                functionsToVisit.append(handOver.callee);
        }
    }
    return objects;
}

// Equal accesses are added up in the order of their heat, so that the sum does not depend on who found which.
static bool isOrderedBefore(const SlotAccessUse& a, const SlotAccessUse& b)
{
    return a.access != b.access ? a.access < b.access : a.fillHeat < b.fillHeat;
}

static void foldEqualAccesses(Vector<SlotAccessUse>& sorted)
{
    size_t count = 0;
    for (const SlotAccessUse& use : sorted) {
        if (!count || sorted[count - 1].access != use.access) {
            sorted[count++] = use;
            continue;
        }
        SlotAccessUse& folded = sorted[count - 1];
        folded.sites += use.sites;
        folded.weight = saturatedSum(folded.weight, use.weight);
        folded.fillHeat += use.fillHeat;
        folded.storesWithoutAllocation += use.storesWithoutAllocation;
        folded.storesIntoFilledSlots += use.storesIntoFilledSlots;
        folded.storesIntoNewObjects += use.storesIntoNewObjects;
    }
    sorted.shrink(count);
}

namespace {

class FunctionScanner {
public:
    FunctionScanner(const ScanInputs&, const SharedCounters&, FunctionID);

    FunctionUses run();

private:
    bool isUndefinedConstant(ValueID) const;
    BlockID blockOf(ValueID);
    bool collectFreshObjects(ValueID, BlockID join, Vector<ValueID, 4>& visitedParameters, Vector<FreshObject, 4>&);
    void findSourcesOfResult();
    void countAllocation(LayoutID, BlockID);
    void scanCallByName(const Site&);

    void scanValue(const Site&);
    void scanSlotRow(const Site&);
    void noteStoreBehindStore(ValueID object, const Value& store, SiteWeight);
    void scanStoreByName(const Site&);
    void scanStoreOfConstructor(const Site&);
    void scanPrivateDefinition(const Site&);
    void countAllocationsByRunTime();
    void scanEnvironmentField(const Value&);
    void scanAllocation(const Site&);
    void scanInitialFields(const Value&, BlockID);
    void scanData(const Site&);
    void scanDataObject(const DataNode::Object&, BlockID);
    void scanKeysLookedUpByName(const Site&);
    void scanVariables(const Value&);

    const ScanInputs& m_inputs;
    const SharedCounters& m_counters;
    const Program& m_program;
    const SlotRows& m_rows;
    FunctionID m_id;
    const Function& m_function;
    ValueID m_thisParameter;
    FunctionAnalyses m_analyses;
    FunctionSiteWeights m_weights;
    FunctionUses m_uses;
    Vector<BlockID> m_blockOfValue;
    struct LastStore {
        PropertyKeyID key;
        LayoutSetID receivers;
        SiteWeight weight { 0 };
    };
    UncheckedKeyHashMap<uint32_t, LastStore, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lastStoreInto;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_fieldsFilledBesideAllocation;
    bool m_readsZeroEncodingCandidate { false };
};

FunctionScanner::FunctionScanner(const ScanInputs& inputs, const SharedCounters& counters, FunctionID id)
    : m_inputs(inputs)
    , m_counters(counters)
    , m_program(inputs.context.program)
    , m_rows(inputs.context.rows)
    , m_id(id)
    , m_function(m_program.at(id))
    , m_thisParameter(thisParameterOf(m_function))
    , m_analyses({ inputs.context.build, m_program, &inputs.context.inputs.flow, nullptr, id }, m_function)
    , m_weights(m_program, inputs.context.inputs.heat, id, m_analyses)
{
}

bool FunctionScanner::isUndefinedConstant(ValueID id) const
{
    auto constant = constantThroughCasts(m_program, m_function, id);
    return constant && m_function.at(*constant).opcode == Opcode::Undefined;
}

BlockID FunctionScanner::blockOf(ValueID id)
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

// A test that passes hands the object on as a parameter of the block behind it. Where several edges meet, the objects that arrive go on together,
// so from there on how often a block runs says the same about each of them. Says whether the value can be nothing else than the objects found.
bool FunctionScanner::collectFreshObjects(ValueID value, BlockID join, Vector<ValueID, 4>& visitedParameters, Vector<FreshObject, 4>& objects)
{
    ValueID root = definitionThroughSameValues(m_function, value);
    const Value& definition = m_function.at(root);
    auto addObjectOf = [&](LayoutID layout, FunctionID maker) {
        objects.append({ layout, maker, root, join ? join : blockOf(root) });
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
        if (CalleeSetID callees = definition.callees()) {
            for (FunctionID callee : m_program.at(callees).functions) {
                if (LayoutID constructed = m_program.at(callee).constructedClass)
                    addObjectOf(constructed, { });
            }
        }
        return objects.size() != numberOfOtherObjects;
    }
    default:
        break;
    }
    if (LayoutID constructed = m_function.constructedClass; constructed && root == m_thisParameter) {
        objects.append({ constructed, { }, { }, join ? join : entryBlock });
        return true;
    }
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
        if (found == handedOver.end())
            return false;
        objects.append({ found->layout, { }, { }, join ? join : entryBlock });
        return true;
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

void FunctionScanner::findSourcesOfResult()
{
    if (m_function.results.size() != 1 || !m_program.at(m_function.results[0].type).layouts)
        return;
    Vector<FreshObject, 4> objects;
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
    for (const FreshObject& object : objects)
        m_uses.sourcesOfResult.append({ object.layout, object.maker });
}

void FunctionScanner::countAllocation(LayoutID layout, BlockID block)
{
    LayoutUse& use = m_counters.layouts[layout.index()];
    add(use.allocationSites, 1);
    m_uses.creations.append({ layout, m_weights.heatOf(block), m_weights.isInLoop(block) });
}

// A function that is written to be called with and without `new` constructs itself once where `this` is not its own: that is no recursion.
void FunctionScanner::scanCallByName(const Site& site)
{
    FunctionID callee = site.value.function();
    if (LayoutID constructed = m_program.at(callee).constructedClass; constructed && site.value.isConstruction())
        m_uses.placesThatMake.append({ constructed, m_weights.heatOf(site.block) });
    bool isClassConstructor = m_function.kind == FunctionKind::BaseConstructor || m_function.kind == FunctionKind::DerivedConstructor;
    if (callee == m_id && site.value.isConstruction() && !isClassConstructor)
        return;
    (m_weights.isInLoop(site.block) ? m_uses.functionsCalledByNameInLoops : m_uses.functionsCalledByName).append(callee);
}

void FunctionScanner::scanSlotRow(const Site& site)
{
    const Value& value = site.value;
    SlotAccessUse use { { value.key(), value.receivers(), isSlotStore(value.opcode) }, 1, m_weights.weightOf(site.block) };
    if (std::optional<uint8_t> storedOperand = storedOf(value.opcode)) {
        ValueID stored = value.operand(*storedOperand);
        if (isUndefinedConstant(stored))
            m_uses.storesOfUndefined.append({ value.key(), value.receivers(), 1 });
        else
            use.fillHeat = m_weights.heatOf(site.block);
        // PutSlot declares no allocation, so it can neither create nor grow an Extension. Behind a proof that the slot is filled it finds the Extension that the creating store left.
        bool cannotAllocate = value.opcode == Opcode::PutSlot;
        use.storesIntoFilledSlots = cannotAllocate && guardProvesSlotFilled(m_analyses, value);
        use.storesWithoutAllocation = cannotAllocate && !use.storesIntoFilledSlots;
        ValueID objectID = definitionThroughSameValues(m_function, value.objectOperand());
        const Value& object = m_function.at(objectID);
        std::span<const LayoutID> receivers = m_program.at(value.receivers()).members.span();
        use.storesIntoNewObjects = object.opcode == Opcode::NewObject && receivers.size() == 1 && receivers[0] == object.layout();
        noteStoreBehindStore(objectID, value, use.weight);
    }
    m_uses.accesses.append(use);
}

// In the order of the blocks. The rarer of the two stores says how often both run.
void FunctionScanner::noteStoreBehindStore(ValueID object, const Value& store, SiteWeight weight)
{
    LastStore current { store.key(), store.receivers(), weight };
    auto result = m_lastStoreInto.add(object.index(), current);
    if (result.isNewEntry)
        return;
    LastStore last = std::exchange(result.iterator->value, current);
    if (last.receivers == current.receivers && last.key != current.key)
        m_uses.keysStoredTogether.append({ std::min(last.key, current.key), std::max(last.key, current.key), current.receivers, std::min(last.weight, weight) });
}

void FunctionScanner::scanStoreByName(const Site& site)
{
    const Value& store = site.value;
    if (std::optional<uint8_t> receiver = receiverOf(store.opcode)) {
        const Value& definition = m_function.at(definitionThroughSameValues(m_function, store.operand(*receiver)));
        if (definition.opcode == Opcode::NewClosure)
            m_uses.functionsWhoseNewClosuresAreStoredInto.append(definition.function());
    }
    LayoutSetID receivers = store.receivers();
    if (!receivers)
        return;
    std::optional<uint8_t> stored = storedOf(store.opcode);
    if (stored && isUndefinedConstant(store.operand(*stored)))
        m_uses.storesOfUndefined.append({ store.key(), receivers, 1 });
    else
        m_uses.storesByName.append({ store.key(), receivers, m_weights.heatOf(site.block) });
    if (std::optional<uint8_t> receiver = receiverOf(store.opcode))
        noteStoreBehindStore(definitionThroughSameValues(m_function, store.operand(*receiver)), store, m_weights.weightOf(site.block));
}

// What a function stores into nearly every object that it is called to construct, that it allocates itself, or that it gets from the function that has made it. The blocks of the function are compared.
// Every object of a class passes its constructor. Any other place is one of several that make objects of the layout, so it counts by its heat among them.
void FunctionScanner::scanStoreOfConstructor(const Site& site)
{
    const Value& store = site.value;
    std::optional<uint8_t> receiver = receiverOf(store.opcode);
    std::optional<uint8_t> stored = storedOf(store.opcode);
    if (!receiver || !stored || !Value::opcodesWithKey.contains(store.opcode) || !store.key())
        return;

    Vector<ValueID, 4> visitedParameters;
    Vector<FreshObject, 4> objects;
    collectFreshObjects(store.operand(*receiver), { }, visitedParameters, objects);
    if (objects.isEmpty())
        return;

    bool storesUndefined = isUndefinedConstant(store.operand(*stored));
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    double frequencyOfStore = frequencies.frequencyOf(site.block);
    for (FreshObject object : objects) {
        if (!storesUndefined && frequencyOfStore * 2 <= frequencies.frequencyOf(object.block))
            continue;
        if (object.allocation) {
            if (storesUndefined || m_fieldsFilledBesideAllocation.add(static_cast<uint64_t>(object.allocation.index()) << 32 | store.key().index()).isNewEntry)
                m_uses.fillsBesideCreation.append({ object.layout, object.maker, store.key(), storesUndefined, m_weights.heatOf(blockOf(object.allocation)) });
            continue;
        }
        auto countStoreInto = [&](LayoutID layout) {
            std::optional<uint32_t> field = m_program.at(layout).findField(store.key());
            if (!field)
                return;
            SlotRowUse& use = m_counters.rows[m_rows.rowOf(layout, *field)];
            add(storesUndefined ? use.storesOfUndefined : use.storesOfConstructorWithValue, 1);
        };
        countStoreInto(object.layout);
        // The objects of a class below pass this constructor too: `Parent.call(this)`, super(). A field that flow has added to their layout alone is inherited from nobody.
        for (LayoutID below : m_inputs.layoutsNamedBelow[object.layout.index()])
            countStoreInto(below);
    }
}

void FunctionScanner::scanPrivateDefinition(const Site& site)
{
    SlotRow row = m_rows.rowOf(site.value.layout(), site.value.field());
    add(m_counters.rows[row].privateDefinitionsByName, 1);
    m_uses.fills.append({ row, m_weights.heatOf(site.block) });
}

// The run time allocates the object of a subclass and of a function called with new, once per activation of the constructor.
void FunctionScanner::countAllocationsByRunTime()
{
    LayoutID constructed = m_function.constructedClass;
    if (!constructed || m_program.at(constructed).constructor != m_id || m_function.kind == FunctionKind::BaseConstructor)
        return;
    m_uses.creations.append({ constructed, m_weights.heatOf(BlockID(0u)), false });
}

void FunctionScanner::scanEnvironmentField(const Value& value)
{
    add(m_counters.rows[m_rows.rowOf(value.layout(), value.field())].environmentUses, 1);
}

void FunctionScanner::scanInitialFields(const Value& allocation, BlockID block)
{
    LayoutID layout = allocation.layout();
    auto fields = allocation.fields();
    auto initialValues = allocation.variadicOperands();
    for (uint32_t i = 0; i < fields.size(); ++i) {
        SlotRow row = m_rows.rowOf(layout, fields[i]);
        ValueID initialValue = initialValues[i];
        add(m_counters.rows[row].initializingSites, 1);
        if (isUndefinedConstant(initialValue))
            continue;
        add(m_counters.rows[row].sitesInitializingWithValue, 1);
        m_uses.fills.append({ row, m_weights.heatOf(block) });
    }
}

void FunctionScanner::scanAllocation(const Site& site)
{
    const Value& value = site.value;
    countAllocation(value.layout(), site.block);
    if (value.opcode == Opcode::NewObject && m_function.constructedClass != value.layout())
        m_uses.placesThatMake.append({ value.layout(), m_weights.heatOf(site.block) });
    if (value.opcode != Opcode::MaterializeEnvironment)
        scanInitialFields(value, site.block);
}

void FunctionScanner::scanDataObject(const DataNode::Object& object, BlockID block)
{
    if (!object.layout)
        return;
    countAllocation(object.layout, block);
    const Layout& layout = m_program.at(object.layout);
    for (const DataMember& member : object.members) {
        std::optional<uint32_t> field = layout.findField(member.key);
        if (!field)
            continue;
        SlotRow row = m_rows.rowOf(object.layout, *field);
        add(m_counters.rows[row].initializingSites, 1);
        if (!member.value.is<DataNode::Undefined>()) {
            add(m_counters.rows[row].sitesInitializingWithValue, 1);
            m_uses.fills.append({ row, m_weights.heatOf(block) });
        }
    }
}

void FunctionScanner::scanData(const Site& site)
{
    forEachDataNodeInPreorder(m_program.at(site.value.data()).root, [&](uint32_t, const DataNode& node) {
        if (const DataNode::Object* object = node.tryAs<DataNode::Object>())
            scanDataObject(*object, site.block);
    });
}

void FunctionScanner::scanKeysLookedUpByName(const Site& site)
{
    SiteWeight weight = m_weights.weightOf(site.block);
    forEachPropertyKey(site.value, [&](PropertyKeyID key) {
        addSaturated(m_counters.keyWeights[key.index()], weight);
    });
}

void FunctionScanner::scanVariables(const Value& value)
{
    forEachVariable(value, [&](VariableID variable) {
        m_uses.variablesNamed.append(variable);
    });
    if (value.opcode == Opcode::GetModuleVariable && m_inputs.zeroEncodingCandidates.quickGet(value.variable().index()))
        m_readsZeroEncodingCandidate = true;
}

void FunctionScanner::scanValue(const Site& site)
{
    Opcode opcode = site.value.opcode;
    if (Value::opcodesWithFieldAccess.contains(opcode))
        scanSlotRow(site);
    else if (siteCachesOf(opcode))
        scanKeysLookedUpByName(site);
    if (storesByName.contains(opcode))
        scanStoreByName(site);
    if (storesByName.contains(opcode) || isSlotStore(opcode))
        scanStoreOfConstructor(site);
    if (environmentFieldOpcodes.contains(opcode))
        scanEnvironmentField(site.value);
    if (opcode == Opcode::PrivateDefine)
        scanPrivateDefinition(site);
    if (allocationOpcodes.contains(opcode))
        scanAllocation(site);
    if (opcode == Opcode::NewClosure && site.value.isLent())
        m_uses.functionsWithLentClosures.append(site.value.function());
    if (opcode == Opcode::CallDirect)
        scanCallByName(site);
    if (opcode == Opcode::Construct) {
        if (CalleeSetID callees = site.value.callees()) {
            for (FunctionID callee : m_program.at(callees).functions) {
                if (LayoutID constructed = m_program.at(callee).constructedClass)
                    m_uses.placesThatMake.append({ constructed, m_weights.heatOf(site.block) });
            }
        }
    }
    if (Value::opcodesWithData.contains(opcode))
        scanData(site);
    scanVariables(site.value);
}

FunctionUses FunctionScanner::run()
{
    m_uses.function = m_id;
    countAllocationsByRunTime();
    findSourcesOfResult();
    for (unsigned i = 0; i < m_function.blocks.size(); ++i) {
        for (ValueID id : m_function.blocks[i].values)
            scanValue({ BlockID(i), m_function.at(id) });
    }
    if (m_readsZeroEncodingCandidate)
        findUncheckedVariableReads(m_analyses, m_inputs.zeroEncodingCandidates, m_uses.variablesWithUncheckedReads);
    std::ranges::sort(m_uses.accesses, isOrderedBefore);
    foldEqualAccesses(m_uses.accesses);
    std::ranges::sort(m_uses.variablesNamed);
    removeRepeatedElements(m_uses.variablesNamed);
    if (m_inputs.runsBeforeEntry.quickGet(m_id.index()))
        m_uses.variablesUsedBeforeProgramEntry = m_uses.variablesNamed;
    return WTF::move(m_uses);
}

} // anonymous namespace

static void mergeAccesses(const ParallelLoops& loops, std::span<const FunctionUses> functions, Vector<SlotAccessUse>& accesses)
{
    size_t count = 0;
    for (const FunctionUses& function : functions)
        count += function.accesses.size();
    accesses.reserveInitialCapacity(count);
    for (const FunctionUses& function : functions)
        accesses.appendVector(function.accesses);
    parallelSort(loops, accesses.mutableSpan(), isOrderedBefore);
    foldEqualAccesses(accesses);
    moveToExactBuffer(accesses);
}

static void mergeStoresByName(const ParallelLoops& loops, std::span<const FunctionUses> functions, Vector<StoreByNameUse>& stores)
{
    for (const FunctionUses& function : functions)
        stores.appendVector(function.storesByName);
    parallelSort(loops, stores.mutableSpan(), [](const StoreByNameUse& a, const StoreByNameUse& b) {
        return std::tuple(a.key, a.receivers, a.heat) < std::tuple(b.key, b.receivers, b.heat);
    });
    size_t count = 0;
    for (const StoreByNameUse& store : stores) {
        if (count && stores[count - 1].key == store.key && stores[count - 1].receivers == store.receivers)
            stores[count - 1].heat += store.heat;
        else
            stores[count++] = store;
    }
    stores.shrink(count);
}

static auto placeOf(const StoreOfUndefined& store)
{
    return std::tuple(store.key, store.receivers);
}

static void mergeStoresOfUndefined(const ParallelLoops& loops, std::span<const FunctionUses> functions, Vector<StoreOfUndefined>& stores)
{
    for (const FunctionUses& function : functions)
        stores.appendVector(function.storesOfUndefined);
    parallelSort(loops, stores.mutableSpan(), [](const StoreOfUndefined& a, const StoreOfUndefined& b) {
        return placeOf(a) < placeOf(b);
    });
    size_t count = 0;
    for (const StoreOfUndefined& store : stores) {
        if (!count || placeOf(stores[count - 1]) != placeOf(store)) {
            stores[count++] = store;
            continue;
        }
        stores[count - 1].sites += store.sites;
    }
    stores.shrink(count);
}

static void findRowsStoredTogether(const SlotPlanContext& context, std::span<const FunctionUses> functions, Vector<RowsStoredTogether>& rows)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    Vector<KeysStoredTogether> keys;
    for (const FunctionUses& function : functions)
        keys.appendVector(function.keysStoredTogether);
    parallelSort(loops, keys.mutableSpan(), [](const KeysStoredTogether& a, const KeysStoredTogether& b) {
        return a.place() < b.place();
    });
    size_t count = 0;
    for (const KeysStoredTogether& stored : keys) {
        if (!count || keys[count - 1].place() != stored.place()) {
            keys[count++] = stored;
            continue;
        }
        keys[count - 1].weight = saturatedSum(keys[count - 1].weight, stored.weight);
    }
    keys.shrink(count);

    for (const KeysStoredTogether& stored : keys) {
        for (LayoutID member : program.at(stored.receivers).members) {
            if (!context.isCompiledObject(member))
                continue;
            std::optional<uint32_t> first = program.at(member).findField(stored.first);
            std::optional<uint32_t> second = program.at(member).findField(stored.second);
            if (first && second)
                rows.append({ context.rows.rowOf(member, *first), context.rows.rowOf(member, *second), stored.weight });
        }
    }
    auto placeOf = [](const RowsStoredTogether& stored) {
        return std::pair(stored.first, stored.second);
    };
    parallelSort(loops, rows.mutableSpan(), [&](const RowsStoredTogether& a, const RowsStoredTogether& b) {
        return placeOf(a) < placeOf(b);
    });
    count = 0;
    for (const RowsStoredTogether& stored : rows) {
        if (!count || placeOf(rows[count - 1]) != placeOf(stored)) {
            rows[count++] = stored;
            continue;
        }
        rows[count - 1].weight = saturatedSum(rows[count - 1].weight, stored.weight);
    }
    rows.shrink(count);
    parallelSort(loops, rows.mutableSpan(), [&](const RowsStoredTogether& a, const RowsStoredTogether& b) {
        return a.weight != b.weight ? a.weight > b.weight : placeOf(a) < placeOf(b);
    });
}

static void markVariables(std::span<const FunctionUses> functions, std::span<VariableUse> variables)
{
    for (const FunctionUses& function : functions) {
        for (VariableID variable : function.variablesNamed)
            variables[variable.index()].isNamedByCode = true;
        for (VariableID variable : function.variablesWithUncheckedReads)
            variables[variable.index()].hasUncheckedRead = true;
        for (VariableID variable : function.variablesUsedBeforeProgramEntry)
            variables[variable.index()].isUsedBeforeProgramEntry = true;
    }
}

// A closure that lies in the frame of whoever lends it has the slim form.
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

static void forEachHolder(const SlotPlanContext& context, PropertyKeyID key, LayoutSetID receivers, NOESCAPE const Invocable<void(LayoutID, SlotRow)> auto& functor)
{
    const Program& program = context.program;
    for (LayoutID member : program.at(receivers).members) {
        if (!context.isCompiledObject(member))
            continue;
        if (std::optional<uint32_t> field = program.at(member).findField(key))
            functor(member, context.rows.rowOf(member, *field));
    }
}

// The objects that reach a store are taken to be of its receivers in the proportion in which those are made.
static void distributeFillHeat(const SlotPlanContext& context, SlotUses& uses, PropertyKeyID key, LayoutSetID receivers, double heat)
{
    Vector<std::pair<SlotRow, double>, 8> holders;
    double made = 0;
    forEachHolder(context, key, receivers, [&](LayoutID member, SlotRow row) {
        holders.append({ row, uses.layouts[member.index()].creationHeat });
        made += holders.last().second;
    });
    if (!made)
        return;
    for (auto [row, madeOfHolder] : holders)
        uses.rows[row].fillHeat += heat * (madeOfHolder / made);
}

// The layout of the object that every call of a function makes and returns, by function. None where it may return something else, or objects of several layouts.
static Vector<LayoutID> findLayoutsOfFreshResults(const Program& program, std::span<const FunctionUses> functions)
{
    enum class State : uint8_t { Unknown, Visiting, Known };
    Vector<LayoutID> layouts(program.functions.size());
    Vector<State> states(FillWith { }, program.functions.size(), State::Known);
    Vector<const FunctionUses*> usesOf(FillWith { }, program.functions.size(), nullptr);
    for (const FunctionUses& uses : functions) {
        if (uses.sourcesOfResult.isEmpty())
            continue;
        usesOf[uses.function.index()] = &uses;
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

// A function that is called by name from a place in a loop, from a function that calls itself again by name, or from such a function, and a callback that a built-in calls for each element.
// Such code may fill an array with any number of objects, however seldom it is taken to run.
static BitVector findFunctionsThatRunInLoops(const SlotPlanContext& context, std::span<const FunctionUses> functions)
{
    unsigned numberOfFunctions = context.program.functions.size();
    Vector<const FunctionUses*> usesOf;
    usesOf.fill(nullptr, numberOfFunctions);
    for (const FunctionUses& uses : functions)
        usesOf[uses.function.index()] = &uses;
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
        if (call.times == BuiltinCallbackTimes::PerElement && call.callee.index() < numberOfFunctions)
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
    // A factory stores what the objects that it makes get as a constructor does. Each place that makes counts with its own heat, and so does a place that goes on to fill: they are one place, on one scale,
    // which the heat of a constructor and that of the function that calls it are not. The arm of a construction that seldom runs, and a second factory that makes a few objects without the field, weigh little.
    // Room in the cell that every other object leaves empty costs less than an Extension for the others.
    Vector<double> heatOfPlacesThatMake(FillWith { }, program.layouts.size(), 0.0);
    Vector<double> heatOfPlacesThatFill(FillWith { }, uses.rows.size(), 0.0);
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
        if (!heatOfPlacesThatMake[index] || !context.isCompiledObject(layout))
            continue;
        for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(layout); ++field) {
            SlotRow row = context.rows.rowOf(layout, field);
            if (heatOfPlacesThatFill[row] * 2 >= heatOfPlacesThatMake[index])
                ++uses.rows[row].storesOfConstructorWithValue;
        }
    }
}

static void addUpCreations(const SlotPlanContext& context, std::span<const FunctionUses> functions, SlotUses& uses)
{
    BitVector runsInLoop = findFunctionsThatRunInLoops(context, functions);
    for (const FunctionUses& function : functions) {
        for (const Creation& creation : function.creations) {
            LayoutUse& use = uses.layouts[creation.layout.index()];
            use.creationHeat += creation.heat;
            use.isMadeInLoop |= creation.isInLoop || runsInLoop.quickGet(function.function.index());
        }
    }
}

static void addUpFills(std::span<const FunctionUses> functions, SlotUses& uses)
{
    for (const FunctionUses& function : functions) {
        for (auto [row, heat] : function.fills)
            uses.rows[row].fillHeat += heat;
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

static void distributeToRows(const SlotPlanContext& context, SlotUses& uses, std::span<const StoreOfUndefined> storesOfUndefined)
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
            SlotRow row = context.rows.rowOf(id, field);
            SlotRowUse& use = rows[row];
            use.holderHeat = uses.layouts[index].creationHeat;
            use.hasHoldersMadeInLoop = uses.layouts[index].isMadeInLoop;
            // No more objects are filled than are made. Far more says that the two heats are not on one scale: a constructor that is called through a variable is taken to run seldom,
            // whatever the heat of the code that calls it.
            if (use.heatFilledBesideCreation && use.heatFilledBesideCreation >= use.holderHeat * 0.4999 && use.heatFilledBesideCreation <= use.holderHeat * 2)
                ++use.storesOfConstructorWithValue;
            OptionSet<FieldFlags> flags = layout.fields[field].flags;
            use.isRequiredByDeclaringType = layout.isDerived() && flags.contains(FieldFlags::IsRequired) && !flags.contains(FieldFlags::IsInherited);
            use.isOptionalInTypeOfThis = !flags.contains(FieldFlags::IsInherited) && isOptionalWhereDeclared(program, { id, field });
        }
    });
    // By one thread, in the order of the lists.
    for (const StoreByNameUse& store : uses.storesByName)
        distributeFillHeat(context, uses, store.key, store.receivers, store.heat);
    for (const SlotAccessUse& use : uses.accesses) {
        if (use.fillHeat)
            distributeFillHeat(context, uses, use.access.key, use.access.receivers, use.fillHeat);
    }
    forEachIndexInParallel(loops, storesOfUndefined.size(), [&](unsigned index) {
        const StoreOfUndefined& store = storesOfUndefined[index];
        forEachHolder(context, store.key, store.receivers, [&](LayoutID, SlotRow row) {
            add(rows[row].storesOfUndefined, store.sites);
        });
    });
    forEachIndexInParallel(loops, uses.accesses.size(), [&](unsigned index) {
        const SlotAccessUse& use = uses.accesses[index];
        forEachHolder(context, use.access.key, use.access.receivers, [&](LayoutID, SlotRow row) {
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
        },
        [&] { inputs.layoutsNamedBelow = findLayoutsNamedBelow(context); },
        [&] { inputs.objectsUnderConstruction = findObjectsUnderConstruction(context); },
        [&] { inputs.zeroEncodingCandidates = findZeroEncodingCandidates(context); },
        [&] { inputs.runsBeforeEntry = computeRunsBeforeEntry(context); },
    });
    SharedCounters counters { uses.rows.mutableSpan(), uses.layouts.mutableSpan(), uses.keyWeights.mutableSpan() };
    ParallelArray<FunctionUses> usesOfFunctions = collectIntoArrayInParallel<FunctionUses>(loops, program.functions.size(), largeFunctions(program, loops).span(), [&](unsigned index, Vector<FunctionUses>& collected) {
        if (program.functions[index].hasBody())
            collected.append(FunctionScanner(inputs, counters, FunctionID(index)).run());
    });
    std::span<const FunctionUses> functions = usesOfFunctions.span();
    Vector<StoreOfUndefined> storesOfUndefined;
    runTasksInParallel(loops, {
        [&] { mergeAccesses(loops, functions, uses.accesses); },
        [&] { mergeStoresByName(loops, functions, uses.storesByName); },
        [&] { mergeStoresOfUndefined(loops, functions, storesOfUndefined); },
        [&] { findRowsStoredTogether(context, functions, uses.rowsStoredTogether); },
        [&] { markVariables(functions, uses.variables.mutableSpan()); },
        [&] { markFunctionsWhoseNewClosuresAreStoredInto(functions, uses.functionsWhoseNewClosuresAreStoredInto); },
        [&] { countStoresOfFactories(context, functions, uses); },
        [&] { addUpCreations(context, functions, uses); },
        [&] { addUpFills(functions, uses); },
        [&] { addUpFillsBesideCreation(context, functions, uses); },
    });
    distributeToRows(context, uses, storesOfUndefined.span());
    context.build.destroyLater(WTF::move(usesOfFunctions));
    return uses;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
