/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromoteEnvironmentsToVariablesPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EnvironmentAllocation {
    LayoutID layout;
    FunctionID function;
    ExecutionCount executions { ExecutionCount::Unknown };
};

struct CaptureSupplier {
    FunctionID callee;
    FunctionID function;
    ValueID site;
    BitVector capturesComputedOnce;
};

struct CaptureSource {
    explicit operator bool() const { return variable || value; }

    friend bool operator==(const CaptureSource&, const CaptureSource&) = default;

    VariableID variable;
    FunctionID function;
    ValueID value;
};

struct FunctionScan {
    bool isEmpty() const { return allocations.isEmpty() && suppliers.isEmpty() && layoutsWithOtherUses.isEmpty() && functionsWithOtherReferences.isEmpty(); }

    Vector<EnvironmentAllocation, 1> allocations;
    Vector<CaptureSupplier, 4> suppliers;
    Vector<LayoutID, 1> layoutsWithOtherUses;
    Vector<FunctionID, 1> functionsWithOtherReferences;
};

struct ProgramScan {
    Vector<EnvironmentAllocation> allocations;
    Vector<CaptureSupplier> suppliers;
    BitVector layoutsWithOtherUses;
    BitVector functionsWithOtherReferences;
};

class EnvironmentTypes {
public:
    explicit EnvironmentTypes(const Program& program)
        : m_layouts(program.types.size(), [&](size_t index) {
            return program.at(TypeID(index)).environment;
        })
    {
    }

    LayoutID layoutOf(TypeID type) const { return type ? m_layouts[type.index()] : LayoutID(); }

private:
    Vector<LayoutID> m_layouts;
};

struct ScanContext {
    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
    const EnvironmentTypes& types;
};

class FunctionScanner {
public:
    FunctionScanner(const ScanContext& context, FunctionID id)
        : m_context(context)
        , m_id(id)
        , m_function(context.program.at(id))
    {
    }

    FunctionScan run();

private:
    LayoutID environmentOf(ValueID value) const { return m_context.types.layoutOf(m_function.at(value).type); }
    ExecutionCount executionCountOf(BlockID);
    bool isComputedAtMostOnce(ValueID);

    void scanSignature();
    void scan(BlockID, ValueID);
    void scanFunctionReference(ValueID, const Value&);
    void scanOutsideValues(const Block&);
    bool isRewrittenUse(const Value& user, uint32_t operandIndex) const;

    const ScanContext& m_context;
    FunctionID m_id;
    const Function& m_function;
    std::optional<FunctionAnalyses> m_analyses;
    Vector<BlockID> m_definingBlocks;
    FunctionScan m_scan;
};

class PromotionPlanner {
public:
    PromotionPlanner(const Program&, const FlowResults&, BuildContext&);

    PromotionPlan run();

private:
    using Suppliers = std::span<const CaptureSupplier>;
    using VariablesByValue = UncheckedKeyHashMap<uint64_t, VariableID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    bool isEnabled(FunctionID function) const { return m_build.isEnabled(PassID::PromoteEnvironmentsToVariables, function); }
    bool hasFixedCaptures(FunctionID function) const { return m_hasFixedCaptures.get(m_plan.closureFunctionOf(function).index()); }
    const Variable& variableAt(VariableID) const;
    VariableID addVariable(Variable&&);
    PlannedCapture& plannedCapture(FunctionID closureFunction, uint32_t capture);
    bool isUnchangedValue(FunctionID closureFunction, uint32_t capture) const;
    bool fitsCapture(TypeID, FunctionID closureFunction, uint32_t capture) const;

