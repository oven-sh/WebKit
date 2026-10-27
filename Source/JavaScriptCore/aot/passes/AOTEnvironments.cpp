/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromoteEnvironmentsToVariables.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTBodyIndex.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDominators.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTHeldOperands.h"
#include "AOTIRBuilder.h"
#include "AOTPredictedValues.h"
#include "AOTKnownValue.h"
#include "AOTLargeVectors.h"
#include "AOTLiveness.h"
#include "AOTProgramPasses.h"
#include "AOTReadCapturesFromEnvironments.h"
#include "AOTReuseEnvironments.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlannedCapture {
    bool isRemoved { false };
    // What the function reads in its place.
    VariableID variable;
    // The primitive that every closure of the function captures.
    KnownValue constant;
};

struct PromotedCaptureSource {
    explicit operator bool() const { return variable || value; }

    friend bool operator==(const PromotedCaptureSource&, const PromotedCaptureSource&) = default;

    VariableID variable;
    FunctionID function;
    ValueID value;
};

struct PromotionPlan {
    bool isPromoted(LayoutID layout) const { return promotedLayouts.get(layout.index()); }
    VariableID variableOf(LayoutID layout, uint32_t field) const { return VariableID(firstVariables[layout.index()].index() + field); }
    FunctionID closureFunctionOf(FunctionID function) const { return closureFunctions[function.index()]; }
    std::span<const PlannedCapture> capturesOf(FunctionID function) const LIFETIME_BOUND { return captures[closureFunctionOf(function).index()].span(); }

    BitVector removedCapturesOf(FunctionID function) const
    {
        std::span<const PlannedCapture> planned = capturesOf(function);
        BitVector removed(planned.size());
        for (size_t capture = 0; capture < planned.size(); ++capture) {
            if (planned[capture].isRemoved)
                removed.quickSet(capture);
        }
        return removed;
    }

    BitVector promotedLayouts;
    Vector<VariableID> firstVariables;
    Vector<FunctionID> closureFunctions;
    Vector<Vector<PlannedCapture>> captures;
    // Ascending by function and value.
    Vector<PromotedCaptureSource> storedValues;
    Vector<Variable> variables;
    Vector<FunctionID> editedFunctions;
    uint64_t numberOfPromotedCaptures { 0 };
};

struct EnvironmentAllocation {
    LayoutID layout;
    FunctionID function;
    bool runsAtMostOnce { false };
};

struct SuppliedCapture {
    ValueID supplied;
    bool isComputedAtMostOnce { false };
    std::optional<uint32_t> entryParameter;
    VariableID variable;
    LayoutID layout;
    uint32_t field { 0 };
};

struct CaptureSupplier {
    FunctionID callee;
    FunctionID function;
    ValueID site;
    // Nothing takes the closure, so it holds what it captures for nobody.
    bool isUnused { false };
    Vector<SuppliedCapture, 2> captures;
};

struct PromotionScan {
    bool isEmpty() const { return allocations.isEmpty() && suppliers.isEmpty() && layoutsWithOtherUses.isEmpty() && capturedLayouts.isEmpty() && functionsWithOtherReferences.isEmpty(); }

    Vector<EnvironmentAllocation, 1> allocations;
    Vector<CaptureSupplier, 4> suppliers;
    Vector<LayoutID, 1> layoutsWithOtherUses;
    Vector<LayoutID, 1> capturedLayouts;
    Vector<FunctionID, 1> functionsWithOtherReferences;
};

// By type: the layout of the environment that the type is, among the layouts that the pass looks at.
struct EnvironmentTypes {
    LayoutID environmentOf(TypeID type) const { return type ? environments[type.index()] : LayoutID(); }
    LayoutID environmentOf(const Function& function, ValueID id) const { return environmentOf(function.at(id).type); }

    bool hasOperandOf(LayoutID layout, const Function& function, const Value& user) const
    {
        return std::ranges::any_of(user.operands(), [&](ValueID operand) {
            return operand && environmentOf(function, operand) == layout;
        });
    }

    Vector<LayoutID> environments;
};

} // anonymous namespace

static Vector<BlockID> findDefiningBlocks(const Function& function)
{
    Vector<BlockID> blocks(function.values.size());
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        for (ValueID parameter : function.blocks[index].parameters)
            blocks[parameter.index()] = BlockID(index);
        for (ValueID id : function.blocks[index].values)
            blocks[id.index()] = BlockID(index);
    }
    return blocks;
}

static bool isRewrittenUse(const Program& program, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::GetEnvironmentField)
        return operandIndex == OperandIndex::GetEnvironmentField::environment;
    if (user.opcode == Opcode::PutEnvironmentField)
        return operandIndex == OperandIndex::PutEnvironmentField::environment;
    if (user.opcode == Opcode::NewClosure)
        return true;
    if (user.opcode == Opcode::CallDirect)
        return *user.variadicOperandIndex(operandIndex) < program.at(user.function()).captures.size();
    return isHeldOperand(user, operandIndex);
}

static PromotionScan scanForPromotion(const Program& program, const FlowResults& flow, BuildContext& build, const EnvironmentTypes& types, FunctionID id)
{
    const Function& function = program.at(id);
    PromotionScan scan;
    std::optional<FunctionAnalyses> analyses;
    Vector<BlockID> definingBlocks;

    auto addUse = [&](TypeID type) {
        if (LayoutID layout = types.environmentOf(type))
            scan.layoutsWithOtherUses.append(layout);
    };
    auto runsAtMostOnce = [&](BlockID block) {
        if (!flow.runsAtMostOnce(program, id))
            return false;
        if (!analyses)
            analyses.emplace(AnalysisContext { build, program, &flow, nullptr, id }, function);
        return analyses->executionCountOf(block) == ExecutionCount::AtMostOnce;
    };
    auto isComputedAtMostOnce = [&](ValueID value) {
        if (!flow.runsAtMostOnce(program, id))
            return false;
        if (definingBlocks.isEmpty())
            definingBlocks = findDefiningBlocks(function);
        BlockID block = definingBlocks[value.index()];
        return block && (runsAtMostOnce(block) || build.isSabotaged(Sabotage::PromotedEnvironmentsAreMadeOften));
    };
    auto isUsed = [&](auto& isUsed, ValueID value) -> bool {
        return std::ranges::any_of(analyses->useLists().usesOf(value), [&](const Use& use) {
            return use.kind != Use::Kind::Operand || !isCastWithoutTest(function.at(use.user).opcode) || isUsed(isUsed, use.user);
        });
    };
    // Only where code runs once does a variable outlive what a closure would have held.
    auto isUnusedClosure = [&](ValueID site, const Value& value) {
        if (value.opcode != Opcode::NewClosure || !flow.runsAtMostOnce(program, id))
            return false;
        if (!analyses)
            analyses.emplace(AnalysisContext { build, program, &flow, nullptr, id }, function);
        return !isUsed(isUsed, site);
    };
    auto scanFunctionReference = [&](ValueID site, const Value& value) {
        FunctionID callee = value.function();
        // A closure that is made on demand lists no captures: the object that holds it keeps what it captures.
        if (value.opcode != Opcode::CallDirect && (value.opcode != Opcode::NewClosure || value.isMadeOnDemand())) {
            scan.functionsWithOtherReferences.append(callee);
            return;
        }
        size_t numberOfCaptures = program.at(callee).captures.size();
        if (!numberOfCaptures)
            return;

        CaptureSupplier supplier { callee, id, site, isUnusedClosure(site, value), { } };
        supplier.captures.reserveInitialCapacity(numberOfCaptures);
        for (ValueID captured : value.variadicOperands().first(numberOfCaptures)) {
            SuppliedCapture description { .supplied = captured, .isComputedAtMostOnce = isComputedAtMostOnce(captured) };
            ValueID definitionID = sameObjectThroughCasts(function, captured);
            const Value& definition = function.at(definitionID);
            if (definition.opcode == Opcode::Parameter) {
                if (size_t position = function.at(entryBlock).parameters.find(definitionID); position != notFound)
                    description.entryParameter = static_cast<uint32_t>(position);
            } else if (definition.opcode == Opcode::GetModuleVariable)
                description.variable = definition.variable();
            else if (definition.opcode == Opcode::GetEnvironmentField) {
                description.layout = definition.layout();
                description.field = definition.field();
            }
            supplier.captures.append(description);
        }
        scan.suppliers.append(WTF::move(supplier));
    };

    addUse(function.thisType.written());
    for (const DeclaredType& parameter : function.parameters)
        addUse(parameter.written());
    for (const DeclaredType& result : function.results)
        addUse(result.written());

    bool holdsEnvironment = function.values.containsIf([&](const Value& value) {
        return !!types.environmentOf(value.type);
    });
    if (!holdsEnvironment) {
        forEachListedValue(program, id, Value::opcodesWithFunction, [&](BlockID, uint32_t, ValueID site, const Value& value) {
            scanFunctionReference(site, value);
        });
        return scan;
    }
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Block& block = function.blocks[index];
        for (ValueID listed : block.values) {
            const Value& value = function.at(listed);
            if (LayoutID layout = types.environmentOf(value.type)) {
                if (value.opcode == Opcode::NewObject)
                    scan.allocations.append({ layout, id, runsAtMostOnce(BlockID(index)) });
                else
                    scan.layoutsWithOtherUses.append(layout);
            }
            if (Value::opcodesWithFunction.contains(value.opcode))
                scanFunctionReference(listed, value);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                LayoutID layout = types.environmentOf(function, operand);
                if (layout && !isRewrittenUse(program, value, operandIndex))
                    scan.layoutsWithOtherUses.append(layout);
                else if (layout && Value::opcodesWithFunction.contains(value.opcode) && !isUnusedClosure(listed, value))
                    scan.capturedLayouts.append(layout);
            });
        }
        block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            addUse(function.at(operand).type);
        });
        if (block.subject)
            addUse(function.at(block.subject).type);
    }
    return scan;
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
    case CreatorRecord::Kind::ResultWrapper:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::EntryChecks:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
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

static Variable createFieldVariable(const Program& program, ModuleID module, const Field& field)
{
    const LocationFlow& location = field.flow.location;
    Variable variable;
    if (field.key)
        variable.name = program.at(field.key).text;
    variable.module = module;
    variable.declared = field.declared;
    if (field.flags.contains(FieldFlags::IsImmutable))
        variable.flags.add(VariableFlags::IsImmutable);
    variable.flow.writers = location.writers;
    variable.flow.stores = location.stores;
    variable.flow.initialization = variableInitializationFor(location.initialization);
    variable.flow.reads = location.reads;
    variable.flow.isUnchangedDuringActivation = location.stores == StoreCount::Never;
    variable.flow.cause = location.cause;
    variable.flow.witness = location.witness;
    return variable;
}

