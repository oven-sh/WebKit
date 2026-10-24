/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidatorBuilder.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTKnownValue.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestSite.h"
#include "AOTValidationPlanText.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t outcomeResultIndex = 0;
static constexpr uint32_t valueResultIndex = 1;

static constexpr int64_t interpretedVerdictIndex = 0;
static constexpr int64_t interpretedValueIndex = 1;

namespace {

struct MergePoint {
    void addCurrentBlock(ValueID value) { predecessors.append({ emission.builder().currentBlock(), value }); }

    ValueID emit(ValueID unreachableResult)
    {
        IRBuilder& builder = emission.builder();
        if (predecessors.isEmpty()) {
            builder.moveToEndOf(builder.addBlock(BlockFrequency::Rare));
            return unreachableResult;
        }
        if (predecessors.size() == 1) {
            builder.moveToEndOf(predecessors[0].first);
            return predecessors[0].second;
        }

        TypeID type = TypeTable::never;
        for (auto [block, value] : predecessors)
            type = emission.inputs().types.unionOf(type, builder.function().at(value).type);

        BlockID merge = builder.addBlock();
        ValueID merged = builder.addParameter(merge, type);
        for (auto [block, value] : predecessors) {
            builder.moveToEndOf(block);
            builder.emitJump(merge, { value });
        }
        builder.moveToEndOf(merge);
        return merged;
    }

    Emission& emission;
    Vector<std::pair<BlockID, ValueID>, 4> predecessors { };
};

struct PlainArray {
    ValueID array;
    ValueID guard;
    ValueID length;
};

struct PlainObject {
    ValueID object;
    ValueID guard;
};

struct OwnKeyLoop {
    ValueID key;
    BlockID next;
    BlockID done;
};

} // anonymous namespace