    void findClosureFunctions();
    void findFixedCaptures();
    void addUsesInRecords();
    void promoteLayouts();
    void promote(const EnvironmentAllocation&);
    void removeEnvironmentCaptures();
    void promoteCapturedValues();
    bool readCapturesFromVariables(FunctionID closureFunction, Suppliers);
    CaptureSource findCommonSource(FunctionID closureFunction, uint32_t capture, Suppliers) const;
    CaptureSource findSource(const CaptureSupplier&, uint32_t capture) const;
    VariableID findStableVariable(const Function&, ValueID) const;
    TypeID typeOf(const CaptureSource&) const;
    VariableID ensureVariable(const CaptureSource&);
    bool suppliesOwnCapture(FunctionID closureFunction, const CaptureSupplier&, uint32_t capture) const;
    void forEachSuppliedFunction(NOESCAPE const Invocable<void(FunctionID, Suppliers)> auto&) const;
    void collectEditedFunctions();

    const Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    EnvironmentTypes m_types;
    ProgramScan m_scan;
    BitVector m_hasFixedCaptures;
    Vector<std::pair<FunctionID, FunctionID>> m_versions;
    Vector<FunctionID> m_allocatingFunctions;
    VariablesByValue m_variablesByValue;
    PromotionPlan m_plan;
};

} // anonymous namespace

ExecutionCount FunctionScanner::executionCountOf(BlockID block)
{
    if (!m_context.flow.runsAtMostOnce(m_context.program, m_id))
        return ExecutionCount::Unknown;
    if (!m_analyses)
        m_analyses.emplace(AnalysisContext { m_context.build, m_context.program, &m_context.flow, nullptr, m_id }, m_function);
    return m_analyses->executionCountOf(block);
}

bool FunctionScanner::isComputedAtMostOnce(ValueID value)
{
    if (!m_context.flow.runsAtMostOnce(m_context.program, m_id))
        return false;
    if (m_definingBlocks.isEmpty()) {
        m_definingBlocks.grow(m_function.values.size());
        for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
            const Block& block = m_function.blocks[index];
            for (ValueID parameter : block.parameters)
                m_definingBlocks[parameter.index()] = BlockID(index);
            for (ValueID id : block.values)
                m_definingBlocks[id.index()] = BlockID(index);
        }
    }
    BlockID block = m_definingBlocks[value.index()];
    return block && executionCountOf(block) == ExecutionCount::AtMostOnce;
}

bool FunctionScanner::isRewrittenUse(const Value& user, uint32_t operandIndex) const
{
    if (user.opcode == Opcode::GetEnvironmentField)
        return operandIndex == OperandIndex::GetEnvironmentField::environment;
    if (user.opcode == Opcode::PutEnvironmentField)
        return operandIndex == OperandIndex::PutEnvironmentField::environment;
    if (user.opcode == Opcode::NewClosure)
        return true;
    if (user.opcode == Opcode::CallDirect)
        return *user.variadicOperandIndex(operandIndex) < m_context.program.at(user.function()).captures.size();
    return false;
}

void FunctionScanner::scanSignature()
{
    auto scanPosition = [&](const DeclaredType& declared) {
        if (LayoutID layout = m_context.types.layoutOf(declared.type))
            m_scan.layoutsWithOtherUses.append(layout);
    };
    scanPosition(m_function.thisType);
    for (const DeclaredType& parameter : m_function.parameters)
        scanPosition(parameter);
    for (const DeclaredType& result : m_function.results)
        scanPosition(result);
}

void FunctionScanner::scanFunctionReference(ValueID id, const Value& value)
{
    FunctionID callee = value.function();
    if (value.opcode != Opcode::NewClosure && value.opcode != Opcode::CallDirect) {
        m_scan.functionsWithOtherReferences.append(callee);
        return;
    }
    size_t numberOfCaptures = m_context.program.at(callee).captures.size();
    if (!numberOfCaptures)
        return;

    CaptureSupplier supplier { callee, m_id, id, BitVector(numberOfCaptures) };
    std::span<const ValueID> captured = value.variadicOperands().first(numberOfCaptures);
    for (size_t capture = 0; capture < numberOfCaptures; ++capture) {
        if (isComputedAtMostOnce(captured[capture]))
            supplier.capturesComputedOnce.quickSet(capture);
    }
    m_scan.suppliers.append(WTF::move(supplier));
}

