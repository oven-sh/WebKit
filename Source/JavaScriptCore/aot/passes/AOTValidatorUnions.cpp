/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidatorBuilder.h"

#if ENABLE(AOT)

#include "AOTKnownValue.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class MergePoint {
    WTF_MAKE_NONCOPYABLE(MergePoint);
public:
    explicit MergePoint(Emission& emission)
        : m_emission(emission)
    {
    }

    void addCurrentBlock(ValueID value) { m_predecessors.append({ m_emission.builder().currentBlock(), value }); }
    ValueID emit(ValueID unreachableResult);

private:
    struct Predecessor {
        BlockID block;
        ValueID value;
    };

    Emission& m_emission;
    Vector<Predecessor, 4> m_predecessors;
};

ValueID MergePoint::emit(ValueID unreachableResult)
{
    IRBuilder& builder = m_emission.builder();
    if (m_predecessors.isEmpty()) {
        builder.moveToEndOf(builder.addBlock(BlockFrequency::Rare));
        return unreachableResult;
    }
    if (m_predecessors.size() == 1) {
        builder.moveToEndOf(m_predecessors[0].block);
        return m_predecessors[0].value;
    }

    TypeID type = TypeTable::never;
    for (const Predecessor& predecessor : m_predecessors)
        type = m_emission.inputs().types.unionOf(type, builder.function().at(predecessor.value).type);

    BlockID merge = builder.addBlock();
    ValueID merged = builder.addParameter(merge, type);
    for (const Predecessor& predecessor : m_predecessors) {
        builder.moveToEndOf(predecessor.block);
        builder.emitJump(merge, { predecessor.value });
    }
    builder.moveToEndOf(merge);
    return merged;
}