// Where the type of the value rests on a check, the generic version of the function stores what the check has turned away.
static Variable createValueVariable(const Function& function, const PredictedValues& predicted, ValueID id, const DeclaredType& capture)
{
    const Value& value = function.at(id);
    Variable variable;
    variable.module = function.module;
    // A position has one type. What is expected of a value is worth keeping where nothing is proven of it.
    bool isOnlyExpected = value.type == TypeTable::anything && value.expected().predicted();
    std::optional<ProvenInEveryVersion> proven = Proven::byRules(predicted, id, value.type).inEveryVersion();
    if (isOnlyExpected)
        variable.declared.expect(value.expected());
    else if (!proven || capture.isExpected())
        variable.declared.expect(ExpectedType(value.type));
    else
        variable.declared.establish(*proven);
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

static PromotionPlan planEnvironmentPromotion(const Program& program, const FlowResults& flow, BuildContext& build)
{
    using Suppliers = std::span<const CaptureSupplier>;
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();
    PromotionPlan plan;
    BitVector isEdited;

    EnvironmentTypes types { Vector<LayoutID>(program.types.size()) };
    forEachIndexInParallel(loops, types.environments.size(), [&](unsigned index) {
        types.environments[index] = program.at(TypeID(index)).environment;
    });

    Vector<EnvironmentAllocation> allocations;
    Vector<CaptureSupplier> suppliers;
    BitVector layoutsWithOtherUses;
    BitVector capturedLayouts;
    BitVector functionsWithOtherReferences;
    for (PromotionScan& scan : collectInParallel<PromotionScan>(loops, numberOfFunctions, [&](unsigned index, Vector<PromotionScan>& found) {
        PromotionScan scan = scanForPromotion(program, flow, build, types, FunctionID(index));
        if (!scan.isEmpty())
            found.append(WTF::move(scan));
    })) {
        allocations.appendVector(scan.allocations);
        suppliers.appendVector(WTF::move(scan.suppliers));
        for (LayoutID layout : scan.layoutsWithOtherUses)
            layoutsWithOtherUses.set(layout.index());
        for (LayoutID layout : scan.capturedLayouts)
            capturedLayouts.set(layout.index());
        for (FunctionID function : scan.functionsWithOtherReferences)
            functionsWithOtherReferences.set(function.index());
    }

    auto isEnabled = [&](FunctionID function) {
        return build.isEnabled(PassID::PromoteEnvironmentsToVariables, function);
    };
    auto variableAt = [&](VariableID id) -> const Variable& {
        size_t numberOfVariables = program.variables.size();
        return id.index() < numberOfVariables ? program.at(id) : plan.variables[id.index() - numberOfVariables];
    };
    auto addVariable = [&](Variable&& variable) {
        plan.variables.append(WTF::move(variable));
        return VariableID(program.variables.size() + plan.variables.size() - 1);
    };
    auto plannedCapture = [&](FunctionID closureFunction, uint32_t capture) -> PlannedCapture& {
        Vector<PlannedCapture>& planned = plan.captures[closureFunction.index()];
        if (planned.isEmpty())
            planned.grow(program.at(closureFunction).captures.size());
        return planned[capture];
    };

    // A version of a function has the captures of the function that it is a version of.
    plan.closureFunctions = Vector<FunctionID>(numberOfFunctions);
    plan.captures.grow(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        const auto* version = program.functions[index].creator.tryAs<CreatorRecord::Version>();
        plan.closureFunctions[index] = version ? version->provenVersion : FunctionID(index);
    });
    Vector<std::pair<FunctionID, FunctionID>> versions;
    for (uint32_t index : findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return plan.closureFunctions[index].index() != index;
    }))
        versions.append({ plan.closureFunctions[index], FunctionID(index) });
    AOT::sort(versions);

    BitVector closureFunctionsWithFixedCaptures;
    auto hasFixedCaptures = [&](FunctionID function) {
        return closureFunctionsWithFixedCaptures.get(plan.closureFunctionOf(function).index());
    };
    for (FunctionID fixed : collectInParallel<FunctionID>(loops, numberOfFunctions, [&](unsigned index, Vector<FunctionID>& found) {
        const Function& function = program.functions[index];
        FunctionID closureFunction = plan.closureFunctions[index];
        bool isVersion = closureFunction.index() != index;
        if (!canChangeCaptures(function) || functionsWithOtherReferences.get(index))
            found.append(closureFunction);
        if (isVersion && function.captures.size() != program.at(closureFunction).captures.size())
            found.append(closureFunction);
        if (isVersion && plan.closureFunctionOf(closureFunction) != closureFunction) {
            found.append(closureFunction);
            found.append(plan.closureFunctionOf(closureFunction));
        }
        if (!isVersion && function.derivation.source)
            found.append(plan.closureFunctionOf(function.derivation.source));
    }))
        closureFunctionsWithFixedCaptures.set(fixed.index());
    for (FunctionID deferred : collectInParallel<FunctionID>(loops, program.layouts.size(), [&](unsigned index, Vector<FunctionID>& found) {
        for (const Field& field : program.layouts[index].fields) {
            if (FunctionID function = field.flow.location.deferredFunction)
                found.append(function);
        }
    }))
        closureFunctionsWithFixedCaptures.set(plan.closureFunctionOf(deferred).index());

    // The records are many and the uses in them few: all threads look, and what they find is noted here.
    auto addUsesIn = [&](unsigned numberOfRecords, const auto& forEachTypeIn) {
        for (LayoutID layout : collectInParallel<LayoutID>(loops, numberOfRecords, [&](unsigned index, Vector<LayoutID>& found) {
            forEachTypeIn(index, [&](TypeID type) {
                if (LayoutID layout = types.environmentOf(type))
                    found.append(layout);
            });
        }))
            layoutsWithOtherUses.set(layout.index());
    };
    addUsesIn(program.types.size(), [&](unsigned index, const auto& addUse) {
        if (types.environmentOf(TypeID(index)) && program.at(TypeID(index)).primitives)
            addUse(TypeID(index));
    });
    addUsesIn(program.layouts.size(), [&](unsigned index, const auto& addUse) {
        for (const Field& field : program.layouts[index].fields)
            addUse(field.declared.written());
    });
    addUsesIn(program.variables.size(), [&](unsigned index, const auto& addUse) {
        addUse(program.variables[index].declared.written());
    });
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout)
                layoutsWithOtherUses.set(binding.layout.index());
        }
    }
    addUsesIn(numberOfFunctions, [&](unsigned index, const auto& addUse) {
        if (!hasFixedCaptures(FunctionID(index)))
            return;
        for (const DeclaredType& capture : program.functions[index].captures)
            addUse(capture.written());
    });

    // An environment of which there is one instance at most.
    plan.firstVariables.grow(program.layouts.size());
    AOT::sort(allocations, { }, [](const EnvironmentAllocation& allocation) {
        return std::tuple(allocation.layout, allocation.function);
    });
    for (auto sites : allocations.span() | std::views::chunk_by([](const EnvironmentAllocation& a, const EnvironmentAllocation& b) { return a.layout == b.layout; })) {
        auto [layoutID, function, runsAtMostOnce] = sites.front();
        const Layout& layout = program.at(layoutID);
        if (sites.size() != 1 || !runsAtMostOnce || layout.kind != LayoutKind::Environment || layoutsWithOtherUses.get(layoutID.index()) || !capturedLayouts.get(layoutID.index()) || !isEnabled(function))
            continue;
        ModuleID module = layout.module ? layout.module : program.at(function).module;
        plan.promotedLayouts.set(layoutID.index());
        plan.firstVariables[layoutID.index()] = VariableID(program.variables.size() + plan.variables.size());
        for (const Field& field : layout.fields)
            addVariable(createFieldVariable(program, module, field));
        isEdited.set(function.index());
    }
    for (auto [function, capture] : collectInParallel<std::pair<FunctionID, uint32_t>>(loops, numberOfFunctions, [&](unsigned index, Vector<std::pair<FunctionID, uint32_t>>& found) {
        if (plan.closureFunctions[index].index() != index)
            return;
        const auto& captures = program.functions[index].captures;
        for (uint32_t capture = 0; capture < captures.size(); ++capture) {
            LayoutID layout = types.environmentOf(captures[capture].written());
            if (layout && plan.isPromoted(layout))
                found.append({ FunctionID(index), capture });
        }
    }))
        plannedCapture(function, capture).isRemoved = true;

    // A captured value that is the same for every closure and every call of the function.
    auto findSource = [&](const CaptureSupplier& supplier, uint32_t capture) -> PromotedCaptureSource {
        const SuppliedCapture& supplied = supplier.captures[capture];
        if (supplied.entryParameter) {
            std::span<const PlannedCapture> planned = plan.capturesOf(supplier.function);
            if (*supplied.entryParameter < planned.size() && planned[*supplied.entryParameter].variable)
                return { planned[*supplied.entryParameter].variable, { }, { } };
        }
        VariableID variable = supplied.layout && plan.isPromoted(supplied.layout) ? plan.variableOf(supplied.layout, supplied.field) : supplied.variable;
        if (variable && hasStableValue(variableAt(variable).flow))
            return { variable, { }, { } };
        if (supplied.isComputedAtMostOnce && isEnabled(supplier.function))
            return { { }, supplier.function, supplied.supplied };
        return { };
    };
    auto findCommonSource = [&](FunctionID closureFunction, uint32_t capture, Suppliers suppliersOfFunction) -> PromotedCaptureSource {
        PromotedCaptureSource common;
        for (const CaptureSupplier& supplier : suppliersOfFunction) {
            // It hands on its own capture.
            if (plan.closureFunctionOf(supplier.function) == closureFunction && supplier.captures[capture].entryParameter == capture)
                continue;
            if (supplier.isUnused)
                continue;
            PromotedCaptureSource source = findSource(supplier, capture);
            if (!source || (common && common != source))
                return { };
            common = source;
        }
        return common;
    };
    auto fitsCapture = [&](const PromotedCaptureSource& source, FunctionID closureFunction, uint32_t capture) {
        TypeID type = source.variable ? program.provenTypeOf(variableAt(source.variable).declared) : program.at(source.function).at(source.value).type;
        auto fits = [&](FunctionID function) {
            return program.at(type).isSubsetOf(program.at(program.provenTypeOf(program.at(function).captures[capture])), program);
        };
        return fits(closureFunction) && std::ranges::all_of(std::ranges::equal_range(versions, closureFunction, { }, &std::pair<FunctionID, FunctionID>::first), [&](const auto& entry) {
            return fits(entry.second);
        });
    };
    auto findConstant = [&](FunctionID closureFunction, uint32_t capture) {
        auto constantIn = [&](FunctionID function) {
            return knownValueOfType(program, program.at(program.provenTypeOf(program.at(function).captures[capture])));
        };
        KnownValue constant = constantIn(closureFunction);
        bool isSameInVersions = std::ranges::all_of(std::ranges::equal_range(versions, closureFunction, { }, &std::pair<FunctionID, FunctionID>::first), [&](const auto& entry) {
            return constantIn(entry.second) == constant;
        });
        return isSameInVersions && !constant.isObject() ? constant : KnownValue();
    };
    UncheckedKeyHashMap<uint64_t, VariableID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> variablesByValue;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<PredictedValues>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> predictedValuesOfFunctions;
    auto ensureVariable = [&](const PromotedCaptureSource& source, const DeclaredType& capture) {
        if (source.variable)
            return source.variable;
        return variablesByValue.ensure(static_cast<uint64_t>(source.function.index()) << 32 | source.value.index(), [&] {
            const Function& function = program.at(source.function);
            const PredictedValues& predicted = *predictedValuesOfFunctions.ensure(source.function.index(), [&] {
                return makeUniqueWithoutFastMallocCheck<PredictedValues>(function, CFG(function));
            }).iterator->value;
            VariableID variable = addVariable(createValueVariable(function, predicted, source.value, capture));
            plan.storedValues.append({ variable, source.function, source.value });
            return variable;
        }).iterator->value;
    };
    AOT::sort(suppliers, { }, [&](const CaptureSupplier& supplier) {
        return std::tuple(plan.closureFunctionOf(supplier.callee), supplier.function, supplier.site);
    });
    for (bool didChange = true; std::exchange(didChange, false);) {
        for (Suppliers suppliersOfFunction : suppliers.span() | std::views::chunk_by([&](const CaptureSupplier& a, const CaptureSupplier& b) { return plan.closureFunctionOf(a.callee) == plan.closureFunctionOf(b.callee); })) {
            FunctionID closureFunction = plan.closureFunctionOf(suppliersOfFunction[0].callee);
            if (hasFixedCaptures(closureFunction) || !isEnabled(closureFunction))
                continue;
            const auto& captures = program.at(closureFunction).captures;
            for (uint32_t capture = 0; capture < captures.size(); ++capture) {
                std::span<const PlannedCapture> planned = plan.capturesOf(closureFunction);
                if ((!planned.empty() && planned[capture].isRemoved) || !program.at(captures[capture].written()).isJavaScriptValue())
                    continue;
                if (KnownValue constant = findConstant(closureFunction, capture); constant.isKnown()) {
                    plannedCapture(closureFunction, capture) = { true, { }, constant };
                    ++plan.numberOfPromotedCaptures;
                    didChange = true;
                    continue;
                }
                PromotedCaptureSource source = findCommonSource(closureFunction, capture, suppliersOfFunction);
                if (!source || !fitsCapture(source, closureFunction, capture))
                    continue;
                plannedCapture(closureFunction, capture) = { true, ensureVariable(source, captures[capture]), { } };
                ++plan.numberOfPromotedCaptures;
                didChange = true;
            }
        }
    }
    AOT::sort(plan.storedValues, { }, [](const PromotedCaptureSource& stored) {
        return std::tuple(stored.function, stored.value);
    });

    for (const PromotedCaptureSource& stored : plan.storedValues)
        isEdited.set(stored.function.index());
    for (uint32_t index : findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return !plan.capturesOf(FunctionID(index)).empty();
    }))
        isEdited.set(index);
    for (const CaptureSupplier& supplier : suppliers) {
        if (!plan.capturesOf(supplier.callee).empty())
            isEdited.set(supplier.function.index());
    }
    for (size_t index : isEdited)
        plan.editedFunctions.append(FunctionID(index));
    return plan;
}

static void promoteInBody(const PromotionPlan& plan, IRBuilder& builder, FunctionID functionID, const BitVector& removedCaptures)
{
    Function& function = builder.function();
    const Program& program = builder.context().program;
    BuildContext& build = builder.context().build;
    auto storedValues = std::ranges::equal_range(plan.storedValues, functionID, { }, &PromotedCaptureSource::function);

    auto appendStore = [&](BlockValues& values, VariableID variable, ValueID stored) {
        Value store = IRBuilder::make(Opcode::PutModuleVariable, { stored });
        store.setVariable(variable);
        values.append(builder.addDetached(WTF::move(store)));
    };
    auto appendCapturedValueStore = [&](BlockValues& values, ValueID id) {
        auto stored = std::ranges::lower_bound(storedValues, id, { }, &PromotedCaptureSource::value);
        if (stored == storedValues.end() || stored->value != id)
            return;
        builder.setOrigin(function.at(id).origin());
        appendStore(values, stored->variable, id);
    };

    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        BlockValues values = std::exchange(function.edit(block, BlockEdit::Values).values, { });
        uint32_t numberOfCaughtValues = 0;
        while (numberOfCaughtValues < values.size() && (function.at(values[numberOfCaughtValues]).opcode == Opcode::CatchValue || function.at(values[numberOfCaughtValues]).opcode == Opcode::CatchException))
            ++numberOfCaughtValues;

        BlockValues rewritten;
        rewritten.reserveInitialCapacity(values.size());
        for (uint32_t index = 0; index <= values.size(); ++index) {
            if (index == numberOfCaughtValues) {
                Vector<ValueID, 8> parameters(function.at(block).parameters.span());
                for (ValueID parameter : parameters)
                    appendCapturedValueStore(rewritten, parameter);
            }
            if (index == values.size())
                break;

            ValueID id = values[index];
            Value& value = function.edit(id);
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::CallDirect) {
                std::span<const PlannedCapture> planned = plan.capturesOf(value.function());
                // A closure that is made on demand lists no captures: the object that holds it keeps what it captures, in itself or in a hidden field.
                if (value.opcode == Opcode::NewClosure && value.isMadeOnDemand())
                    RELEASE_ASSERT(planned.empty());
                for (uint32_t capture = planned.size(); capture--;) {
                    if (planned[capture].isRemoved)
                        value.removeVariadicOperand(capture);
                }
            } else if (value.opcode == Opcode::NewObject && plan.isPromoted(value.layout())) {
                if (build.options().collectsAllocationRemarks() && value.allocationSite())
                    build.addAllocationRemark({ .site = value.allocationSite(), .location = function.locationOf(id), .outcome = AllocationOutcome::Removed });
                LayoutID layout = value.layout();
                Vector<uint32_t, 8> fields(value.fields());
                Vector<ValueID, 8> initialValues(value.valuesOperands());
                builder.setOrigin(value.origin());
                for (size_t position = 0; position < fields.size(); ++position)
                    appendStore(rewritten, plan.variableOf(layout, fields[position]), initialValues[position]);
                continue;
            } else if (value.opcode == Opcode::GetEnvironmentField && plan.isPromoted(value.layout())) {
                VariableID variable = plan.variableOf(value.layout(), value.field());
                value.become(Opcode::GetModuleVariable);
                value.setVariable(variable);
            } else if (value.opcode == Opcode::PutEnvironmentField && plan.isPromoted(value.layout())) {
                VariableID variable = plan.variableOf(value.layout(), value.field());
                ValueID stored = value.valueOperand();
                value.become(Opcode::PutModuleVariable);
                value.setVariable(variable);
                value.setValueOperand(stored);
            } else if (numberOfHeldOperandsOf(value)) {
                // The variables hold what the environment held.
                Vector<ValueID, 8> held;
                for (ValueID listed : heldOperandsOf(value)) {
                    LayoutID environment = program.typeOf(function, listed).environment;
                    if (!environment || !plan.isPromoted(environment))
                        held.append(listed);
                }
                setHeldOperands(value, held.span());
            }
            rewritten.append(id);
            appendCapturedValueStore(rewritten, id);
        }
        function.edit(block, BlockEdit::Values).values = WTF::move(rewritten);
    }

    for (uint32_t blockIndex = 1; blockIndex < function.blocks.size(); ++blockIndex) {
        const auto& parameters = function.at(BlockID(blockIndex)).parameters;
        BitVector removed(parameters.size());
        for (size_t index = 0; index < parameters.size(); ++index) {
            LayoutID environment = program.typeOf(function, parameters[index]).environment;
            if (environment && plan.isPromoted(environment))
                removed.quickSet(index);
        }
        builder.removeParameters(BlockID(blockIndex), removed);
    }

    std::span<const PlannedCapture> planned = plan.capturesOf(functionID);
    if (planned.empty())
        return;
    BlockValues values;
    Vector<ValueID, 4> replacedParameters;
    Vector<Value, 4> reads;
    Vector<std::pair<ValueID, KnownValue>, 2> constants;
    const auto& parameters = function.at(entryBlock).parameters;
    for (size_t capture = 0; capture < planned.size(); ++capture) {
        if (planned[capture].constant.isKnown()) {
            values.append(parameters[capture]);
            constants.append({ parameters[capture], planned[capture].constant });
        }
        if (!planned[capture].variable)
            continue;
        Value read = function.at(parameters[capture]);
        TypeID provenType = read.type;
        read.become(Opcode::GetModuleVariable);
        read.setVariable(planned[capture].variable);
        // One variable stands for the captures of several functions, and is only expected as soon as one of them is. What is proven of this capture is then said by a cast.
        if (program.at(planned[capture].variable).declared.isExpected() && provenType != TypeTable::anything) {
            builder.setOrigin(read.origin());
            ValueID readOfVariable = builder.addDetached(WTF::move(read));
            values.append(readOfVariable);
            read = makeAssumeType(readOfVariable, Proven::byPosition(provenType));
            read.setOrigin(function.at(readOfVariable).origin());
            read.type = provenType;
        }
        values.append(parameters[capture]);
        replacedParameters.append(parameters[capture]);
        reads.append(WTF::move(read));
    }

    removeEntryParameters(function, removedCaptures);
    for (size_t index = 0; index < reads.size(); ++index)
        function.edit(replacedParameters[index]) = WTF::move(reads[index]);
    for (auto [parameter, constant] : constants)
        becomeConstant(program, function, builder.types(), parameter, constant);
    values.appendVector(function.at(entryBlock).values);
    function.edit(entryBlock, BlockEdit::Values).values = WTF::move(values);
}

void promoteEnvironmentsToVariables(ProgramEditor& editor, FlowResults& flow, BuildContext& build)
{
    if (!flow.hasResults())
        return;

    const Program& program = editor.program();
    PromotionPlan plan = planEnvironmentPromotion(program, flow, build);
    if (plan.editedFunctions.isEmpty())
        return;

    for (Variable& variable : plan.variables)
        editor.addVariable(WTF::move(variable));
    editFunctionsInParallel(editor, build, plan.editedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        BitVector removedCaptures = plan.removedCapturesOf(id);
        if (function.hasBody()) {
            IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
            promoteInBody(plan, builder, id, removedCaptures);
            settleProofsBehindOperandsThatWereOnlyKept(function);
        } else
            removeEntryParameters(function, removedCaptures);
        for (ParameterOrigin& origin : function.derivation.parameterMap) {
            if (!origin.original)
                continue;
            uint32_t numberOfRemovedBefore = 0;
            for (size_t removed : removedCaptures)
                numberOfRemovedBefore += removed < *origin.original;
            origin.original = *origin.original - numberOfRemovedBefore;
        }
    });

    for (FunctionID function : plan.editedFunctions)
        flow.removeEntryParameters(function, plan.removedCapturesOf(function));
    for (size_t layout : plan.promotedLayouts)
        editor.at(LayoutID(layout)).flow.allocation = AllocationState::NeverAllocated;

    build.statistics().count(StatisticID::EnvironmentsPromotedToVariables, plan.promotedLayouts.bitCount());
    build.statistics().count(StatisticID::CapturesPromotedToVariables, plan.numberOfPromotedCaptures);
}