static TypeID typeOfClass(TypeArena& types, RuntimeClass runtimeClass)
{
    switch (runtimeClass) {
    case RuntimeClass::Undefined:
        return TypeTable::undefined;
    case RuntimeClass::Null:
        return TypeTable::null;
    case RuntimeClass::Boolean:
        return TypeTable::boolean;
    case RuntimeClass::Number:
        return TypeTable::number;
    case RuntimeClass::String:
        return TypeTable::string;
    case RuntimeClass::BigInt:
        return types.intern(Type::fromPrimitives(Primitive::BigInt));
    case RuntimeClass::Array:
        return types.intern(Type::builtinClass(BuiltinClasses::Array));
    case RuntimeClass::Object:
        return TypeTable::object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID emitRequireClass(ValidatorBuilder& validator, RuntimeClass runtimeClass, ValueID value, BlockID otherwise)
{
    return validator.emitTypeTest(value, typeOfClass(validator.inputs().types, runtimeClass), otherwise);
}

static ValueID appendWithKey(IRBuilder& builder, Opcode opcode, std::initializer_list<ValueID> operands, PropertyKeyID key)
{
    Value value = IRBuilder::make(opcode, operands);
    value.setKey(key);
    return builder.append(WTF::move(value));
}

static bool validationEndsStretch(ValidatorBuilder& validator, ResultNodeRef ref)
{
    ResultNode node = describeNode(validator.program(), ref);
    if (node.isContainer() || decideNodeCode(validator.decisionInputs(), ref, NodeUse::CopiedFromValue).code == NodeCode::Interpreted)
        return true;
    return std::ranges::any_of(node.rules, [&](const ValidationRule& rule) {
        RuleCode code = validator.rules().codeFor(rule, node.shape);
        return code != RuleCode::Inline && code != RuleCode::Nothing;
    });
}

// Arrays.

static Value makePlainArrayTest(ValueID value)
{
    return makePristineTest(value, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names });
}

static Value makeElementRead(ValueID array, ValueID index, ValueID guard)
{
    Value read = IRBuilder::make(Opcode::GetArrayElement, { array, index, guard });
    read.setArrayStorage(ArrayStorage::Other);
    read.setKeyIsIntegral(true);
    return read;
}

static PlainArray emitPlainArray(ValidatorBuilder& validator, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    BlockID notArrayInstance = builder.addBlock(BlockFrequency::Rare);
    ValueID array = emitRequireClass(validator, RuntimeClass::Array, value, notArrayInstance);
    ValueID guard = validator.emitProvenBranch(builder.append(makePlainArrayTest(array)), exits.unknown);

    // The elements of a proxy of an array are read by the general validator, like those of every array that is not plain.
    BlockID resumed = builder.currentBlock();
    builder.moveToEndOf(notArrayInstance);
    validator.emitJumpUnless(validator.appendHelperCall(Builtins::ArrayIsArray, { value }), exits.rejected);
    builder.emitJump(exits.unknown);
    builder.moveToEndOf(resumed);
    return { array, guard, builder.append(Opcode::GetArrayLength, { array, guard }) };
}

static ValueID emitArray(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    RELEASE_ASSERT(node.children.size() == 1);
    IRBuilder& builder = validator.builder();
    ResultNodeRef element = node.children[0];
    bool elementEndsStretch = validationEndsStretch(validator, element);

    auto [array, guard, length] = emitPlainArray(validator, value, exits);
    validator.rules().emitLengthRules(node, length, exits.rejected);
    ValueID copy = builder.append(makeArrayOf(validator.program(), node, { }));

    BlockID header = builder.addBlock();
    ValueID position = builder.addParameter(header, validator.inputs().types.intern(Type::integer(Range::uint32())));
    Arguments entryArguments { builder.appendInteger(0) };
    if (elementEndsStretch) {
        entryArguments.append(guard);
        guard = builder.addParameter(header, TypeTable::guard);
    }
    builder.emitJump(header, WTF::move(entryArguments));

    builder.moveToEndOf(header);
    builder.append(Opcode::LoopHint, { });
    ValueID isInRange = builder.append(Opcode::Less, { position, length });
    IRBuilder::ProvenBranch inRange = builder.emitBranchWithProof(isInRange, testedOperandIndex, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID index = inRange.provenOnTaken;

    ValueID validated = validator.emitNode(element, builder.append(makeElementRead(array, index, guard)), exits);
    builder.append(Opcode::AppendElement, { copy, validated });
    Arguments backEdgeArguments { builder.append(Opcode::Add, { index, builder.appendInteger(1) }) };
    if (elementEndsStretch)
        backEdgeArguments.append(validator.emitProvenBranch(builder.append(makePlainArrayTest(array)), exits.unknown));
    builder.emitJump(header, WTF::move(backEdgeArguments));

    builder.moveToEndOf(inRange.notTaken);
    return copy;
}

static ValueID emitTuple(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    PlainArray plain = emitPlainArray(validator, value, exits);
    ValueID expectedLength = builder.appendInteger(static_cast<int64_t>(node.children.size()));
    validator.emitJumpUnless(builder.append(Opcode::StrictEq, { plain.length, expectedLength }), exits.rejected);

    Vector<ValueID, 4> elements;
    for (size_t index = 0; index < node.children.size(); ++index)
        elements.append(builder.append(makeElementRead(plain.array, builder.appendInteger(static_cast<int64_t>(index)), plain.guard)));
    for (size_t index = 0; index < node.children.size(); ++index)
        elements[index] = validator.emitNode(node.children[index], elements[index], exits);
    return builder.append(makeArrayOf(validator.program(), node, elements.span()));
}

// Objects.

static bool replacesValue(const Program& program, ResultNodeRef node)
{
    return describeNode(program, node).isContainer() || appliesDefault(program, node);
}

static bool isRequiredWithoutDefault(const Program& program, const ResultMember& member)
{
    return !member.isOptional && !appliesDefault(program, member.node);
}

static bool copiesAsLayout(const NodeDecisionInputs& inputs, const ResultNode& node)
{
    if (!allocatesLayout(inputs, node))
        return false;
    return std::ranges::all_of(node.members, [&](const ResultMember& member) {
        return member.field || !replacesValue(inputs.program, member.node);
    });
}

static ValueID appendCopyOfOwnProperties(Emission& emission, ValueID object)
{
    ValueID copy = emission.appendHelperCall(Builtins::CompilerHelpersCopyOwnProperties, { object });
    return emission.builder().append(Opcode::ToObject, { copy });
}

static void emitDefineUnlessUndefined(IRBuilder& builder, ValueID object, PropertyKeyID key, ValueID value)
{
    BlockID after = builder.addBlock();
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(value, TypeTable::undefined));
    builder.emitJump(after);

    builder.moveToEndOf(branch.notTaken);
    appendWithKey(builder, Opcode::PropDefine, { object, branch.provenOnNotTaken }, key);
    builder.emitJump(after);

    builder.moveToEndOf(after);
}

// Flow fills the field with the copies that validators create. Code that runs during the validation sees the array of the caller, never its copy.
static Emission::ProofsByOrigin proofsByOriginOf(ValidatorBuilder& validator, const ResultMember& member)
{
    bool createsArrays = createsEveryArrayItself(validator.decisionInputs(), member.node, NodeUse::CopiedFromValue);
    return createsArrays ? Emission::ProofsByOrigin::AboutArrays : Emission::ProofsByOrigin::None;
}

static ValueID emitRequireTypeOfMember(ValidatorBuilder& validator, ValueID value, const ResultNode& node, const ResultMember& member, BlockID otherwise)
{
    TypeID fieldType = fieldOf(validator.program(), node, member).declared.type;
    Emission::ProofsByOrigin proofs = proofsByOriginOf(validator, member);
    bool isStoredByClone = isRequiredWithoutDefault(validator.program(), member);
    TypeID tested = validator.fieldTypeToTest(fieldType, proofs);
    if (!isStoredByClone)
        tested = validator.inputs().types.withPrimitives(tested, Primitive::Undefined);

    const Type& validated = validator.typeOf(value);
    if (!validated.hasOnlyPrimitives(Primitive::Undefined) && validator.passesOnlyUndefinedIntoStoredType(validated, tested))
        addInterpretedNodeDecline(validator.decisionInputs(), node.layout, InterpretedCause::FieldTypeIsNotTestable);
    if (!isStoredByClone)
        return validator.emitRequireStoredType(value, tested, otherwise);
    return validator.emitRequireFieldType(value, fieldType, proofs, otherwise);
}

static ValueID emitRequirePlainData(Emission& emission, ValueID object, BlockID otherwise)
{
    return emission.emitProvenBranch(emission.builder().append(Opcode::IsPlainDataObject, { object }), otherwise);
}

// A validator runs no code of the program: the general validator spreads the object, once, and whatever it reads it reads from that copy.
static ValueID emitPlainDataTest(Emission& emission, ValueID object, const ValidationExits& exits)
{
    IRBuilder& builder = emission.builder();
    ValueID test = builder.append(Opcode::IsPlainDataObject, { object });
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Rare);

    builder.moveToEndOf(branch.notTaken);
    emission.emitJumpIf(emission.appendHelperCall(Builtins::ArrayIsArray, { object }), exits.rejected);
    builder.emitJump(exits.unknown);

    builder.moveToEndOf(branch.taken);
    return branch.provenOnTaken;
}

