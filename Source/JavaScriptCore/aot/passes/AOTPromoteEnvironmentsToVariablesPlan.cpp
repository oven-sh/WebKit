/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromoteEnvironmentsToVariablesPlan.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
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

struct SuppliedCapture {
    static constexpr uint32_t noPosition = UINT32_MAX;

    ValueID supplied;
    uint32_t entryParameter { noPosition };
    VariableID variable;
    LayoutID layout;
    uint32_t field { 0 };
};

struct CaptureSupplier {
    FunctionID callee;
    FunctionID function;
    ValueID site;
    BitVector capturesComputedOnce;
    Vector<SuppliedCapture, 2> captures;
};

struct PromotedCaptureSource {
    explicit operator bool() const { return variable || value; }

    friend bool operator==(const PromotedCaptureSource&, const PromotedCaptureSource&) = default;

    VariableID variable;
    FunctionID function;
    ValueID value;
};

struct PromotionScan {
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
    EnvironmentTypes(const Program& program, const ParallelLoops& loops)
        : m_layouts(program.types.size())
    {
        forEachIndexInParallel(loops, m_layouts.size(), [&](unsigned index) {
            m_layouts[index] = program.at(TypeID(index)).environment;
        });
    }

    LayoutID layoutOf(TypeID type) const { return type ? m_layouts[type.index()] : LayoutID(); }

private:
    Vector<LayoutID> m_layouts;
};

struct PromotionScanContext {
    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
    const EnvironmentTypes& types;
};

class PromotionScanner {
public:
    PromotionScanner(const PromotionScanContext& context, FunctionID id)
        : m_context(context)
        , m_id(id)
        , m_function(context.program.at(id))
    {
    }

    PromotionScan run();

private:
    LayoutID environmentOf(ValueID value) const { return m_context.types.layoutOf(m_function.at(value).type); }
    ExecutionCount executionCountOf(BlockID);
    bool isComputedAtMostOnce(ValueID);

    void scanSignature();
    void scan(BlockID, ValueID);
    void scanFunctionReference(ValueID, const Value&);
    void scanOutsideValues(const Block&);
    bool isRewrittenUse(const Value& user, uint32_t operandIndex) const;

    const PromotionScanContext& m_context;
    FunctionID m_id;
    const Function& m_function;
    std::optional<FunctionAnalyses> m_analyses;
    Vector<BlockID> m_definingBlocks;
    PromotionScan m_scan;
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
    PromotedCaptureSource findCommonSource(FunctionID closureFunction, uint32_t capture, Suppliers) const;
    PromotedCaptureSource findSource(const CaptureSupplier&, uint32_t capture) const;
    VariableID findStableVariable(const SuppliedCapture&) const;
    TypeID typeOf(const PromotedCaptureSource&) const;
    VariableID ensureVariable(const PromotedCaptureSource&, const DeclaredType& capture);
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

ExecutionCount PromotionScanner::executionCountOf(BlockID block)
{
    if (!m_context.flow.runsAtMostOnce(m_context.program, m_id))
        return ExecutionCount::Unknown;
    if (!m_analyses)
        m_analyses.emplace(AnalysisContext { m_context.build, m_context.program, &m_context.flow, nullptr, m_id }, m_function);
    return m_analyses->executionCountOf(block);
}

bool PromotionScanner::isComputedAtMostOnce(ValueID value)
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

bool PromotionScanner::isRewrittenUse(const Value& user, uint32_t operandIndex) const
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

void PromotionScanner::scanSignature()
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

void PromotionScanner::scanFunctionReference(ValueID id, const Value& value)
{
    FunctionID callee = value.function();
    // A closure that is made on demand lists no captures: the object that holds it keeps what it captures.
    bool listsCaptures = value.opcode == Opcode::CallDirect || (value.opcode == Opcode::NewClosure && !value.isMadeOnDemand());
    if (!listsCaptures) {
        m_scan.functionsWithOtherReferences.append(callee);
        return;
    }
    size_t numberOfCaptures = m_context.program.at(callee).captures.size();
    if (!numberOfCaptures)
        return;

    CaptureSupplier supplier { callee, m_id, id, BitVector(numberOfCaptures), { } };
    std::span<const ValueID> captured = value.variadicOperands().first(numberOfCaptures);
    supplier.captures.reserveInitialCapacity(numberOfCaptures);
    for (size_t capture = 0; capture < numberOfCaptures; ++capture) {
        if (isComputedAtMostOnce(captured[capture]))
            supplier.capturesComputedOnce.quickSet(capture);
        SuppliedCapture description { .supplied = captured[capture] };
        ValueID definitionID = sameObjectThroughCasts(m_function, captured[capture]);
        const Value& definition = m_function.at(definitionID);
        if (definition.opcode == Opcode::Parameter) {
            if (size_t position = m_function.at(entryBlock).parameters.find(definitionID); position != notFound)
                description.entryParameter = static_cast<uint32_t>(position);
        } else if (definition.opcode == Opcode::GetModuleVariable)
            description.variable = definition.variable();
        else if (definition.opcode == Opcode::GetEnvironmentField) {
            description.layout = definition.layout();
            description.field = definition.field();
        }
        supplier.captures.append(description);
    }
    m_scan.suppliers.append(WTF::move(supplier));
}

void PromotionScanner::scan(BlockID block, ValueID id)
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

void PromotionScanner::scanOutsideValues(const Block& block)
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

PromotionScan PromotionScanner::run()
{
    scanSignature();
    bool holdsEnvironment = m_function.values.containsIf([&](const Value& value) {
        return !!m_context.types.layoutOf(value.type);
    });
    if (!holdsEnvironment) {
        forEachListedValue(m_context.program, m_id, Value::opcodesWithFunction, [&](BlockID, uint32_t, ValueID id, const Value& value) {
            scanFunctionReference(id, value);
        });
        return WTF::move(m_scan);
    }
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        BlockID id(index);
        const Block& block = m_function.at(id);
        for (ValueID value : block.values)
            scan(id, value);
        scanOutsideValues(block);
    }
    return WTF::move(m_scan);
}

