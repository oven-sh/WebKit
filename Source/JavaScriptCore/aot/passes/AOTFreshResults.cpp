/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResults.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTDeferClosuresInFields.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowApply.h"
#include "AOTFunctionEditing.h"
#include "AOTHeat.h"
#include "AOTInlineCopy.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTSinkAllocationsInterpreter.h"
#include "AOTSort.h"
#include "AOTSummarizeForInlining.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <atomic>
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxVirtualObjectVariants, Architecture::ARM64) <= maxTagBitsInInt32);
static_assert(defaultLimitValue(LimitID::MaxVirtualObjectVariants, Architecture::X86_64) <= maxTagBitsInInt32);

static bool canReturnParts(const Function& function)
{
    if (!function.hasBody() || !function.mayChangeSignature() || !function.mayBeCloned() || !function.hasOrdinaryFrame())
        return false;
    if (function.results.size() != 1 || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return false;
    if (isWrapperOrWorkerOf(function.role))
        return false;
    // A construction by value names the function itself, and is resolved only while that is no wrapper. The body that a resolved construction calls is split like any other.
    if (function.role == FunctionRole::Source && function.constructedClass)
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
        return true;
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool returnsWithoutValue(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
        return !terminator.valueOperand();
    case TerminatorKind::ReturnCall:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAllocation(const Function& function, ValueID value)
{
    return interpretedOf(function.at(value).opcode) == Interpretation::Allocation;
}

static bool allocates(const Function& function, const Block& block)
{
    return block.values.containsIf([&](ValueID value) {
        return isAllocation(function, value);
    });
}

static bool mayBeAllocation(FunctionAnalyses& analyses, ValueID value)
{
    const Function& function = analyses.function();
    if (isAllocation(function, analyses.sameObjectThroughCasts(value)))
        return true;
    if (!mayBeReference(analyses.program().typeOf(function, value)))
        return false;

    const EscapeAnalysis& escape = analyses.escape();
    PointsToSet pointsTo = escape.pointsTo(value);
    for (ObjectID object : pointsTo.objects) {
        if (escape.at(object).kind == ObjectKind::Allocation)
            return true;
    }
    return false;
}

FreshResultVariant variantOfShape(const AllocationSinking& sinking, VirtualObjectID object, const VirtualShape& shape)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields);
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualObjectDescription& description = family.descriptionOf(shape);

    FreshResultVariant variant {
        .kind = VirtualShapeKind::Fields,
        .description = description,
        .prototype = sinking.function().at(description.allocation),
        .length = shape.length,
    };

    if (description.hasKeyOrder) {
        for (uint32_t field : sinking.orders().fieldsOf(shape.order))
            variant.properties.append(FreshResultField { family.fieldAt(hasStored(shape, field) ? field : family.findCounterpart(field).value_or(field)), 0 });
    }
    for (size_t storedField : shape.stored) {
        const VirtualField& field = family.fieldAt(static_cast<uint32_t>(storedField));
        if (field.isProperty()) {
            if (!description.hasKeyOrder)
                variant.properties.append(FreshResultField { field, 0 });
        } else if (field.kind != VirtualFieldKind::Length)
            variant.parts.append(FreshResultField { field, 0 });
    }
    return variant;
}

static FreshResultShape shapeOfReturns(const AllocationSinking& sinking)
{
    const Function& function = sinking.function();
    std::span<const VirtualReturn> returns = sinking.virtualReturns();
    if (returns.empty())
        return { .state = FreshResultsState::NotFresh };
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        if (sinking.headOf(BlockID(index)) && returnsWithoutValue(function.blocks[index].terminator))
            return { .state = FreshResultsState::Unknown };
    }

    FreshResultShape shape;
    auto addVariant = [&](FreshResultVariant&& variant) {
        bool isPresent = shape.variants.containsIf([&](const FreshResultVariant& other) {
            return other.buildsSameValueAs(variant);
        });
        if (!isPresent)
            shape.variants.append(WTF::move(variant));
    };
    auto hasVariantOfKind = [&](VirtualShapeKind kind) {
        return shape.variants.containsIf([&](const FreshResultVariant& variant) {
            return variant.kind == kind;
        });
    };
    for (const VirtualReturn& returned : returns) {
        if (!returned.object) {
            if (std::optional<VirtualConstant> constant = virtualConstantOf(sinking.program(), function, returned.value))
                addVariant({ .kind = VirtualShapeKind::Constant, .constant = *constant });
            else
                addVariant({ .kind = VirtualShapeKind::Cell });
            continue;
        }

        const VirtualFamily& family = sinking.familyOf(returned.object);
        for (const ObjectVariant& objectVariant : returned.state.variants) {
            const VirtualShape& objectShape = family.at(objectVariant.shape);
            switch (objectShape.kind) {
            case VirtualShapeKind::Cell:
                addVariant({ .kind = VirtualShapeKind::Cell });
                break;
            case VirtualShapeKind::Constant:
                addVariant({ .kind = VirtualShapeKind::Constant, .constant = objectShape.constant });
                break;
            case VirtualShapeKind::Fields: {
                FreshResultVariant variant = variantOfShape(sinking, returned.object, objectShape);
                // Only a rare arm stores a value that nothing proves to fit its field, whatever merges with it on the way to the return.
                if (hasUnprovenField(family, objectShape))
                    variant.frequency = BlockFrequency::Rare;
                addVariant(WTF::move(variant));
                break;
            }
            }
        }
    }

    if (!hasVariantOfKind(VirtualShapeKind::Fields))
        return { .state = FreshResultsState::NotFresh };
    if (shape.variants.size() > sinking.limit(LimitID::MaxVirtualObjectVariants))
        return shape;

    auto addPart = [&](const VirtualField& field, TypeID type) {
        shape.fieldOfPart.append(field);
        shape.partTypes.append(type);
        return static_cast<uint32_t>(shape.partTypes.size() - 1);
    };
    if (hasVariantOfKind(VirtualShapeKind::Cell))
        shape.cellPart = addPart({ }, sinking.program().provenTypeOf(function.result()));
    for (FreshResultVariant& variant : shape.variants) {
        if (variant.kind != VirtualShapeKind::Fields)
            continue;
        auto assignPart = [&](FreshResultField& entry) {
            TypeID type = typeOfField(sinking.objectContext(), variant.description, entry.field);
            for (entry.part = 0; entry.part < shape.partTypes.size(); ++entry.part) {
                if (entry.part != shape.cellPart && shape.fieldOfPart[entry.part] == entry.field && shape.partTypes[entry.part] == type)
                    return;
            }
            addPart(entry.field, type);
        };
        std::ranges::for_each(variant.properties, assignPart);
        std::ranges::for_each(variant.parts, assignPart);
    }

    // Nothing can be stored in such a field, so the allocation is never reached, and no function has a result of the type never beside others.
    if (shape.partTypes.contains(TypeTable::never))
        return { .state = FreshResultsState::Unknown };
    shape.state = FreshResultsState::Fresh;
    return shape;
}