static PlainObject emitPlainObject(Emission& emission, ValueID value, const ValidationExits& exits)
{
    ValueID object = emission.emitTypeTest(value, TypeTable::object, exits.rejected);
    ValueID guard = emitPlainDataTest(emission, object, exits);
    emission.emitJumpIf(emission.builder().appendTypeTest(object, Type::builtinClass(BuiltinClasses::Array)), exits.rejected);
    return { object, guard };
}

// The other way into the merge block brings undefined.
static ValueID emitNodeAndMerge(ValidatorBuilder& validator, ResultNodeRef node, ValueID value, BlockID merge, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    ValueID validated = validator.emitNode(node, value, exits);
    TypeID mergedType = validator.inputs().types.unionOf(TypeTable::undefined, builder.function().at(validated).type);
    builder.emitJump(merge, { validated });

    ValueID merged = builder.addParameter(merge, mergedType);
    builder.moveToEndOf(merge);
    return merged;
}

static ValueID emitNodeUnlessUndefined(ValidatorBuilder& validator, ResultNodeRef node, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    BlockID merge = builder.addBlock();
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(value, TypeTable::undefined));
    builder.emitJump(merge, { branch.provenOnTaken });

    builder.moveToEndOf(branch.notTaken);
    return emitNodeAndMerge(validator, node, branch.provenOnNotTaken, merge, exits);
}

static OwnKeyLoop beginOwnKeyLoop(ValidatorBuilder& validator, ValueID object)
{
    IRBuilder& builder = validator.builder();
    ValueID keys = validator.appendHelperCall(Builtins::ObjectKeys, { object });
    ValueID length = appendWithKey(builder, Opcode::PropGet, { keys }, validator.program().keyOf(BuiltinNames::length));

    BlockID header = builder.addBlock();
    ValueID index = builder.addParameter(header, TypeTable::number);
    BlockID next = builder.addBlock();
    builder.emitJump(header, { builder.appendInteger(0) });

    builder.moveToEndOf(next);
    builder.emitJump(header, { builder.append(Opcode::Increment, { index }) });

    builder.moveToEndOf(header);
    builder.append(Opcode::LoopHint, { });
    IRBuilder::BranchBlocks branch = builder.emitBranch(builder.append(Opcode::Less, { index, length }));

    // Object.keys() lists strings only, which the type of its result does not say.
    BlockID isNotString = builder.addBlock(BlockFrequency::Rare);
    ValueID key = validator.emitTypeTest(builder.append(Opcode::ElemGet, { keys, index }), TypeTable::string, isNotString);
    BlockID afterTest = builder.currentBlock();
    builder.moveToEndOf(isNotString);
    builder.emitJump(next);
    builder.moveToEndOf(afterTest);
    return { key, next, branch.notTaken };
}

static void emitBranchOnDeclaredKey(ValidatorBuilder& validator, const ResultNode& node, ValueID key, BlockID whenDeclared, BlockID whenUndeclared)
{
    if (node.members.isEmpty()) {
        validator.builder().emitJump(whenUndeclared);
        return;
    }

    Vector<std::pair<StringID, Target>> cases;
    cases.reserveInitialCapacity(node.members.size());
    for (const ResultMember& member : node.members)
        cases.append({ validator.program().at(member.key).text, Target { whenDeclared, { }, { } } });
    validator.builder().emitSwitchString(key, WTF::move(cases), Target { whenUndeclared, { }, { } });
}