void FunctionScanner::scan(BlockID block, ValueID id)
{
    const Value& value = m_function.at(id);
    if (LayoutID layout = m_context.types.layoutOf(value.type)) {
        if (value.opcode == Opcode::NewObject)
            m_scan.allocations.append(EnvironmentAllocation { layout, m_id, executionCountOf(block) });
        else
            m_scan.layoutsWithOtherUses.append(layout);
    }
    if (Value::opcodesWithFunction.contains(value.opcode))
        scanFunctionReference(id, value);
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        LayoutID layout = environmentOf(operand);
        if (layout && !isRewrittenUse(value, operandIndex))
            m_scan.layoutsWithOtherUses.append(layout);
    });
}

void FunctionScanner::scanOutsideValues(const Block& block)
{
    auto addUse = [&](ValueID operand) {
        if (LayoutID layout = environmentOf(operand))
            m_scan.layoutsWithOtherUses.append(layout);
    };
    block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
        addUse(operand);
    });
    if (block.subject)
        addUse(block.subject);
}

FunctionScan FunctionScanner::run()
{
    scanSignature();
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        BlockID id(index);
        const Block& block = m_function.at(id);
        for (ValueID value : block.values)
            scan(id, value);
        scanOutsideValues(block);
    }
    return WTF::move(m_scan);
}

static ProgramScan scanProgram(const ScanContext& context)
{
    const Program& program = context.program;
    Vector<FunctionScan> scans = collectInParallel<FunctionScan>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionScan>& found) {
        FunctionScan scan = FunctionScanner(context, FunctionID(index)).run();
        if (!scan.isEmpty())
            found.append(WTF::move(scan));
    });

    ProgramScan result;
    for (const FunctionScan& scan : scans) {
        result.allocations.appendVector(scan.allocations);
        result.suppliers.appendVector(scan.suppliers);
        for (LayoutID layout : scan.layoutsWithOtherUses)
            result.layoutsWithOtherUses.set(layout.index());
        for (FunctionID function : scan.functionsWithOtherReferences)
            result.functionsWithOtherReferences.set(function.index());
    }
    return result;
}