namespace {

constexpr uint32_t noCandidate = UINT32_MAX;
constexpr uint32_t noField = UINT32_MAX;

enum class Obstacle : uint8_t {
    None,
    NamedByEvalScope,
    NamedByOtherOperation,
    FilledWithOtherValue,
    HoldsSeveralFunctions,
    IsNeverFilled,
    IsFilledTwiceInOneBlock,
    MayBeFilledAgain,
    FunctionIsNotEdited,
};

ASCIILiteral obstacleNameOf(Obstacle obstacle)
{
    switch (obstacle) {
    case Obstacle::None:
        return "none"_s;
    case Obstacle::NamedByEvalScope:
        return "named by an eval scope"_s;
    case Obstacle::NamedByOtherOperation:
        return "the layout is named by another operation"_s;
    case Obstacle::FilledWithOtherValue:
        return "filled with something else than a new closure"_s;
    case Obstacle::HoldsSeveralFunctions:
        return "holds closures of several functions"_s;
    case Obstacle::IsNeverFilled:
        return "never filled"_s;
    case Obstacle::IsFilledTwiceInOneBlock:
        return "filled twice in one block"_s;
    case Obstacle::MayBeFilledAgain:
        return "a place that fills it may get the same environment again"_s;
    case Obstacle::FunctionIsNotEdited:
        return "named in a function that the pass leaves alone"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What becomes of the closure. Whatever it is, a read that only leads to GetCapture is answered by the environment.
enum class Fate : uint8_t { IsLeftAlone, IsNeverMade, IsMadeAtFirstUse, IsMadeAsBefore };

struct CaptureCandidateTable {
    std::optional<uint32_t> find(LayoutID layout, uint32_t field) const
    {
        if (!layouts.get(layout.index()))
            return std::nullopt;
        FieldReference reference { layout, field };
        auto found = std::ranges::lower_bound(fields, reference);
        if (found == fields.end() || *found != reference)
            return std::nullopt;
        return static_cast<uint32_t>(found - fields.begin());
    }

    void forEachCandidateOf(LayoutID layout, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        size_t candidate = std::ranges::lower_bound(fields, layout, { }, &FieldReference::layout) - fields.begin();
        for (; candidate < fields.size() && fields[candidate].layout == layout; ++candidate)
            functor(static_cast<uint32_t>(candidate));
    }

    // Ascending.
    Vector<FieldReference> fields;
    BitVector layouts;
};

// An entry parameter that every caller fills with the closure of a candidate, or with undefined where its type allows that, and that is only taken apart or asked whether it is there. What the closures of the function capture there is the same value.
struct ClosureParameter {
    FunctionID function;
    uint32_t index { 0 };
    uint32_t candidate { noCandidate };

    friend auto operator<=>(const ClosureParameter&, const ClosureParameter&) = default;
};

struct ClosureParameterTable {
    std::span<const ClosureParameter> parametersOf(FunctionID function) const
    {
        if (!functions.get(function.index()))
            return { };
        auto range = std::ranges::equal_range(parameters, function, { }, &ClosureParameter::function);
        return { range.begin(), range.end() };
    }

    uint32_t candidateOf(FunctionID function, uint32_t index) const
    {
        std::span<const ClosureParameter> ofFunction = parametersOf(function);
        auto found = std::ranges::find(ofFunction, index, &ClosureParameter::index);
        return found != ofFunction.end() ? found->candidate : noCandidate;
    }

    void removeAllMatching(NOESCAPE const Invocable<bool(const ClosureParameter&)> auto& matches)
    {
        parameters.removeAllMatching(matches);
        functions.clearAll();
        capturesOneValue.clearAll();
        for (const ClosureParameter& parameter : parameters) {
            functions.set(parameter.function.index());
            if (parameter.candidate >= firstCandidateWithoutField)
                capturesOneValue.set(functionsWithOneCapture[parameter.candidate - firstCandidateWithoutField].index());
        }
    }

    // The candidate that a closure of the function is wherever it is made.
    uint32_t candidateMadeOf(FunctionID function) const
    {
        if (!capturesOneValue.get(function.index()))
            return noCandidate;
        return firstCandidateWithoutField + static_cast<uint32_t>(std::ranges::lower_bound(functionsWithOneCapture, function) - functionsWithOneCapture.begin());
    }

    // Ascending.
    Vector<ClosureParameter> parameters;
    BitVector functions;
    // Ascending. A closure over one value stands for that value and needs no field. These are the candidates behind those of the fields.
    Vector<FunctionID> functionsWithOneCapture;
    // Those of them that a parameter is left of.
    BitVector capturesOneValue;
    uint32_t firstCandidateWithoutField { 0 };
    // The functions that have such a parameter, and those that name a function that has one.
    BitVector users;
};

struct FoundCaptures {
    enum class Kind : uint8_t { Store, Reader, Callee, ReadAsValue, Obstacle };

    // Behind this operation the environment and all that the closure captures are at hand, and nothing has used the closure yet.
    uint32_t indexOfFirstUse() const { return indexOfClosure.value_or(indexInBlock); }

    Kind kind;
    Obstacle obstacle { Obstacle::None };
    Opcode user { Opcode::Undefined };
    uint32_t candidate { noCandidate };
    FunctionID function;
    FunctionID callee;
    BlockID block;
    uint32_t indexInBlock { 0 };
    ValueID environment;
    ValueID store;
    ValueID closure;
    // Where the closure is made, if that is in front of the store in the same block and the closure captures the environment.
    std::optional<uint32_t> indexOfClosure;
    bool closureHasOtherUses { false };
};

// The closure is a value of the function that makes it too. There it stands for the environment it is stored in, which is at hand wherever the closure is.
static std::optional<uint32_t> findClosureInFrontOf(const Function& function, const Block& block, uint32_t indexOfStore)
{
    const Value& store = function.at(block.values[indexOfStore]);
    if (store.opcode != Opcode::PutEnvironmentField)
        return std::nullopt;
    ValueID stored = store.valueOperand();
    const Value& closure = function.at(stored);
    if (closure.opcode != Opcode::NewClosure || closure.isMadeOnDemand() || !std::ranges::contains(closure.variadicOperands(), store.operand(0)))
        return std::nullopt;
    for (uint32_t index = indexOfStore; index--;) {
        if (block.values[index] == stored)
            return index;
    }
    return std::nullopt;
}

static ValueID findSecond(std::span<const std::pair<ValueID, ValueID>> ascendingPairs, ValueID first)
{
    auto found = std::ranges::lower_bound(ascendingPairs, first, { }, &std::pair<ValueID, ValueID>::first);
    return found != ascendingPairs.end() && found->first == first ? found->second : ValueID();
}

// The values of a function that are the closure of a candidate: the reads, the closure where it is made, their casts, the block parameters that such values reach, and what a test proves of them.
struct ClosureWebs {
    struct Cause {
        uint32_t candidate;
        Opcode user;
    };

    ClosureWebs(const Program& program, const Function& function, FunctionID functionID, const ClosureParameterTable& closureParameters, NOESCAPE const Invocable<uint32_t(const Value&)> auto& candidateReadBy, NOESCAPE const Invocable<uint32_t(const Value&)> auto& candidateStoredBy)
    {
        candidates.fill(noCandidate, function.values.size());
        for (const ClosureParameter& parameter : closureParameters.parametersOf(functionID)) {
            candidates[function.at(entryBlock).parameters[parameter.index].index()] = parameter.candidate;
            isEmpty = false;
        }
        for (const Block& block : function.blocks) {
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                const Value& value = function.at(id);
                if (value.opcode == Opcode::GetEnvironmentField) {
                    candidates[id.index()] = candidateReadBy(value);
                    isEmpty &= candidateOf(id) == noCandidate;
                    continue;
                }
                if (value.opcode == Opcode::GetCapture) {
                    candidates[id.index()] = closureParameters.candidateOf(value.function(), value.index());
                    isEmpty &= candidateOf(id) == noCandidate;
                    continue;
                }
                if (value.opcode == Opcode::NewClosure && !value.isMadeOnDemand()) {
                    candidates[id.index()] = closureParameters.candidateMadeOf(value.function());
                    isEmpty &= candidateOf(id) == noCandidate;
                    continue;
                }
                if (value.opcode != Opcode::PutEnvironmentField)
                    continue;
                uint32_t candidate = candidateStoredBy(value);
                if (candidate == noCandidate || !findClosureInFrontOf(function, block, index))
                    continue;
                candidates[value.valueOperand().index()] = candidate;
                closuresWithStores.append({ value.valueOperand(), id });
                isEmpty = false;
            }
        }
        if (isEmpty)
            return;
        AOT::sort(closuresWithStores);
        objects.ensureSize(function.values.size());

        // What a parameter of the target is on this edge: an argument, or what the test in front of the branch has asked. Nothing for a guard.
        auto givenOnEdge = [&](const Block& block, const Target& target, uint32_t index) -> ValueID {
            if (index < target.arguments.size())
                return target.arguments[index];
            if (block.terminator.kind != TerminatorKind::Branch)
                return { };
            const Value& test = function.at(block.terminator.conditionOperand());
            if (test.opcode != Opcode::ToBoolean && test.opcode != Opcode::TypeTest)
                return { };
            // Among the closures of several functions the test picks one out, and what it proves is no name of them all.
            ValueID asked = test.operand(target.proofs[index - target.arguments.size()]);
            return candidateOf(asked) == noCandidate || program.typeOf(function, asked).singleFunction() ? asked : ValueID();
        };
        // From what is cast to the cast, and from what an edge gives to the block parameter.
        auto forEachLink = [&](const Invocable<void(ValueID, ValueID)> auto& functor) {
            for (const Block& block : function.blocks) {
                for (ValueID id : block.values) {
                    if (isCast(function.at(id).opcode))
                        functor(function.at(id).operand(0), id);
                }
                block.forEachTarget([&](const Target& target) {
                    const auto& parameters = function.at(target.block).parameters;
                    for (uint32_t index = 0; index < target.arguments.size() + target.proofs.size(); ++index) {
                        if (ValueID given = givenOnEdge(block, target, index))
                            functor(given, parameters[index]);
                    }
                });
            }
        };
        for (bool didChange = true; std::exchange(didChange, false);) {
            forEachLink([&](ValueID from, ValueID to) {
                if (candidateOf(from) == noCandidate || candidateOf(to) != noCandidate)
                    return;
                candidates[to.index()] = candidateOf(from);
                didChange = true;
            });
        }

        auto noteObject = [&](ValueID id, Opcode user) {
            if (candidateOf(id) == noCandidate || objects.quickSet(id.index()))
                return;
            causes.append({ candidateOf(id), user });
        };
        for (const Block& block : function.blocks) {
            block.forEachTarget([&](const Target& target) {
                const auto& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index) {
                    ValueID given = index < target.arguments.size() + target.proofs.size() ? givenOnEdge(block, target, index) : ValueID();
                    if (given && candidateOf(given) == candidateOf(parameters[index]))
                        continue;
                    noteObject(parameters[index], Opcode::Undefined);
                    if (given)
                        noteObject(given, Opcode::Undefined);
                }
            });
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::GetCapture || (value.opcode == Opcode::PutEnvironmentField && storeOf(value.valueOperand()) == id))
                    continue;
                if ((value.opcode == Opcode::ToBoolean || value.opcode == Opcode::TypeTest) && candidateOf(value.operand(0)) != noCandidate && asksForClosure(program, function, value))
                    continue;
                if ((value.opcode == Opcode::CallDirect || value.opcode == Opcode::NewClosure) && closureParameters.functions.get(value.function().index())) {
                    std::span<const ValueID> given = value.variadicOperands();
                    for (uint32_t position = 0; position < given.size(); ++position) {
                        if (given[position] && candidateOf(given[position]) != closureParameters.candidateOf(value.function(), position))
                            noteObject(given[position], value.opcode);
                    }
                    continue;
                }
                if (numberOfHeldOperandsOf(value)) {
                    // A closure holds its environment, and the environment holds whatever else the closure captures.
                    for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                        ValueID operand = value.operand(operandIndex);
                        if (!operand)
                            continue;
                        if (isHeldOperand(value, operandIndex) && candidateOf(operand) != noCandidate && function.at(operand).opcode == Opcode::NewClosure)
                            isHeld = true;
                        else
                            noteObject(operand, value.opcode);
                    }
                    continue;
                }
                // What stands for a closure that may be missing does not pass for one that is there.
                bool isLink = isCast(value.opcode) && (program.typeOf(function, id).primitives || !program.typeOf(function, value.operand(0)).primitives);
                value.forEachChild([&](ValueID operand) {
                    if (candidateOf(id) == noCandidate || candidateOf(id) != candidateOf(operand) || !isLink)
                        noteObject(operand, value.opcode);
                });
            }
            static_cast<const Instruction&>(block.terminator).forEachChild([&](ValueID operand) {
                noteObject(operand, Opcode::Undefined);
            });
            if (block.subject)
                noteObject(block.subject, Opcode::Undefined);
        }
        for (bool didChange = !objects.isEmpty(); std::exchange(didChange, false);) {
            forEachLink([&](ValueID from, ValueID to) {
                if (candidateOf(to) == noCandidate || candidateOf(to) != candidateOf(from) || isObject(from) == isObject(to))
                    return;
                objects.quickSet(from.index());
                objects.quickSet(to.index());
                didChange = true;
            });
        }
    }

    static bool isCast(Opcode opcode) { return opcode == Opcode::AssumeType || isCastThatProvesNothing(opcode); }

    // Of a test of what is a closure or undefined: whether it holds for the closure alone, or for undefined alone. Nothing for any other operation.
    static std::optional<bool> asksForClosure(const Program& program, const Function& function, const Value& test)
    {
        if (test.opcode == Opcode::ToBoolean)
            return true;
        if (test.opcode != Opcode::TypeTest)
            return std::nullopt;
        const Type& tested = program.at(test.testedType());
        bool holdsForClosure = program.typeOf(function, test.operand(0)).withoutPrimitives(Primitive::Undefined).isSubsetOf(tested, program);
        bool holdsForUndefined = tested.primitives & Primitive::Undefined;
        return holdsForClosure != holdsForUndefined ? std::optional(holdsForClosure) : std::nullopt;
    }
    // Nothing that was added behind the scan belongs to a web.
    uint32_t candidateOf(ValueID id) const { return id.index() < candidates.size() ? candidates[id.index()] : noCandidate; }
    bool isObject(ValueID id) const { return objects.quickGet(id.index()); }
    bool standsForCapturesOnly(ValueID id) const { return candidateOf(id) != noCandidate && !isObject(id); }

    // Of a closure that belongs to a web because of where it is made.
    ValueID storeOf(ValueID closure) const { return findSecond(closuresWithStores.span(), closure); }

    Vector<uint32_t> candidates;
    // Ascending.
    Vector<std::pair<ValueID, ValueID>, 8> closuresWithStores;
    BitVector objects;
    Vector<Cause, 4> causes;
    bool isEmpty { true };
    // A suspension lists a closure that belongs to a web because of where it is made.
    bool isHeld { false };
};

struct PlannedClosure {
    Obstacle obstacle { Obstacle::None };
    Opcode user { Opcode::Undefined };
    Fate fate { Fate::IsLeftAlone };
    FunctionID callee;
    // NewClosure, for a closure that is made at its first use.
    Value closure;
    LayoutID layout;
    TypeID typeOfEnvironment;
    // For a place that holds undefined where there is no closure.
    TypeID typeOfEnvironmentOrAbsent;
    // The field of the layout that holds each capture. No field for the environment itself.
    Vector<uint32_t, 4> fields;
    // The first capture that is the environment itself.
    size_t environment { notFound };
};

// Behind the first store of a closure that captures the value.
struct AddedStore {
    FunctionID function;
    BlockID block;
    uint32_t indexInBlock { 0 };
    LayoutID layout;
    ValueID environment;
    uint32_t field { 0 };
    ValueID value;
};

// A store of a closure that is made at its first use from now on.
struct RemovedStore {
    FunctionID function;
    ValueID store;
    uint32_t field { 0 };
    // Nothing else uses it.
    ValueID closure;

    friend auto operator<=>(const RemovedStore&, const RemovedStore&) = default;
};

struct CaptureReadPlan {
    // By candidate.
    Vector<PlannedClosure> closures;
    // Ascending by function. Within a function the last place comes first.
    Vector<AddedStore> addedStores;
    // Ascending.
    Vector<RemovedStore> removedStores;
    Vector<FunctionID> editedFunctions;
    size_t numberOfAddedFields { 0 };
};

} // anonymous namespace

template<typename T>
static std::span<const T> entriesIn(const Vector<T>& ascendingByFunction, FunctionID function)
{
    auto range = std::ranges::equal_range(ascendingByFunction, function, { }, &T::function);
    return { range.begin(), range.end() };
}

static CaptureCandidateTable findCandidates(const Program& program)
{
    CaptureCandidateTable candidates;
    candidates.layouts.ensureSize(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!layout.isEnvironment() || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            const DeclaredType& declared = layout.fields[field].declared;
            const Type& type = program.at(declared.written());
            if (declared.isExpected() || type.primitives || !type.singleFunction())
                continue;
            candidates.fields.append({ LayoutID(index), field });
            candidates.layouts.quickSet(index);
        }
    }
    return candidates;
}

// A field that stands for a capture is right only as long as the closure in the environment is the one that was stored first.
static void rejectWhatMayBeFilledAgain(const Function& function, Vector<FoundCaptures>& found, size_t firstFound)
{
    Vector<size_t, 16> stores;
    for (size_t index = firstFound; index < found.size(); ++index) {
        if (found[index].kind == FoundCaptures::Kind::Store)
            stores.append(index);
    }
    if (stores.isEmpty())
        return;

    Vector<BlockID> blockOf;
    // Nothing for a value that may be made outside this activation. A block where one allocation is all that the value can be: whoever enters this block gets another environment.
    auto allocationBlockOf = [&](ValueID environment) -> std::optional<BlockID> {
        if (blockOf.isEmpty())
            blockOf = findDefiningBlocks(function);
        BitVector seen(function.values.size());
        Vector<ValueID, 4> worklist { environment };
        BlockID allocationBlock;
        unsigned numberOfAllocations = 0;
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            if (seen.quickSet(id.index()))
                continue;
            const Value& value = function.at(id);
            BlockID block = blockOf[id.index()];
            if (value.opcode == Opcode::NewObject) {
                ++numberOfAllocations;
                allocationBlock = block;
                continue;
            }
            if (ClosureWebs::isCast(value.opcode)) {
                worklist.append(value.operand(0));
                continue;
            }
            if (value.opcode != Opcode::Parameter || !block || block == entryBlock)
                return std::nullopt;
            size_t position = function.at(block).parameters.find(id);
            bool isPassedOnEveryEdge = true;
            for (const Block& predecessor : function.blocks) {
                predecessor.forEachTarget([&](const Target& target) {
                    if (target.block != block)
                        return;
                    if (position < target.arguments.size())
                        worklist.append(target.arguments[position]);
                    else
                        isPassedOnEveryEdge = false;
                });
            }
            if (!isPassedOnEveryEdge)
                return std::nullopt;
        }
        return numberOfAllocations == 1 ? allocationBlock : BlockID();
    };
    // Whether control gets from the end of one block to the start of the other without entering `avoided`.
    auto canReach = [&](BlockID from, BlockID to, BlockID avoided) {
        BitVector seen(function.blocks.size());
        Vector<BlockID, 16> worklist;
        for (BlockID block = from;; block = worklist.takeLast()) {
            function.at(block).forEachTarget([&](const Target& target) {
                if (target.block != avoided && !seen.quickSet(target.block.index()))
                    worklist.append(target.block);
            });
            if (worklist.isEmpty())
                return false;
            if (worklist.last() == to)
                return true;
        }
    };
    // Most functions store all their closures into one environment in one block.
    Vector<std::pair<std::tuple<BlockID, BlockID, ValueID>, bool>, 2> answers;
    auto mayBeFilledAgain = [&](const FoundCaptures& earlier, const FoundCaptures& later) {
        std::tuple question { earlier.block, later.block, later.environment };
        size_t known = answers.findIf([&](const auto& answer) {
            return answer.first == question;
        });
        if (known != notFound)
            return answers[known].second;
        std::optional<BlockID> allocationBlock = allocationBlockOf(later.environment);
        answers.append({ question, !allocationBlock || canReach(earlier.block, later.block, *allocationBlock) });
        return answers.last().second;
    };
    for (size_t later : stores) {
        for (size_t earlier : stores) {
            if (found[earlier].candidate != found[later].candidate || !mayBeFilledAgain(found[earlier], found[later]))
                continue;
            found.append({ .kind = FoundCaptures::Kind::Obstacle, .obstacle = Obstacle::MayBeFilledAgain, .user = function.at(found[later].environment).opcode, .candidate = found[later].candidate });
            break;
        }
    }
}