// Validates what the copy holds under each of its own keys, but for the members that `declaring` declares.
static void emitValidateOwnValues(ValidatorBuilder& validator, ValueID copy, ResultNodeRef node, const ResultNode* declaring, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    OwnKeyLoop loop = beginOwnKeyLoop(validator, copy);
    if (declaring) {
        BlockID undeclared = builder.addBlock();
        emitBranchOnDeclaredKey(validator, *declaring, loop.key, loop.next, undeclared);
        builder.moveToEndOf(undeclared);
    }

    ValueID validated = validator.emitNode(node, builder.append(Opcode::ElemGet, { copy, loop.key }), exits);
    if (replacesValue(validator.program(), node))
        builder.append(Opcode::ElemDefine, { copy, loop.key, validated });
    builder.emitJump(loop.next);

    builder.moveToEndOf(loop.done);
}

static ValueID emitDictionary(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    RELEASE_ASSERT(node.children.size() == 1);
    ValueID copy = appendCopyOfOwnProperties(validator, emitPlainObject(validator, value, exits).object);
    emitValidateOwnValues(validator, copy, node.children[0], nullptr, exits);
    return copy;
}

// What holds for every object of a Structure is found out once for the Structure: that it is plain data, that each of its keys is a member, that no required member is missing, where the members lie and in which order.
static bool canReadByPlan(ValidatorBuilder& validator, const ResultNode& node)
{
    const Program& program = validator.program();
    if (!copiesAsLayout(validator.decisionInputs(), node))
        return false;
    return std::ranges::all_of(node.members, [&](const ResultMember& member) {
        bool isNamedField = member.field && program.at(member.key).kind == PropertyKeyKind::Text && !program.isIndexLike(member.key);
        return isNamedField && !member.mustBeOwn && !appliesDefault(program, member.node);
    });
}

static ValueID emitObjectByPlan(ValidatorBuilder& validator, const ResultNode& node, ValueID object, BlockID otherwise, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    const Program& program = validator.program();
    const FlowResults& flow = validator.inputs().flow;
    size_t numberOfMembers = node.members.size();
    Vector<uint32_t, 8> requested;
    for (const ResultMember& member : node.members)
        requested.append(*member.field);

    AllocationSiteID site = flow.typedResultSite(node.layout);
    Value find = IRBuilder::make(Opcode::FindSourcePlan, { object });
    find.setLayout(node.layout);
    find.setFields(requested.span());
    if (site)
        find.setAllocationSite(site);
    ValueID found = builder.append(WTF::move(find));
    validator.emitJumpUnless(validator.appendResultAt(found, 0), otherwise);
    ValueID plan = validator.appendResultAt(found, 1);
    ValueID order = validator.appendResultAt(found, 2);
    ValueID planned = validator.appendResultAt(found, 3);

    Vector<ValueID, 8> values;
    Vector<ValueID, 8> isPresent;
    for (size_t index = 0; index < numberOfMembers; ++index) {
        Value read = IRBuilder::make(Opcode::GetPlannedProperty, { planned, plan });
        read.setMemberIndex(static_cast<uint32_t>(index));
        ValueID results = builder.append(WTF::move(read));
        values.append(validator.appendResultAt(results, 0));
        isPresent.append(validator.appendResultAt(results, 1));
    }

    for (size_t index = 0; index < numberOfMembers; ++index) {
        const ResultMember& member = node.members[index];
        if (!member.isOptional) {
            values[index] = validator.emitNode(member.node, values[index], exits);
            continue;
        }

        BlockID merge = builder.addBlock();
        IRBuilder::BranchBlocks whetherPresent = builder.emitBranch(isPresent[index]);
        builder.moveToEndOf(whetherPresent.notTaken);
        builder.emitJump(merge, { builder.appendUndefined() });

        // A key that holds undefined has a place in the order, and a slot that holds nothing says that there is no such key.
        builder.moveToEndOf(whetherPresent.taken);
        IRBuilder::ProvenBranch whetherUndefined = builder.emitBranchWithProof(builder.appendTypeTest(values[index], TypeTable::undefined), 0, BlockFrequency::Rare);
        builder.emitJump(otherwise);

        builder.moveToEndOf(whetherUndefined.notTaken);
        values[index] = emitNodeAndMerge(validator, member.node, whetherUndefined.provenOnNotTaken, merge, exits);
    }

    auto fieldTypeOf = [&](size_t index) {
        return fieldOf(program, node, node.members[index]).declared.type;
    };
    auto emitRequireTypeOfField = [&](size_t index) {
        return validator.emitRequireFieldType(values[index], fieldTypeOf(index), proofsByOriginOf(validator, node.members[index]), exits.unknown);
    };

    Vector<uint32_t, 8> fields;
    Vector<ValueID, 8> initialValues;
    for (size_t index = 0; index < numberOfMembers; ++index) {
        if (node.members[index].isOptional)
            continue;
        fields.append(requested[index]);
        initialValues.append(emitRequireTypeOfField(index));
    }

    Value allocation = IRBuilder::make(Opcode::NewObject);
    allocation.setLayout(node.layout);
    allocation.setFields(fields.span());
    allocation.setValuesOperands(initialValues.span());
    if (site) {
        allocation.setAllocationSite(site);
        allocation.setAllocationClass(flow.at(site).allocationClass);
    }
    ValueID copy = builder.append(WTF::move(allocation));

    for (size_t index = 0; index < numberOfMembers; ++index) {
        if (!node.members[index].isOptional)
            continue;
        IRBuilder::BranchBlocks whetherPresent = builder.emitBranch(isPresent[index]);
        BlockID next = builder.addBlock();
        builder.moveToEndOf(whetherPresent.notTaken);
        builder.emitJump(next);

        builder.moveToEndOf(whetherPresent.taken);
        TypeID tested = validator.fieldTypeToTest(fieldTypeOf(index), proofsByOriginOf(validator, node.members[index]));
        if (validator.passesOnlyUndefinedIntoStoredType(validator.typeOf(values[index]), tested))
            builder.emitJump(exits.unknown);
        else {
            if (!isRemovedField(program, { node.layout, requested[index] })) {
                // The allocation is the guard of its own shape.
                Value store = IRBuilder::make(Opcode::PutSlot, { copy, copy, emitRequireTypeOfField(index) });
                store.setKey(fieldOf(program, node, node.members[index]).key);
                store.setReceivers(validator.layoutSetOf(singleElementSpan(node.layout)));
                store.setLevel(KnowledgeLevel::Expected);
                builder.append(WTF::move(store));
            }
            builder.emitJump(next);
        }
        builder.moveToEndOf(next);
    }

    Value finish = IRBuilder::make(Opcode::JSONObjectFinish, { order, copy, builder.appendUndefined() });
    finish.setLayout(node.layout);
    BlockID finished = builder.addBlock();
    validator.emitBranchOnOutcome(builder.append(WTF::move(finish)), finished, exits.unknown, exits.unknown);
    builder.moveToEndOf(finished);
    return copy;
}