static ProgramScan scanProgram(const PromotionScanContext& context)
{
    const Program& program = context.program;
    Vector<PromotionScan> scans = collectInParallel<PromotionScan>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<PromotionScan>& found) {
        PromotionScan scan = PromotionScanner(context, FunctionID(index)).run();
        if (!scan.isEmpty())
            found.append(WTF::move(scan));
    });

    ProgramScan result;
    for (PromotionScan& scan : scans) {
        result.allocations.appendVector(scan.allocations);
        result.suppliers.appendVector(WTF::move(scan.suppliers));
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
    case CreatorRecord::Kind::TableLookup:
    case CreatorRecord::Kind::Coroutine:
    case CreatorRecord::Kind::BoundFunction:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::EntryChecks:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::Validator:
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

static TypeID provenTypeOf(const DeclaredType& declared)
{
    return declared.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : declared.type;
}

// A position has one type. What is expected of a value is worth keeping where nothing is proven of it.
static DeclaredType declaredTypeOf(const Value& value)
{
    DeclaredType declared { .type = value.type };
    if (value.type == TypeTable::anything && value.expected()) {
        declared.type = value.expected();
        declared.flags.add(PositionFlags::IsExpected);
    }
    return declared;
}

static void appendSourcesOfParameter(const Function& function, ValueID parameter, Vector<ValueID, 8>& sources)
{
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const auto& parameters = function.blocks[blockIndex].parameters;
        auto found = std::ranges::find(parameters, parameter);
        if (found == parameters.end())
            continue;
        size_t index = found - parameters.begin();
        for (const Block& from : function.blocks) {
            for (const Target& target : from.terminator.targets) {
                if (target.block != BlockID(blockIndex))
                    continue;
                if (index < target.arguments.size()) {
                    sources.append(target.arguments[index]);
                    continue;
                }
                from.terminator.forEachOperand([&](ValueID operand, uint32_t) {
                    if (!operand)
                        return;
                    sources.append(operand);
                    function.at(operand).forEachOperand([&](ValueID tested, uint32_t) {
                        if (tested)
                            sources.append(tested);
                    });
                });
            }
        }
        return;
    }
}

static bool restsOnCheck(const Function& function, ValueID value)
{
    BitVector visited;
    Vector<ValueID, 8> pending { value };
    while (!pending.isEmpty()) {
        CastChain chain(function, pending.takeLast());
        for (ValueID cast : chain) {
            if (visited.set(cast.index()))
                break;
            if (function.at(cast).opcode == Opcode::Check)
                return true;
            if (cast == chain.last() && function.at(cast).opcode == Opcode::Parameter)
                appendSourcesOfParameter(function, cast, pending);
        }
    }
    return false;
}

