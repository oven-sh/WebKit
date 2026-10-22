/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResults.h"

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTFlowApply.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTSinkAllocationsInterpreter.h"
#include <optional>
#include <span>
#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<FunctionCapabilities> inheritedCapabilities {
    FunctionCapabilities::HasOnlyDirectCallers,
    FunctionCapabilities::IsNeverCalledByHost,
};

static void emitSwitchOnTag(IRBuilder& builder, ValueID tag, std::span<const BlockID> arms, NOESCAPE const Invocable<int64_t(size_t)> auto& tagOfArm)
{
    Vector<std::pair<int64_t, Target>> cases;
    cases.reserveInitialCapacity(arms.size() - 1);
    for (size_t index = 0; index + 1 < arms.size(); ++index)
        cases.constructAndAppend(tagOfArm(index), Target { arms[index], { }, { } });
    builder.emitSwitch(tag, WTF::move(cases), Target { arms.back(), { }, { } });
}

void ObjectInterpreter::returnParts(const Terminator& terminator)
{
    RELEASE_ASSERT(isRewriting() && m_sinking.resultShape());

    ValueID value = terminator.valueOperand();
    VirtualObjectID object = objectOf(value);
    if (!object) {
        if (std::optional<VirtualConstant> constant = virtualConstantOf(program(), function(), value))
            returnVariant(findResultVariant({ }, VirtualShape::ofConstant(*constant)), { }, nullptr, { });
        else
            returnVariant(findResultVariant({ }, VirtualShape { }), { }, nullptr, resolve(value));
        return;
    }

    const ObjectState state = *stateOf(object);
    const ObjectValues* carried = values().find(object);
    RELEASE_ASSERT(carried);
    ValueID cell = carried->cell;
    if (state.hasOneShape()) {
        const VirtualShape& shape = shapeOf(object, state.variants[0]);
        returnVariant(findResultVariant(object, shape), object, &shape, cell);
        return;
    }

    ValueID tag = carried->tag;
    RELEASE_ASSERT(tag);
    Vector<BlockID, 4> arms;
    for (size_t index = 0; index < state.variants.size(); ++index)
        arms.append(cursor().addUncoveredBlock());
    emitSwitchOnTag(cursor().builder(), tag, arms.span(), [&](size_t index) {
        std::optional<uint8_t> tagBit = state.variants[index].tagBit;
        RELEASE_ASSERT(tagBit);
        return int64_t(1) << *tagBit;
    });
    for (size_t index = 0; index < arms.size(); ++index) {
        const VirtualShape& shape = shapeOf(object, state.variants[index]);
        cursor().moveTo(arms[index]);
        returnVariant(findResultVariant(object, shape), object, &shape, cell);
    }
}