static FreshResultShape computeShapeOfReturns(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    bool hasAllocation = false;
    Vector<ValueID, 4> returnedValues;
    for (BlockID id : analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (returnsWithoutValue(block.terminator))
            return { .state = FreshResultsState::Unknown };
        if (block.terminator.kind == TerminatorKind::Return)
            returnedValues.append(block.terminator.valueOperand());
        hasAllocation = hasAllocation || allocates(function, block);
    }
    bool mayReturnAllocation = hasAllocation && std::ranges::any_of(returnedValues, [&](ValueID value) {
        return mayBeAllocation(analyses, value);
    });
    if (!mayReturnAllocation)
        return { .state = FreshResultsState::NotFresh };

    AllocationSinking sinking(analyses, { ReturnedObjects::StayVirtual, false });
    if (!sinking.collectCandidates())
        return { .state = FreshResultsState::NotFresh };
    sinking.analyze();
    return shapeOfReturns(sinking);
}

FreshResultShape computeFreshResults(FunctionAnalyses& analyses)
{
    if (!canReturnParts(analyses.function()))
        return { .state = FreshResultsState::NotFresh };
    return computeShapeOfReturns(analyses);
}

bool returnsFreshAggregate(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    return function.hasBody() && function.results.size() == 1 && computeShapeOfReturns(analyses).isFresh();
}

bool FreshResultVariant::buildsSameValueAs(const FreshResultVariant& other) const
{
    auto haveSameFields = [](const auto& a, const auto& b) {
        return std::ranges::equal(a, b, { }, &FreshResultField::field, &FreshResultField::field);
    };
    // The place of an allocation is what reports and counts go by: two places that say the same otherwise build the same value.
    auto haveSamePayloadButForPlace = [](const Value& a, const Value& b) {
        if (!Value::opcodesWithAllocationSite.contains(a.opcode) || a.allocationSite() == b.allocationSite())
            return a.hasSamePayloadAs(b);
        Value atSamePlace = b;
        atSamePlace.setAllocationSite(a.allocationSite());
        return atSamePlace.hasSamePayloadAs(a);
    };
    return kind == other.kind
        && constant == other.constant
        && description.kind == other.description.kind
        && description.elementType == other.description.elementType
        && prototype.opcode == other.prototype.opcode
        && haveSamePayloadButForPlace(prototype, other.prototype)
        && length == other.length
        && haveSameFields(properties, other.properties)
        && haveSameFields(parts, other.parts);
}

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
    auto returnVariantAt = [&](size_t index) {
        const VirtualShape& shape = shapeOf(object, state.variants[index]);
        returnVariant(findResultVariant(object, shape), object, &shape, cell);
    };
    if (state.hasOneShape()) {
        returnVariantAt(0);
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
        cursor().moveTo(arms[index]);
        returnVariantAt(index);
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
            const VirtualFamily& family = m_sinking.familyOf(object);
            std::optional<uint32_t> field = family.findField(entry.field);
            RELEASE_ASSERT(field && shape && hasStored(*shape, *field));
            // Where lists meet, an element is carried with a type that takes in the elements of all of them. The list that is made of the parts is born with the storage that fits their types.
            bool isElement = entry.field.kind == VirtualFieldKind::Element;
            operands[resultShape.resultOfPart(entry.part)] = isElement ? valueOfField(object, *field, typeOfField(m_sinking.objectContext(), family.descriptionOf(*shape), entry.field)) : valueOfField(object, *field);
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

    if (operands.size() <= 1) {
        builder.emitReturn(operands.isEmpty() ? std::nullopt : std::optional(operands[0]));
        return;
    }
    Value results(Opcode::Results);
    results.setValuesOperands(operands.span());
    builder.emitReturn(cursor().emit(WTF::move(results)));
}

