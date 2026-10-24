/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidatorBuilder.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

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

static void appendDefine(IRBuilder& builder, ValueID object, PropertyKeyID key, ValueID value)
{
    Value define = IRBuilder::make(Opcode::PropDefine, { object, value });
    define.setKey(key);
    builder.append(WTF::move(define));
}

static void emitDefineUnlessUndefined(IRBuilder& builder, ValueID object, PropertyKeyID key, ValueID value)
{
    BlockID after = builder.addBlock();
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(value, TypeTable::undefined));
    builder.emitJump(after);

    builder.moveToEndOf(branch.notTaken);
    appendDefine(builder, object, key, branch.provenOnNotTaken);
    builder.emitJump(after);

    builder.moveToEndOf(after);
}

static ValueID emitRequireTypeOfMember(Emission& emission, ValueID value, const ResultNode& node, const ResultMember& member, BlockID otherwise)
{
    const EmissionInputs& inputs = emission.inputs();
    NodeDecisionInputs decisionInputs { inputs.program, inputs.flow, inputs.build };
    TypeID fieldType = fieldOf(inputs.program, node, member).declared.type;

    // Flow fills the field with the copies that validators create. Code that runs during the validation sees the array of the caller, never its copy.
    bool createsArrays = createsEveryArrayItself(decisionInputs, member.node, NodeUse::CopiedFromValue);
    Emission::ProofsByOrigin proofs = createsArrays ? Emission::ProofsByOrigin::AboutArrays : Emission::ProofsByOrigin::None;
    bool isStoredByClone = isRequiredWithoutDefault(inputs.program, member);
    TypeID tested = emission.fieldTypeToTest(fieldType, proofs);
    if (!isStoredByClone)
        tested = inputs.types.withPrimitives(tested, Primitive::Undefined);

    const Type& validated = emission.typeOf(value);
    if (!validated.hasOnlyPrimitives(Primitive::Undefined) && emission.passesOnlyUndefinedIntoStoredType(validated, tested))
        addInterpretedNodeDecline(decisionInputs, node.layout, InterpretedCause::FieldTypeIsNotTestable);
    if (!isStoredByClone)
        return emission.emitRequireStoredType(value, tested, otherwise);
    return emission.emitRequireFieldType(value, fieldType, proofs, otherwise);
}

static ValueID emitRequirePlainData(Emission& emission, ValueID object, BlockID otherwise)
{
    return emission.emitProvenBranch(emission.builder().append(Opcode::IsPlainDataObject, { object }), otherwise);
}

static ValueID emitNodeUnlessUndefined(ValidatorBuilder& validator, ResultNodeRef node, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    BlockID merge = builder.addBlock();
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(value, TypeTable::undefined));
    builder.emitJump(merge, { branch.provenOnTaken });

    builder.moveToEndOf(branch.notTaken);
    ValueID validated = validator.emitNode(node, branch.provenOnNotTaken, exits);
    TypeID mergedType = validator.inputs().types.unionOf(TypeTable::undefined, builder.function().at(validated).type);
    builder.emitJump(merge, { validated });

    ValueID merged = builder.addParameter(merge, mergedType);
    builder.moveToEndOf(merge);
    return merged;
}

static void emitValidateOwnValue(ValidatorBuilder& validator, ValueID copy, ValueID key, ResultNodeRef node, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    ValueID raw = builder.append(Opcode::ElemGet, { copy, key });
    ValueID validated = validator.emitNode(node, raw, exits);
    if (replacesValue(validator.program(), node))
        builder.append(Opcode::ElemDefine, { copy, key, validated });
}

auto ValidatorBuilder::emitPlainObject(ValueID value, const ValidationExits& exits) -> PlainObject
{
    ValueID object = emitTypeTest(value, TypeTable::object, exits.rejected);
    ValueID test = builder().append(Opcode::IsPlainDataObject, { object });
    IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Rare);

    // A validator runs no code of the program: the general validator spreads the object, once, and whatever it reads it reads from that copy.
    builder().moveToEndOf(branch.notTaken);
    emitJumpIf(appendHelperCall(Builtins::ArrayIsArray, { object }), exits.rejected);
    builder().emitJump(exits.unknown);

    builder().moveToEndOf(branch.taken);
    emitJumpIf(builder().appendTypeTest(object, Type::builtinClass(BuiltinClasses::Array)), exits.rejected);
    return { object, branch.provenOnTaken };
}

static Emission::ProofsByOrigin proofsByOriginOf(Emission& emission, const ResultMember& member)
{
    const EmissionInputs& inputs = emission.inputs();
    bool createsArrays = createsEveryArrayItself({ inputs.program, inputs.flow, inputs.build }, member.node, NodeUse::CopiedFromValue);
    return createsArrays ? Emission::ProofsByOrigin::AboutArrays : Emission::ProofsByOrigin::None;
}