// `filled` gets the parameters that a call or a closure in the function fills as the table says.
static void refuteClosureParameters(const Program& program, const BuildContext& build, const CaptureCandidateTable& candidates, const ClosureParameterTable& table, FunctionID id, Vector<ClosureParameter>& refuted, Vector<ClosureParameter>& filled)
{
    const Function& function = program.at(id);
    Vector<ValueID, 8> users;
    auto namesFunctionWithParameter = [&](NameKind kind, uint32_t index) {
        return kind == NameKind::Function && table.functions.get(index);
    };
    forEachListedValueNaming(program, id, Value::opcodesWithFunction, namesFunctionWithParameter, [&](BlockID, ValueID valueID, const Value&) {
        users.append(valueID);
    });
    std::span<const ClosureParameter> own = table.parametersOf(id);
    if (own.empty() && users.isEmpty())
        return;
    if (!build.isEnabled(PassID::ReadCapturesFromEnvironments, id)) {
        refuted.append(own);
        for (ValueID user : users)
            refuted.append(table.parametersOf(function.at(user).function()));
        return;
    }

    auto candidateNamedBy = [&](const Value& access) {
        return candidates.find(access.layout(), access.field()).value_or(noCandidate);
    };
    ClosureWebs webs(program, function, id, table, candidateNamedBy, candidateNamedBy);
    auto isTakenApart = [&](ValueID value, uint32_t candidate) {
        return value && webs.candidateOf(value) == candidate && !webs.isObject(value);
    };
    for (const ClosureParameter& parameter : own) {
        if (!isTakenApart(function.at(entryBlock).parameters[parameter.index], parameter.candidate))
            refuted.append(parameter);
    }
    for (ValueID user : users) {
        const Value& value = function.at(user);
        std::span<const ClosureParameter> parameters = table.parametersOf(value.function());
        if (value.opcode == Opcode::IsClosureOf)
            continue;
        if (value.opcode == Opcode::GetCapture) {
            for (const ClosureParameter& parameter : parameters) {
                if (parameter.index == value.index() && !isTakenApart(user, parameter.candidate))
                    refuted.append(parameter);
            }
            continue;
        }
        if (value.opcode != Opcode::CallDirect && value.opcode != Opcode::NewClosure) {
            refuted.append(parameters);
            continue;
        }
        std::span<const ValueID> given = value.variadicOperands();
        for (const ClosureParameter& parameter : parameters) {
            if (parameter.index >= given.size()) {
                if (value.opcode == Opcode::CallDirect)
                    refuted.append(parameter);
                continue;
            }
            ValueID operand = given[parameter.index];
            if (isTakenApart(operand, parameter.candidate))
                filled.append(parameter);
            else if (value.opcode != Opcode::CallDirect || !operand || function.at(operand).type != TypeTable::undefined)
                refuted.append(parameter);
        }
    }
}

// A closure that goes into a call, or into another closure, only to be taken apart there need not be made for that. The guess is that every parameter of the type of such a closure is one. What the bodies and the callers do decides.
static ClosureParameterTable findClosureParameters(const Program& program, const BuildContext& build, const CaptureCandidateTable& candidates)
{
    const ParallelLoops& loops = build.parallelLoops();
    ClosureParameterTable table;
    table.firstCandidateWithoutField = candidates.fields.size();
    // Ascending. A function that lies in several fields is left out.
    Vector<std::pair<FunctionID, uint32_t>> candidatesOfFunctions;
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        FieldReference reference = candidates.fields[candidate];
        candidatesOfFunctions.append({ program.at(program.at(reference.layout).fields[reference.field].declared.written()).singleFunction(), candidate });
    }
    AOT::sort(candidatesOfFunctions);
    BitVector liesInOneField(program.functions.size());
    BitVector liesInField(program.functions.size());
    for (auto fieldsOfFunction : candidatesOfFunctions.span() | std::views::chunk_by([](const auto& a, const auto& b) { return a.first == b.first; })) {
        liesInField.quickSet(fieldsOfFunction[0].first.index());
        if (fieldsOfFunction.size() == 1)
            liesInOneField.quickSet(fieldsOfFunction[0].first.index());
    }
    constexpr OptionSet<FunctionCapabilities> needed { FunctionCapabilities::HasOrdinaryFrame, FunctionCapabilities::IsNeverCalledByHost, FunctionCapabilities::MayChangeSignature };
    // What is captured can stand for the closure if it is told from a missing closure, and is not itself something that another value comes to stand for. A function that somebody unseen may call has closures that are objects.
    auto capturesOneValue = [&](FunctionID closure) {
        const auto& captures = program.at(closure).captures;
        if (liesInField.quickGet(closure.index()) || captures.size() != 1 || captures[0].isExpected() || !program.at(closure).capabilities.containsAll(needed))
            return false;
        const Type& captured = program.at(captures[0].written());
        return !(captured.primitives & Primitive::Absent) && !captured.singleFunction();
    };

    table.parameters = collectInParallel<ClosureParameter>(loops, program.functions.size(), [&](unsigned index, Vector<ClosureParameter>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !function.capabilities.containsAll(needed))
            return;
        const auto& parameters = function.at(entryBlock).parameters;
        for (uint32_t position = 0; position < parameters.size(); ++position) {
            const Type& type = program.typeOf(function, parameters[position]);
            FunctionID closure = type.primitives & ~Primitive::Undefined ? FunctionID() : type.singleFunction();
            if (!closure)
                continue;
            EntryParameter kind = function.entryParameterKind(position);
            if (kind != EntryParameter::Capture && kind != EntryParameter::Parameter)
                continue;
            if (liesInOneField.quickGet(closure.index()))
                found.append({ FunctionID(index), position, std::ranges::lower_bound(candidatesOfFunctions, closure, { }, &std::pair<FunctionID, uint32_t>::first)->second });
            else if (capturesOneValue(closure))
                found.append({ FunctionID(index), position, table.firstCandidateWithoutField + closure.index() });
        }
    });
    // So far such a candidate is numbered by its function.
    for (const ClosureParameter& parameter : table.parameters) {
        if (parameter.candidate >= table.firstCandidateWithoutField)
            table.functionsWithOneCapture.append(FunctionID(parameter.candidate - table.firstCandidateWithoutField));
    }
    AOT::sort(table.functionsWithOneCapture);
    removeRepeatedElements(table.functionsWithOneCapture);
    for (FunctionID closure : table.functionsWithOneCapture)
        table.capturesOneValue.set(closure.index());
    for (ClosureParameter& parameter : table.parameters) {
        if (parameter.candidate >= table.firstCandidateWithoutField)
            parameter.candidate = table.candidateMadeOf(FunctionID(parameter.candidate - table.firstCandidateWithoutField));
    }
    AOT::sort(table.parameters);
    table.removeAllMatching([](const ClosureParameter&) {
        return false;
    });

    if (table.parameters.isEmpty())
        return table;

    auto isOrNamesOneOf = [&](const BitVector& functions, unsigned index) {
        bool isFound = functions.get(index);
        if (!isFound && program.functions[index].hasBody()) {
            auto namesOne = [&](NameKind kind, uint32_t name) {
                return !isFound && kind == NameKind::Function && functions.get(name);
            };
            forEachListedValueNaming(program, FunctionID(index), Value::opcodesWithFunction, namesOne, [&](BlockID, ValueID, const Value&) {
                isFound = true;
            });
        }
        return isFound;
    };
    auto isUser = [&](unsigned index) {
        return isOrNamesOneOf(table.functions, index);
    };
    Vector<uint32_t> users = findIndicesInParallel(loops, program.functions.size(), isUser);

    // What a function says of a parameter: that it fills it as the table says, or that the table is wrong.
    struct Evidence {
        uint32_t user;
        bool isFilled;
        ClosureParameter parameter;
    };
    Vector<Evidence> evidence;
    Vector<uint32_t> asked = users;
    // A parameter that falls takes with it those that hand their value on to it.
    AOT_THRESHOLD(unsigned, maxRounds, 8);
    for (unsigned round = 0; !table.parameters.isEmpty(); ++round) {
        BitVector isAsked(program.functions.size());
        for (uint32_t user : asked)
            isAsked.quickSet(user);
        evidence.removeAllMatching([&](const Evidence& entry) {
            return isAsked.quickGet(entry.user);
        });
        evidence.appendVector(collectInParallel<Evidence>(loops, asked.size(), [&](unsigned index, Vector<Evidence>& evidenceInFunction) {
            Vector<ClosureParameter> refuted;
            Vector<ClosureParameter> filled;
            refuteClosureParameters(program, build, candidates, table, FunctionID(asked[index]), refuted, filled);
            for (const ClosureParameter& parameter : refuted)
                evidenceInFunction.append({ asked[index], false, parameter });
            for (const ClosureParameter& parameter : filled)
                evidenceInFunction.append({ asked[index], true, parameter });
        }));
        Vector<ClosureParameter> refuted;
        Vector<ClosureParameter> filled;
        for (const Evidence& entry : evidence)
            (entry.isFilled ? filled : refuted).append(entry.parameter);
        AOT::sort(refuted);
        AOT::sort(filled);
        if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]]
            dataLogLn("round ", round, ": ", table.parameters.size(), " parameters of ", table.functions.bitCount(), " functions, ", table.capturesOneValue.bitCount(), " functions with one capture, ", asked.size(), " of ", users.size(), " functions asked");
        // Nothing is known of a parameter that nobody fills.
        BitVector lostParameter(program.functions.size());
        table.removeAllMatching([&](const ClosureParameter& parameter) {
            bool falls = round + 1 == maxRounds || std::ranges::binary_search(refuted, parameter) || !std::ranges::binary_search(filled, parameter);
            if (falls)
                lostParameter.quickSet(parameter.function.index());
            return falls;
        });
        if (lostParameter.isEmpty())
            break;
        // Nobody else can answer otherwise than before.
        asked = findIndicesInParallel(loops, users.size(), [&](unsigned index) {
            return isOrNamesOneOf(lostParameter, users[index]);
        }).map([&](uint32_t index) {
            return users[index];
        });
    }
    for (uint32_t index : users) {
        if (isUser(index))
            table.users.set(index);
    }
    return table;
}

static void scanFunction(const Program& program, const BuildContext& build, const CaptureCandidateTable& candidates, const ClosureParameterTable& closureParameters, FunctionID id, Vector<FoundCaptures>& found)
{
    using Kind = FoundCaptures::Kind;
    const Function& function = program.at(id);
    bool isEdited = build.isEnabled(PassID::ReadCapturesFromEnvironments, id);
    size_t firstFound = found.size();

    auto reject = [&](uint32_t candidate, Obstacle obstacle, Opcode user) {
        found.append({ .kind = Kind::Obstacle, .obstacle = obstacle, .user = user, .candidate = candidate });
    };
    Vector<uint8_t> useCounts;
    auto hasOneUse = [&](ValueID used) {
        if (useCounts.isEmpty()) {
            useCounts.fill(0, function.values.size());
            auto count = [&](ValueID operand) {
                useCounts[operand.index()] = std::min(useCounts[operand.index()] + 1, 2);
            };
            for (const Block& block : function.blocks) {
                for (ValueID user : block.values)
                    function.at(user).forEachChild(count);
                block.forEachChildOutsideValues(count);
            }
        }
        return useCounts[used.index()] == 1;
    };
    auto noteStore = [&](uint32_t candidate, BlockID block, uint32_t indexInBlock, ValueID environment, ValueID stored) {
        const Value& closure = function.at(stored);
        if (closure.opcode != Opcode::NewClosure || closure.isMadeOnDemand()) {
            reject(candidate, Obstacle::FilledWithOtherValue, closure.opcode);
            return;
        }
        found.append({ .kind = Kind::Store, .candidate = candidate, .function = id, .callee = closure.function(), .block = block, .indexInBlock = indexInBlock, .environment = environment, .store = function.at(block).values[indexInBlock], .closure = stored, .indexOfClosure = findClosureInFrontOf(function, function.at(block), indexInBlock), .closureHasOtherUses = !hasOneUse(stored) });
    };

    bool hasRead = false;
    forEachListedValue(program, id, Value::opcodesWithLayout, [&](BlockID block, uint32_t indexInBlock, ValueID valueID, const Value& value) {
        LayoutID layout = value.layout();
        if (!layout || !candidates.layouts.get(layout.index()))
            return;
        if (!isEdited) {
            candidates.forEachCandidateOf(layout, [&](uint32_t candidate) {
                reject(candidate, Obstacle::FunctionIsNotEdited, value.opcode);
            });
            return;
        }
        switch (value.opcode) {
        case Opcode::NewObject: {
            std::span<const uint32_t> fields = value.fields();
            std::span<const ValueID> initialValues = value.variadicOperands();
            for (size_t index = 0; index < fields.size(); ++index) {
                if (auto candidate = candidates.find(layout, fields[index]))
                    noteStore(*candidate, block, indexInBlock, valueID, initialValues[index]);
            }
            break;
        }
        case Opcode::PutEnvironmentField:
            if (auto candidate = candidates.find(layout, value.field()))
                noteStore(*candidate, block, indexInBlock, value.operand(0), value.valueOperand());
            break;
        case Opcode::GetEnvironmentField:
            hasRead |= candidates.find(layout, value.field()).has_value();
            break;
        default:
            candidates.forEachCandidateOf(layout, [&](uint32_t candidate) {
                reject(candidate, Obstacle::NamedByOtherOperation, value.opcode);
            });
            break;
        }
    });
    rejectWhatMayBeFilledAgain(function, found, firstFound);
    bool hasClosureWithOtherUses = std::ranges::any_of(found.span().subspan(firstFound), [](const FoundCaptures& entry) {
        return entry.kind == Kind::Store && entry.indexOfClosure && entry.closureHasOtherUses;
    });
    if (!hasRead && !hasClosureWithOtherUses && !closureParameters.users.get(id.index()))
        return;

    found.append({ .kind = Kind::Reader, .function = id });
    auto candidateNamedBy = [&](const Value& access) {
        return candidates.find(access.layout(), access.field()).value_or(noCandidate);
    };
    ClosureWebs webs(program, function, id, closureParameters, candidateNamedBy, candidateNamedBy);
    for (FoundCaptures& entry : found.mutableSpan().subspan(firstFound)) {
        if (entry.kind == Kind::Store && entry.indexOfClosure)
            entry.closureHasOtherUses = webs.isObject(entry.closure);
    }
    // A closure that is an object where it is made need not come out of the field.
    auto userOf = [&](uint32_t candidate) {
        auto cause = std::ranges::find(webs.causes, candidate, &ClosureWebs::Cause::candidate);
        return cause != webs.causes.end() ? cause->user : Opcode::Undefined;
    };
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (value.opcode == Opcode::GetEnvironmentField && webs.candidateOf(valueID) != noCandidate && webs.isObject(valueID))
                found.append({ .kind = Kind::ReadAsValue, .user = userOf(webs.candidateOf(valueID)), .candidate = webs.candidateOf(valueID) });
            if (value.opcode != Opcode::GetCapture)
                continue;
            if (uint32_t candidate = webs.candidateOf(value.operand(0)); candidate < candidates.fields.size())
                found.append({ .kind = Kind::Callee, .candidate = candidate, .callee = value.function() });
        }
    }
}

// The type of what stands for a value of the given type, which is the closure, undefined, or either.
static TypeID typeInPlaceOf(const PlannedClosure& planned, const Type& type)
{
    if (!type.primitives)
        return planned.typeOfEnvironment;
    return type.hasObjects() ? planned.typeOfEnvironmentOrAbsent : TypeTable::absent;
}