static void refinePartTypes(Function& worker, FreshResultShape& shape, TypeArena& types)
{
    Vector<ValueID, 4> returnedValues;
    for (const Block& block : worker.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            returnedValues.append(block.terminator.valueOperand());
    }

    uint32_t numberOfResults = shape.numberOfResults();
    auto returnedOperandAt = [&](ValueID returned, uint32_t result) {
        if (numberOfResults == 1)
            return returned;
        const Value& results = worker.at(returned);
        RELEASE_ASSERT(results.opcode == Opcode::Results && results.numberOfVariadicOperands() == numberOfResults);
        return results.variadicOperands()[result];
    };
    for (uint32_t part = 0; part < shape.partTypes.size(); ++part) {
        uint32_t result = shape.resultOfPart(part);
        TypeID refined;
        for (ValueID returned : returnedValues) {
            const Value& operand = worker.at(returnedOperandAt(returned, result));
            if (operand.opcode != Opcode::Unspecified)
                refined = refined ? types.unionOf(refined, operand.type) : operand.type;
        }
        // A parameter of the function may have the type never with code behind it. A result may not: its read ends the block in every caller, and nothing stands in for it in the other variants.
        if (!refined || refined == TypeTable::never)
            continue;

        for (ValueID returned : returnedValues) {
            Value& operand = worker.edit(returnedOperandAt(returned, result));
            if (operand.opcode != Opcode::Unspecified)
                continue;
            operand.setValueType(refined);
            operand.type = refined;
        }
        shape.partTypes[part] = refined;
        worker.results[result].changeType(Proven::byConstruction(refined));
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

        worker.results.clear();
        if (shape.hasTag())
            worker.results.append(DeclaredType(Proven::byPosition(TypeTable::int32)));
        for (TypeID type : shape.partTypes)
            worker.results.append(DeclaredType(Proven::byPosition(type)));
        // A function has at least one result. The worker of an aggregate without parts, such as {}, returns undefined.
        if (worker.results.isEmpty())
            worker.results.append(DeclaredType(Proven::byPosition(TypeTable::undefined)));

        sinking.rewrite(builder, &shape);
        sinking.countStatistics();
        refinePartTypes(worker, shape, types);
        return shape;
    }

    ASSERT_NOT_REACHED();
    return std::nullopt;
}