static ValueID emitObjectMemberByMember(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    const Program& program = validator.program();
    auto [object, guard] = emitPlainObject(validator, value, exits);

    bool readsNeedNewGuard = node.undeclared == UndeclaredMembers::Rejected;
    if (readsNeedNewGuard) {
        OwnKeyLoop loop = beginOwnKeyLoop(validator, object);
        emitBranchOnDeclaredKey(validator, node, loop.key, loop.next, exits.rejected);
        builder.moveToEndOf(loop.done);
    }
    for (const ResultMember& member : node.members) {
        if (!member.mustBeOwn)
            continue;
        validator.emitJumpUnless(appendWithKey(builder, Opcode::PropHasOwn, { object }, member.key), exits.rejected);
        readsNeedNewGuard = true;
    }
    if (readsNeedNewGuard)
        guard = emitRequirePlainData(validator, object, exits.unknown);

    Vector<ValueID, 8> rawValues;
    rawValues.reserveInitialCapacity(node.members.size());
    for (const ResultMember& member : node.members)
        rawValues.append(appendWithKey(builder, Opcode::GetOwnDataProperty, { object, guard }, member.key));

    bool isCopiedAsLayout = copiesAsLayout(validator.decisionInputs(), node);
    bool cloneNeedsNewGuard = false;
    Vector<std::pair<const ResultMember*, ValueID>, 8> replaced;
    for (size_t index = 0; index < node.members.size(); ++index) {
        const ResultMember& member = node.members[index];
        bool acceptsUndefinedAsIs = member.isOptional && !appliesDefault(program, member.node);
        ValueID validated = acceptsUndefinedAsIs
            ? emitNodeUnlessUndefined(validator, member.node, rawValues[index], exits)
            : validator.emitNode(member.node, rawValues[index], exits);
        if (isCopiedAsLayout && validationEndsStretch(validator, member.node))
            cloneNeedsNewGuard = true;
        if (!replacesValue(program, member.node))
            continue;
        if (isCopiedAsLayout)
            validated = emitRequireTypeOfMember(validator, validated, node, member, exits.unknown);
        replaced.append({ &member, validated });
    }
    if (cloneNeedsNewGuard)
        guard = emitRequirePlainData(validator, object, exits.unknown);

    ValueID copy;
    if (isCopiedAsLayout) {
        Vector<uint32_t, 8> fields;
        Vector<ValueID, 8> values;
        for (auto [member, validated] : replaced) {
            if (!isRequiredWithoutDefault(program, *member))
                continue;
            fields.append(*member->field);
            values.append(validated);
        }

        Value clone = IRBuilder::make(Opcode::CloneAs, { object, guard });
        clone.setLayout(node.layout);
        clone.setFields(fields.span());
        clone.setValuesOperands(values.span());
        if (AllocationSiteID site = validator.inputs().flow.typedResultSite(node.layout))
            clone.setAllocationSite(site);
        // Flow knows the copies of a validator as objects of the layout, so an ordinary object, which the run time makes of a source that does not fit, is none of them.
        copy = validator.emitTypeTest(builder.append(WTF::move(clone)), validator.inputs().types.intern(Type::forLayout(node.layout, validator.inputs().types)), exits.unknown);
    } else
        copy = appendCopyOfOwnProperties(validator, object);

    for (auto [member, validated] : replaced) {
        if (!isRequiredWithoutDefault(program, *member))
            emitDefineUnlessUndefined(builder, copy, member->key, validated);
        else if (!isCopiedAsLayout)
            appendWithKey(builder, Opcode::PropDefine, { copy, validated }, member->key);
    }

    if (node.undeclared == UndeclaredMembers::Validated) {
        RELEASE_ASSERT(node.undeclaredValue);
        emitValidateOwnValues(validator, copy, *node.undeclaredValue, &node, exits);
    }
    return copy;
}