// Where the type of the value rests on a check, the generic version of the function stores what the check has turned away.
static Variable createValueVariable(const Function& function, ValueID id, const DeclaredType& capture)
{
    Variable variable;
    variable.module = function.module;
    variable.declared = declaredTypeOf(function.at(id));
    if (restsOnCheck(function, id) || capture.flags.contains(PositionFlags::IsExpected))
        variable.declared.flags.add(PositionFlags::IsExpected);
    variable.flags.add(VariableFlags::IsImmutable);
    variable.flow.writers = WriterState::AllKnown;
    variable.flow.stores = StoreCount::AtMostOnce;
    variable.flow.initialization = InitializationState::StoredBeforeEveryRead;
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
    , m_types(program, build.parallelLoops())
    , m_scan(scanProgram(PromotionScanContext { program, flow, build, m_types }))
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
        return m_program.at(type).isSubsetOf(m_program.at(provenTypeOf(m_program.at(function).captures[capture])), m_program);
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
    m_plan.closureFunctions = Vector<FunctionID>(m_program.functions.size());
    forEachIndexInParallel(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index) {
        const auto* version = m_program.functions[index].creator.tryAs<CreatorRecord::Version>();
        m_plan.closureFunctions[index] = version ? version->provenVersion : FunctionID(index);
    });
    for (uint32_t index : findIndicesInParallel(m_build.parallelLoops(), m_plan.closureFunctions.size(), [&](unsigned index) {
        return m_plan.closureFunctions[index].index() != index;
    }))
        m_versions.append({ m_plan.closureFunctions[index], FunctionID(index) });
    AOT::sort(m_versions);
    m_plan.captures.grow(m_program.functions.size());
}

void PromotionPlanner::findFixedCaptures()
{
    for (FunctionID fixed : collectInParallel<FunctionID>(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        const Function& function = m_program.functions[index];
        FunctionID closureFunction = m_plan.closureFunctions[index];
        bool isVersion = closureFunction.index() != index;
        if (!canChangeCaptures(function) || m_scan.functionsWithOtherReferences.get(index))
            found.append(closureFunction);
        if (isVersion && function.captures.size() != m_program.at(closureFunction).captures.size())
            found.append(closureFunction);
        if (isVersion && m_plan.closureFunctionOf(closureFunction) != closureFunction) {
            found.append(closureFunction);
            found.append(m_plan.closureFunctionOf(closureFunction));
        }
        if (!isVersion && function.derivation.source)
            found.append(m_plan.closureFunctionOf(function.derivation.source));
    }))
        m_hasFixedCaptures.set(fixed.index());
    for (FunctionID deferred : collectInParallel<FunctionID>(m_build.parallelLoops(), m_program.layouts.size(), [&](unsigned index, Vector<FunctionID>& found) {
        for (const Field& field : m_program.layouts[index].fields) {
            if (FunctionID function = field.flow.location.deferredFunction)
                found.append(function);
        }
    }))
        m_hasFixedCaptures.set(m_plan.closureFunctionOf(deferred).index());
}

void PromotionPlanner::addUsesInRecords()
{
    // The records are many and the uses in them few: all threads look, and what they find is noted here.
    const ParallelLoops& loops = m_build.parallelLoops();
    auto addUsesIn = [&](unsigned numberOfRecords, const Invocable<void(unsigned, const ScopedLambda<void(TypeID)>&)> auto& forEachTypeIn) {
        for (LayoutID layout : collectInParallel<LayoutID>(loops, numberOfRecords, [&](unsigned index, Vector<LayoutID>& found) {
            auto addUse = [&](TypeID type) {
                if (LayoutID layout = m_types.layoutOf(type))
                    found.append(layout);
            };
            forEachTypeIn(index, ScopedLambda<void(TypeID)>(addUse));
        }))
            m_scan.layoutsWithOtherUses.set(layout.index());
    };
    addUsesIn(m_program.types.size(), [&](unsigned index, const ScopedLambda<void(TypeID)>& addUse) {
        if (m_types.layoutOf(TypeID(index)) && m_program.at(TypeID(index)).primitives)
            addUse(TypeID(index));
    });
    addUsesIn(m_program.layouts.size(), [&](unsigned index, const ScopedLambda<void(TypeID)>& addUse) {
        for (const Field& field : m_program.layouts[index].fields)
            addUse(field.declared.type);
    });
    addUsesIn(m_program.variables.size(), [&](unsigned index, const ScopedLambda<void(TypeID)>& addUse) {
        addUse(m_program.variables[index].declared.type);
    });
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout)
                m_scan.layoutsWithOtherUses.set(binding.layout.index());
        }
    }
    addUsesIn(m_program.functions.size(), [&](unsigned index, const ScopedLambda<void(TypeID)>& addUse) {
        if (!hasFixedCaptures(FunctionID(index)))
            return;
        for (const DeclaredType& capture : m_program.functions[index].captures)
            addUse(capture.type);
    });
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
    AOT::sort(m_scan.allocations, { }, [](const EnvironmentAllocation& allocation) {
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
    struct Capture {
        FunctionID function;
        uint32_t capture;
    };
    for (Capture removed : collectInParallel<Capture>(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index, Vector<Capture>& found) {
        FunctionID id(index);
        if (m_plan.closureFunctionOf(id) != id)
            return;
        const auto& captures = m_program.functions[index].captures;
        for (uint32_t capture = 0; capture < captures.size(); ++capture) {
            LayoutID layout = m_types.layoutOf(captures[capture].type);
            if (layout && m_plan.isPromoted(layout))
                found.append({ id, capture });
        }
    }))
        plannedCapture(removed.function, removed.capture).change = CaptureChange::Remove;
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
    return m_plan.closureFunctionOf(supplier.function) == closureFunction && supplier.captures[capture].entryParameter == capture;
}