// What holds for every object of a Structure is found out once for the Structure: that it is plain data, that each of its keys is a member, that no required member is missing, where the members lie and in which order.
bool ValidatorBuilder::canReadByPlan(const ResultNode& node)
{
    if (!copiesAsLayout(decisionInputs(), node))
        return false;
    return std::ranges::all_of(node.members, [&](const ResultMember& member) {
        bool isNamedField = member.field && program().at(member.key).kind == PropertyKeyKind::Text && !program().isIndexLike(member.key);
        return isNamedField && !member.mustBeOwn && !appliesDefault(program(), member.node);
    });
}

ValueID ValidatorBuilder::emitObject(ResultNodeRef, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    if (!canReadByPlan(node))
        return emitObjectMemberByMember(node, value, exits);

    ValueID object = emitTypeTest(value, TypeTable::object, exits.rejected);
    BlockID otherwise = builder().addBlock(BlockFrequency::Rare);
    BlockID done = builder().addBlock();
    ValueID byPlan = emitObjectByPlan(node, object, otherwise, exits);
    builder().emitJump(done, { byPlan });

    builder().moveToEndOf(otherwise);
    ValueID memberByMember = emitObjectMemberByMember(node, object, exits);
    builder().emitJump(done, { memberByMember });

    ValueID copy = builder().addParameter(done, inputs().types.unionOf(function().at(byPlan).type, function().at(memberByMember).type));
    builder().moveToEndOf(done);
    return copy;
}

ValueID ValidatorBuilder::emitObjectByPlan(const ResultNode& node, ValueID object, BlockID otherwise, const ValidationExits& exits)
{
    size_t numberOfMembers = node.members.size();
    Vector<uint32_t, 8> requested;
    for (const ResultMember& member : node.members)
        requested.append(*member.field);

    AllocationSiteID site = inputs().flow.typedResultSite(node.layout);
    Value find = IRBuilder::make(Opcode::FindSourcePlan, { object });
    find.setLayout(node.layout);
    find.setFields(requested.span());
    if (site)
        find.setAllocationSite(site);
    ValueID found = builder().append(WTF::move(find));
    emitJumpUnless(appendResultAt(found, 0), otherwise);
    ValueID plan = appendResultAt(found, 1);
    ValueID order = appendResultAt(found, 2);
    ValueID planned = appendResultAt(found, 3);

    Vector<ValueID, 8> values;
    Vector<ValueID, 8> isPresent;
    for (size_t index = 0; index < numberOfMembers; ++index) {
        Value read = IRBuilder::make(Opcode::GetPlannedProperty, { planned, plan });
        read.setMemberIndex(static_cast<uint32_t>(index));
        ValueID results = builder().append(WTF::move(read));
        values.append(appendResultAt(results, 0));
        isPresent.append(appendResultAt(results, 1));
    }

    for (size_t index = 0; index < numberOfMembers; ++index) {
        const ResultMember& member = node.members[index];
        if (!member.isOptional) {
            values[index] = emitNode(member.node, values[index], exits);
            continue;
        }

        BlockID merge = builder().addBlock();
        IRBuilder::BranchBlocks whetherPresent = builder().emitBranch(isPresent[index]);
        builder().moveToEndOf(whetherPresent.notTaken);
        builder().emitJump(merge, { builder().appendUndefined() });

        // A key that holds undefined has a place in the order, and a slot that holds nothing says that there is no such key.
        builder().moveToEndOf(whetherPresent.taken);
        IRBuilder::ProvenBranch whetherUndefined = builder().emitBranchWithProof(builder().appendTypeTest(values[index], TypeTable::undefined), 0, BlockFrequency::Rare);
        builder().emitJump(otherwise);

        builder().moveToEndOf(whetherUndefined.notTaken);
        ValueID validated = emitNode(member.node, whetherUndefined.provenOnNotTaken, exits);
        TypeID mergedType = inputs().types.unionOf(TypeTable::undefined, function().at(validated).type);
        builder().emitJump(merge, { validated });

        values[index] = builder().addParameter(merge, mergedType);
        builder().moveToEndOf(merge);
    }

    auto fitsField = [&](size_t index) {
        const ResultMember& member = node.members[index];
        TypeID tested = fieldTypeToTest(fieldOf(program(), node, member).declared.type, proofsByOriginOf(*this, member));
        return !passesOnlyUndefinedIntoStoredType(typeOf(values[index]), tested);
    };
    auto emitRequireTypeOfField = [&](size_t index) {
        const ResultMember& member = node.members[index];
        return emitRequireFieldType(values[index], fieldOf(program(), node, member).declared.type, proofsByOriginOf(*this, member), exits.unknown);
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
        allocation.setAllocationClass(inputs().flow.at(site).allocationClass);
    }
    ValueID copy = builder().append(WTF::move(allocation));

    for (size_t index = 0; index < numberOfMembers; ++index) {
        if (!node.members[index].isOptional)
            continue;
        IRBuilder::BranchBlocks whetherPresent = builder().emitBranch(isPresent[index]);
        BlockID next = builder().addBlock();
        builder().moveToEndOf(whetherPresent.notTaken);
        builder().emitJump(next);

        builder().moveToEndOf(whetherPresent.taken);
        if (!fitsField(index))
            builder().emitJump(exits.unknown);
        else {
            if (!isRemovedField(program(), { node.layout, requested[index] })) {
                // The allocation is the guard of its own shape.
                Value store = IRBuilder::make(Opcode::PutSlot, { copy, copy, emitRequireTypeOfField(index) });
                store.setKey(fieldOf(program(), node, node.members[index]).key);
                store.setReceivers(layoutSetOf(singleElementSpan(node.layout)));
                store.setLevel(KnowledgeLevel::Expected);
                builder().append(WTF::move(store));
            }
            builder().emitJump(next);
        }
        builder().moveToEndOf(next);
    }

    Value finish = IRBuilder::make(Opcode::JSONObjectFinish, { order, copy, builder().appendUndefined() });
    finish.setLayout(node.layout);
    BlockID finished = builder().addBlock();
    emitBranchOnOutcome(builder().append(WTF::move(finish)), finished, exits.unknown, exits.unknown);
    builder().moveToEndOf(finished);
    return copy;
}