static ValueID emitObject(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    if (!canReadByPlan(validator, node))
        return emitObjectMemberByMember(validator, node, value, exits);

    IRBuilder& builder = validator.builder();
    ValueID object = validator.emitTypeTest(value, TypeTable::object, exits.rejected);
    BlockID otherwise = builder.addBlock(BlockFrequency::Rare);
    BlockID done = builder.addBlock();
    ValueID byPlan = emitObjectByPlan(validator, node, object, otherwise, exits);
    builder.emitJump(done, { byPlan });

    builder.moveToEndOf(otherwise);
    ValueID memberByMember = emitObjectMemberByMember(validator, node, object, exits);
    builder.emitJump(done, { memberByMember });

    ValueID copy = builder.addParameter(done, validator.inputs().types.unionOf(builder.function().at(byPlan).type, builder.function().at(memberByMember).type));
    builder.moveToEndOf(done);
    return copy;
}

// Unions.

static ValueID emitArmsInOrder(ValidatorBuilder& validator, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    if (arms.empty())
        builder.emitJump(exits.rejected);

    MergePoint merge { validator };
    for (size_t index = 0; index < arms.size(); ++index) {
        bool isLast = index + 1 == arms.size();
        BlockID nextArm = isLast ? exits.rejected : builder.addBlock();
        merge.addCurrentBlock(validator.emitNode(arms[index], value, { nextArm, exits.unknown }));
        if (!isLast)
            builder.moveToEndOf(nextArm);
    }
    return merge.emit(value);
}

static ValueID emitArmsByDiscriminant(ValidatorBuilder& validator, const ResultDiscriminant& discriminant, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    std::span<const ResultLiteral> tags = discriminant.tagOfArm.span();
    RELEASE_ASSERT(tags.size() == arms.size());
    ValueID tag = appendWithKey(builder, Opcode::GetOwnDataProperty, { value, emitPlainDataTest(validator, value, exits) }, discriminant.key);

    Vector<BlockID, 4> armBlocks(arms.size(), [&](size_t) {
        return builder.addBlock();
    });
    auto isStringTag = [](const ResultLiteral& tag) {
        return tag.kind == ResultLiteral::Kind::String;
    };
    bool hasOtherTag = !std::ranges::all_of(tags, isStringTag);
    if (std::ranges::any_of(tags, isStringTag)) {
        BlockID notString = hasOtherTag ? builder.addBlock() : exits.rejected;
        ValueID string = validator.emitTypeTest(tag, TypeTable::string, notString);
        Vector<std::pair<StringID, Target>> cases;
        for (size_t index = 0; index < tags.size(); ++index) {
            if (isStringTag(tags[index]))
                cases.append({ tags[index].string, Target { armBlocks[index], { }, { } } });
        }
        builder.emitSwitchString(string, WTF::move(cases), Target { exits.rejected, { }, { } });
        if (hasOtherTag)
            builder.moveToEndOf(notString);
    }
    for (size_t index = 0; index < tags.size(); ++index) {
        if (isStringTag(tags[index]))
            continue;
        ValueID isEqual = builder.append(Opcode::StrictEq, { tag, validator.rules().appendLiteral(tags[index]) });
        BlockID nextTag = builder.addBlock();
        builder.emitBranch(isEqual, Target { armBlocks[index], { }, { } }, Target { nextTag, { }, { } });
        builder.moveToEndOf(nextTag);
    }
    if (hasOtherTag)
        builder.emitJump(exits.rejected);

    MergePoint merge { validator };
    for (size_t index = 0; index < arms.size(); ++index) {
        builder.moveToEndOf(armBlocks[index]);
        merge.addCurrentBlock(validator.emitNode(arms[index], value, exits));
    }
    return merge.emit(value);
}

static ValueID emitArmsOfClass(ValidatorBuilder& validator, RuntimeClass runtimeClass, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits& exits)
{
    if (arms.size() == 1)
        return validator.emitNode(arms.front(), value, exits);
    if (runtimeClass != RuntimeClass::Object)
        return emitArmsInOrder(validator, arms, value, exits);

    std::optional<TaggedArms> taggedArms = findTaggedArms(validator.program(), arms);
    if (!taggedArms)
        return emitArmsInOrder(validator, arms, value, exits);
    if (taggedArms->untagged.isEmpty())
        return emitArmsByDiscriminant(validator, taggedArms->discriminant, taggedArms->tagged.span(), value, exits);

    MergePoint merge { validator };
    BlockID untagged = validator.builder().addBlock();
    merge.addCurrentBlock(emitArmsByDiscriminant(validator, taggedArms->discriminant, taggedArms->tagged.span(), value, { untagged, exits.unknown }));
    validator.builder().moveToEndOf(untagged);
    merge.addCurrentBlock(emitArmsInOrder(validator, taggedArms->untagged.span(), value, exits));
    return merge.emit(value);
}