uint32_t ObjectInterpreter::findResultVariant(VirtualObjectID object, const VirtualShape& shape) const
{
    FreshResultVariant fields;
    if (shape.kind == VirtualShapeKind::Fields)
        fields = variantOfShape(m_sinking, object, shape);

    size_t index = m_sinking.resultShape()->variants.findIf([&](const FreshResultVariant& variant) {
        if (variant.kind != shape.kind)
            return false;
        switch (shape.kind) {
        case VirtualShapeKind::Cell:
            return true;
        case VirtualShapeKind::Constant:
            return variant.constant == shape.constant;
        case VirtualShapeKind::Fields:
            return variant.buildsSameValueAs(fields);
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    RELEASE_ASSERT(index != notFound);
    return static_cast<uint32_t>(index);
}

void ObjectInterpreter::returnVariant(uint32_t resultVariant, VirtualObjectID object, const VirtualShape* shape, ValueID cell)
{
    const FreshResultShape& resultShape = *m_sinking.resultShape();
    const FreshResultVariant& variant = resultShape.variants[resultVariant];
    IRBuilder& builder = cursor().builder();

    Vector<ValueID, 8> operands(resultShape.numberOfResults());
    if (resultShape.hasTag())
        operands[0] = builder.appendInteger(resultShape.tagOf(resultVariant));

    auto returnFields = [&](std::span<const FreshResultField> entries) {
        for (const FreshResultField& entry : entries) {
            std::optional<uint32_t> field = m_sinking.familyOf(object).findField(entry.field);
            RELEASE_ASSERT(field && shape && hasStored(*shape, *field));
            operands[resultShape.resultOfPart(entry.part)] = valueOfField(object, *field);
        }
    };
    returnFields(variant.properties.span());
    returnFields(variant.parts.span());

    if (variant.kind == VirtualShapeKind::Cell) {
        RELEASE_ASSERT(cell);
        operands[resultShape.resultOfPart(*resultShape.cellPart)] = cell;
    }

    for (uint32_t part = 0; part < resultShape.partTypes.size(); ++part) {
        ValueID& operand = operands[resultShape.resultOfPart(part)];
        if (operand)
            continue;
        Value unspecified(Opcode::Unspecified);
        unspecified.setValueType(resultShape.partTypes[part]);
        operand = cursor().emit(WTF::move(unspecified));
    }

    if (operands.isEmpty()) {
        builder.emitReturn(std::nullopt);
        return;
    }
    if (operands.size() == 1) {
        builder.emitReturn(operands[0]);
        return;
    }
    Value results(Opcode::Results);
    results.setValuesOperands(operands.span());
    builder.emitReturn(cursor().emit(WTF::move(results)));
}

static void declareResults(Function& worker, const FreshResultShape& shape)
{
    worker.results.clear();
    if (shape.hasTag())
        worker.results.append(DeclaredType { .type = TypeTable::int32 });
    for (TypeID type : shape.partTypes)
        worker.results.append(DeclaredType { .type = type });

    // A function has at least one result. The worker of an aggregate without parts, such as {}, returns undefined.
    if (worker.results.isEmpty())
        worker.results.append(DeclaredType { .type = TypeTable::undefined });
}

static ValueID returnedOperandAt(const Function& worker, ValueID returned, uint32_t numberOfResults, uint32_t result)
{
    if (numberOfResults == 1)
        return returned;
    const Value& results = worker.at(returned);
    RELEASE_ASSERT(results.opcode == Opcode::Results && results.numberOfVariadicOperands() == numberOfResults);
    return results.variadicOperands()[result];
}

static void refinePartTypes(Function& worker, FreshResultShape& shape, TypeArena& types)
{
    Vector<ValueID, 4> returnedValues;
    for (const Block& block : worker.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            returnedValues.append(block.terminator.valueOperand());
    }

    uint32_t numberOfResults = shape.numberOfResults();
    for (uint32_t part = 0; part < shape.partTypes.size(); ++part) {
        uint32_t result = shape.resultOfPart(part);
        TypeID refined;
        for (ValueID returned : returnedValues) {
            const Value& operand = worker.at(returnedOperandAt(worker, returned, numberOfResults, result));
            if (operand.opcode != Opcode::Unspecified)
                refined = refined ? types.unionOf(refined, operand.type) : operand.type;
        }
        // A parameter of the function may have the type never with code behind it. A result may not: its read ends the block in every caller, and nothing stands in for it in the other variants.
        if (!refined || refined == TypeTable::never)
            continue;

        for (ValueID returned : returnedValues) {
            Value& operand = worker.edit(returnedOperandAt(worker, returned, numberOfResults, result));
            if (operand.opcode != Opcode::Unspecified)
                continue;
            operand.setValueType(refined);
            operand.type = refined;
        }
        shape.partTypes[part] = refined;
        worker.results[result].type = refined;
    }
}

static std::optional<FreshResultShape> tryReturnParts(const AnalysisContext& context, Function& worker, TypeArena& types)
{
    constexpr unsigned maxEdgeSplittingRounds = 3;

    FunctionAnalyses analyses(context, worker);
    IRBuilder builder(context, worker, types, &analyses);
    for (unsigned round = 0; round < maxEdgeSplittingRounds; ++round) {
        AllocationSinking sinking(analyses, SinkingPolicy { ReturnedObjects::StayVirtual, true });
        if (!sinking.collectCandidates())
            return std::nullopt;
        sinking.analyze();

        FreshResultShape shape = shapeOfReturns(sinking);
        if (!shape.isFresh())
            return std::nullopt;
        if (sinking.splitEdgesNeedingCode(builder))
            continue;

        declareResults(worker, shape);
        sinking.rewrite(builder, &shape);
        sinking.countStatistics();
        refinePartTypes(worker, shape, types);
        return shape;
    }

    ASSERT_NOT_REACHED();
    return std::nullopt;
}

static FreshResultShape shapeOfOrdinaryResult(const Function& function)
{
    FreshResultShape shape;
    shape.state = FreshResultsState::NotFresh;
    shape.variants.append(FreshResultVariant { });
    shape.fieldOfPart.append(VirtualField { });
    const DeclaredType& result = function.result();
    shape.partTypes.append(result.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : result.type);
    shape.cellPart = 0;
    return shape;
}

ResultWorker createResultWorker(const AnalysisContext& context, const ResultWorkerRequest& request, TypeArena& types)
{
    Function worker = request.body;
    worker.role = FunctionRole::ResultWorker;
    worker.creator = CreatorRecord::ResultWorker { .origin = request.origin };
    worker.derivation = Derivation {
        .source = request.origin,
        .parameterMap = Vector<ParameterOrigin>(worker.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };
    worker.capabilities = initialCapabilitiesOf(worker);

    std::optional<FreshResultShape> shape = tryReturnParts(context, worker, types);
    if (!shape)
        shape = shapeOfOrdinaryResult(worker);
    return { WTF::move(worker), WTF::move(*shape) };
}

static Vector<ValueID, 8> appendResultReads(IRBuilder& builder, uint32_t numberOfResults, ValueID call)
{
    Vector<ValueID, 8> results;
    if (numberOfResults == 1) {
        results.append(call);
        return results;
    }
    for (uint32_t index = 0; index < numberOfResults; ++index) {
        Value read = IRBuilder::make(Opcode::ResultAt, { call });
        read.setIndex(index);
        results.append(builder.append(WTF::move(read)));
    }
    return results;
}

static Vector<FieldValue, 8> fieldValuesOf(const FreshResultShape& shape, std::span<const FreshResultField> entries, std::span<const ValueID> results)
{
    Vector<FieldValue, 8> values;
    for (const FreshResultField& entry : entries)
        values.append(FieldValue { entry.field, results[shape.resultOfPart(entry.part)] });
    return values;
}

// The casts that carried the proofs of the flow analysis are not rebuilt. The proofs are about declared positions, so they hold for the rebuilt values too.
static ValueID assumeDeclaredType(IRBuilder& builder, ValueID value, TypeID declared)
{
    const Program& program = builder.context().program;
    if (program.typeOf(builder.function(), value).isSubsetOf(program.at(declared), program))
        return value;
    if (builder.context().build.validates())
        builder.append(makeValidateType(value, declared));
    return builder.append(makeAssumeType(value, declared));
}

static void assumeDeclaredOperandTypes(IRBuilder& builder, Value& allocation)
{
    TypingContext typing { builder.context().program, builder.function(), builder.types() };
    for (uint32_t index = 0; index < allocation.numberOfOperands(); ++index) {
        ValueID operand = allocation.operand(index);
        if (!operand)
            continue;
        if (TypeID required = requiredTypeOfOperand(typing, allocation, index))
            allocation.setOperand(index, assumeDeclaredType(builder, operand, required));
    }
}

static ValueID appendRebuiltVariant(IRBuilder& builder, const FreshResultShape& shape, const FreshResultVariant& variant, std::span<const ValueID> results)
{
    switch (variant.kind) {
    case VirtualShapeKind::Cell:
        return results[shape.resultOfPart(*shape.cellPart)];
    case VirtualShapeKind::Constant:
        return builder.append(makeConstant(variant.constant));
    case VirtualShapeKind::Fields:
        break;
    }

    Vector<FieldValue, 8> properties = fieldValuesOf(shape, variant.properties.span(), results);
    Vector<FieldValue, 8> parts = fieldValuesOf(shape, variant.parts.span(), results);
    Vector<FieldValue, 2> storedByName;
    for (FieldValue& property : properties) {
        if (property.field.kind != VirtualFieldKind::UnprovenNamed)
            continue;
        storedByName.append(property);
        property.value = builder.append(makeFiller(builder.context().program, variant.description, property.field));
    }
    Value allocation = makeAllocation(builder.context().program, { variant.description, variant.prototype, properties.span(), parts.span(), variant.length });
    assumeDeclaredOperandTypes(builder, allocation);

    // The position is one in the function that the shape describes, and the builder's origin names the frame of that function.
    Origin origin = builder.origin();
    if (!variant.prototype.inlineFrame && variant.prototype.hasPosition())
        builder.setOrigin({ variant.prototype.position, origin.inlineFrame });
    ValueID rebuilt = builder.append(WTF::move(allocation));
    for (const FieldValue& stored : storedByName)
        builder.append(makeStoreByName(stored, rebuilt));
    builder.setOrigin(origin);
    return rebuilt;
}

ValueID appendRebuiltResult(IRBuilder& builder, const FreshResultShape& shape, ValueID call)
{
    Vector<ValueID, 8> results = appendResultReads(builder, shape.numberOfResults(), call);
    if (!shape.hasTag())
        return appendRebuiltVariant(builder, shape, shape.variants[0], results.span());

    Function& function = builder.function();
    BlockFrequency frequency = function.at(builder.currentBlock()).frequency;
    Vector<BlockID, 4> arms;
    for (const FreshResultVariant& variant : shape.variants)
        arms.append(builder.addBlock(rarerOf(frequency, variant.frequency)));
    BlockID continuation = builder.addBlock(frequency);
    emitSwitchOnTag(builder, results[0], arms.span(), [&](size_t index) {
        return shape.tagOf(static_cast<uint32_t>(index));
    });

    TypeID mergedType = TypeTable::never;
    for (size_t index = 0; index < arms.size(); ++index) {
        builder.moveToEndOf(arms[index]);
        ValueID rebuilt = appendRebuiltVariant(builder, shape, shape.variants[index], results.span());
        mergedType = builder.types().unionOf(mergedType, function.at(rebuilt).type);
        builder.emitJump(continuation, { rebuilt });
    }
    ValueID merged = builder.addParameter(continuation, mergedType);
    builder.moveToEndOf(continuation);
    return merged;
}

static NullishResult nullishResultOf(const FreshResultShape& shape)
{
    bool mayBeUndefined = false;
    bool mayBeNull = false;
    for (const FreshResultVariant& variant : shape.variants) {
        if (variant.kind != VirtualShapeKind::Constant)
            continue;
        mayBeUndefined |= variant.constant == VirtualConstant::Undefined;
        mayBeNull |= variant.constant == VirtualConstant::Null;
    }
    if (mayBeUndefined == mayBeNull)
        return NullishResult::Never;
    return mayBeUndefined ? NullishResult::Undefined : NullishResult::Null;
}

static LayoutID singleLayoutOf(const FreshResultShape& shape)
{
    LayoutID layout;
    unsigned numberOfVariantsWithLayout = 0;
    for (const FreshResultVariant& variant : shape.variants) {
        if (variant.kind != VirtualShapeKind::Fields || !variant.description.layout)
            continue;
        layout = variant.description.layout;
        ++numberOfVariantsWithLayout;
    }
    return numberOfVariantsWithLayout == 1 ? layout : LayoutID();
}

Function createResultWrapper(const AnalysisContext& context, FunctionID worker, const Function& original, const FreshResultShape& shape, TypeArena& types)
{
    RELEASE_ASSERT(canReturnParts(original));

    Function wrapper;
    static_cast<FunctionRecord&>(wrapper) = original;
    wrapper.inlineFrames.clear();
    wrapper.role = FunctionRole::ResultWrapper;
    wrapper.creator = CreatorRecord::ResultWrapper { .worker = worker, .nullish = nullishResultOf(shape), .layout = singleLayoutOf(shape) };
    wrapper.capabilities = (initialCapabilitiesOf(wrapper) - inheritedCapabilities) | (original.capabilities & inheritedCapabilities);

    IRBuilder builder(context, wrapper, types);
    builder.setOrigin({ original.position, 0 });
    BlockID entry = builder.addBlock();
    Vector<ValueID, 8> entryParameters;
    for (ValueID parameter : original.at(entryBlock).parameters)
        entryParameters.append(builder.addParameter(entry, original.at(parameter).type));
    RELEASE_ASSERT(entryParameters.size() == wrapper.numberOfEntryParameters());
    builder.moveToEndOf(entry);

    auto entryParameterAt = [&](std::optional<unsigned> index) {
        return index ? entryParameters[*index] : ValueID();
    };
    ValueID call = appendDirectCall(builder, {
        .callee = worker,
        .captures = entryParameters.span().first(wrapper.captures.size()),
        .thisValue = entryParameterAt(wrapper.thisParameterIndex()),
        .calleeValue = entryParameterAt(wrapper.calleeParameterIndex()),
        .newTarget = entryParameterAt(wrapper.newTargetParameterIndex()),
        .arguments = entryParameters.span().subspan(wrapper.firstParameterIndex(), wrapper.parameters.size()),
    });

    ValueID result = appendRebuiltResult(builder, shape, call);
    TypingContext typing { builder.context().program, builder.function(), builder.types() };
    builder.emitReturn(assumeDeclaredType(builder, result, typing.typeIDOf(wrapper.result())));
    return wrapper;
}

Function buildResultWorker(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    AnalysisContext context { inputs.build, inputs.program, &inputs.flow, nullptr, key.target };
    return createResultWorker(context, { key.target, inputs.program.at(key.target) }, inputs.types).function;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