ValueID ValidatorBuilder::emitObjectMemberByMember(const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    PlainObject object = emitPlainObject(value, exits);

    bool readsNeedNewGuard = false;
    if (node.undeclared == UndeclaredMembers::Rejected) {
        emitRejectUndeclaredMembers(object, node, exits);
        readsNeedNewGuard = true;
    }
    for (const ResultMember& member : node.members) {
        if (!member.mustBeOwn)
            continue;
        Value hasOwn = IRBuilder::make(Opcode::PropHasOwn, { object.object });
        hasOwn.setKey(member.key);
        emitJumpUnless(builder().append(WTF::move(hasOwn)), exits.rejected);
        readsNeedNewGuard = true;
    }
    if (readsNeedNewGuard)
        object.guard = emitRequirePlainData(*this, object.object, exits.unknown);

    Vector<ValueID, 8> rawValues;
    rawValues.reserveInitialCapacity(node.members.size());
    for (const ResultMember& member : node.members)
        rawValues.append(emitMember(object, member, exits));

    bool isCopiedAsLayout = copiesAsLayout(decisionInputs(), node);
    bool cloneNeedsNewGuard = false;
    Vector<CopiedMember, 8> replaced;
    for (size_t index = 0; index < node.members.size(); ++index) {
        const ResultMember& member = node.members[index];
        bool acceptsUndefinedAsIs = member.isOptional && !appliesDefault(program(), member.node);
        ValueID validated = acceptsUndefinedAsIs
            ? emitNodeUnlessUndefined(*this, member.node, rawValues[index], exits)
            : emitNode(member.node, rawValues[index], exits);
        if (isCopiedAsLayout && validationEndsStretch(member.node))
            cloneNeedsNewGuard = true;
        if (!replacesValue(program(), member.node))
            continue;
        if (isCopiedAsLayout)
            validated = emitRequireTypeOfMember(*this, validated, node, member, exits.unknown);
        replaced.append(CopiedMember { &member, validated });
    }
    if (cloneNeedsNewGuard)
        object.guard = emitRequirePlainData(*this, object.object, exits.unknown);

    ValueID copy = emitCopy(object, node, replaced.span(), exits);
    if (node.undeclared == UndeclaredMembers::Validated)
        emitValidateUndeclaredMembers(object, node, copy, exits);
    return copy;
}

ValueID ValidatorBuilder::emitMember(const PlainObject& object, const ResultMember& member, const ValidationExits&)
{
    Value read = IRBuilder::make(Opcode::GetOwnDataProperty, { object.object, object.guard });
    read.setKey(member.key);
    return builder().append(WTF::move(read));
}