static ValueID emitUnion(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    const Program& program = validator.program();
    std::span<const ResultNodeRef> arms = node.children.span();
    Vector<OptionSet<RuntimeClass>, 4> armClasses(arms.size(), [&](size_t index) {
        return runtimeClassesOf(program, arms[index]);
    });
    OptionSet<RuntimeClass> acceptedClasses;
    for (OptionSet<RuntimeClass> classes : armClasses)
        acceptedClasses.add(classes);

    MergePoint merge { validator };
    for (RuntimeClass runtimeClass : acceptedClasses) {
        const Type& classType = program.at(typeOfClass(validator.inputs().types, runtimeClass));
        if (evaluateTypeTest(program, validator.typeOf(value), classType) == KnownBoolean::False)
            continue;

        Vector<ResultNodeRef, 4> candidates;
        for (size_t index = 0; index < arms.size(); ++index) {
            if (armClasses[index].contains(runtimeClass))
                candidates.append(arms[index]);
        }
        BlockID nextClass = builder.addBlock();
        ValueID narrowed = emitRequireClass(validator, runtimeClass, value, nextClass);
        merge.addCurrentBlock(emitArmsOfClass(validator, runtimeClass, candidates.span(), narrowed, exits));
        builder.moveToEndOf(nextClass);
        if (runtimeClass != RuntimeClass::Array)
            continue;

        ValueID isArrayProxy = validator.appendHelperCall(Builtins::ArrayIsArray, { value });
        IRBuilder::BranchBlocks branch = builder.emitBranch(isArrayProxy, BlockFrequency::Rare, BlockFrequency::Normal);
        merge.addCurrentBlock(emitArmsOfClass(validator, runtimeClass, candidates.span(), value, exits));
        builder.moveToEndOf(branch.notTaken);
    }

    bool mayAcceptSymbol = acceptedClasses == OptionSet<RuntimeClass>::all();
    builder.emitJump(mayAcceptSymbol ? exits.unknown : exits.rejected);
    return merge.emit(value);
}

// Nodes.

static ValueID emitDefault(ValidatorBuilder& validator, const ResultNode& node, ValueID value)
{
    IRBuilder& builder = validator.builder();
    BlockID whenDefined = builder.addBlock();
    ValueID isUndefined = builder.appendTypeTest(value, TypeTable::undefined);
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isUndefined, testedOperandIndex, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID defaultValue = validator.rules().appendLiteral(*node.defaultValue);

    TypeID definedType = validator.inputs().types.unionOf(builder.function().at(defaultValue).type, builder.function().at(branch.provenOnNotTaken).type);
    ValueID defined = builder.addParameter(whenDefined, definedType);
    builder.emitJump(whenDefined, { defaultValue });

    builder.moveToEndOf(branch.notTaken);
    builder.emitJump(whenDefined, { branch.provenOnNotTaken });

    builder.moveToEndOf(whenDefined);
    return defined;
}