static CaptureReadPlan planCaptureReads(ProgramEditor& editor, const CaptureCandidateTable& candidates, ClosureParameterTable& closureParameters, Vector<FoundCaptures>&& found)
{
    using Stores = std::span<const FoundCaptures>;
    const Program& program = editor.program();
    CaptureReadPlan plan;
    plan.closures.grow(candidates.fields.size() + closureParameters.functionsWithOneCapture.size());
    for (FunctionID closure : closureParameters.functionsWithOneCapture) {
        uint32_t candidate = closureParameters.candidateMadeOf(closure);
        if (candidate == noCandidate)
            continue;
        PlannedClosure& planned = plan.closures[candidate];
        planned.fate = Fate::IsNeverMade;
        planned.callee = closure;
        planned.typeOfEnvironment = program.at(closure).captures[0].written();
        planned.fields.append(noField);
    }
    BitVector isReadAsValue(candidates.fields.size());
    BitVector isUsedWhereItIsMade(candidates.fields.size());
    auto reject = [&](uint32_t candidate, Obstacle obstacle, Opcode user = Opcode::Undefined) {
        if (plan.closures[candidate].obstacle != Obstacle::None)
            return;
        plan.closures[candidate].obstacle = obstacle;
        plan.closures[candidate].user = user;
    };

    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!binding.layout || !binding.field)
                continue;
            if (auto candidate = candidates.find(binding.layout, *binding.field))
                reject(*candidate, Obstacle::NamedByEvalScope);
        }
    }
    for (const FoundCaptures& entry : found) {
        switch (entry.kind) {
        case FoundCaptures::Kind::Store:
        case FoundCaptures::Kind::Callee: {
            FunctionID& callee = plan.closures[entry.candidate].callee;
            if (!callee)
                callee = entry.callee;
            else if (callee != entry.callee)
                reject(entry.candidate, Obstacle::HoldsSeveralFunctions);
            if (entry.closureHasOtherUses)
                isUsedWhereItIsMade.quickSet(entry.candidate);
            break;
        }
        case FoundCaptures::Kind::ReadAsValue:
            if (!isReadAsValue.quickSet(entry.candidate))
                plan.closures[entry.candidate].user = entry.user;
            break;
        case FoundCaptures::Kind::Reader:
            plan.editedFunctions.append(entry.function);
            break;
        case FoundCaptures::Kind::Obstacle:
            reject(entry.candidate, entry.obstacle, entry.user);
            break;
        }
    }
    found.removeAllMatching([](const FoundCaptures& entry) {
        return entry.kind != FoundCaptures::Kind::Store;
    });
    auto layoutOf = [&](const FoundCaptures& store) {
        return candidates.fields[store.candidate].layout;
    };
    AOT::sort(found, { }, [&](const FoundCaptures& store) {
        return std::tuple { layoutOf(store), store.function, store.environment, store.block, store.candidate, store.indexInBlock, store.store, store.closure };
    });
    BitVector isFilled(candidates.fields.size());
    for (const FoundCaptures& store : found)
        isFilled.quickSet(store.candidate);
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        if (!isFilled.quickGet(candidate))
            reject(candidate, Obstacle::IsNeverFilled);
    }

    // A candidate and the runs that fill it, ascending.
    struct Member {
        Vector<uint32_t, 2> runs;
        uint32_t candidate;
    };
    // Ascending. The closures that are made in front of their stores, with the environments that they are stored in.
    Vector<std::tuple<FunctionID, ValueID, ValueID>> environmentsOfClosures;
    // What a value that is only taken apart becomes.
    auto environmentInPlaceOf = [&](FunctionID id, ValueID closure) {
        const Function& function = program.at(id);
        while (ClosureWebs::isCast(function.at(closure).opcode))
            closure = function.at(closure).operand(0);
        if (function.at(closure).opcode == Opcode::GetEnvironmentField)
            return function.at(closure).operand(0);
        if (function.at(closure).opcode != Opcode::NewClosure)
            return closure;
        if (closureParameters.candidateMadeOf(function.at(closure).function()) != noCandidate)
            return function.at(closure).variadicOperands()[0];
        auto made = std::ranges::lower_bound(environmentsOfClosures, std::pair { id, closure }, { }, [](const auto& entry) {
            return std::pair { std::get<0>(entry), std::get<1>(entry) };
        });
        RELEASE_ASSERT(made != environmentsOfClosures.end() && std::get<0>(*made) == id && std::get<1>(*made) == closure);
        return std::get<2>(*made);
    };
    auto planGroup = [&](LayoutID layout, std::span<const Stores> runsOfLayout, std::span<const Member> group, uint32_t& nextField) {
        std::span<const uint32_t> runs = group[0].runs.span();
        // A field serves the captures that every run fills with one value and that are declared alike.
        struct FieldOfGroup {
            uint32_t field;
            DeclaredType declared;
            // What each run of the group captures here, and where in its block the closure can first be used.
            Vector<ValueID, 2> operands;
            Vector<uint32_t, 2> indexesInBlocks;
        };
        Vector<FieldOfGroup, 8> fields;
        for (const Member& member : group) {
            PlannedClosure& planned = plan.closures[member.candidate];
            const auto& declaredCaptures = program.at(planned.callee).captures;
            for (uint32_t index = 0; index < declaredCaptures.size(); ++index) {
                FieldOfGroup wanted { nextField, declaredCaptures[index], { }, { } };
                uint32_t capturedClosure = closureParameters.candidateOf(planned.callee, index);
                if (capturedClosure != noCandidate)
                    wanted.declared.changeType(Proven::byPosition(typeInPlaceOf(plan.closures[capturedClosure], program.at(wanted.declared.written()))));
                bool isEnvironmentItself = true;
                for (uint32_t run : runs) {
                    const FoundCaptures& store = *std::ranges::find(runsOfLayout[run], member.candidate, &FoundCaptures::candidate);
                    std::span<const ValueID> captured = program.at(store.function).at(store.closure).variadicOperands();
                    RELEASE_ASSERT(index < captured.size());
                    wanted.operands.append(capturedClosure != noCandidate ? environmentInPlaceOf(store.function, captured[index]) : captured[index]);
                    wanted.indexesInBlocks.append(store.indexOfFirstUse());
                    isEnvironmentItself &= wanted.operands.last() == store.environment;
                }
                if (isEnvironmentItself) {
                    planned.environment = std::min<size_t>(planned.environment, index);
                    planned.fields.append(noField);
                    continue;
                }
                size_t shared = fields.findIf([&](const FieldOfGroup& field) {
                    return field.operands == wanted.operands && field.declared == wanted.declared;
                });
                if (shared == notFound) {
                    // It is filled where the field of the closure is.
                    const Field& fieldOfClosure = program.at(layout).fields[candidates.fields[member.candidate].field];
                    Field field;
                    field.declared = wanted.declared;
                    field.flags = fieldOfClosure.flags | OptionSet<FieldFlags> { FieldFlags::IsHidden, FieldFlags::IsAddedLate };
                    field.flow.location.writers = WriterState::AllKnown;
                    field.flow.location.stores = fieldOfClosure.flow.location.stores;
                    field.flow.location.storeNaming = StoreNaming::EveryStoreNamesLocation;
                    field.flow.location.initialization = fieldOfClosure.flow.location.initialization;
                    field.flow.occupancy = fieldOfClosure.flow.occupancy;
                    field.flow.stability = fieldOfClosure.flow.stability;
                    uint32_t indexOfField = editor.at(layout).appendField(WTF::move(field));
                    RELEASE_ASSERT(indexOfField == nextField);
                    ++nextField;
                    ++plan.numberOfAddedFields;
                    shared = fields.size();
                    fields.append(WTF::move(wanted));
                } else {
                    for (size_t position = 0; position < runs.size(); ++position)
                        fields[shared].indexesInBlocks[position] = std::min(fields[shared].indexesInBlocks[position], wanted.indexesInBlocks[position]);
                }
                planned.fields.append(fields[shared].field);
            }
        }
        for (const FieldOfGroup& field : fields) {
            for (size_t position = 0; position < runs.size(); ++position) {
                const FoundCaptures& store = runsOfLayout[runs[position]][0];
                plan.addedStores.append({ store.function, store.block, field.indexesInBlocks[position], layout, store.environment, field.field, field.operands[position] });
            }
        }
    };

    auto haveSameLayout = [&](const FoundCaptures& a, const FoundCaptures& b) {
        return layoutOf(a) == layoutOf(b);
    };
    auto forEachLayout = [&](NOESCAPE const Invocable<void(LayoutID, Stores, std::span<const Stores>, Vector<Member, 16>&)> auto& functor) {
        for (Stores stores : found.span() | std::views::chunk_by(haveSameLayout)) {
            LayoutID layout = layoutOf(stores[0]);
            // The stores of one block into one environment.
            Vector<Stores, 2> runs;
            for (auto run : stores | std::views::chunk_by([](const FoundCaptures& a, const FoundCaptures& b) { return a.function == b.function && a.environment == b.environment && a.block == b.block; }))
                runs.append(Stores(run));

            Vector<Member, 16> members;
            candidates.forEachCandidateOf(layout, [&](uint32_t candidate) {
                if (plan.closures[candidate].obstacle != Obstacle::None)
                    return;
                Member member { { }, candidate };
                for (uint32_t index = 0; index < runs.size(); ++index) {
                    size_t numberOfStores = std::ranges::count(runs[index], candidate, &FoundCaptures::candidate);
                    if (numberOfStores > 1) {
                        reject(candidate, Obstacle::IsFilledTwiceInOneBlock);
                        return;
                    }
                    if (numberOfStores)
                        member.runs.append(index);
                }
                members.append(WTF::move(member));
            });
            if (!members.isEmpty())
                functor(layout, stores, runs.span(), members);
        }
    };

    forEachLayout([&](LayoutID layout, Stores stores, std::span<const Stores>, Vector<Member, 16>& members) {
        for (const Member& member : members) {
            PlannedClosure& planned = plan.closures[member.candidate];
            planned.layout = layout;
            planned.typeOfEnvironment = program.at(stores[0].function).at(stores[0].environment).type;
            if (!isReadAsValue.quickGet(member.candidate))
                planned.fate = Fate::IsNeverMade;
            else
                planned.fate = isUsedWhereItIsMade.quickGet(member.candidate) ? Fate::IsMadeAsBefore : Fate::IsMadeAtFirstUse;
        }
    });
    // A closure that is made as before is an object wherever it goes.
    closureParameters.removeAllMatching([&](const ClosureParameter& parameter) {
        Fate fate = plan.closures[parameter.candidate].fate;
        return fate != Fate::IsNeverMade && fate != Fate::IsMadeAtFirstUse;
    });
    if (!closureParameters.parameters.isEmpty()) {
        for (const FoundCaptures& store : found) {
            if (store.indexOfClosure)
                environmentsOfClosures.append({ store.function, store.closure, store.environment });
        }
        AOT::sort(environmentsOfClosures);
    }
    for (const ClosureParameter& parameter : closureParameters.parameters) {
        PlannedClosure& planned = plan.closures[parameter.candidate];
        if (!planned.typeOfEnvironmentOrAbsent)
            planned.typeOfEnvironmentOrAbsent = editor.types().intern(program.at(planned.typeOfEnvironment).withPrimitives(Primitive::Absent));
    }

    forEachLayout([&](LayoutID layout, Stores stores, std::span<const Stores> runs, Vector<Member, 16>& members) {
        // Closures that are stored at the same places can share the fields for what they capture.
        AOT::sort(members, [](const Member& a, const Member& b) {
            if (a.runs != b.runs)
                return std::ranges::lexicographical_compare(a.runs, b.runs);
            return a.candidate < b.candidate;
        });
        uint32_t nextField = program.at(layout).fields.size();
        for (std::span<const Member> group : members.span() | std::views::chunk_by([](const Member& a, const Member& b) { return a.runs == b.runs; }))
            planGroup(layout, runs, group, nextField);

        for (const FoundCaptures& store : stores) {
            if (plan.closures[store.candidate].fate != Fate::IsMadeAtFirstUse)
                continue;
            plan.closures[store.candidate].closure = program.at(store.function).at(store.closure);
            plan.removedStores.append({ store.function, store.store, candidates.fields[store.candidate].field, store.closure });
        }
    });

    AOT::sort(plan.addedStores, [](const AddedStore& a, const AddedStore& b) {
        if (a.function != b.function)
            return a.function < b.function;
        return std::tuple { b.block, b.indexInBlock, b.field } < std::tuple { a.block, a.indexInBlock, a.field };
    });
    AOT::sort(plan.removedStores);
    for (const AddedStore& added : plan.addedStores)
        plan.editedFunctions.append(added.function);
    for (const RemovedStore& removed : plan.removedStores)
        plan.editedFunctions.append(removed.function);
    AOT::sort(plan.editedFunctions);
    removeRepeatedElements(plan.editedFunctions);

    // Only the operations of the program and the scopes of eval reach a field of an environment, and the scan has seen them all.
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        FieldReference reference = candidates.fields[candidate];
        if (plan.closures[candidate].fate == Fate::IsNeverMade) {
            LocationFlow& location = editor.at(reference.layout).fields[reference.field].flow.location;
            location.writers = WriterState::AllKnown;
            location.storeNaming = StoreNaming::EveryStoreNamesLocation;
            location.reads = ReadState::NeverRead;
        }
        if (plan.closures[candidate].fate == Fate::IsMadeAtFirstUse) {
            Field& field = editor.at(reference.layout).fields[reference.field];
            field.declared.changeType(Proven::byPosition(editor.types().intern(editor.program().at(field.declared.written()).withPrimitives(Primitive::Absent))));
            field.flags.remove({ FieldFlags::IsRequired, FieldFlags::IsImmutable });
            field.flow.location.stores = StoreCount::Unknown;
            field.flow.location.initialization = InitializationState::Unknown;
            field.flow.location.reads = ReadState::Unknown;
            field.flow.occupancy = Occupancy::Unknown;
        }
    }
    return plan;
}

static Value makeFieldAccess(Opcode opcode, std::initializer_list<ValueID> operands, LayoutID layout, uint32_t field)
{
    Value access = IRBuilder::make(opcode, operands);
    access.setLayout(layout);
    access.setField(field);
    return access;
}

static void eraseListedValues(IRBuilder& builder, const BitVector& erasedValues)
{
    Function& function = builder.function();
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (erasedValues.get(values[index].index()))
                erasedIndexes.set(index);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), erasedIndexes);
    }
}

static void removeStores(const CaptureReadPlan& plan, FunctionID id, IRBuilder& builder)
{
    Function& function = builder.function();
    std::span<const RemovedStore> removed = entriesIn(plan.removedStores, id);
    if (removed.empty())
        return;
    BitVector erasedValues(function.values.size());
    for (std::span<const RemovedStore> removedFromStore : removed | std::views::chunk_by([](const RemovedStore& a, const RemovedStore& b) { return a.store == b.store; })) {
        for (const RemovedStore& entry : removedFromStore)
            erasedValues.quickSet(entry.closure.index());
        Value& store = function.edit(removedFromStore[0].store);
        if (store.opcode == Opcode::PutEnvironmentField) {
            erasedValues.quickSet(removedFromStore[0].store.index());
            continue;
        }
        RELEASE_ASSERT(store.opcode == Opcode::NewObject);
        Vector<uint32_t, 16> remainingFields;
        Vector<ValueID, 16> remainingValues;
        std::span<const uint32_t> fields = store.fields();
        std::span<const ValueID> initialValues = store.variadicOperands();
        for (size_t position = 0; position < fields.size(); ++position) {
            if (std::ranges::contains(removedFromStore, fields[position], &RemovedStore::field))
                continue;
            remainingFields.append(fields[position]);
            remainingValues.append(initialValues[position]);
        }
        store.setFields(remainingFields.span());
        store.setVariadicOperands(remainingValues.span());
    }
    eraseListedValues(builder, erasedValues);
}

// Where a call hands undefined to a parameter that takes what stands for a closure, it hands over the mark of a missing one.
static void giveAbsentInPlaceOfUndefined(const ClosureParameterTable& closureParameters, const ClosureWebs& webs, IRBuilder& builder)
{
    Function& function = builder.function();
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID call = values[index];
            if (function.at(call).opcode != Opcode::CallDirect)
                continue;
            for (const ClosureParameter& parameter : closureParameters.parametersOf(function.at(call).function())) {
                ValueID given = function.at(call).operand(parameter.index);
                if (webs.standsForCapturesOnly(given))
                    continue;
                RELEASE_ASSERT(function.at(given).type == TypeTable::undefined);
                builder.setOrigin(function.at(call).origin());
                ValueID absent = builder.insertBefore({ BlockID(blockIndex), index }, IRBuilder::make(Opcode::Absent));
                function.edit(call).setOperand(parameter.index, absent);
            }
        }
    }
    if (builder.hasInsertions())
        builder.applyInsertions();
}