static std::pair<Function, FreshResultShape> createResultWorker(const AnalysisContext& context, FunctionID origin, const Function& body, TypeArena& types)
{
    Function worker = body;
    worker.role = FunctionRole::ResultWorker;
    worker.creator = CreatorRecord::ResultWorker { .origin = origin };
    worker.derivation = Derivation {
        .source = origin,
        .parameterMap = Vector<ParameterOrigin>(worker.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };
    worker.capabilities = initialCapabilitiesOf(worker);

    std::optional<FreshResultShape> shape = tryReturnParts(context, worker, types);
    if (!shape)
        shape = FreshResultShape { FreshResultsState::NotFresh, { FreshResultVariant { } }, { VirtualField { } }, { context.program.provenTypeOf(worker.result()) }, 0 };
    return { WTF::move(worker), WTF::move(*shape) };
}

Function buildResultWorker(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    AnalysisContext context { inputs.build, inputs.program, &inputs.flow, nullptr, key.target };
    return createResultWorker(context, key.target, inputs.program.at(key.target), inputs.types).first;
}

// The casts that carried the proofs of the flow analysis are not rebuilt. The proofs are about declared positions, so they hold for the rebuilt values too.
static ValueID assumeDeclaredType(IRBuilder& builder, ValueID value, TypeID declared)
{
    const Program& program = builder.context().program;
    if (program.typeOf(builder.function(), value).isSubsetOf(program.at(declared), program))
        return value;
    if (builder.context().build.validates())
        builder.append(makeValidateType(value, declared));
    return builder.append(makeAssumeType(value, Proven::byPosition(declared)));
}

static ValueID appendRebuiltVariant(IRBuilder& builder, const FreshResultShape& shape, const FreshResultVariant& variant, std::span<const ValueID> results)
{
    const Program& program = builder.context().program;
    switch (variant.kind) {
    case VirtualShapeKind::Cell:
        return results[shape.resultOfPart(*shape.cellPart)];
    case VirtualShapeKind::Constant:
        return builder.append(makeConstant(variant.constant));
    case VirtualShapeKind::Fields:
        break;
    }

    auto fieldValuesOf = [&](const auto& entries) {
        return entries.template map<Vector<FieldValue, 8>>([&](const FreshResultField& entry) {
            return FieldValue { entry.field, results[shape.resultOfPart(entry.part)] };
        });
    };
    Vector<FieldValue, 8> properties = fieldValuesOf(variant.properties);
    Vector<FieldValue, 8> parts = fieldValuesOf(variant.parts);
    Vector<FieldValue, 2> storedByName;
    for (FieldValue& property : properties) {
        if (property.field.kind != VirtualFieldKind::UnprovenNamed)
            continue;
        storedByName.append(property);
        property.value = builder.append(makeFiller(program, variant.description, property.field));
    }
    Value allocation = makeAllocation(program, { variant.description, variant.prototype, properties.span(), parts.span(), variant.length });
    TypingContext typing { program, builder.function(), builder.types() };
    for (uint32_t index = 0; index < allocation.numberOfOperands(); ++index) {
        ValueID operand = allocation.operand(index);
        if (!operand)
            continue;
        if (TypeID required = requiredTypeOfOperand(typing, allocation, index))
            allocation.setOperand(index, assumeDeclaredType(builder, operand, required));
    }

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

static ValueID appendRebuiltResult(IRBuilder& builder, const FreshResultShape& shape, ValueID call)
{
    Vector<ValueID, 8> results;
    if (shape.numberOfResults() == 1)
        results.append(call);
    else {
        for (uint32_t index = 0; index < shape.numberOfResults(); ++index) {
            Value read = IRBuilder::make(Opcode::ResultAt, { call });
            read.setIndex(index);
            results.append(builder.append(WTF::move(read)));
        }
    }
    if (!shape.hasTag())
        return appendRebuiltVariant(builder, shape, shape.variants[0], results.span());

    Function& function = builder.function();
    Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
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

static Function createResultWrapper(const AnalysisContext& context, FunctionID worker, const Function& original, const FreshResultShape& shape, TypeArena& types)
{
    RELEASE_ASSERT(canReturnParts(original));

    bool mayBeUndefined = false;
    bool mayBeNull = false;
    LayoutID layout;
    unsigned numberOfVariantsWithLayout = 0;
    for (const FreshResultVariant& variant : shape.variants) {
        if (variant.kind == VirtualShapeKind::Constant) {
            mayBeUndefined |= variant.constant == VirtualConstant::Undefined;
            mayBeNull |= variant.constant == VirtualConstant::Null;
        } else if (variant.kind == VirtualShapeKind::Fields && variant.description.layout) {
            layout = variant.description.layout;
            ++numberOfVariantsWithLayout;
        }
    }
    NullishResult nullish = mayBeUndefined == mayBeNull ? NullishResult::Never : mayBeUndefined ? NullishResult::Undefined : NullishResult::Null;

    Function wrapper;
    static_cast<FunctionRecord&>(wrapper) = original;
    wrapper.inlineFrames.clear();
    wrapper.role = FunctionRole::ResultWrapper;
    wrapper.creator = CreatorRecord::ResultWrapper { .worker = worker, .nullish = nullish, .layout = numberOfVariantsWithLayout == 1 ? layout : LayoutID() };
    wrapper.capabilities = capabilitiesInPlaceOf(wrapper, original.capabilities, { FunctionCapabilities::RecordsCallee, FunctionCapabilities::RecordsCalleeOnceRenamed });

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

static constexpr OpcodeSet storesIntoNamedFields { Opcode::PropSet, Opcode::PropDefine, Opcode::PutSlot, Opcode::PutSlotOrAdd };

// DeferClosuresInFields leaves the closure of a field unmade until the field is read, but only where the allocation is given the NewClosure itself.
static bool givesFieldsClosuresMadeOnDemand(FunctionAnalyses& analyses, const FreshResultShape& shape)
{
    const AnalysisContext& context = analyses.context();
    if (!context.build.isEnabled(PassID::DeferClosuresInFields, context.id))
        return false;
    const Function& function = analyses.function();

    // A variant names one of the allocations that build it.
    Vector<LayoutID, 2> layouts;
    for (const FreshResultVariant& variant : shape.variants) {
        if (variant.kind != VirtualShapeKind::Fields || !variant.description.layout)
            continue;
        LayoutKind kind = context.program.at(variant.description.layout).kind;
        if (kind == LayoutKind::Record || kind == LayoutKind::Instance)
            layouts.appendIfNotContains(variant.description.layout);
    }
    if (layouts.isEmpty())
        return false;
    BitVector objects;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::NewObject && layouts.contains(function.at(id).layout()))
                objects.set(id.index());
        }
    }
    if (objects.isEmpty())
        return false;

    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::NewObject) {
                bool listsClosure = objects.get(id.index()) && std::ranges::any_of(value.valuesOperands(), [&](ValueID listed) {
                    return listed && canLeaveClosureUnmade(analyses, block, id, listed);
                });
                if (listsClosure)
                    return true;
                continue;
            }
            if (!storesIntoNamedFields.contains(value.opcode))
                continue;
            ValueID receiver = sameObjectThroughCasts(function, value.operand(*receiverOf(value.opcode)));
            if (objects.get(receiver.index()) && canLeaveClosureUnmade(analyses, block, receiver, value.operand(*storedOf(value.opcode))))
                return true;
        }
    }
    return false;
}

static bool returnsFreshResult(const AnalysisContext& context, const Function& function)
{
    FunctionAnalyses analyses(context, function);
    FreshResultShape shape = computeFreshResults(analyses);
    if (shape.state == FreshResultsState::Unknown && shape.variants.size() > context.build.limit(LimitID::MaxVirtualObjectVariants))
        context.build.gaveUp(LimitID::MaxVirtualObjectVariants, analyses.subject());
    if (!shape.isFresh())
        return false;
    if (givesFieldsClosuresMadeOnDemand(analyses, shape)) {
        context.build.statistics().count(StatisticID::FreshResultsKeptWholeForClosures);
        return false;
    }
    return true;
}

