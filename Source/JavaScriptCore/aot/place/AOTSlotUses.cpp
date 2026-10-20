/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTGuardFactDetails.h"
#include "AOTKnownClosures.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>
#include <wtf/Atomics.h>
#include <wtf/Int128.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet environmentFieldOpcodes { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
static constexpr OpcodeSet storesByName { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet allocationOpcodes { Opcode::NewObject, Opcode::CloneAs, Opcode::MaterializeEnvironment };
static constexpr Range inlineInt32Range { minInlineInt32, INT32_MAX };

namespace {

struct ScanInputs {
    const SlotPlanContext& context;
    BitVector zeroEncodingCandidates;
    BitVector runsBeforeEntry;
};

struct SharedCounters {
    std::span<SlotRowUse> rows;
    std::span<LayoutUse> layouts;
    std::span<SiteWeight> keyWeights;
};

struct FunctionUses {
    Vector<SlotAccessUse> accesses;
    Vector<StoreByNameUse> storesByName;
    Vector<VariableID> variablesNamed;
    Vector<VariableID> variablesWithUncheckedReads;
    Vector<VariableID> variablesUsedBeforeProgramEntry;
};

struct Site {
    BlockID block;
    ValueID id;
    const Value& value;
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

static bool isOrderedBefore(const SlotAccessUse& a, const SlotAccessUse& b)
{
    return a.access < b.access;
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
        folded.integerUses += use.integerUses;
        folded.doubleUses += use.doubleUses;
        folded.doubleStores += use.doubleStores;
        folded.rangeTestedStores += use.rangeTestedStores;
        folded.storesWithoutAllocation += use.storesWithoutAllocation;
        folded.storesIntoFilledSlots += use.storesIntoFilledSlots;
        folded.wideStoresWithoutAllocation += use.wideStoresWithoutAllocation;
    }
    sorted.shrink(count);
}

namespace {

class FunctionScanner {
public:
    FunctionScanner(const ScanInputs&, const SharedCounters&, FunctionID);

    FunctionUses run();

private:
    bool isStoredAfterRangeTest(ValueID) const;
    bool isStoredAsDoubleValue(ValueID) const;
    bool fitsInlineInt32Slot(ValueID) const;
    void countStore(SlotRow, ValueID stored);
    void countStoredNumber(SlotRow, double stored);
    void countAllocation(LayoutID, SiteWeight);

    void scanValue(const Site&);
    void scanSlotRow(const Site&);
    void scanStoreByName(const Site&);
    void scanStoreOfConstructor(const Site&);
    void countAllocationsByRunTime();
    void scanEnvironmentField(const Value&);
    void scanAllocation(const Site&);
    bool isCreatedClosureOf(FunctionID, ValueID stored);
    void countStoreIntoDeferringFields(PropertyKeyID, LayoutSetID receivers, ValueID stored);
    void scanInitialFields(const Value&, SiteWeight);
    void scanData(const Site&);
    void scanDataObject(const DataNode::Object&, SiteWeight);
    void scanKeysLookedUpByName(const Site&);
    void scanVariables(const Value&);

    const ScanInputs& m_inputs;
    const SharedCounters& m_counters;
    const Program& m_program;
    const SlotRows& m_rows;
    FunctionID m_id;
    const Function& m_function;
    FunctionAnalyses m_analyses;
    FunctionSiteWeights m_weights;
    FunctionUses m_uses;
    NumberSources m_numberSources;
    bool m_readsZeroEncodingCandidate { false };
};

FunctionScanner::FunctionScanner(const ScanInputs& inputs, const SharedCounters& counters, FunctionID id)
    : m_inputs(inputs)
    , m_counters(counters)
    , m_program(inputs.context.program)
    , m_rows(inputs.context.rows)
    , m_id(id)
    , m_function(m_program.at(id))
    , m_analyses({ inputs.context.build, m_program, &inputs.context.inputs.flow, nullptr, id }, m_function)
    , m_weights(m_program, inputs.context.inputs.heat, id, m_analyses)
{
}

bool FunctionScanner::isStoredAfterRangeTest(ValueID id) const
{
    const Type& type = m_program.typeOf(m_function, id);
    return type.isInteger() && !type.integers.isSubsetOf(inlineInt32Range);
}

bool FunctionScanner::isStoredAsDoubleValue(ValueID id) const
{
    CastChain chain(m_function, id);
    for (ValueID cast : chain) {
        if (m_program.typeOf(m_function, cast).isInteger())
            return false;
    }
    return m_program.typeOf(m_function, chain.last()).primitives & Primitive::Number;
}

bool FunctionScanner::fitsInlineInt32Slot(ValueID id) const
{
    const Type& type = m_program.typeOf(m_function, id);
    return type.hasOnlyPrimitives(Primitive::Integer | Primitive::UndefinedOrNull) && type.integers.isSubsetOf(inlineInt32Range);
}

void FunctionScanner::countStore(SlotRow row, ValueID stored)
{
    if (isStoredAsDoubleValue(stored))
        add(m_counters.rows[row].doubleStores, 1);
    if (isStoredAfterRangeTest(stored))
        add(m_counters.rows[row].rangeTestedStores, 1);
}

void FunctionScanner::countStoredNumber(SlotRow row, double stored)
{
    if (!tryConvertToStrictInt32(stored))
        add(m_counters.rows[row].doubleStores, 1);
    if (isTrackedInteger(stored) && !inlineInt32Range.contains(static_cast<int64_t>(stored)))
        add(m_counters.rows[row].rangeTestedStores, 1);
}

void FunctionScanner::countAllocation(LayoutID layout, SiteWeight weight)
{
    LayoutUse& use = m_counters.layouts[layout.index()];
    add(use.allocationSites, 1);
    addSaturated(use.allocationWeight, weight);
}

void FunctionScanner::scanSlotRow(const Site& site)
{
    const Value& value = site.value;
    SlotAccessUse use { { value.key(), value.receivers(), isSlotStore(value.opcode) }, 1, m_weights.weightOf(site.block) };
    if (std::optional<uint8_t> storedOperand = storedOf(value.opcode)) {
        ValueID stored = value.operand(*storedOperand);
        countStoreIntoDeferringFields(value.key(), value.receivers(), stored);
        use.doubleStores = isStoredAsDoubleValue(stored);
        use.rangeTestedStores = isStoredAfterRangeTest(stored);
        // PutSlot declares no allocation, so it can neither create nor grow an Extension. Behind a proof that the slot is filled it finds the Extension that the creating store left.
        bool cannotAllocate = value.opcode == Opcode::PutSlot;
        use.storesIntoFilledSlots = cannotAllocate && guardProvesSlotFilled(m_analyses, value);
        use.storesWithoutAllocation = cannotAllocate && !use.storesIntoFilledSlots;
        use.wideStoresWithoutAllocation = cannotAllocate && !fitsInlineInt32Slot(stored);
    } else if (m_program.at(value.type).isNumber() && isPredictedInt32(m_program, value.key(), value.receivers()))
        m_numberSources.add(site.id, Vector<uint32_t, 1> { }).iterator->value.append(static_cast<uint32_t>(m_uses.accesses.size()));
    m_uses.accesses.append(use);
}

void FunctionScanner::scanStoreByName(const Site& site)
{
    if (LayoutSetID receivers = site.value.receivers())
        m_uses.storesByName.append({ site.value.key(), receivers, m_weights.weightOf(site.block) });
}

void FunctionScanner::scanStoreOfConstructor(const Site& site)
{
    LayoutID constructed = m_function.constructedClass;
    const Value& store = site.value;
    std::optional<uint8_t> receiver = receiverOf(store.opcode);
    std::optional<uint8_t> stored = storedOf(store.opcode);
    std::optional<unsigned> thisIndex = m_function.thisParameterIndex();
    if (!constructed || !receiver || !stored || !thisIndex || !Value::opcodesWithKey.contains(store.opcode) || !store.key())
        return;
    const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
    if (*thisIndex >= parameters.size() || definitionThroughSameValues(m_function, store.operand(*receiver)) != parameters[*thisIndex])
        return;
    if (m_weights.weightOf(site.block) < m_weights.weightOf(entryBlock))
        return;
    auto constant = constantThroughCasts(m_program, m_function, store.operand(*stored));
    if (constant && m_function.at(*constant).opcode == Opcode::Undefined)
        return;
    if (std::optional<uint32_t> field = m_program.at(constructed).findField(store.key()))
        add(m_counters.rows[m_rows.rowOf(constructed, *field)].storesOfConstructorWithValue, 1);
}

// The run time allocates the object of a subclass and of a function called with new, once per activation of the constructor.
void FunctionScanner::countAllocationsByRunTime()
{
    LayoutID constructed = m_function.constructedClass;
    if (!constructed || m_program.at(constructed).constructor != m_id || m_function.kind == FunctionKind::BaseConstructor)
        return;
    addSaturated(m_counters.layouts[constructed.index()].allocationWeight, m_weights.weightOf(BlockID(0u)));
}

void FunctionScanner::scanEnvironmentField(const Value& value)
{
    SlotRow row = m_rows.rowOf(value.layout(), value.field());
    add(m_counters.rows[row].environmentUses, 1);
    if (value.opcode == Opcode::PutEnvironmentField)
        countStore(row, value.operand(OperandIndex::PutEnvironmentField::value));
}

// One created closure may be the value of both arms of a store, so its uses are not counted here.
bool FunctionScanner::isCreatedClosureOf(FunctionID function, ValueID stored)
{
    const Value& closure = m_function.at(definitionThroughUpcasts(m_function, stored));
    return closure.opcode == Opcode::NewClosure && closure.function() == function;
}

// Nothing is emitted for a store into a slot that defers its closure, so a store of anything else leaves the field an ordinary slot.
void FunctionScanner::countStoreIntoDeferringFields(PropertyKeyID key, LayoutSetID receivers, ValueID stored)
{
    for (LayoutID member : m_program.at(receivers).members) {
        std::optional<uint32_t> field = m_program.at(member).findField(key);
        if (!field)
            continue;
        if (FunctionID deferred = flowOf(m_program, { member, *field }).location.deferredFunction; deferred && !isCreatedClosureOf(deferred, stored))
            add(m_counters.rows[m_rows.rowOf(member, *field)].storesOfOtherThanDeferredClosure, 1);
    }
}

void FunctionScanner::scanInitialFields(const Value& allocation, SiteWeight weight)
{
    LayoutID layout = allocation.layout();
    auto fields = allocation.fields();
    auto initialValues = allocation.variadicOperands();
    for (uint32_t i = 0; i < fields.size(); ++i) {
        SlotRow row = m_rows.rowOf(layout, fields[i]);
        ValueID initialValue = initialValues[i];
        add(m_counters.rows[row].initializingSites, 1);
        countStore(row, initialValue);
        if (FunctionID deferred = flowOf(m_program, { layout, fields[i] }).location.deferredFunction; deferred && !isCreatedClosureOf(deferred, initialValue))
            add(m_counters.rows[row].storesOfOtherThanDeferredClosure, 1);
        auto constant = constantThroughCasts(m_program, m_function, initialValue);
        if (constant && m_function.at(*constant).opcode == Opcode::Undefined)
            continue;
        add(m_counters.rows[row].sitesInitializingWithValue, 1);
        addSaturated(m_counters.rows[row].fillWeight, weight);
    }
}

void FunctionScanner::scanAllocation(const Site& site)
{
    const Value& value = site.value;
    SiteWeight weight = m_weights.weightOf(site.block);
    countAllocation(value.layout(), weight);
    if (value.opcode != Opcode::MaterializeEnvironment)
        scanInitialFields(value, weight);
}

void FunctionScanner::scanDataObject(const DataNode::Object& object, SiteWeight weight)
{
    countAllocation(object.layout, weight);
    const Layout& layout = m_program.at(object.layout);
    for (const DataMember& member : object.members) {
        std::optional<uint32_t> field = layout.findField(member.key);
        if (!field)
            continue;
        SlotRow row = m_rows.rowOf(object.layout, *field);
        add(m_counters.rows[row].initializingSites, 1);
        if (!member.value.is<DataNode::Undefined>()) {
            add(m_counters.rows[row].sitesInitializingWithValue, 1);
            addSaturated(m_counters.rows[row].fillWeight, weight);
        }
        if (const DataNode::Number* number = member.value.tryAs<DataNode::Number>())
            countStoredNumber(row, number->value);
    }
}

void FunctionScanner::scanData(const Site& site)
{
    SiteWeight weight = m_weights.weightOf(site.block);
    forEachDataNodeInPreorder(m_program.at(site.value.data()).root, [&](uint32_t, const DataNode& node) {
        if (const DataNode::Object* object = node.tryAs<DataNode::Object>())
            scanDataObject(*object, weight);
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
        add(m_counters.rows[m_rows.rowOf(site.value.layout(), site.value.field())].privateDefinitionsByName, 1);
    if (allocationOpcodes.contains(opcode))
        scanAllocation(site);
    if (Value::opcodesWithData.contains(opcode))
        scanData(site);
    scanVariables(site.value);
}

FunctionUses FunctionScanner::run()
{
    countAllocationsByRunTime();
    for (unsigned i = 0; i < m_function.blocks.size(); ++i) {
        for (ValueID id : m_function.blocks[i].values)
            scanValue({ BlockID(i), id, m_function.at(id) });
    }
    if (m_readsZeroEncodingCandidate)
        findUncheckedVariableReads(m_analyses, m_inputs.zeroEncodingCandidates, m_uses.variablesWithUncheckedReads);
    if (!m_numberSources.isEmpty())
        countNumberUses(m_inputs.context, m_analyses, WTF::move(m_numberSources), m_uses.accesses);
    std::ranges::sort(m_uses.accesses, isOrderedBefore);
    foldEqualAccesses(m_uses.accesses);
    std::ranges::sort(m_uses.variablesNamed);
    removeRepeatedElements(m_uses.variablesNamed);
    if (m_inputs.runsBeforeEntry.quickGet(m_id.index()))
        m_uses.variablesUsedBeforeProgramEntry = m_uses.variablesNamed;
    return WTF::move(m_uses);
}

} // anonymous namespace

static void mergeAccesses(const ParallelLoops& loops, const Vector<FunctionUses>& functions, Vector<SlotAccessUse>& accesses)
{
    size_t count = 0;
    for (const FunctionUses& function : functions)
        count += function.accesses.size();
    accesses.reserveInitialCapacity(count);
    for (const FunctionUses& function : functions)
        accesses.appendVector(function.accesses);
    parallelSort(loops, accesses.mutableSpan(), isOrderedBefore);
    foldEqualAccesses(accesses);
    accesses.shrinkToFit();
}

static void mergeStoresByName(const ParallelLoops& loops, const Vector<FunctionUses>& functions, Vector<StoreByNameUse>& stores)
{
    for (const FunctionUses& function : functions)
        stores.appendVector(function.storesByName);
    parallelSort(loops, stores.mutableSpan(), [](const StoreByNameUse& a, const StoreByNameUse& b) {
        return std::tuple(a.key, a.receivers, a.weight) < std::tuple(b.key, b.receivers, b.weight);
    });
    size_t count = 0;
    for (const StoreByNameUse& store : stores) {
        if (count && stores[count - 1].key == store.key && stores[count - 1].receivers == store.receivers)
            stores[count - 1].weight = saturatedSum(stores[count - 1].weight, store.weight);
        else
            stores[count++] = store;
    }
    stores.shrink(count);
}

static void markVariables(const Vector<FunctionUses>& functions, std::span<VariableUse> variables)
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

// The objects that reach a store are taken to be of its receivers in the proportion in which those are allocated.
static void distributeFillWeight(const SlotPlanContext& context, SlotUses& uses, PropertyKeyID key, LayoutSetID receivers, SiteWeight weight)
{
    const Program& program = context.program;
    Vector<std::pair<SlotRow, SiteWeight>, 8> holders;
    SiteWeight allocated = 0;
    for (LayoutID member : program.at(receivers).members) {
        std::optional<uint32_t> field = context.isCompiledObject(member) ? program.at(member).findField(key) : std::nullopt;
        if (!field)
            continue;
        holders.append({ context.rows.rowOf(member, *field), uses.layouts[member.index()].allocationWeight });
        allocated = saturatedSum(allocated, holders.last().second);
    }
    if (!allocated)
        return;
    for (auto [row, allocatedOfHolder] : holders)
        addSaturated(uses.rows[row].fillWeight, static_cast<SiteWeight>(static_cast<UInt128>(weight) * allocatedOfHolder / allocated));
}

static void distributeToRows(const SlotPlanContext& context, SlotUses& uses)
{
    const Program& program = context.program;
    std::span<SlotRowUse> rows = uses.rows.mutableSpan();
    forEachIndexInParallel(context.build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!context.isCompiledObject(id))
            return;
        const Layout& layout = program.at(id);
        for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(id); ++field) {
            SlotRowUse& use = rows[context.rows.rowOf(id, field)];
            use.holderWeight = uses.layouts[index].allocationWeight;
            OptionSet<FieldFlags> flags = layout.fields[field].flags;
            use.isRequiredByDeclaringType = layout.isDerived() && flags.contains(FieldFlags::IsRequired) && !flags.contains(FieldFlags::IsInherited);
        }
    });
    forEachIndexInParallel(context.build.parallelLoops(), uses.storesByName.size(), [&](unsigned index) {
        const StoreByNameUse& store = uses.storesByName[index];
        distributeFillWeight(context, uses, store.key, store.receivers, store.weight);
    });
    forEachIndexInParallel(context.build.parallelLoops(), uses.accesses.size(), [&](unsigned index) {
        const SlotAccessUse& use = uses.accesses[index];
        if (use.access.isStore)
            distributeFillWeight(context, uses, use.access.key, use.access.receivers, use.weight);
        for (LayoutID member : program.at(use.access.receivers).members) {
            if (!context.isCompiledObject(member))
                continue;
            std::optional<uint32_t> field = program.at(member).findField(use.access.key);
            if (!field)
                continue;
            SlotRowUse& row = rows[context.rows.rowOf(member, *field)];
            addSaturated(row.accessWeight, use.weight);
            add(row.integerUses, use.integerUses);
            add(row.doubleUses, use.doubleUses);
            add(row.doubleStores, use.doubleStores);
            add(row.rangeTestedStores, use.rangeTestedStores);
            add(row.storesWithoutAllocation, use.storesWithoutAllocation);
            add(row.storesIntoFilledSlots, use.storesIntoFilledSlots);
            add(row.wideStoresWithoutAllocation, use.wideStoresWithoutAllocation);
        }
    });
}

SlotUses collectSlotUses(const SlotPlanContext& context)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    SlotUses uses;
    uses.rows.fill({ }, context.rows.count());
    uses.layouts.fill({ }, program.layouts.size());
    uses.variables.fill({ }, program.variables.size());
    uses.keyWeights.fill(0, program.types.numberOfPropertyKeys());
    ScanInputs inputs { context, findZeroEncodingCandidates(context), computeRunsBeforeEntry(context) };
    SharedCounters counters { uses.rows.mutableSpan(), uses.layouts.mutableSpan(), uses.keyWeights.mutableSpan() };
    Vector<FunctionUses> functions = collectInParallel<FunctionUses>(loops, program.functions.size(), largeFunctions(program, loops).span(), [&](unsigned index, Vector<FunctionUses>& collected) {
        if (program.functions[index].hasBody())
            collected.append(FunctionScanner(inputs, counters, FunctionID(index)).run());
    });
    runTasksInParallel(loops, {
        [&] { mergeAccesses(loops, functions, uses.accesses); },
        [&] { mergeStoresByName(loops, functions, uses.storesByName); },
        [&] { markVariables(functions, uses.variables.mutableSpan()); },
    });
    distributeToRows(context, uses);
    context.build.destroyLater(WTF::move(functions));
    return uses;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