static void readCaptures(const Program& program, const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, const ClosureWebs& webs, IRBuilder& builder)
{
    Function& function = builder.function();
    ValueSubstitution substitution(function);
    auto typeInPlaceOfValue = [&](ValueID id) {
        return typeInPlaceOf(plan.closures[webs.candidateOf(id)], program.typeOf(function, id));
    };
    // While what they ask still has the type of the closure.
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode != Opcode::ToBoolean && function.at(id).opcode != Opcode::TypeTest)
                continue;
            ValueID asked = function.at(id).operand(0);
            if (!webs.standsForCapturesOnly(asked))
                continue;
            bool asksForClosure = *ClosureWebs::asksForClosure(program, function, function.at(id));
            Value& test = function.edit(id);
            test.become(Opcode::TypeTest);
            test.setOperand(0, asked);
            TypeID whatIsThere = plan.closures[webs.candidateOf(asked)].typeOfEnvironment;
            test.setTestedType(!asksForClosure ? TypeTable::absent : program.at(whatIsThere).isJavaScriptValue() ? TypeTable::anything : whatIsThere);
        }
    }
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID parameter : function.at(block).parameters) {
            if (!webs.standsForCapturesOnly(parameter))
                continue;
            function.edit(parameter, ValueEdit::Type).type = typeInPlaceOfValue(parameter);
            function.edit(parameter, ValueEdit::Type).setExpected({ });
        }

        const BlockValues& values = function.at(block).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            Value& value = function.edit(id);
            if (webs.standsForCapturesOnly(id) && value.opcode == Opcode::GetCapture) {
                value.type = typeInPlaceOfValue(id);
                value.setExpected({ });
            } else if (webs.standsForCapturesOnly(id)) {
                ValueID store = webs.storeOf(id);
                substitution.replace(id, store ? function.at(store).operand(0) : value.operand(0));
                erasedIndexes.set(index);
                continue;
            }
            if (value.opcode == Opcode::PutEnvironmentField && webs.storeOf(value.valueOperand()) == id && webs.standsForCapturesOnly(value.valueOperand())) {
                erasedIndexes.set(index);
                continue;
            }
            if (value.opcode != Opcode::GetCapture)
                continue;
            ValueID closure = value.operand(0);
            if (!webs.standsForCapturesOnly(closure))
                continue;
            uint32_t candidate = webs.candidateOf(closure);
            uint32_t source = plan.closures[candidate].fields[value.index()];
            if (source == noField) {
                substitution.replace(id, closure);
                erasedIndexes.set(index);
                continue;
            }
            value.become(Opcode::GetEnvironmentField);
            value.setOperand(0, closure);
            value.setLayout(candidates.fields[candidate].layout);
            value.setField(source);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(block, erasedIndexes);
    }
    substitution.apply(function, nullptr);
    if (!webs.isHeld)
        return;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            std::span<const ValueID> listed = heldOperandsOf(function.at(id));
            Vector<ValueID, 8> held;
            for (ValueID operand : listed) {
                if (!held.contains(operand))
                    held.append(operand);
            }
            if (held.size() != listed.size())
                setHeldOperands(function.edit(id), held.span());
        }
    }
}

// The field holds nothing until somebody wants the object. It stays the same object from then on.
static void makeClosuresAtReads(const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, const ClosureWebs& webs, IRBuilder& builder)
{
    Function& function = builder.function();
    uint32_t numberOfBlocks = function.blocks.size();
    uint32_t numberOfValues = function.values.size();
    Vector<std::pair<uint32_t, uint32_t>, 8> reads;
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        reads.shrink(0);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            if (id.index() >= numberOfValues || function.at(id).opcode != Opcode::GetEnvironmentField)
                continue;
            uint32_t candidate = webs.candidateOf(id);
            if (candidate != noCandidate && webs.isObject(id) && plan.closures[candidate].fate == Fate::IsMadeAtFirstUse)
                reads.append({ index, candidate });
        }
        // What follows a read moves to another block. What precedes it stays where it is.
        for (auto [index, candidate] : reads.span() | std::views::reverse) {
            auto [layout, field] = candidates.fields[candidate];
            ValueID environment = function.at(function.at(block).values[index]).operand(0);

            IRBuilder::Expansion expansion = builder.beginExpansion({ block, index });
            ValueID held = builder.append(makeFieldAccess(Opcode::GetEnvironmentField, { environment }, layout, field));
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(held, TypeTable::absent), 0, BlockFrequency::Rare, BlockFrequency::Normal);

            Vector<ValueID, 8> captures;
            for (uint32_t source : plan.closures[candidate].fields)
                captures.append(source == noField ? environment : builder.append(makeFieldAccess(Opcode::GetEnvironmentField, { environment }, layout, source)));
            Value closure(plan.closures[candidate].closure);
            closure.setVariadicOperands(captures.span());
            ValueID made = builder.append(WTF::move(closure));
            builder.append(makeFieldAccess(Opcode::PutEnvironmentField, { environment, made }, layout, field));
            builder.endExpansion(expansion, made);

            builder.moveToEndOf(branch.notTaken);
            builder.endExpansion(expansion, branch.provenOnNotTaken);
        }
    }
}

static void dump(const Program& program, const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, PrintStream& out)
{
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        FieldReference reference = candidates.fields[candidate];
        const PlannedClosure& planned = plan.closures[candidate];
        out.print("closure in environment L", reference.layout.index(), " field ", reference.field);
        if (planned.callee)
            out.print(" f", planned.callee.index(), " captures ", program.at(planned.callee).captures.size());
        switch (planned.fate) {
        case Fate::IsLeftAlone:
            out.print(": ", obstacleNameOf(planned.obstacle));
            break;
        case Fate::IsNeverMade:
            out.print(": never made");
            break;
        case Fate::IsMadeAtFirstUse:
            out.print(": made at its first use as a value");
            break;
        case Fate::IsMadeAsBefore:
            out.print(": made as before, used where it is made and read as a value");
            break;
        }
        if (planned.user != Opcode::Undefined)
            out.print(" (", nameOf(planned.user), ")");
        out.println();
    }
    out.println("fields added for captures: ", plan.numberOfAddedFields);
}

namespace {

// A claim about a function: entry parameter `environment` is an environment of the layout, and entry parameter k holds what lies in field fields[k] of that environment.
using OwnCaptures = PlannedClosure;
// By function. The versions of a function share the claim about it.
using OwnCapturesTable = Vector<const OwnCaptures*>;

// The claim about `function` holds only if the one about `restsOn` does. Without that function it is refuted.
struct Doubt {
    FunctionID function;
    FunctionID restsOn;
    // Where the doubt comes from.
    FunctionID user;
    ValueID value;
};

// The value that a cast, or the proof of a test, is another name of.
struct Roots {
    ValueID rootOf(ValueID id)
    {
        for (unsigned steps = 0; id && steps < 64; ++steps) {
            const Value& value = function.at(id);
            if (ClosureWebs::isCast(value.opcode)) {
                id = value.operand(0);
                continue;
            }
            if (value.opcode != Opcode::Parameter)
                return id;
            prepare();
            ValueID proven = provenBy[id.index()];
            if (!proven)
                return id;
            id = proven;
        }
        return id;
    }

    void prepare()
    {
        if (!provenBy.isEmpty())
            return;
        provenBy.grow(function.values.size());
        BitVector hasSeveral(function.values.size());
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            const Value& test = function.at(block.terminator.conditionOperand());
            // The proof of any other test is a guard, which is no name of what was tested.
            if (test.opcode != Opcode::TypeTest)
                continue;
            for (const Target& target : block.terminator.targets) {
                const auto& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < target.proofs.size(); ++index) {
                    ValueID parameter = parameters[target.arguments.size() + index];
                    ValueID proven = test.operand(target.proofs[index]);
                    if (provenBy[parameter.index()] && provenBy[parameter.index()] != proven)
                        hasSeveral.quickSet(parameter.index());
                    provenBy[parameter.index()] = proven;
                }
            }
        }
        for (size_t index : hasSeveral)
            provenBy[index] = { };
    }

    const Function& function;
    Vector<ValueID> provenBy { };
};

struct CallBehindEntryChecks {
    FunctionID function;
    Vector<ValueID, 8> operands;
    // What the body has proven of its result.
    TypeID type;
};

} // anonymous namespace

static bool hasParametersOfSource(const Function& function)
{
    const Vector<ParameterOrigin>& parameterMap = function.derivation.parameterMap;
    for (uint32_t index = 0; index < parameterMap.size(); ++index) {
        if (parameterMap[index].original != index)
            return false;
    }
    return true;
}

// Empty where nothing is claimed.
static OwnCapturesTable claimOwnCaptures(const Program& program, const CaptureReadPlan& plan)
{
    OwnCapturesTable table;
    BitVector liesInSeveralFields(program.functions.size());
    for (const PlannedClosure& planned : plan.closures) {
        if (planned.fate == Fate::IsLeftAlone || planned.environment == notFound)
            continue;
        if (table.isEmpty())
            table.fill(nullptr, program.functions.size());
        if (std::exchange(table[planned.callee.index()], &planned))
            liesInSeveralFields.quickSet(planned.callee.index());
    }
    for (size_t index : liesInSeveralFields)
        table[index] = nullptr;
    if (table.isEmpty())
        return table;

    // What is claimed here is only a guess. The calls decide.
    constexpr OptionSet<FunctionCapabilities> needed { FunctionCapabilities::HasOrdinaryFrame, FunctionCapabilities::IsNeverCalledByHost };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !function.capabilities.containsAll(needed)) {
            table[index] = nullptr;
            continue;
        }
        if (table[index] || !hasParametersOfSource(function))
            continue;
        FunctionID source = function.derivation.source;
        for (unsigned steps = 0; source && steps < 4 && !table[source.index()]; ++steps)
            source = program.at(source).derivation.source;
        if (source && table[source.index()] && function.captures.size() == program.at(source).captures.size())
            table[index] = table[source.index()];
    }
    return table;
}

// Whether every way to the point leads over a store of the value into the field.
static bool isStoredInFrontOf(const Function& function, ProgramPoint point, Roots& roots, ValueID environment, FieldReference field, ValueID stored)
{
    std::optional<CFG> cfg;
    std::optional<Dominators> dominators;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& store = function.at(values[index]);
            if (store.opcode != Opcode::PutEnvironmentField || store.layout() != field.layout || store.field() != field.field
                || roots.rootOf(store.operand(0)) != environment || roots.rootOf(store.valueOperand()) != stored)
                continue;
            if (block == point.block) {
                if (index < point.index)
                    return true;
                continue;
            }
            if (!dominators) {
                cfg.emplace(function);
                dominators.emplace(*cfg, false);
            }
            if (dominators->strictlyDominates(block, point.block))
                return true;
        }
    }
    return false;
}

// Whether what a call hands to a function, or a closure of it captures, is as the claim says.
static bool givesOwnCaptures(const OwnCapturesTable& table, const Function& function, FunctionID id, Roots& roots, const OwnCaptures& claim, std::span<const ValueID> given, ProgramPoint point, Vector<FunctionID, 2>& restsOn)
{
    if (given.size() < claim.fields.size())
        return false;
    ValueID environment = roots.rootOf(given[claim.environment]);
    if (!environment)
        return false;
    const OwnCaptures* own = table[id.index()];
    const auto& parameters = function.at(entryBlock).parameters;
    bool givesOwnEnvironment = own && own->layout == claim.layout && parameters[own->environment] == environment;
    for (uint32_t index = 0; index < claim.fields.size(); ++index) {
        uint32_t field = claim.fields[index];
        if (field == noField)
            continue;
        ValueID root = roots.rootOf(given[index]);
        if (!root)
            return false;
        const Value& value = function.at(root);
        if (value.opcode == Opcode::GetEnvironmentField && value.layout() == claim.layout && value.field() == field && roots.rootOf(value.operand(0)) == environment)
            continue;
        if (givesOwnEnvironment) {
            size_t position = parameters.find(root);
            if (position != notFound && position < own->fields.size() && own->fields[position] == field) {
                restsOn.append(id);
                continue;
            }
        }
        if (value.opcode == Opcode::GetCapture && value.index() == index && table[value.function().index()] == &claim) {
            const Value& capturedEnvironment = function.at(environment);
            if (capturedEnvironment.opcode == Opcode::GetCapture && capturedEnvironment.index() == claim.environment && capturedEnvironment.function() == value.function()
                && roots.rootOf(capturedEnvironment.operand(0)) == roots.rootOf(value.operand(0))) {
                restsOn.append(value.function());
                continue;
            }
        }
        if (!isStoredInFrontOf(function, point, roots, environment, { claim.layout, field }, root))
            return false;
    }
    return true;
}

static void doubtOwnCaptures(const Program& program, const OwnCapturesTable& table, FunctionID id, Vector<Doubt>& doubts)
{
    const Function& function = program.at(id);
    std::optional<Roots> roots;
    Vector<FunctionID, 2> restsOn;
    auto namesFunctionWithClaim = [&](NameKind kind, uint32_t index) {
        return kind == NameKind::Function && table[index];
    };
    forEachListedValueNaming(program, id, Value::opcodesWithFunction, namesFunctionWithClaim, [&](BlockID block, ValueID valueID, const Value& value) {
        if (value.opcode == Opcode::GetCapture || value.opcode == Opcode::IsClosureOf)
            return;
        const OwnCaptures* claim = table[value.function().index()];
        if (!claim)
            return;
        if (value.opcode != Opcode::CallDirect && value.opcode != Opcode::NewClosure) {
            doubts.append({ value.function(), { }, id, valueID });
            return;
        }
        if (!roots)
            roots.emplace(function);
        restsOn.shrink(0);
        // What a closure captures is stored behind it, in the same block.
        const BlockValues& valuesOfBlock = function.at(block).values;
        ProgramPoint point { block, static_cast<uint32_t>(value.opcode == Opcode::NewClosure ? valuesOfBlock.size() : valuesOfBlock.find(valueID)) };
        if (!givesOwnCaptures(table, function, id, *roots, *claim, value.variadicOperands(), point, restsOn)) {
            doubts.append({ value.function(), { }, id, valueID });
            return;
        }
        for (FunctionID other : restsOn) {
            if (other != value.function())
                doubts.append({ value.function(), other, id, valueID });
        }
    });
}

// Where the operands of the call pass every test of the function that it goes to: the call that stands behind the tests.
static std::optional<CallBehindEntryChecks> findCallBehindEntryChecks(const Program& program, const BitVector& holdsEntryChecks, const Function& caller, const Value& call)
{
    if (!holdsEntryChecks.get(call.function().index()))
        return std::nullopt;
    const Function& checks = program.at(call.function());
    const auto& parameters = checks.at(entryBlock).parameters;
    if (parameters.size() != call.numberOfOperands())
        return std::nullopt;
    Vector<ValueID, 32> given(checks.values.size());
    for (uint32_t index = 0; index < parameters.size(); ++index)
        given[parameters[index].index()] = call.operand(index);
    auto enter = [&](const Target& target, const Value* test) {
        const auto& targetParameters = checks.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index)
            given[targetParameters[index].index()] = given[target.arguments[index].index()];
        for (uint32_t index = 0; index < target.proofs.size(); ++index)
            given[targetParameters[target.arguments.size() + index].index()] = test ? given[test->operand(target.proofs[index]).index()] : ValueID();
        return target.block;
    };

    BlockID block = entryBlock;
    for (size_t steps = 0; steps < checks.blocks.size(); ++steps) {
        const Block& current = checks.at(block);
        const Terminator& terminator = current.terminator;
        switch (terminator.kind) {
        case TerminatorKind::Jump:
            if (!current.values.isEmpty())
                return std::nullopt;
            block = enter(terminator.next(), nullptr);
            continue;
        case TerminatorKind::Branch: {
            if (current.values.size() != 1 || terminator.conditionOperand() != current.values[0])
                return std::nullopt;
            const Value& test = checks.at(current.values[0]);
            if (test.opcode != Opcode::TypeTest)
                return std::nullopt;
            ValueID tested = given[test.valueOperand().index()];
            if (!tested || !program.typeOf(caller, tested).isSubsetOf(program.at(test.testedType()), program))
                return std::nullopt;
            block = enter(terminator.taken(), &test);
            continue;
        }
        case TerminatorKind::ReturnCall: {
            if (current.values.size() != 1 || terminator.operand(0) != current.values[0] || terminator.casts().size())
                return std::nullopt;
            const Value& callOfBody = checks.at(current.values[0]);
            if (callOfBody.opcode != Opcode::CallDirect || !program.at(callOfBody.type).isSubsetOf(program.at(call.type), program))
                return std::nullopt;
            CallBehindEntryChecks found { callOfBody.function(), { }, callOfBody.type };
            for (ValueID operand : callOfBody.variadicOperands()) {
                if (!given[operand.index()])
                    return std::nullopt;
                found.operands.append(given[operand.index()]);
            }
            return found;
        }
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Return:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static void callBodiesBehindEntryChecks(const AnalysisContext& context, const BitVector& holdsEntryChecks, Function& function, TypeArena& types)
{
    const Program& program = context.program;
    Vector<ValueID, 8> callsWithNarrowerTypes;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            Value& call = function.edit(id);
            if (call.opcode != Opcode::CallDirect)
                continue;
            std::optional<CallBehindEntryChecks> found = findCallBehindEntryChecks(program, holdsEntryChecks, function, call);
            if (!found)
                continue;
            call.setFunction(found->function);
            call.setArgumentsOperands(found->operands.span());
            if (call.type != found->type) {
                call.type = found->type;
                call.setExpected({ });
                callsWithNarrowerTypes.append(id);
            }
        }
    }
    // What is computed from such a result has its type, and later its form, from the type of the result.
    if (callsWithNarrowerTypes.isEmpty())
        return;
    FunctionAnalyses analyses(context, function);
    narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, types, context.build }, callsWithNarrowerTypes.span());
}