// A loop lasts as long as its longest task from the moment that one begins.
static Vector<uint32_t> findLargestFirst(unsigned count, const Invocable<const Function&(unsigned)> auto& functionAt)
{
    AOT_THRESHOLD(size_t, fewestValues, 4096);
    Vector<uint64_t> keys;
    for (unsigned position = 0; position < count; ++position) {
        size_t numberOfValues = functionAt(position).values.size();
        if (numberOfValues >= fewestValues)
            keys.append((UINT32_MAX - static_cast<uint64_t>(numberOfValues)) << 32 | position);
    }
    AOT::sort(keys);
    return keys.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
    });
}

static void remapShape(const BodyRemaps& remaps, FreshResultShape& shape)
{
    for (TypeID& type : shape.partTypes)
        remaps.types.apply(type);

    Function prototypes;
    for (FreshResultVariant& variant : shape.variants) {
        remaps.types.apply(variant.description.type);
        remaps.types.apply(variant.description.elementType);
        prototypes.add(WTF::move(variant.prototype));
    }
    applyRemaps(remaps, prototypes);
    for (size_t index = 0; index < shape.variants.size(); ++index)
        shape.variants[index].prototype = WTF::move(prototypes.edit(ValueID(index)));
}

// Nothing is gained in rare code by itself. But where its result meets another one at a block parameter, the object that comes the frequent way is built for as long as this one is a cell.
static bool meetsOtherValues(FunctionAnalyses& analyses, ValueID result)
{
    const Function& function = analyses.function();
    Vector<ValueID, 4> worklist { result };
    while (!worklist.isEmpty()) {
        for (const Use& use : analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind == Use::Kind::EdgeArgument)
                return true;
            if (use.kind == Use::Kind::Operand && interpretedOf(function.at(use.user).opcode) == Interpretation::Cast)
                worklist.append(use.user);
        }
    }
    return false;
}