struct TagDispatch {
    std::span<const ResultLiteral> tags;
    std::span<const BlockID> armBlocks;
    ValueID tag;
    BlockID noMatch;
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

static Vector<OptionSet<RuntimeClass>, 4> computeArmClasses(const Program& program, std::span<const ResultNodeRef> arms)
{
    return Vector<OptionSet<RuntimeClass>, 4>(arms.size(), [&](size_t index) {
        return runtimeClassesOf(program, arms[index]);
    });
}

static Vector<ResultNodeRef, 4> selectClassCandidates(std::span<const ResultNodeRef> arms, std::span<const OptionSet<RuntimeClass>> armClasses, RuntimeClass runtimeClass)
{
    Vector<ResultNodeRef, 4> candidates;
    for (size_t index = 0; index < arms.size(); ++index) {
        if (armClasses[index].contains(runtimeClass))
            candidates.append(arms[index]);
    }
    return candidates;
}

static Value makePropertyRead(Opcode opcode, std::initializer_list<ValueID> operands, PropertyKeyID key)
{
    Value read = IRBuilder::make(opcode, operands);
    read.setKey(key);
    return read;
}

static ValueID emitTagRead(Emission& emission, ValueID object, PropertyKeyID key, const ValidationExits& exits)
{
    IRBuilder& builder = emission.builder();
    ValueID isPlainData = builder.append(Opcode::IsPlainDataObject, { object });
    IRBuilder::ProvenBranch plainData = builder.emitBranchWithProof(isPlainData, 0, BlockFrequency::Normal, BlockFrequency::Rare);

    builder.moveToEndOf(plainData.notTaken);
    emission.emitJumpIf(emission.appendHelperCall(Builtins::ArrayIsArray, { object }), exits.rejected);
    builder.emitJump(exits.unknown);

    builder.moveToEndOf(plainData.taken);
    return builder.append(makePropertyRead(Opcode::GetOwnDataProperty, { object, plainData.provenOnTaken }, key));
}

static bool isStringTag(const ResultLiteral& tag)
{
    return tag.kind == ResultLiteral::Kind::String;
}

static void emitStringTagSwitch(Emission& emission, const TagDispatch& dispatch, BlockID notString)
{
    ValueID string = emission.emitTypeTest(dispatch.tag, TypeTable::string, notString);
    Vector<std::pair<StringID, Target>> cases;
    for (size_t index = 0; index < dispatch.tags.size(); ++index) {
        if (isStringTag(dispatch.tags[index]))
            cases.append({ dispatch.tags[index].string, Target { dispatch.armBlocks[index], { }, { } } });
    }
    emission.builder().emitSwitchString(string, WTF::move(cases), Target { dispatch.noMatch, { }, { } });
}

static void emitTagComparisons(Emission& emission, ValidationRuleEmitter& rules, const TagDispatch& dispatch)
{
    IRBuilder& builder = emission.builder();
    for (size_t index = 0; index < dispatch.tags.size(); ++index) {
        if (isStringTag(dispatch.tags[index]))
            continue;
        ValueID isEqual = builder.append(Opcode::StrictEq, { dispatch.tag, rules.appendLiteral(dispatch.tags[index]) });
        BlockID nextTag = builder.addBlock();
        builder.emitBranch(isEqual, Target { dispatch.armBlocks[index], { }, { } }, Target { nextTag, { }, { } });
        builder.moveToEndOf(nextTag);
    }
    builder.emitJump(dispatch.noMatch);
}

ValueID ValidatorBuilder::emitUnion(ResultNodeRef, const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    std::span<const ResultNodeRef> arms = node.children.span();
    Vector<OptionSet<RuntimeClass>, 4> armClasses = computeArmClasses(program(), arms);
    OptionSet<RuntimeClass> acceptedClasses;
    for (OptionSet<RuntimeClass> classes : armClasses)
        acceptedClasses.add(classes);

    MergePoint merge(*this);
    for (RuntimeClass runtimeClass : acceptedClasses) {
        const Type& classType = program().at(typeOfClass(inputs().types, runtimeClass));
        if (evaluateTypeTest(program(), typeOf(value), classType) == KnownBoolean::False)
            continue;

        Vector<ResultNodeRef, 4> candidates = selectClassCandidates(arms, armClasses.span(), runtimeClass);
        BlockID nextClass = builder().addBlock();
        ValueID narrowed = emitRequireClass(runtimeClass, value, nextClass);
        merge.addCurrentBlock(emitArmsOfClass(runtimeClass, candidates.span(), narrowed, exits));
        builder().moveToEndOf(nextClass);
        if (runtimeClass != RuntimeClass::Array)
            continue;

        ValueID isArrayProxy = appendHelperCall(Builtins::ArrayIsArray, { value });
        IRBuilder::BranchBlocks branch = builder().emitBranch(isArrayProxy, BlockFrequency::Rare, BlockFrequency::Normal);
        merge.addCurrentBlock(emitArmsOfClass(runtimeClass, candidates.span(), value, exits));
        builder().moveToEndOf(branch.notTaken);
    }

    bool mayAcceptSymbol = acceptedClasses == OptionSet<RuntimeClass>::all();
    builder().emitJump(mayAcceptSymbol ? exits.unknown : exits.rejected);
    return merge.emit(value);
}

ValueID ValidatorBuilder::emitRequireClass(RuntimeClass runtimeClass, ValueID value, BlockID otherwise)
{
    return emitTypeTest(value, typeOfClass(inputs().types, runtimeClass), otherwise);
}

ValueID ValidatorBuilder::emitArmsOfClass(RuntimeClass runtimeClass, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits& exits)
{
    if (arms.size() == 1)
        return emitNode(arms.front(), value, exits);
    if (runtimeClass != RuntimeClass::Object)
        return emitArmsInOrder(arms, value, exits);

    std::optional<TaggedArms> taggedArms = findTaggedArms(program(), arms);
    if (!taggedArms)
        return emitArmsInOrder(arms, value, exits);
    if (taggedArms->untagged.isEmpty())
        return emitArmsByDiscriminant(taggedArms->discriminant, taggedArms->tagged.span(), value, exits);

    MergePoint merge(*this);
    BlockID untagged = builder().addBlock();
    merge.addCurrentBlock(emitArmsByDiscriminant(taggedArms->discriminant, taggedArms->tagged.span(), value, { untagged, exits.unknown }));
    builder().moveToEndOf(untagged);
    merge.addCurrentBlock(emitArmsInOrder(taggedArms->untagged.span(), value, exits));
    return merge.emit(value);
}

ValueID ValidatorBuilder::emitArmsInOrder(std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits& exits)
{
    if (arms.empty())
        builder().emitJump(exits.rejected);

    MergePoint merge(*this);
    for (size_t index = 0; index < arms.size(); ++index) {
        bool isLast = index + 1 == arms.size();
        BlockID nextArm = isLast ? exits.rejected : builder().addBlock();
        merge.addCurrentBlock(emitNode(arms[index], value, { nextArm, exits.unknown }));
        if (!isLast)
            builder().moveToEndOf(nextArm);
    }
    return merge.emit(value);
}

ValueID ValidatorBuilder::emitArmsByDiscriminant(const ResultDiscriminant& discriminant, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits& exits)
{
    RELEASE_ASSERT(discriminant.tagOfArm.size() == arms.size());
    ValueID tag = emitTagRead(*this, value, discriminant.key, exits);

    Vector<BlockID, 4> armBlocks(arms.size(), [&](size_t) {
        return builder().addBlock();
    });
    TagDispatch dispatch { discriminant.tagOfArm.span(), armBlocks.span(), tag, exits.rejected };
    bool hasStringTag = std::ranges::any_of(dispatch.tags, isStringTag);
    bool hasOtherTag = !std::ranges::all_of(dispatch.tags, isStringTag);
    if (hasStringTag) {
        BlockID notString = hasOtherTag ? builder().addBlock() : exits.rejected;
        emitStringTagSwitch(*this, dispatch, notString);
        if (hasOtherTag)
            builder().moveToEndOf(notString);
    }
    if (hasOtherTag)
        emitTagComparisons(*this, m_rules, dispatch);

    MergePoint merge(*this);
    for (size_t index = 0; index < arms.size(); ++index) {
        builder().moveToEndOf(armBlocks[index]);
        merge.addCurrentBlock(emitNode(arms[index], value, exits));
    }
    return merge.emit(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