static void readOwnCaptures(const Program& program, const OwnCaptures& own, IRBuilder& builder)
{
    Function& function = builder.function();
    const auto& parameters = function.at(entryBlock).parameters;
    Roots roots { function };
    ValueSubstitution substitution(function);
    BitVector replacedReads(function.values.size());
    // Reads with a wider type than the entry parameter has.
    Vector<std::pair<ValueID, ValueID>, 8> widerReads;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& read = function.at(values[index]);
            if (read.opcode != Opcode::GetEnvironmentField || read.layout() != own.layout || roots.rootOf(read.operand(0)) != parameters[own.environment])
                continue;
            size_t capture = own.fields.find(read.field());
            if (capture == notFound || !program.typeOf(function, parameters[capture]).isSubsetOf(program.at(read.type), program))
                continue;
            bool hasSameType = function.at(parameters[capture]).type == read.type;
            keepReadForValidation(builder, { block, index }, parameters[capture]);
            if (!hasSameType) {
                widerReads.append({ values[index], parameters[capture] });
                continue;
            }
            substitution.replace(values[index], parameters[capture]);
            replacedReads.quickSet(values[index].index());
        }
    }
    if (substitution.isEmpty() && widerReads.isEmpty())
        return;
    builder.applyInsertions();
    if (!widerReads.isEmpty()) {
        // Ascending: the blocks were visited in order, but the values of a function are not numbered by block.
        AOT::sort(widerReads);
        BitVector hasOtherUse(function.values.size());
        auto noteOtherUse = [&](ValueID operand) {
            if (findSecond(widerReads.span(), operand))
                hasOtherUse.set(operand.index());
        };
        for (Block& block : function.editAllBlocks()) {
            for (ValueID id : block.values) {
                Value& user = function.edit(id);
                if (user.opcode != Opcode::CallDirect && user.opcode != Opcode::NewClosure) {
                    user.forEachChild(noteOtherUse);
                    continue;
                }
                for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                    if (ValueID entryParameter = user.operand(operandIndex) ? findSecond(widerReads.span(), user.operand(operandIndex)) : ValueID())
                        user.setOperand(operandIndex, entryParameter);
                }
            }
            block.forEachChildOutsideValues(noteOtherUse);
        }
        for (auto [read, entryParameter] : widerReads) {
            if (!hasOtherUse.get(read.index()))
                replacedReads.quickSet(read.index());
        }
    }
    eraseListedValues(builder, replacedReads);
    substitution.apply(function, nullptr);
}

// A function that lies in an environment runs with that environment among its captures, so what it would read there for a call of itself, or of a function beside it, it has at hand.
static void passOwnCaptures(ProgramEditor& editor, const FlowResults& flow, BuildContext& build, const CaptureReadPlan& plan)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    OwnCapturesTable table = claimOwnCaptures(program, plan);
    auto claimAbout = [&](FunctionID id) {
        return table.isEmpty() ? nullptr : table[id.index()];
    };
    Vector<Doubt> doubts;
    if (!table.isEmpty()) {
        doubts = collectInParallel<Doubt>(loops, program.functions.size(), [&](unsigned index, Vector<Doubt>& doubtsInFunction) {
            doubtOwnCaptures(program, table, FunctionID(index), doubtsInFunction);
        });
    }
    for (bool didChange = !doubts.isEmpty(); std::exchange(didChange, false);) {
        for (const Doubt& doubt : doubts) {
            if (!table[doubt.function.index()] || (doubt.restsOn && table[doubt.restsOn.index()]))
                continue;
            table[doubt.function.index()] = nullptr;
            didChange = true;
            if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]]
                dataLogLn("does not know its captures: f", doubt.function.index(), " because of v", doubt.value.index(), " (", nameOf(program.at(doubt.user).at(doubt.value).opcode), ") in f", doubt.user.index(), doubt.restsOn ? ", which hands on its own" : "");
        }
    }

    BitVector holdsEntryChecks(program.functions.size());
    for (uint32_t index : findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        return facts.role == FunctionRole::EntryChecks && facts.hasBody;
    }))
        holdsEntryChecks.quickSet(index);

    Vector<FunctionID> functions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const OwnCaptures* own = claimAbout(FunctionID(index));
        OpcodeSet wanted { Opcode::CallDirect };
        if (own)
            wanted.add(Opcode::GetEnvironmentField);
        // The tests are read while the others are edited.
        if (!mayListValueWith(program, FunctionID(index), wanted) || holdsEntryChecks.quickGet(index) || !build.isEnabled(PassID::ReadCapturesFromEnvironments, FunctionID(index)))
            return false;
        bool isFound = false;
        auto mayBeWanted = [&](NameKind kind, uint32_t name) {
            if (isFound)
                return false;
            return kind == NameKind::Function ? holdsEntryChecks.quickGet(name) : kind == NameKind::Layout && own && name == own->layout.index();
        };
        forEachListedValueNaming(program, FunctionID(index), wanted, mayBeWanted, [&](BlockID, ValueID, const Value& value) {
            if (value.opcode == Opcode::GetEnvironmentField)
                isFound = value.layout() == own->layout && own->fields.contains(value.field());
            else
                isFound = !!findCallBehindEntryChecks(program, holdsEntryChecks, program.functions[index], value);
        });
        return isFound;
    }).map([](uint32_t index) {
        return FunctionID(index);
    });
    if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]] {
        for (FunctionID id : functions) {
            if (const OwnCaptures* own = claimAbout(id))
                dataLogLn("has its captures at hand: f", id.index(), " in environment L", own->layout.index(), " at entry parameter ", own->environment);
        }
    }
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        AnalysisContext context { build, program, &flow, nullptr, id };
        if (const OwnCaptures* own = claimAbout(id)) {
            IRBuilder builder(context, function, arenas.types);
            readOwnCaptures(program, *own, builder);
        }
        callBodiesBehindEntryChecks(context, holdsEntryChecks, function, arenas.types);
    });
}

void readCapturesFromEnvironments(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    CaptureCandidateTable candidates = findCandidates(program);
    ClosureParameterTable closureParameters;
    CaptureReadPlan plan;
    if (!candidates.fields.isEmpty()) {
        closureParameters = findClosureParameters(program, build, candidates);
        plan = planCaptureReads(editor, candidates, closureParameters, collectInParallel<FoundCaptures>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FoundCaptures>& found) {
            if (program.functions[index].hasBody())
                scanFunction(program, build, candidates, closureParameters, FunctionID(index), found);
        }));
        for (const ClosureParameter& parameter : closureParameters.parameters) {
            DeclaredType& declared = *editor.at(parameter.function).declaredEntryParameter(parameter.index);
            declared.changeType(Proven::byPosition(typeInPlaceOf(plan.closures[parameter.candidate], program.at(declared.written()))));
        }
        if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]] {
            dump(program, candidates, plan, WTF::dataFile());
            for (const ClosureParameter& parameter : closureParameters.parameters) {
                dataLog("takes the environment in place of the closure: f", parameter.function.index(), " entry parameter ", parameter.index);
                if (parameter.candidate < candidates.fields.size())
                    dataLogLn(", L", candidates.fields[parameter.candidate].layout.index(), " field ", candidates.fields[parameter.candidate].field);
                else
                    dataLogLn(", what f", plan.closures[parameter.candidate].callee.index(), " captures");
            }
        }
    }
    bool takesAny = plan.closures.containsIf([](const PlannedClosure& planned) {
        return planned.fate != Fate::IsLeftAlone;
    });
    if (takesAny) {
        editFunctionsInParallel(editor, build, plan.editedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
            // Of the body that the scan saw: a store that is added for a capture is no use of what it stores.
            ClosureWebs webs(program, function, id, closureParameters, [&](const Value& read) {
                auto candidate = candidates.find(read.layout(), read.field());
                return candidate && plan.closures[*candidate].fate != Fate::IsLeftAlone ? *candidate : noCandidate;
            }, [&](const Value& store) {
                // A closure that is made as before is an object wherever it goes.
                auto candidate = candidates.find(store.layout(), store.field());
                return candidate && (plan.closures[*candidate].fate == Fate::IsNeverMade || plan.closures[*candidate].fate == Fate::IsMadeAtFirstUse) ? *candidate : noCandidate;
            });
            for (const ClosureParameter& parameter : closureParameters.parametersOf(id))
                RELEASE_ASSERT(webs.standsForCapturesOnly(function.at(entryBlock).parameters[parameter.index]));
            for (const AddedStore& added : entriesIn(plan.addedStores, id)) {
                builder.setOrigin(function.at(function.at(added.block).values[added.indexInBlock]).origin());
                ValueID store = builder.addDetached(makeFieldAccess(Opcode::PutEnvironmentField, { added.environment, added.value }, added.layout, added.field));
                function.edit(added.block, BlockEdit::Values).values.insert(added.indexInBlock + 1, store);
            }
            giveAbsentInPlaceOfUndefined(closureParameters, webs, builder);
            if (!webs.isEmpty)
                readCaptures(program, candidates, plan, webs, builder);
            removeStores(plan, id, builder);
            if (!webs.isEmpty)
                makeClosuresAtReads(candidates, plan, webs, builder);
        });
    }
    passOwnCaptures(editor, flow, build, plan);
}

namespace {

enum class HolderAction : uint8_t { Disqualify, Ignore, Mark };

constexpr OperandUses usesLeavingFunction { OperandUse::Retains, OperandUse::Returns };

struct EscapeMark {
    ProgramPoint point;
    ValueID environment;
    LayoutID layout;
};

struct ReleasePoint {
    std::optional<Edge> edge;
    ProgramPoint point;
    ValueID environment;
    LayoutID layout;
};

struct EnvironmentSite {
    LayoutID layout;
    BlockID block;

    friend auto operator<=>(const EnvironmentSite&, const EnvironmentSite&) = default;
};

struct FunctionPlan {
    Vector<EscapeMark> marks;
    Vector<ReleasePoint> releases;
    Vector<LayoutID> disqualifiedLayouts;
    // Ascending.
    Vector<EnvironmentSite> sites;
    // Which of its several results are environments that the function has made itself, whichever way it returns.
    BitVector resultsMadeHere;
    // No body and no summary changes until the functions are rewritten. So the first step that needs the analysis leaves what it says for the steps behind it.
    bool knowsOwnedValues { false };
    // The values that can only be objects that the function has allocated on the heap and that the analysis has followed.
    BitVector owned;
    // Those of them that may be an object that the function hands back among several results.
    BitVector handedBack;
};

} // anonymous namespace

static bool haveSameLayout(const EnvironmentSite& a, const EnvironmentSite& b)
{
    return a.layout == b.layout;
}

static bool isAlwaysCell(const Program& program, const Function& function, ValueID id)
{
    return !program.typeOf(function, id).primitives;
}

static const FunctionPlan& withOwnedValues(FunctionPlan& plan, FunctionAnalyses& analyses)
{
    if (std::exchange(plan.knowsOwnedValues, true))
        return plan;
    const Function& function = analyses.function();
    // A value is owned only if it is an object that the function has allocated on the heap.
    auto isAllocationOnHeap = [](const Value& value) {
        return value.opcode == Opcode::NewObject && value.storage() == AllocationStorage::Heap;
    };
    if (!function.values.containsIf(isAllocationOnHeap))
        return plan;
    const EscapeAnalysis& escape = analyses.escape();
    if (escape.gaveUp())
        return plan;

    BitVector objectsHandedBack;
    for (const Value& value : function.values) {
        if (value.opcode != Opcode::Results)
            continue;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            objectsHandedBack.merge(escape.pointsTo(operand).objects.bits());
        });
    }
    auto isOwned = [&](const PointsToSet& pointsTo) {
        if (pointsTo.mayBeUntracked || pointsTo.objects.isEmpty())
            return false;
        for (ObjectID object : pointsTo.objects) {
            const TrackedObject& tracked = escape.at(object);
            if (tracked.kind != ObjectKind::Allocation || tracked.state == EscapeState::Unknown || !isAllocationOnHeap(function.at(tracked.definition)))
                return false;
        }
        return true;
    };
    for (unsigned index = 0; index < function.values.size(); ++index) {
        PointsToSet pointsTo = escape.pointsTo(ValueID(index));
        if (!isOwned(pointsTo))
            continue;
        plan.owned.set(index);
        for (ObjectID object : pointsTo.objects) {
            if (objectsHandedBack.get(object.index()))
                plan.handedBack.set(index);
        }
    }
    return plan;
}

static void sortReachableSites(const Function& function, Vector<EnvironmentSite>& sites)
{
    CFG cfg(function);
    sites.removeAllMatching([&](const EnvironmentSite& site) {
        return !cfg.isReachable(site.block);
    });
    AOT::sort(sites);
    removeRepeatedElements(sites);
}

static void classifyHolders(const AnalysisContext& context, const EffectContext& effects, const EnvironmentTypes& types, FunctionPlan& plan)
{
    const Program& program = context.program;
    if (!hasBody(program, context.id))
        return;
    const Function& function = program.at(context.id);
    BodyIndex* bodyIndex = program.bodyIndex();
    std::optional<NamesInBody> names = bodyIndex ? bodyIndex->namesIn(program, context.id) : std::nullopt;
    bool mayHaveCandidateValue = !names || std::ranges::any_of(names->types, [&](TypeID type) {
        return !!types.environmentOf(type);
    });
    bool hasCandidateValue = mayHaveCandidateValue && function.values.containsIf([&](const Value& value) {
        return !!types.environmentOf(value.type);
    });
    if (!hasCandidateValue)
        return;

    bool mayChange = context.build.isEnabled(PassID::ReuseEnvironments, context.id);
    auto environmentOf = [&](ValueID id) {
        return types.environmentOf(function, id);
    };
    auto disqualify = [&](LayoutID layout) {
        if (layout)
            plan.disqualifiedLayouts.append(layout);
    };
    auto apply = [&](HolderAction action, ProgramPoint point, ValueID environment, LayoutID layout) {
        if (action == HolderAction::Mark && (!mayChange || !isAlwaysCell(program, function, environment)))
            action = HolderAction::Disqualify;
        switch (action) {
        case HolderAction::Disqualify:
            plan.disqualifiedLayouts.append(layout);
            return;
        case HolderAction::Ignore:
            return;
        case HolderAction::Mark:
            plan.marks.append({ point, environment, layout });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto classifyOperand = [&](ValueID userID, uint32_t operandIndex, LayoutID layout) {
        const Value& user = function.at(userID);
        if (user.opcode == Opcode::CallDirect) {
            const Function& callee = program.at(user.function());
            std::optional<uint32_t> entryParameter = entryParameterOf(callee, user, operandIndex);
            const DeclaredType* declared = callee.hasBody() && entryParameter ? callee.declaredEntryParameter(*entryParameter) : nullptr;
            return declared && types.environmentOf(declared->written()) == layout ? HolderAction::Ignore : HolderAction::Disqualify;
        }
        // One of several results goes to the caller under its declared type, and the caller's uses of it are classified like any other. What holds the results in between is followed like the environment itself.
        if (user.opcode == Opcode::Results)
            return operandIndex < function.results.size() && types.environmentOf(function.results[operandIndex].written()) == layout ? HolderAction::Ignore : HolderAction::Disqualify;
        // No operation reads the hidden field that holds the environment. The closures that the object makes on demand do: they are cells of the heap wherever the object lies.
        if (user.opcode == Opcode::NewObject) {
            std::optional<uint32_t> field = program.at(user.layout()).flow.closureEnvironmentField;
            std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
            if (field && position && *position < user.fields().size() && user.fields()[*position] == *field)
                return HolderAction::Mark;
        }

        OperandSummary summary = summarizeOperand(effects, function, user, operandIndex);
        if (summary.uses.contains(OperandUse::PassesToUserCode))
            return HolderAction::Mark;
        // A closure that is lent may be copied to the heap by the stub of its call, and the copy holds what the closure holds.
        if (summary.uses.contains(OperandUse::Retains))
            return summary.retainedIn == RetainedIn::Result && isFrameCell(function, userID) && !(user.opcode == Opcode::NewClosure && user.isLent()) ? HolderAction::Ignore : HolderAction::Mark;
        if (summary.uses.contains(OperandUse::Returns) && environmentOf(userID) != layout)
            return HolderAction::Disqualify;
        return HolderAction::Ignore;
    };

    Vector<BlockID> blocksWithSuspensions;
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        const Block& block = function.blocks[blockIndex];
        bool hasSuspension = false;
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID userID = block.values[index];
            const Value& user = function.at(userID);
            if (user.opcode == Opcode::Unspecified)
                disqualify(environmentOf(userID));
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (LayoutID layout = environmentOf(operand))
                    apply(classifyOperand(userID, operandIndex, layout), { blockID, index }, operand, layout);
            });
            hasSuspension |= maySuspend(user.opcode);
        }
        if (hasSuspension)
            blocksWithSuspensions.append(blockID);

        block.terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            LayoutID layout = environmentOf(operand);
            if (layout && summarizeOperand(effects, function, block.terminator, operandIndex).uses.containsAny(usesLeavingFunction))
                plan.disqualifiedLayouts.append(layout);
        });
        if (block.subject)
            disqualify(environmentOf(block.subject));
        block.forEachTarget([&](const Target& target) {
            auto& parameters = function.at(target.block).parameters;
            for (unsigned argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
                LayoutID layout = environmentOf(target.arguments[argumentIndex]);
                if (layout && (argumentIndex >= parameters.size() || environmentOf(parameters[argumentIndex]) != layout))
                    plan.disqualifiedLayouts.append(layout);
            }
        });
    }

    if (!blocksWithSuspensions.isEmpty()) {
        BitVector candidateValues(function.values.size());
        for (unsigned index = 0; index < function.values.size(); ++index) {
            if (environmentOf(ValueID(index)))
                candidateValues.quickSet(index);
        }
        FunctionAnalyses analyses(context, function);
        Liveness liveness(function, analyses.cfg(), analyses.useLists(), LivenessOptions { .trackedValues = &candidateValues });
        for (BlockID blockID : blocksWithSuspensions) {
            if (!analyses.cfg().isReachable(blockID))
                continue;
            auto& values = function.at(blockID).values;
            Liveness::LocalCalc calc(liveness, blockID);
            for (uint32_t index = values.size(); index--;) {
                ValueID suspension = values[index];
                if (maySuspend(function.at(suspension).opcode)) {
                    calc.forEachLive([&](ValueID live) {
                        if (live != suspension && !withOwnedValues(plan, analyses).owned.get(live.index()))
                            apply(HolderAction::Mark, { blockID, index }, live, environmentOf(live));
                    });
                }
                calc.execute(index);
            }
        }
    }

    AOT::sort(plan.disqualifiedLayouts);
    removeRepeatedElements(plan.disqualifiedLayouts);
    AOT::sort(plan.marks, { }, [](const EscapeMark& mark) {
        return std::tuple { mark.point.block, mark.environment, mark.point.index };
    });
    removeRepeatedElements(plan.marks, [](const EscapeMark& a, const EscapeMark& b) {
        return a.point.block == b.point.block && a.environment == b.environment;
    });
}