static ValueID returnedValueOf(const Function& function, const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
        if (ValueID value = terminator.valueOperand())
            return sameObjectThroughCasts(function, value);
        return { };
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isRemovedOrSunk(SunkAllocationOutcome outcome)
{
    switch (outcome) {
    case SunkAllocationOutcome::Removed:
    case SunkAllocationOutcome::Sunk:
        return true;
    case SunkAllocationOutcome::Unknown:
    case SunkAllocationOutcome::NotACandidate:
    case SunkAllocationOutcome::Abandoned:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

enum class TailCalls : uint8_t { Required, Allowed, None };

struct SplitCandidate {
    FunctionID id;
    std::optional<Function> expandedBody;
    Function worker { };
    FreshResultShape shape { };
};

struct WrapperCallSite {
    ValueID call;
    FunctionID wrapper;

    friend bool operator==(const WrapperCallSite&, const WrapperCallSite&) = default;
};

struct ExpandedWrapperCall {
    unsigned siteIndex { 0 };
    size_t firstValue { 0 };
    size_t endValue { 0 };
    unsigned numberOfAllocations { 0 };
    unsigned numberOfUnsunkAllocations { 0 };
};

struct FreshResultSplitter {
    ProgramEditor& editor;
    const OptimizationInputs& inputs;
    const PassRunner& runner;
    const Program& program { editor.program() };
    BuildContext& build { runner.build() };
    const ParallelLoops& loops { build.parallelLoops() };
    BitVector wrappers { };
    BitVector wrappersOfLastRound { };

    AnalysisContext contextFor(FunctionID id) const { return { build, program, &inputs.flow, &inputs.effects, id }; }
    bool isWrapper(FunctionID id) const { return wrappers.get(id.index()); }

    bool shouldConsider(FunctionID id) const
    {
        const CallGraph& callGraph = inputs.callGraph;
        // Only a caller that names the function takes the wrapper in and does without the allocation. A function that is reached by value alone would pay for the wrapper, its call and the parts on every call.
        return !isWrapper(id)
            && (id.index() >= callGraph.numberOfFunctions() || callGraph.numberOfCallSitesInCode(id) || callGraph.isExpectedByCode(id))
            && canReturnParts(program.at(id))
            && build.isEnabled(PassID::SplitFreshResults, id)
            && !isCold(inputs.heat.of(program, id)).predicted();
    }

    static TailCalls classifyTailCalls(const Function& function)
    {
        TailCalls tailCalls = TailCalls::None;
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                continue;
            switch (block.terminator.requirement()) {
            case TailCallRequirement::Required:
                return TailCalls::Required;
            case TailCallRequirement::Allowed:
                tailCalls = TailCalls::Allowed;
                break;
            }
        }
        return tailCalls;
    }

    void appendIfFresh(FunctionID id, Function&& body, TypeArena& types, Vector<SplitCandidate>& found) const
    {
        if (classifyTailCalls(body) == TailCalls::Required)
            return;
        AnalysisContext context = contextFor(id);
        unsigned numberOfDemotedCalls = 0;
        {
            IRBuilder builder(context, body, types);
            for (uint32_t index = 0; index < body.blocks.size(); ++index) {
                if (body.blocks[index].terminator.kind != TerminatorKind::ReturnCall)
                    continue;
                builder.demoteReturnCall(BlockID(index));
                ++numberOfDemotedCalls;
            }
        }
        if (!returnsFreshResult(context, body))
            return;
        build.statistics().count(StatisticID::ReturnCallsDemoted, numberOfDemotedCalls);
        found.append(SplitCandidate { id, WTF::move(body) });
    }

    void remapExpandedBodies(const BodyRemaps& remaps, Vector<SplitCandidate>& candidates) const
    {
        if (remaps.isIdentity())
            return;
        forEachIndexInParallel(loops, candidates.size(), [&](unsigned index) {
            if (std::optional<Function>& body = candidates[index].expandedBody)
                applyRemaps(remaps, *body);
        });
    }

    Vector<SplitCandidate> findCandidates()
    {
        unsigned count = program.functions.size();
        auto phases = makeUnique<InterningPhases>(editor, count);
        Vector<SplitCandidate> candidates;
        {
            ProgramCaches caches(program, build);
            candidates = collectInParallel<SplitCandidate>(loops, count, largeFunctions(program, loops).span(), [&](unsigned index, Vector<SplitCandidate>& found) {
                FunctionID id(index);
                if (!shouldConsider(id))
                    return;
                const Function& function = program.at(id);
                switch (classifyTailCalls(function)) {
                case TailCalls::Required:
                    return;
                case TailCalls::Allowed: {
                    bool returnsValue = std::ranges::any_of(function.blocks, [](const Block& block) {
                        return block.terminator.kind == TerminatorKind::Return && block.terminator.valueOperand();
                    });
                    bool returnsValueAndAllocates = returnsValue && std::ranges::any_of(function.blocks, [&](const Block& block) {
                        return allocates(function, block);
                    });
                    if (!returnsValueAndAllocates)
                        return;
                    TaskArenas arenas(*phases, index);
                    appendIfFresh(id, Function(function), arenas.types, found);
                    return;
                }
                case TailCalls::None:
                    if (returnsFreshResult(contextFor(id), function))
                        found.append(SplitCandidate { id, std::nullopt });
                    return;
                }
            });
        }
        remapExpandedBodies(phases->finish(), candidates);
        build.destroyLater(WTF::move(phases));
        return candidates;
    }

    // The last call first: a copy moves only the values behind its call.
    Vector<ExpandedWrapperCall, 4> expandCalls(FunctionPassContext& context, std::span<const WrapperCallSite> sites) const
    {
        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        const UseLists& useLists = context.analyses.useLists();
        Vector<std::pair<ProgramPoint, unsigned>, 4> calls;
        for (unsigned siteIndex = 0; siteIndex < sites.size(); ++siteIndex) {
            ValueLocation location = useLists.locationOf(sites[siteIndex].call);
            if (location.placement == ValuePlacement::InBlock)
                calls.append(std::pair { ProgramPoint { location.block, location.index }, siteIndex });
        }
        AOT::sort(calls, [](const auto& a, const auto& b) {
            if (a.first.block != b.first.block)
                return a.first.block > b.first.block;
            return a.first.index > b.first.index;
        });

        Vector<ExpandedWrapperCall, 4> expansions;
        for (auto [point, siteIndex] : calls) {
            auto [call, wrapper] = sites[siteIndex];
            size_t firstValue = context.function.values.size();
            // A cold function's summary has everything that inlining a whole body reads. It skips the head, the conditions and the called parameters.
            InliningSummary summary = summarizeFunctionForInlining(contextFor(wrapper), program.at(wrapper), HeatClass::Cold);
            if (!summary.canInlineWholeBody() || canInline(context, summary, call) != InlineDecision::Unknown)
                continue;
            copyIn(context, builder, InlineRequest { point, call, InlinedPart::WholeBody, summary });
            expansions.append(ExpandedWrapperCall { siteIndex, firstValue, context.function.values.size() });
        }
        return expansions;
    }

    Vector<WrapperCallSite, 2> findForwardedCalls(FunctionID id) const
    {
        if (!shouldConsider(id) || inputs.callGraph.recursion(id) != Recursion::NotRecursive)
            return { };

        const Function& function = program.at(id);
        Vector<WrapperCallSite, 2> calls;
        BitVector collectedCalls;
        std::optional<CFG> cfg;
        bool forwardsLastRound = false;
        for (uint32_t index = 0; index < function.blocks.size(); ++index) {
            ValueID returned = returnedValueOf(function, function.blocks[index].terminator);
            if (!returned)
                continue;
            const Value& call = function.at(returned);
            if (call.opcode != Opcode::CallDirect || !isWrapper(call.function()))
                continue;
            if (!cfg)
                cfg.emplace(function);
            if (!cfg->isReachable(BlockID(index)) || collectedCalls.set(returned.index()))
                continue;
            calls.append(WrapperCallSite { returned, call.function() });
            forwardsLastRound |= wrappersOfLastRound.get(call.function().index());
        }
        if (!forwardsLastRound)
            return { };
        return calls;
    }

    // A function that has not been split has the body that the call graph was built from, so whoever forwards the result of a wrapper is among its callers.
    Vector<SplitCandidate> findForwarders()
    {
        Vector<FunctionID> callers;
        for (size_t callee : wrappersOfLastRound)
            callers.append(inputs.callGraph.callersOf(FunctionID(callee)));
        AOT::sort(callers);
        removeRepeatedElements(callers);
        if (callers.isEmpty())
            return { };

        unsigned count = callers.size();
        Vector<uint32_t> largestFirst = findLargestFirst(count, [&](unsigned position) -> const Function& {
            return program.at(callers[position]);
        });
        InterningPhases phases(editor, count);
        Vector<SplitCandidate> forwarders;
        {
            ProgramCaches caches(program, build);
            forwarders = collectInParallel<SplitCandidate>(loops, count, largestFirst.span(), [&](unsigned position, Vector<SplitCandidate>& found) {
                FunctionID id = callers[position];
                auto calls = findForwardedCalls(id);
                if (calls.isEmpty())
                    return;

                TaskArenas arenas(phases, position);
                Function copy = program.at(id);
                {
                    FunctionAnalyses analyses(contextFor(id), copy);
                    FunctionPassContext context { program, inputs, id, copy, analyses, arenas.types, arenas.strings, build };
                    if (expandCalls(context, calls.span()).isEmpty())
                        return;
                }
                appendIfFresh(id, WTF::move(copy), arenas.types, found);
            });
        }
        remapExpandedBodies(phases.finish(), forwarders);
        return forwarders;
    }

    unsigned split(Vector<SplitCandidate>&& candidates)
    {
        wrappersOfLastRound.clearAll();
        if (candidates.isEmpty())
            return 0;

        auto bodyOf = [&](unsigned index) -> const Function& {
            const SplitCandidate& candidate = candidates[index];
            return candidate.expandedBody ? *candidate.expandedBody : program.at(candidate.id);
        };
        Vector<uint32_t> largestFirst = findLargestFirst(candidates.size(), bodyOf);

        InterningPhases workerPhases(editor, candidates.size());
        {
            ProgramCaches caches(program, build);
            forEachIndexInParallel(loops, candidates.size(), largestFirst.span(), [&](unsigned index) {
                TaskArenas arenas(workerPhases, index);
                SplitCandidate& candidate = candidates[index];
                std::tie(candidate.worker, candidate.shape) = createResultWorker(contextFor(candidate.id), candidate.id, bodyOf(index), arenas.types);
                if (candidate.shape.isFresh())
                    simplifyEditedFunction(runner, program, inputs, candidate.id, candidate.worker, arenas);
            });
        }
        BodyRemaps workerRemaps = workerPhases.finish();

        // A worker is analyzed again after its edges are split, and the larger body can reach a limit of the analysis.
        candidates.removeAllMatching([](const SplitCandidate& candidate) {
            return !candidate.shape.isFresh();
        });
        if (candidates.isEmpty())
            return 0;

        unsigned count = candidates.size();
        FunctionID firstWorker = editor.reserveFunctions(count);
        auto workerAt = [&](size_t index) {
            return FunctionID(firstWorker.index() + index);
        };
        forEachIndexInParallel(loops, count, [&](unsigned index) {
            editor.replaceFunction(workerAt(index), WTF::move(candidates[index].worker));
            if (workerRemaps.isIdentity())
                return;
            applyRemaps(workerRemaps, editor.at(workerAt(index)));
            remapShape(workerRemaps, candidates[index].shape);
        });

        Vector<Function> createdWrappers(count);
        InterningPhases wrapperPhases(editor, count);
        {
            ProgramCaches caches(program, build);
            forEachIndexInParallel(loops, count, [&](unsigned index) {
                TaskArenas arenas(wrapperPhases, index);
                FunctionID id = candidates[index].id;
                createdWrappers[index] = createResultWrapper(contextFor(id), workerAt(index), program.at(id), candidates[index].shape, arenas.types);
            });
        }
        BodyRemaps wrapperRemaps = wrapperPhases.finish();
        forEachIndexInParallel(loops, count, [&](unsigned index) {
            applyRemaps(wrapperRemaps, createdWrappers[index]);
            editor.replaceFunction(candidates[index].id, WTF::move(createdWrappers[index]));
        });

        // An inline frame holds a copy of the body, which the worker has. Stack traces hide the wrapper.
        Vector<FunctionID> workerOf(program.functions.size());
        for (size_t index = 0; index < count; ++index)
            workerOf[candidates[index].id.index()] = workerAt(index);
        auto isSplit = [&](const InlineFrame& frame) {
            return frame.callee && workerOf[frame.callee.index()];
        };
        forEachIndexInParallel(loops, workerOf.size(), [&](unsigned index) {
            if (std::ranges::none_of(program.functions[index].inlineFrames, isSplit))
                return;
            for (InlineFrame& frame : editor.at(FunctionID(index)).inlineFrames) {
                if (isSplit(frame))
                    frame.callee = workerOf[frame.callee.index()];
            }
        });

        for (const SplitCandidate& candidate : candidates) {
            wrappers.set(candidate.id.index());
            wrappersOfLastRound.set(candidate.id.index());
        }
        build.destroyLater(WTF::move(candidates));
        return count;
    }

    Vector<WrapperCallSite, 4> findWrapperCalls(FunctionID id, const Function& caller) const
    {
        Vector<WrapperCallSite, 4> sites;
        forEachDirectCall(program, id, [&](BlockID block, ValueID call, FunctionID callee) {
            // In frequent code a call is marked because it would have closed a cycle when its callee was the whole function. The wrapper only calls the worker.
            if (isWrapper(callee) && (!caller.at(call).isNeverInlined() || caller.at(block).frequency.predicted() == BlockFrequency::Normal))
                sites.append(WrapperCallSite { call, callee });
        });
        if (sites.isEmpty())
            return sites;

        FunctionAnalyses analyses(contextFor(id), caller);
        sites.removeAllMatching([&](const WrapperCallSite& site) {
            ValueLocation location = analyses.useLists().locationOf(site.call);
            if (location.placement != ValuePlacement::InBlock)
                return true;
            return isCold(heatClassOf(inputs.heat, analyses, location.block)).predicted() && !meetsOtherValues(analyses, site.call);
        });
        return sites;
    }

    Vector<WrapperCallSite, 4> chooseProfitableCalls(FunctionID caller, const Function& function, std::span<const WrapperCallSite> sites, TaskArenas& arenas) const
    {
        Function copy = function;
        Vector<ExpandedWrapperCall, 4> expansions;
        {
            FunctionAnalyses analyses(contextFor(caller), copy);
            FunctionPassContext context { program, inputs, caller, copy, analyses, arenas.types, arenas.strings, build };
            expansions = expandCalls(context, sites);
        }
        if (expansions.isEmpty())
            return { };

        FunctionAnalyses analyses(contextFor(caller), copy);
        for (const SunkAllocation& allocation : predictSinking(analyses)) {
            size_t index = allocation.allocation.index();
            auto next = std::ranges::upper_bound(expansions, index, { }, &ExpandedWrapperCall::firstValue);
            if (next == expansions.begin() || index >= std::prev(next)->endValue)
                continue;
            ++std::prev(next)->numberOfAllocations;
            if (!isRemovedOrSunk(allocation.outcome))
                ++std::prev(next)->numberOfUnsunkAllocations;
        }

        BitVector isProfitable(sites.size());
        for (const ExpandedWrapperCall& expansion : expansions) {
            if (expansion.numberOfAllocations && !expansion.numberOfUnsunkAllocations)
                isProfitable.set(expansion.siteIndex);
        }

        Vector<WrapperCallSite, 4> chosen;
        for (size_t siteIndex : isProfitable)
            chosen.append(sites[siteIndex]);
        return chosen;
    }

    unsigned expandProfitableCalls(FunctionID caller, Function& function, std::span<const WrapperCallSite> sites, TaskArenas& arenas) const
    {
        if (build.options().verifiesAfterEveryPass)
            RELEASE_ASSERT(std::ranges::equal(sites, findWrapperCalls(caller, function)));
        Vector<WrapperCallSite, 4> chosen = chooseProfitableCalls(caller, function, sites, arenas);
        if (chosen.isEmpty())
            return 0;

        {
            FunctionAnalyses analyses(contextFor(caller), function);
            FunctionPassContext context { program, inputs, caller, function, analyses, arenas.types, arenas.strings, build };
            expandCalls(context, chosen.span());
            runner.runFunctionPass(PassID::SinkAllocations, context);
        }
        simplifyEditedFunction(runner, program, inputs, caller, function, arenas);
        return chosen.size();
    }

    void expandCallSites()
    {
        if (wrappers.isEmpty())
            return;

        using CallerOfWrappers = std::pair<FunctionID, Vector<WrapperCallSite, 4>>;
        Vector<CallerOfWrappers> callers = collectInParallel<CallerOfWrappers>(loops, program.functions.size(), largeFunctions(program, loops).span(), [&](unsigned index, Vector<CallerOfWrappers>& found) {
            FunctionID id(index);
            if (!hasBody(program, id) || isWrapper(id) || !build.isEnabled(PassID::SplitFreshResults, id))
                return;
            Vector<WrapperCallSite, 4> sites = findWrapperCalls(id, program.at(id));
            if (!sites.isEmpty())
                found.append(CallerOfWrappers { id, WTF::move(sites) });
        });
        Vector<FunctionID> ids = callers.map([](const CallerOfWrappers& caller) {
            return caller.first;
        });

        // An edit changes its own caller alone, and what is a site in a caller hangs on nothing in another one.
        std::atomic<uint64_t> numberOfExpandedCalls { 0 };
        editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
            numberOfExpandedCalls.fetch_add(expandProfitableCalls(id, function, callers[index].second.span(), arenas), std::memory_order_relaxed);
        });
        build.statistics().count(StatisticID::ResultsReturnedInRegisters, numberOfExpandedCalls.load(std::memory_order_relaxed));
    }
};

} // anonymous namespace