ValueID ValidatorBuilder::emitCopy(const PlainObject& object, const ResultNode& node, std::span<const CopiedMember> replaced, const ValidationExits& exits)
{
    bool isCopiedAsLayout = copiesAsLayout(decisionInputs(), node);
    ValueID copy;
    if (isCopiedAsLayout) {
        Vector<uint32_t, 8> fields;
        Vector<ValueID, 8> values;
        for (const CopiedMember& copied : replaced) {
            if (!isRequiredWithoutDefault(program(), *copied.member))
                continue;
            fields.append(*copied.member->field);
            values.append(copied.value);
        }

        Value clone = IRBuilder::make(Opcode::CloneAs, { object.object, object.guard });
        clone.setLayout(node.layout);
        clone.setFields(fields.span());
        clone.setValuesOperands(values.span());
        if (AllocationSiteID site = inputs().flow.typedResultSite(node.layout))
            clone.setAllocationSite(site);
        // Flow knows the copies of a validator as objects of the layout, so an ordinary object, which the run time makes of a source that does not fit, is none of them.
        copy = emitTypeTest(builder().append(WTF::move(clone)), inputs().types.intern(Type::forLayout(node.layout, inputs().types)), exits.unknown);
    } else
        copy = appendCopyOfOwnProperties(*this, object.object);

    for (const CopiedMember& copied : replaced) {
        if (!isRequiredWithoutDefault(program(), *copied.member))
            emitDefineUnlessUndefined(builder(), copy, copied.member->key, copied.value);
        else if (!isCopiedAsLayout)
            appendDefine(builder(), copy, copied.member->key, copied.value);
    }
    return copy;
}

void ValidatorBuilder::emitRejectUndeclaredMembers(const PlainObject& object, const ResultNode& node, const ValidationExits& exits)
{
    OwnKeyLoop loop = beginOwnKeyLoop(object.object);
    emitBranchOnDeclaredKey(node, loop.key, loop.next, exits.rejected);
    builder().moveToEndOf(loop.done);
}

void ValidatorBuilder::emitValidateUndeclaredMembers(const PlainObject&, const ResultNode& node, ValueID copy, const ValidationExits& exits)
{
    RELEASE_ASSERT(node.undeclaredValue);
    OwnKeyLoop loop = beginOwnKeyLoop(copy);
    BlockID undeclared = builder().addBlock();
    emitBranchOnDeclaredKey(node, loop.key, loop.next, undeclared);

    builder().moveToEndOf(undeclared);
    emitValidateOwnValue(*this, copy, loop.key, *node.undeclaredValue, exits);
    builder().emitJump(loop.next);

    builder().moveToEndOf(loop.done);
}

void ValidatorBuilder::emitBranchOnDeclaredKey(const ResultNode& node, ValueID key, BlockID whenDeclared, BlockID whenUndeclared)
{
    if (node.members.isEmpty()) {
        builder().emitJump(whenUndeclared);
        return;
    }

    Vector<std::pair<StringID, Target>> cases;
    cases.reserveInitialCapacity(node.members.size());
    for (const ResultMember& member : node.members)
        cases.append({ program().at(member.key).text, Target { whenDeclared, { }, { } } });
    builder().emitSwitchString(key, WTF::move(cases), Target { whenUndeclared, { }, { } });
}

auto ValidatorBuilder::beginOwnKeyLoop(ValueID object) -> OwnKeyLoop
{
    OwnKeyLoop loop;
    loop.keys = appendHelperCall(Builtins::ObjectKeys, { object });
    Value lengthRead = IRBuilder::make(Opcode::PropGet, { loop.keys });
    lengthRead.setKey(program().keyOf(BuiltinNames::length));
    ValueID length = builder().append(WTF::move(lengthRead));

    BlockID header = builder().addBlock();
    ValueID index = builder().addParameter(header, TypeTable::number);
    loop.next = builder().addBlock();
    builder().emitJump(header, { builder().appendInteger(0) });

    builder().moveToEndOf(loop.next);
    builder().emitJump(header, { builder().append(Opcode::Increment, { index }) });

    builder().moveToEndOf(header);
    builder().append(Opcode::LoopHint, { });
    IRBuilder::BranchBlocks branch = builder().emitBranch(builder().append(Opcode::Less, { index, length }));
    loop.body = branch.taken;
    loop.done = branch.notTaken;

    // Object.keys() lists strings only, which the type of its result does not say.
    BlockID isNotString = builder().addBlock(BlockFrequency::Rare);
    loop.key = emitTypeTest(builder().append(Opcode::ElemGet, { loop.keys, index }), TypeTable::string, isNotString);
    BlockID afterTest = builder().currentBlock();
    builder().moveToEndOf(isNotString);
    builder().emitJump(loop.next);
    builder().moveToEndOf(afterTest);
    return loop;
}

ValueID ValidatorBuilder::emitDictionary(const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    RELEASE_ASSERT(node.children.size() == 1);
    PlainObject object = emitPlainObject(value, exits);
    ValueID copy = appendCopyOfOwnProperties(*this, object.object);

    OwnKeyLoop loop = beginOwnKeyLoop(copy);
    emitValidateOwnValue(*this, copy, loop.key, node.children[0], exits);
    builder().emitJump(loop.next);

    builder().moveToEndOf(loop.done);
    return copy;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