static bool canChangeCaptures(const Function& function)
{
    switch (function.creator.kind()) {
    case CreatorRecord::Kind::None:
        return function.mayChangeSignature();
    case CreatorRecord::Kind::Version:
        return true;
    case CreatorRecord::Kind::Entrypoint:
    case CreatorRecord::Kind::Coroutine:
    case CreatorRecord::Kind::BoundFunction:
    case CreatorRecord::Kind::ResultWrapper:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::Continuation:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::Validator:
    case CreatorRecord::Kind::ChainStep:
    case CreatorRecord::Kind::StubBody:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static InitializationState variableInitializationFor(InitializationState fieldInitialization)
{
    switch (fieldInitialization) {
    case InitializationState::Unknown:
        return InitializationState::Unknown;
    case InitializationState::StoredBeforeEveryRead:
    case InitializationState::SetAtCreation:
        return InitializationState::StoredBeforeEveryRead;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LocationFlow variableFlowFor(const LocationFlow& fieldFlow)
{
    LocationFlow flow;
    flow.writers = fieldFlow.writers;
    flow.stores = fieldFlow.stores;
    flow.initialization = variableInitializationFor(fieldFlow.initialization);
    flow.reads = fieldFlow.reads;
    flow.isUnchangedDuringActivation = fieldFlow.stores == StoreCount::Never;
    flow.mayKeepObservedObjectAlive = fieldFlow.mayKeepObservedObjectAlive;
    flow.cause = fieldFlow.cause;
    flow.witness = fieldFlow.witness;
    return flow;
}

static Variable createFieldVariable(const Program& program, ModuleID module, const Field& field)
{
    Variable variable;
    if (field.key)
        variable.name = program.at(field.key).text;
    variable.module = module;
    variable.declared = field.declared;
    if (field.flags.contains(FieldFlags::IsImmutable))
        variable.flags.add(VariableFlags::IsImmutable);
    variable.flow = variableFlowFor(field.flow.location);
    return variable;
}

static Variable createValueVariable(const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    Variable variable;
    variable.module = function.module;
    variable.declared.type = value.type;
    variable.declared.written = value.written();
    variable.declared.predicted = value.prediction();
    variable.flags.add(VariableFlags::IsImmutable);
    variable.flow.writers = WriterState::AllKnown;
    variable.flow.stores = StoreCount::AtMostOnce;
    variable.flow.initialization = InitializationState::StoredBeforeEveryRead;
    // Flow has no answer for a location that it has not seen.
    variable.flow.mayKeepObservedObjectAlive = true;
    return variable;
}

static bool hasStableValue(const LocationFlow& flow)
{
    if (flow.writers != WriterState::AllKnown || flow.initialization != InitializationState::StoredBeforeEveryRead)
        return false;
    switch (flow.stores) {
    case StoreCount::Unknown:
        return false;
    case StoreCount::Never:
    case StoreCount::AtMostOnce:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector PromotionPlan::removedCapturesOf(FunctionID function) const
{
    std::span<const PlannedCapture> planned = capturesOf(function);
    BitVector removed(planned.size());
    for (size_t capture = 0; capture < planned.size(); ++capture) {
        if (planned[capture].change != CaptureChange::None)
            removed.quickSet(capture);
    }
    return removed;
}

std::span<const StoredValue> PromotionPlan::storedValuesIn(FunctionID function) const
{
    auto values = std::ranges::equal_range(storedValues, function, { }, &StoredValue::function);
    return storedValues.subspan(values.begin() - storedValues.begin(), values.size());
}

PromotionPlanner::PromotionPlanner(const Program& program, const FlowResults& flow, BuildContext& build)
    : m_program(program)
    , m_flow(flow)
    , m_build(build)
    , m_types(program)
    , m_scan(scanProgram(ScanContext { program, flow, build, m_types }))
{
}

const Variable& PromotionPlanner::variableAt(VariableID id) const
{
    size_t numberOfVariables = m_program.variables.size();
    return id.index() < numberOfVariables ? m_program.at(id) : m_plan.variables[id.index() - numberOfVariables];
}

VariableID PromotionPlanner::addVariable(Variable&& variable)
{
    VariableID id(m_program.variables.size() + m_plan.variables.size());
    m_plan.variables.append(WTF::move(variable));
    return id;
}

PlannedCapture& PromotionPlanner::plannedCapture(FunctionID closureFunction, uint32_t capture)
{
    Vector<PlannedCapture>& planned = m_plan.captures[closureFunction.index()];
    if (planned.isEmpty())
        planned.grow(m_program.at(closureFunction).captures.size());
    return planned[capture];
}

bool PromotionPlanner::isUnchangedValue(FunctionID closureFunction, uint32_t capture) const
{
    std::span<const PlannedCapture> planned = m_plan.capturesOf(closureFunction);
    if (!planned.empty() && planned[capture].change != CaptureChange::None)
        return false;
    return m_program.at(m_program.at(closureFunction).captures[capture].type).isJavaScriptValue();
}

bool PromotionPlanner::fitsCapture(TypeID type, FunctionID closureFunction, uint32_t capture) const
{
    auto fits = [&](FunctionID function) {
        return m_program.at(type).isSubsetOf(m_program.at(m_program.at(function).captures[capture].type), m_program);
    };
    if (!fits(closureFunction))
        return false;
    auto versions = std::ranges::equal_range(m_versions, closureFunction, { }, &std::pair<FunctionID, FunctionID>::first);
    return std::ranges::all_of(versions, [&](const auto& entry) {
        return fits(entry.second);
    });
}

void PromotionPlanner::findClosureFunctions()
{
    m_plan.closureFunctions = Vector<FunctionID>(m_program.functions.size(), [&](size_t index) {
        const auto* version = m_program.functions[index].creator.tryAs<CreatorRecord::Version>();
        return version ? version->provenVersion : FunctionID(index);
    });
    for (size_t index = 0; index < m_plan.closureFunctions.size(); ++index) {
        if (m_plan.closureFunctions[index].index() != index)
            m_versions.append({ m_plan.closureFunctions[index], FunctionID(index) });
    }
    std::ranges::sort(m_versions);
    m_plan.captures.grow(m_program.functions.size());
}

void PromotionPlanner::findFixedCaptures()
{
    for (size_t index = 0; index < m_program.functions.size(); ++index) {
        const Function& function = m_program.functions[index];
        FunctionID closureFunction = m_plan.closureFunctions[index];
        bool isVersion = closureFunction.index() != index;
        if (!canChangeCaptures(function) || m_scan.functionsWithOtherReferences.get(index))
            m_hasFixedCaptures.set(closureFunction.index());
        if (isVersion && function.captures.size() != m_program.at(closureFunction).captures.size())
            m_hasFixedCaptures.set(closureFunction.index());
        if (isVersion && m_plan.closureFunctionOf(closureFunction) != closureFunction) {
            m_hasFixedCaptures.set(closureFunction.index());
            m_hasFixedCaptures.set(m_plan.closureFunctionOf(closureFunction).index());
        }
        if (!isVersion && function.derivation.source)
            m_hasFixedCaptures.set(m_plan.closureFunctionOf(function.derivation.source).index());
    }
    for (const Layout& layout : m_program.layouts) {
        for (const Field& field : layout.fields) {
            if (FunctionID function = field.flow.location.deferredFunction)
                m_hasFixedCaptures.set(m_plan.closureFunctionOf(function).index());
        }
    }
}

void PromotionPlanner::addUsesInRecords()
{
    auto addUse = [&](TypeID type) {
        if (LayoutID layout = m_types.layoutOf(type))
            m_scan.layoutsWithOtherUses.set(layout.index());
    };
    for (unsigned index = 0; index < m_program.types.size(); ++index) {
        if (m_program.at(TypeID(index)).primitives)
            addUse(TypeID(index));
    }
    for (const Layout& layout : m_program.layouts) {
        for (const Field& field : layout.fields)
            addUse(field.declared.type);
    }
    for (const Variable& variable : m_program.variables)
        addUse(variable.declared.type);
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout)
                m_scan.layoutsWithOtherUses.set(binding.layout.index());
        }
    }
    for (size_t index = 0; index < m_program.functions.size(); ++index) {
        if (!hasFixedCaptures(FunctionID(index)))
            continue;
        for (const DeclaredType& capture : m_program.functions[index].captures)
            addUse(capture.type);
    }
}

void PromotionPlanner::promote(const EnvironmentAllocation& allocation)
{
    const Layout& layout = m_program.at(allocation.layout);
    ModuleID module = layout.module ? layout.module : m_program.at(allocation.function).module;
    m_plan.promotedLayouts.set(allocation.layout.index());
    m_plan.firstVariables[allocation.layout.index()] = VariableID(m_program.variables.size() + m_plan.variables.size());
    for (const Field& field : layout.fields)
        addVariable(createFieldVariable(m_program, module, field));
    m_allocatingFunctions.append(allocation.function);
}

void PromotionPlanner::promoteLayouts()
{
    m_plan.firstVariables.grow(m_program.layouts.size());
    std::ranges::sort(m_scan.allocations, { }, [](const EnvironmentAllocation& allocation) {
        return std::tuple(allocation.layout, allocation.function);
    });

    std::span<const EnvironmentAllocation> allocations = m_scan.allocations.span();
    while (!allocations.empty()) {
        const EnvironmentAllocation& first = allocations.front();
        size_t numberOfSites = std::ranges::find_if(allocations, [&](const EnvironmentAllocation& allocation) {
            return allocation.layout != first.layout;
        }) - allocations.begin();
        bool isOnlyInstance = numberOfSites == 1 && first.executions == ExecutionCount::AtMostOnce;
        if (isOnlyInstance
            && m_program.at(first.layout).kind == LayoutKind::Environment
            && !m_scan.layoutsWithOtherUses.get(first.layout.index())
            && isEnabled(first.function))
            promote(first);
        allocations = allocations.subspan(numberOfSites);
    }
}

void PromotionPlanner::removeEnvironmentCaptures()
{
    for (size_t index = 0; index < m_program.functions.size(); ++index) {
        FunctionID id(index);
        if (m_plan.closureFunctionOf(id) != id)
            continue;
        const auto& captures = m_program.functions[index].captures;
        for (uint32_t capture = 0; capture < captures.size(); ++capture) {
            LayoutID layout = m_types.layoutOf(captures[capture].type);
            if (layout && m_plan.isPromoted(layout))
                plannedCapture(id, capture).change = CaptureChange::Remove;
        }
    }
}

void PromotionPlanner::forEachSuppliedFunction(NOESCAPE const Invocable<void(FunctionID, Suppliers)> auto& functor) const
{
    Suppliers suppliers = m_scan.suppliers.span();
    while (!suppliers.empty()) {
        FunctionID closureFunction = m_plan.closureFunctionOf(suppliers.front().callee);
        size_t count = std::ranges::find_if(suppliers, [&](const CaptureSupplier& supplier) {
            return m_plan.closureFunctionOf(supplier.callee) != closureFunction;
        }) - suppliers.begin();
        if (!hasFixedCaptures(closureFunction) && isEnabled(closureFunction))
            functor(closureFunction, suppliers.first(count));
        suppliers = suppliers.subspan(count);
    }
}

bool PromotionPlanner::suppliesOwnCapture(FunctionID closureFunction, const CaptureSupplier& supplier, uint32_t capture) const
{
    if (m_plan.closureFunctionOf(supplier.function) != closureFunction)
        return false;
    const Function& function = m_program.at(supplier.function);
    ValueID supplied = sameObjectThroughCasts(function, function.at(supplier.site).variadicOperands()[capture]);
    return function.at(entryBlock).parameters[capture] == supplied;
}

VariableID PromotionPlanner::findStableVariable(const Function& function, ValueID id) const
{
    const Value& value = function.at(id);
    VariableID variable;
    if (value.opcode == Opcode::GetModuleVariable)
        variable = value.variable();
    else if (value.opcode == Opcode::GetEnvironmentField && m_plan.isPromoted(value.layout()))
        variable = m_plan.variableOf(value.layout(), value.field());
    return variable && hasStableValue(variableAt(variable).flow) ? variable : VariableID();
}

CaptureSource PromotionPlanner::findSource(const CaptureSupplier& supplier, uint32_t capture) const
{
    const Function& function = m_program.at(supplier.function);
    ValueID supplied = function.at(supplier.site).variadicOperands()[capture];
    ValueID definition = sameObjectThroughCasts(function, supplied);

    if (function.at(definition).opcode == Opcode::Parameter) {
        std::span<const PlannedCapture> planned = m_plan.capturesOf(supplier.function);
        size_t position = function.at(entryBlock).parameters.find(definition);
        if (position < planned.size() && planned[position].variable)
            return { planned[position].variable, { }, { } };
    }
    if (VariableID variable = findStableVariable(function, definition))
        return { variable, { }, { } };
    if (supplier.capturesComputedOnce.get(capture) && isEnabled(supplier.function))
        return { { }, supplier.function, supplied };
    return { };
}

CaptureSource PromotionPlanner::findCommonSource(FunctionID closureFunction, uint32_t capture, Suppliers suppliers) const
{
    CaptureSource common;
    for (const CaptureSupplier& supplier : suppliers) {
        if (suppliesOwnCapture(closureFunction, supplier, capture))
            continue;
        CaptureSource source = findSource(supplier, capture);
        if (!source || (common && common != source))
            return { };
        common = source;
    }
    return common;
}

TypeID PromotionPlanner::typeOf(const CaptureSource& source) const
{
    return source.variable ? variableAt(source.variable).declared.type : m_program.at(source.function).at(source.value).type;
}

VariableID PromotionPlanner::ensureVariable(const CaptureSource& source)
{
    if (source.variable)
        return source.variable;
    uint64_t key = static_cast<uint64_t>(source.function.index()) << 32 | source.value.index();
    return m_variablesByValue.ensure(key, [&] {
        VariableID variable = addVariable(createValueVariable(m_program.at(source.function), source.value));
        m_plan.storedValues.append(StoredValue { source.function, source.value, variable });
        return variable;
    }).iterator->value;
}

bool PromotionPlanner::readCapturesFromVariables(FunctionID closureFunction, Suppliers suppliers)
{
    bool didChange = false;
    uint32_t numberOfCaptures = m_program.at(closureFunction).captures.size();
    for (uint32_t capture = 0; capture < numberOfCaptures; ++capture) {
        if (!isUnchangedValue(closureFunction, capture))
            continue;
        CaptureSource source = findCommonSource(closureFunction, capture, suppliers);
        if (!source || !fitsCapture(typeOf(source), closureFunction, capture))
            continue;
        plannedCapture(closureFunction, capture) = { CaptureChange::ReadVariable, ensureVariable(source) };
        ++m_plan.numberOfPromotedCaptures;
        didChange = true;
    }
    return didChange;
}

void PromotionPlanner::promoteCapturedValues()
{
    std::ranges::sort(m_scan.suppliers, { }, [&](const CaptureSupplier& supplier) {
        return std::tuple(m_plan.closureFunctionOf(supplier.callee), supplier.function, supplier.site);
    });

    for (bool didChange = true; didChange;) {
        didChange = false;
        forEachSuppliedFunction([&](FunctionID closureFunction, Suppliers suppliers) {
            didChange |= readCapturesFromVariables(closureFunction, suppliers);
        });
    }
    std::ranges::sort(m_plan.storedValues, { }, [](const StoredValue& stored) {
        return std::tuple(stored.function, stored.value);
    });
}

void PromotionPlanner::collectEditedFunctions()
{
    BitVector isEdited;
    for (FunctionID function : m_allocatingFunctions)
        isEdited.set(function.index());
    for (const StoredValue& stored : m_plan.storedValues)
        isEdited.set(stored.function.index());
    for (size_t index = 0; index < m_program.functions.size(); ++index) {
        if (!m_plan.capturesOf(FunctionID(index)).empty())
            isEdited.set(index);
    }
    for (const CaptureSupplier& supplier : m_scan.suppliers) {
        if (!m_plan.capturesOf(supplier.callee).empty())
            isEdited.set(supplier.function.index());
    }
    for (size_t index : isEdited)
        m_plan.editedFunctions.append(FunctionID(index));
}

PromotionPlan PromotionPlanner::run()
{
    findClosureFunctions();
    findFixedCaptures();
    addUsesInRecords();
    promoteLayouts();
    removeEnvironmentCaptures();
    promoteCapturedValues();
    collectEditedFunctions();
    return WTF::move(m_plan);
}

PromotionPlan planEnvironmentPromotion(const Program& program, const FlowResults& flow, BuildContext& build)
{
    return PromotionPlanner(program, flow, build).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