void splitFreshResults(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    FreshResultSplitter splitter { editor, inputs, runner };
    BuildContext& build = splitter.build;
    PhaseTimes times(build, PassID::SplitFreshResults);
    const Vector<Function>& functions = splitter.program.functions;
    Vector<uint32_t> existingWrappers = findIndicesInParallel(splitter.loops, functions.size(), [&](unsigned index) {
        return functions[index].role == FunctionRole::ResultWrapper;
    });
    for (uint32_t index : existingWrappers)
        splitter.wrappers.set(index);
    Vector<SplitCandidate> candidates = splitter.findCandidates();
    times.didFinish("findCandidates"_s);
    unsigned numberOfSplits = splitter.split(WTF::move(candidates));
    times.didFinish("split"_s);
    for (uint64_t depth = 0; !splitter.wrappersOfLastRound.isEmpty(); ++depth) {
        if (depth >= build.limit(LimitID::MaxForwardingDepth)) {
            build.gaveUp(LimitID::MaxForwardingDepth, Subject { });
            break;
        }
        numberOfSplits += splitter.split(splitter.findForwarders());
    }
    times.didFinish("splitForwarders"_s);
    build.statistics().count(StatisticID::FreshResultsSplit, numberOfSplits);
    splitter.expandCallSites();
    times.didFinish("expandCallSites"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