// Several results are held by a value that has no type of its own: by Results until the function returns, and by the call until the last ResultAt.
static bool holdsAmongSeveralResults(const Program& program, const EnvironmentTypes& types, const Function& function, const Value& value, LayoutID layout)
{
    if (value.opcode == Opcode::Results)
        return types.hasOperandOf(layout, function, value);
    if (value.opcode != Opcode::CallDirect && value.opcode != Opcode::CallTable)
        return false;
    auto& results = program.at(value.function()).results;
    return results.size() > 1 && results.containsIf([&](const auto& result) {
        return types.environmentOf(result.written()) == layout;
    });
}

// An environment that a function has made and hands back among its results is the caller's from there on: nobody else has it, but for those who have marked it.
static void findResultsMadeHere(const AnalysisContext& context, const EnvironmentTypes& types, FunctionPlan& plan)
{
    RecordFacts facts = recordFactsOf(context.program, context.id);
    if (!facts.hasBody || facts.numberOfResults < 2)
        return;
    const Function& function = context.program.at(context.id);
    BitVector madeHere;
    for (unsigned index = 0; index < function.results.size(); ++index) {
        if (types.environmentOf(function.results[index].written()))
            madeHere.set(index);
    }
    if (madeHere.isEmpty())
        return;

    FunctionAnalyses analyses(context, function);
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        if (!analyses.cfg().isReachable(BlockID(blockIndex)))
            continue;
        const Terminator& terminator = function.blocks[blockIndex].terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return;
        if (terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = terminator.valueOperand();
        if (!returned || function.at(returned).opcode != Opcode::Results || function.at(returned).numberOfVariadicOperands() != function.results.size())
            return;
        function.at(returned).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (madeHere.get(operandIndex) && !(isAlwaysCell(context.program, function, operand) && withOwnedValues(plan, analyses).owned.get(operand.index())))
                madeHere.clear(operandIndex);
        });
    }
    plan.resultsMadeHere = WTF::move(madeHere);
}

static bool isMadeByCallee(const Function& function, std::span<const FunctionPlan> plans, const Value& value)
{
    if (value.opcode != Opcode::ResultAt)
        return false;
    const Value& call = function.at(value.resultsOperand());
    return call.opcode == Opcode::CallDirect && plans[call.function().index()].resultsMadeHere.get(value.index());
}

static void findReleasePoints(const AnalysisContext& context, const EnvironmentTypes& types, const BitVector& candidateLayouts, std::span<const FunctionPlan> plans, FunctionPlan& plan)
{
    const Program& program = context.program;
    const Function& function = program.at(context.id);
    FunctionAnalyses analyses(context, function);
    FramePointers framePointers(function);

    for (auto sites : plan.sites.span() | std::views::chunk_by(haveSameLayout)) {
        LayoutID layout = sites.front().layout;
        if (!candidateLayouts.get(layout.index()))
            continue;

        bool tracksFramePointers = std::ranges::any_of(framePointers.allocations(), [&](ValueID frameCell) {
            return types.hasOperandOf(layout, function, function.at(frameCell));
        });
        const UseLists& useLists = analyses.useLists();
        Vector<ValueID> trackedValues;
        BitVector isTracked(function.values.size());
        BitVector definesTrackedValue(function.blocks.size());
        for (unsigned index = 0; index < function.values.size(); ++index) {
            ValueID id(index);
            bool isFollowed = types.environmentOf(function, id) == layout
                || (tracksFramePointers && framePointers.contains(id))
                || holdsAmongSeveralResults(program, types, function, function.at(id), layout);
            if (!isFollowed)
                continue;
            ValueLocation location = useLists.locationOf(id);
            if (location.placement == ValuePlacement::Detached)
                continue;
            trackedValues.append(id);
            isTracked.quickSet(index);
            definesTrackedValue.quickSet(location.block.index());
        }
        Liveness liveness(function, analyses.cfg(), useLists, LivenessOptions { .trackedValues = &isTracked });

        auto isAnyLive = [&](const Liveness::LocalCalc& calc) {
            return std::ranges::any_of(trackedValues, [&](ValueID id) {
                return calc.isLive(id);
            });
        };
        auto isOwnedEnvironment = [&](ValueID id) {
            if (types.environmentOf(function, id) != layout || !isAlwaysCell(program, function, id))
                return false;
            if (isMadeByCallee(function, plans, function.at(id)))
                return true;
            // This function has done with an environment that it hands back among its results, and the caller has not: it is the caller's to let go of.
            return withOwnedValues(plan, analyses).owned.get(id.index()) && !plan.handedBack.get(id.index());
        };
        auto findReleasableEnvironment = [&](const Liveness::LocalCalc& calc) {
            for (ValueID live : trackedValues) {
                if (!calc.isLive(live))
                    continue;
                if (isOwnedEnvironment(live))
                    return live;
                if (!std::ranges::binary_search(framePointers.allocations(), live))
                    continue;
                for (ValueID operand : function.at(live).operands()) {
                    if (operand && isOwnedEnvironment(operand))
                        return operand;
                }
            }
            return ValueID();
        };

        for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID blockID(blockIndex);
            if (liveness.liveAtHead(blockID).empty() && !definesTrackedValue.quickGet(blockIndex))
                continue;
            const Block& block = function.at(blockID);
            Liveness::LocalCalc calc(liveness, blockID);
            bool isLive = isAnyLive(calc);
            if (isLive) {
                Vector<Edge, 2> edges;
                for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
                    BlockID successor = block.terminator.targets[targetIndex].block;
                    if (!liveness.liveAtHead(successor).empty())
                        continue;
                    bool hasTrackedParameter = std::ranges::any_of(function.at(successor).parameters, [&](ValueID parameter) {
                        return isTracked.quickGet(parameter.index());
                    });
                    if (!hasTrackedParameter)
                        edges.append(Edge { blockID, targetIndex });
                }
                if (ValueID environment = edges.isEmpty() ? ValueID() : findReleasableEnvironment(calc)) {
                    for (Edge edge : edges)
                        plan.releases.append({ edge, { }, environment, layout });
                }
            }
            for (uint32_t index = block.values.size(); index--;) {
                calc.execute(index);
                bool wasLive = std::exchange(isLive, isAnyLive(calc));
                // Nothing stands between a tail call and the end of the block, and the callee still has the environment.
                if (wasLive || !isLive || (block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == block.values[index]))
                    continue;
                if (hasNeverResult(function.at(block.values[index])))
                    continue;
                if (ValueID environment = findReleasableEnvironment(calc))
                    plan.releases.append({ std::nullopt, { blockID, index + 1 }, environment, layout });
            }
        }
    }
}

static bool reachesUnmarkedRelease(const Function& function, const FunctionPlan& plan, std::span<const EnvironmentSite> sites)
{
    LayoutID layout = sites[0].layout;
    BitVector blocksWithReleases(function.blocks.size());
    for (auto& release : plan.releases) {
        if (release.layout == layout)
            blocksWithReleases.quickSet((release.edge ? release.edge->from : release.point.block).index());
    }
    if (blocksWithReleases.isEmpty())
        return false;
    BitVector blocksWithMarks(function.blocks.size());
    for (auto& mark : plan.marks) {
        if (mark.layout == layout)
            blocksWithMarks.quickSet(mark.point.block.index());
    }

    BitVector seen(function.blocks.size());
    Vector<BlockID, 16> worklist;
    auto visit = [&](BlockID block) {
        if (!seen.quickSet(block.index()))
            worklist.append(block);
    };
    for (const EnvironmentSite& site : sites)
        visit(site.block);
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (blocksWithMarks.quickGet(block.index()))
            continue;
        if (blocksWithReleases.quickGet(block.index()))
            return true;
        function.at(block).forEachTarget([&](const Target& target) {
            visit(target.block);
        });
    }
    return false;
}

static void rewriteFunction(const AnalysisContext& context, Function& function, TypeArena& types, const FunctionPlan& plan, const BitVector& reusedLayouts)
{
    IRBuilder builder(context, function, types);
    Vector<ReleasePoint> releases;
    Vector<ReleasePoint*> edgeReleases;
    for (const ReleasePoint& release : plan.releases) {
        if (reusedLayouts.get(release.layout.index()))
            releases.append(release);
    }
    for (ReleasePoint& release : releases) {
        if (release.edge)
            edgeReleases.append(&release);
    }
    if (!edgeReleases.isEmpty()) {
        AOT::sort(edgeReleases, { }, [](const ReleasePoint* release) {
            return std::pair { release->edge->from, release->edge->targetIndex };
        });
        CFG cfg(function);
        for (std::span<ReleasePoint* const> releasesOnEdge : edgeReleases.span() | std::views::chunk_by([](const ReleasePoint* a, const ReleasePoint* b) { return a->edge == b->edge; })) {
            BlockID target = targetOf(function, *releasesOnEdge[0]->edge).block;
            BlockID block = cfg.singleIncomingEdge(target) ? target : builder.splitEdge(*releasesOnEdge[0]->edge);
            for (ReleasePoint* release : releasesOnEdge)
                release->point = { block, 0 };
        }
    }

    auto insertOperation = [&](ProgramPoint point, Opcode opcode, ValueID environment) {
        const Block& block = function.at(point.block);
        if (block.values.isEmpty())
            builder.setOrigin(block.terminator.origin());
        else
            builder.setOrigin(function.at(point.index < block.values.size() ? block.values[point.index] : block.values.last()).origin());
        builder.insertBefore(point, IRBuilder::make(opcode, { environment }));
    };
    for (const ReleasePoint& release : releases)
        insertOperation(release.point, Opcode::ReleaseEnvironment, release.environment);
    for (const EscapeMark& mark : plan.marks) {
        if (reusedLayouts.get(mark.layout.index()))
            insertOperation(mark.point, Opcode::MarkEnvironmentEscaped, mark.environment);
    }
    builder.applyInsertions();
}

void EnvironmentReuse::dump(PrintStream& out, const Program&) const
{
    out.print("reused environments: [");
    CommaPrinter separator(" "_s);
    for (size_t index : m_reusedLayouts)
        out.print(separator, "L", index);
    out.println("]");
}

EnvironmentReuse reuseEnvironments(ProgramEditor& editor, const FlowResults& flow, const FrameAllocation& frameAllocation, BuildContext& build, NOESCAPE const ScopedLambda<BorrowedParameters()>& computeBorrowedParameters)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Vector<FunctionPlan> plans = createVectorInParallel<FunctionPlan>(loops, program.functions.size());

    // The next turn of a loop can take the cell that this turn has done with, and so can the next call of the function.
    BitVector environmentLayouts(program.layouts.size());
    for (uint32_t index : findIndicesInParallel(loops, program.layouts.size(), [&](unsigned index) {
        return program.layouts[index].kind == LayoutKind::Environment;
    }))
        environmentLayouts.quickSet(index);
    auto isEnvironmentLayout = [&](NameKind kind, uint32_t index) {
        return kind == NameKind::Layout && environmentLayouts.get(index);
    };
    forEachIndexInParallel(loops, plans.size(), [&](unsigned index) {
        Vector<EnvironmentSite>& sites = plans[index].sites;
        forEachListedValueNaming(program, FunctionID(index), { Opcode::NewObject }, isEnvironmentLayout, [&](BlockID block, ValueID, const Value& value) {
            if (value.opcode == Opcode::NewObject && value.storage() == AllocationStorage::Heap && program.at(value.layout()).kind == LayoutKind::Environment && !frameAllocation.mayBeInFrame(value.layout()))
                sites.append({ value.layout(), block });
        });
        if (!sites.isEmpty())
            sortReachableSites(program.functions[index], sites);
    });
    BitVector candidateLayouts;
    for (const FunctionPlan& plan : plans) {
        for (const EnvironmentSite& site : plan.sites)
            candidateLayouts.set(site.layout.index());
    }
    if (candidateLayouts.isEmpty())
        return { };

    EffectSummaries summaries = createSummariesFromFlow(program, flow, computeBorrowedParameters(), build);
    EffectContext effects { program, summaries };
    auto contextOf = [&](FunctionID id) {
        return AnalysisContext { build, program, &flow, &summaries, id };
    };
    EnvironmentTypes types { Vector<LayoutID>(program.types.size()) };
    for (unsigned index = 0; index < types.environments.size(); ++index) {
        LayoutID environment = program.at(TypeID(index)).environment;
        if (environment && candidateLayouts.get(environment.index()))
            types.environments[index] = environment;
    }
    Vector<uint32_t> largestFunctionsFirst = largeFunctions(program, loops);
    forEachIndexInParallel(loops, plans.size(), largestFunctionsFirst.span(), [&](unsigned index) {
        classifyHolders(contextOf(FunctionID(index)), effects, types, plans[index]);
        findResultsMadeHere(contextOf(FunctionID(index)), types, plans[index]);
    });
    for (const FunctionPlan& plan : plans) {
        for (LayoutID layout : plan.disqualifiedLayouts)
            candidateLayouts.clear(layout.index());
    }
    bool someResultIsMadeByItsFunction = plans.containsIf([](const FunctionPlan& plan) {
        return !plan.resultsMadeHere.isEmpty();
    });
    if (someResultIsMadeByItsFunction) {
        // Where a caller takes an environment that the callee has made out of the results, it has one to let go of, as where it makes one.
        forEachIndexInParallel(loops, plans.size(), largestFunctionsFirst.span(), [&](unsigned index) {
            const Function& function = program.functions[index];
            Vector<EnvironmentSite>& sites = plans[index].sites;
            size_t numberOfSitesBefore = sites.size();
            forEachListedValue(program, FunctionID(index), { Opcode::ResultAt }, [&](BlockID block, uint32_t, ValueID, const Value& value) {
                LayoutID layout = types.environmentOf(value.type);
                if (layout && candidateLayouts.get(layout.index()) && isMadeByCallee(function, plans.span(), value))
                    sites.append({ layout, block });
            });
            if (sites.size() != numberOfSitesBefore)
                sortReachableSites(function, sites);
        });
    }

    Vector<FunctionID> functionsWithSites;
    for (unsigned index = 0; index < plans.size(); ++index) {
        bool hasSite = plans[index].sites.containsIf([&](const EnvironmentSite& site) {
            return candidateLayouts.get(site.layout.index());
        });
        if (hasSite && build.isEnabled(PassID::ReuseEnvironments, FunctionID(index)))
            functionsWithSites.append(FunctionID(index));
    }
    Vector<Vector<LayoutID>> profitableLayouts(functionsWithSites.size());
    Vector<uint32_t> largestWithSitesFirst;
    for (uint32_t large : largestFunctionsFirst) {
        auto found = std::ranges::lower_bound(functionsWithSites, FunctionID(large));
        if (found != functionsWithSites.end() && *found == FunctionID(large))
            largestWithSitesFirst.append(static_cast<uint32_t>(found - functionsWithSites.begin()));
    }
    forEachIndexInParallel(loops, functionsWithSites.size(), largestWithSitesFirst.span(), [&](unsigned index) {
        FunctionID id = functionsWithSites[index];
        FunctionPlan& plan = plans[id.index()];
        findReleasePoints(contextOf(id), types, candidateLayouts, plans.span(), plan);
        for (std::span<const EnvironmentSite> sites : plan.sites.span() | std::views::chunk_by(haveSameLayout)) {
            if (reachesUnmarkedRelease(program.at(id), plan, sites))
                profitableLayouts[index].append(sites[0].layout);
        }
    });

    EnvironmentReuse result;
    BitVector& reusedLayouts = result.m_reusedLayouts;
    for (auto& profitableInFunction : profitableLayouts) {
        for (LayoutID layout : profitableInFunction)
            reusedLayouts.set(layout.index());
    }
    reusedLayouts.filter(candidateLayouts);
    if (!reusedLayouts.isEmpty()) {
        auto isReused = [&](const auto& operation) {
            return reusedLayouts.get(operation.layout.index());
        };
        Vector<FunctionID> changedFunctions;
        for (unsigned index = 0; index < plans.size(); ++index) {
            if (std::ranges::any_of(plans[index].marks, isReused) || std::ranges::any_of(plans[index].releases, isReused))
                changedFunctions.append(FunctionID(index));
        }
        editFunctionsInParallel(editor, build, changedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            rewriteFunction(contextOf(id), function, arenas.types, plans[id.index()], reusedLayouts);
        });
        build.statistics().count(StatisticID::EnvironmentsReused, reusedLayouts.bitCount());
    }
    build.destroyLater(WTF::move(summaries));
    build.destroyLater(WTF::move(plans));
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