static ValueID emitShape(ValidatorBuilder& validator, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    switch (node.shape) {
    case ResultShape::Unknown:
        return value;
    case ResultShape::Never:
        builder.emitJump(exits.rejected);
        builder.moveToEndOf(builder.addBlock(BlockFrequency::Rare));
        return builder.appendUndefined();
    case ResultShape::Undefined:
        return emitRequireClass(validator, RuntimeClass::Undefined, value, exits.rejected);
    case ResultShape::Null:
        return emitRequireClass(validator, RuntimeClass::Null, value, exits.rejected);
    case ResultShape::AnyObject:
        return emitRequireClass(validator, RuntimeClass::Object, value, exits.rejected);
    case ResultShape::String:
        return emitRequireClass(validator, RuntimeClass::String, value, exits.rejected);
    case ResultShape::Boolean:
        return emitRequireClass(validator, RuntimeClass::Boolean, value, exits.rejected);
    case ResultShape::BigInt:
        return emitRequireClass(validator, RuntimeClass::BigInt, value, exits.rejected);
    case ResultShape::Number: {
        ValueID number = emitRequireClass(validator, RuntimeClass::Number, value, exits.rejected);
        validator.rules().emitRequireFinite(number, exits.rejected);
        return number;
    }
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        return validator.emitProvenBranch(builder.append(Opcode::StrictEq, { value, validator.rules().appendLiteral(node.literal) }), exits.rejected);
    case ResultShape::Array:
        return emitArray(validator, node, value, exits);
    case ResultShape::Tuple:
        return emitTuple(validator, node, value, exits);
    case ResultShape::Dictionary:
        return emitDictionary(validator, node, value, exits);
    case ResultShape::Object:
        return emitObject(validator, node, value, exits);
    case ResultShape::Union:
        return emitUnion(validator, node, value, exits);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID ValidatorBuilder::emitNodeBody(ResultNodeRef ref, ValueID value, const ValidationExits& exits)
{
    ResultNode node = describeNode(program(), ref);
    if (node.defaultValue)
        value = emitDefault(*this, node, value);
    ValueID result = emitShape(*this, node, value, exits);

    Vector<ValidationRule, 2> rulesAfterCopy;
    if (node.shape == ResultShape::Array) {
        for (const ValidationRule& rule : node.rules) {
            if (subjectOf(rule) != RuleSubject::Length)
                rulesAfterCopy.append(rule);
        }
        node.rules = rulesAfterCopy.span();
    }
    m_rules.emitRules(node, result, exits.rejected);
    return result;
}

ValueID ValidatorBuilder::emitNode(ResultNodeRef ref, ValueID value, const ValidationExits& exits)
{
    NodeDecision decision = decideNodeCode(decisionInputs(), ref, NodeUse::CopiedFromValue);
    if (decision.code == NodeCode::Interpreted) {
        addInterpretedNodeDecline(decisionInputs(), firstLayoutOf(program(), ref), decision.cause);
        StringID plan = validationPlanText(program(), inputs().strings, ref.expectedResult(), ref.part);
        ValueID pair = appendHelperCall(Builtins::CompilerHelpersInterpretNode, { value, appendString(plan), builder().append(Opcode::GetPreparedPlans, { }) });
        ValueID verdict = builder().append(Opcode::ElemGet, { pair, builder().appendInteger(interpretedVerdictIndex) });
        emitJumpUnless(builder().append(Opcode::ToBoolean, { verdict }), exits.rejected);
        return builder().append(Opcode::ElemGet, { pair, builder().appendInteger(interpretedValueIndex) });
    }
    if (!describeNode(program(), ref).isContainer())
        return emitNodeBody(ref, value, exits);

    ValueID results = appendCallOf(inputs().functions.ensure(validatorKeyOf(ref)), { value });
    BlockID parsed = builder().addBlock();
    emitBranchOnOutcome(appendResultAt(results, outcomeResultIndex), parsed, exits.rejected, exits.unknown);
    builder().moveToEndOf(parsed);
    return appendResultAt(results, valueResultIndex);
}

DerivedSignature ValidatorBuilder::signatureOf(Emission& emission, const ResultNode& node)
{
    Type validated = emission.program().at(node.type).withoutClassProofs().withUntestableBuiltinsWidened().withPrimitives(Primitive::Undefined);
    return { { TypeTable::anything }, { emission.outcomeType(), emission.inputs().types.intern(validated) } };
}

Function buildValidator(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    ResultNodeRef ref = nodeOfKey(key);
    EmissionInputs inputs(derived, { });
    ResultNode node = describeNode(inputs.program, ref);

    Function scratch;
    IRBuilder scratchBuilder(inputs.analysisContext(), scratch, inputs.types);
    Emission scratchEmission(scratchBuilder, inputs);
    DerivedFunctionHead head = createDerivedFunction(inputs, FunctionRole::Validator,
        CreatorRecord::Validator { .type = node.type, .layout = node.layout, .expectedResult = ref.expectedResult(), .node = ref.part },
        ValidatorBuilder::signatureOf(scratchEmission, node));

    IRBuilder builder(inputs.analysisContext(), head.function, inputs.types);
    builder.moveToEndOf(head.entry);
    ValidatorBuilder validator(builder, inputs);
    ValidationExits exits { builder.addBlock(BlockFrequency::Rare), builder.addBlock(BlockFrequency::Rare) };

    if (node.isContainer()) {
        builder.emitFailureOfPredictions(Target { exits.unknown, { }, { } });
        validator.emitJumpUnless(builder.append(Opcode::JSONHasStackSpace, { }), exits.unknown);
    }

    bool isInterpreted = decideNodeCode(validator.decisionInputs(), ref, NodeUse::CopiedFromValue).code == NodeCode::Interpreted;
    ValueID input = head.parameters[0];
    ValueID validated = isInterpreted ? validator.emitNode(ref, input, exits) : validator.emitNodeBody(ref, input, exits);
    validated = validator.emitTypeTest(validated, head.function.results[valueResultIndex].type, exits.unknown);
    validator.emitReturnOf({ validator.appendOutcome(JSONOutcome::Parsed), validated });

    builder.moveToEndOf(exits.rejected);
    validator.emitReturnOf({ validator.appendOutcome(JSONOutcome::Rejected), builder.appendUndefined() });

    builder.moveToEndOf(exits.unknown);
    validator.emitReturnOf({ validator.appendOutcome(JSONOutcome::Unknown), builder.appendUndefined() });
    return WTF::move(head.function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