VariableID PromotionPlanner::findStableVariable(const SuppliedCapture& supplied) const
{
    VariableID variable = supplied.variable;
    if (supplied.layout && m_plan.isPromoted(supplied.layout))
        variable = m_plan.variableOf(supplied.layout, supplied.field);
    return variable && hasStableValue(variableAt(variable).flow) ? variable : VariableID();
}

PromotedCaptureSource PromotionPlanner::findSource(const CaptureSupplier& supplier, uint32_t capture) const
{
    const SuppliedCapture& supplied = supplier.captures[capture];
    if (supplied.entryParameter != SuppliedCapture::noPosition) {
        std::span<const PlannedCapture> planned = m_plan.capturesOf(supplier.function);
        if (supplied.entryParameter < planned.size() && planned[supplied.entryParameter].variable)
            return { planned[supplied.entryParameter].variable, { }, { } };
    }
    if (VariableID variable = findStableVariable(supplied))
        return { variable, { }, { } };
    if (supplier.capturesComputedOnce.get(capture) && isEnabled(supplier.function))
        return { { }, supplier.function, supplied.supplied };
    return { };
}

PromotedCaptureSource PromotionPlanner::findCommonSource(FunctionID closureFunction, uint32_t capture, Suppliers suppliers) const
{
    PromotedCaptureSource common;
    for (const CaptureSupplier& supplier : suppliers) {
        if (suppliesOwnCapture(closureFunction, supplier, capture))
            continue;
        PromotedCaptureSource source = findSource(supplier, capture);
        if (!source || (common && common != source))
            return { };
        common = source;
    }
    return common;
}

TypeID PromotionPlanner::typeOf(const PromotedCaptureSource& source) const
{
    return source.variable ? provenTypeOf(variableAt(source.variable).declared) : m_program.at(source.function).at(source.value).type;
}

VariableID PromotionPlanner::ensureVariable(const PromotedCaptureSource& source, const DeclaredType& capture)
{
    if (source.variable)
        return source.variable;
    uint64_t key = static_cast<uint64_t>(source.function.index()) << 32 | source.value.index();
    return m_variablesByValue.ensure(key, [&] {
        VariableID variable = addVariable(createValueVariable(m_program.at(source.function), source.value, capture));
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
        PromotedCaptureSource source = findCommonSource(closureFunction, capture, suppliers);
        if (!source || !fitsCapture(typeOf(source), closureFunction, capture))
            continue;
        plannedCapture(closureFunction, capture) = { CaptureChange::ReadVariable, ensureVariable(source, m_program.at(closureFunction).captures[capture]) };
        ++m_plan.numberOfPromotedCaptures;
        didChange = true;
    }
    return didChange;
}

void PromotionPlanner::promoteCapturedValues()
{
    AOT::sort(m_scan.suppliers, { }, [&](const CaptureSupplier& supplier) {
        return std::tuple(m_plan.closureFunctionOf(supplier.callee), supplier.function, supplier.site);
    });

    for (bool didChange = true; didChange;) {
        didChange = false;
        forEachSuppliedFunction([&](FunctionID closureFunction, Suppliers suppliers) {
            didChange |= readCapturesFromVariables(closureFunction, suppliers);
        });
    }
    AOT::sort(m_plan.storedValues, { }, [](const StoredValue& stored) {
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
    for (uint32_t index : findIndicesInParallel(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index) {
        return !m_plan.capturesOf(FunctionID(index)).empty();
    }))
        isEdited.set(index);
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
