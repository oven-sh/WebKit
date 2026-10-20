/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTaggedLayouts.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBuildContext.h"
#include "AOTIRBuilder.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void appendTaggedLayouts(Program& program, std::span<const TaggedFamily> families)
{
    TypeArena types(program.types);
    for (const TaggedFamily& family : families) {
        for (const TaggedLayout& member : family.members) {
            RELEASE_ASSERT(member.layout.index() == program.layouts.size());
            program.layouts.append(createDerivedLayout(program, family.maker));
            program.setDeclaredMembers(member.layout, types.completeLayoutSet(singleElementSpan(member.layout)));
        }
    }
}

static void appendFieldsOf(Program& program, TypeArena& types, LayoutID member, LayoutID declaringType)
{
    for (uint32_t index = 0; index < program.at(declaringType).fields.size(); ++index) {
        const Field& declared = program.at(declaringType).fields[index];
        if (!declared.key || declared.flags.contains(FieldFlags::IsMethod))
            continue;

        Layout& layout = program.at(member);
        if (std::optional<uint32_t> position = layout.findField(declared.key)) {
            Field& present = layout.fields[*position];
            if (present.flags.contains(FieldFlags::IsInherited))
                continue;
            if (!declared.flags.contains(FieldFlags::IsRequired))
                present.flags.remove(FieldFlags::IsRequired);
            present.declared.type = types.unionOf(present.declared.type, declared.declared.type);
            if (present.declared.written != declared.declared.written)
                present.declared.written = { };
            if (present.declared.predicted != declared.declared.predicted)
                present.declared.predicted = { };
            continue;
        }

        Field field;
        field.key = declared.key;
        field.declared = declared.declared;
        field.declared.site = { };
        field.flags = { FieldFlags::IsAppendedByFlow };
        if (declared.flags.contains(FieldFlags::IsRequired))
            field.flags.add(FieldFlags::IsRequired);
        layout.appendField(WTF::move(field));
    }
}

void appendFieldsOfTaggedLayouts(Program& program, std::span<const TaggedFamily> families)
{
    TypeArena types(program.types);
    for (const TaggedFamily& family : families) {
        for (const TaggedLayout& member : family.members) {
            appendInheritedFields(program, member.layout);
            for (LayoutID declaringType : member.declaringTypes)
                appendFieldsOf(program, types, member.layout, declaringType);
        }
    }
}

static ValueID skipRequests(const Function& function, ValueID id)
{
    while (id) {
        const Value& value = function.at(id);
        if (value.opcode != Opcode::ClaimDeclared && value.opcode != Opcode::Predict && value.opcode != Opcode::Upcast)
            break;
        id = value.valueOperand();
    }
    return id;
}

static ValueID findParameterStoredAsTag(const Function& function, ValueID thisValue, PropertyKeyID tagKey)
{
    const auto& entryParameters = function.at(entryBlock).parameters;
    ValueID tag;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::PropSet || value.key() != tagKey || skipRequests(function, value.objectOperand()) != thisValue)
                continue;
            ValueID stored = skipRequests(function, value.valueOperand());
            if (tag || function.at(stored).opcode != Opcode::Parameter || !entryParameters.contains(stored))
                return { };
            tag = stored;
        }
    }
    return tag;
}

static bool returnsNothing(const Function& function)
{
    return std::ranges::all_of(function.blocks, [&](const Block& block) {
        if (block.terminator.kind != TerminatorKind::Return)
            return block.terminator.kind != TerminatorKind::ReturnCall;
        ValueID returned = block.terminator.valueOperand();
        return !returned || function.at(returned).opcode == Opcode::Undefined;
    });
}

static bool canAllocateItsObjects(const Function& function)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::IsSloppy };
    return function.hasBody()
        && function.kind == FunctionKind::Plain
        && function.has(FunctionFlags::HasThis)
        && !function.flags.containsAny(otherForms)
        && returnsNothing(function);
}

static ValueID ensureEntryParameter(IRBuilder& builder, FunctionFlags flag, TypeID type)
{
    Function& function = builder.function();
    auto indexOf = [&] {
        return flag == FunctionFlags::TakesCallee ? function.calleeParameterIndex() : function.newTargetParameterIndex();
    };
    auto& parameters = function.at(entryBlock).parameters;
    if (function.has(flag))
        return parameters[*indexOf()];

    function.flags.add(flag);
    ValueID added = builder.addParameter(entryBlock, type);
    auto position = std::ranges::find(parameters, added);
    std::rotate(parameters.begin() + *indexOf(), position, position + 1);
    return added;
}

static void allocateInConstructor(Program& program, BuildContext& build, const TaggedFamily& family, FunctionID id, ValueID thisValue, ValueID tag)
{
    Function& function = program.at(id);
    TypeArena types(program.types);
    AnalysisContext context { build, program, nullptr, nullptr, id };
    IRBuilder builder(context, function, types);

    auto append = [&](Value&& value) {
        ValueID id = builder.append(WTF::move(value));
        function.at(id).type = { };
        return id;
    };

    BlockID body = builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    ValueID object = builder.addParameter(body, function.at(thisValue).type);
    ValueID result = builder.addParameter(body, TypeTable::anything);
    ValueID expected = object;
    if (TypeID predicted = function.thisType.predicted) {
        Value predict = IRBuilder::make(Opcode::Predict, { object });
        predict.setPredicted(predicted);
        expected = builder.insertBefore(ProgramPoint::atHeadOf(body), WTF::move(predict));
        builder.applyInsertions();
        function.at(expected).type = { };
    }

    ValueSubstitution substitution(function);
    substitution.replace(thisValue, expected);
    substitution.apply(function, nullptr);
    if (expected != object)
        function.at(expected).setValueOperand(object);

    for (Block& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            block.terminator.setValueOperand(result);
    }

    ValueID callee = ensureEntryParameter(builder, FunctionFlags::TakesCallee, TypeTable::function);
    ValueID newTarget = ensureEntryParameter(builder, FunctionFlags::HasNewTarget, TypeTable::anything);

    function.at(entryBlock).terminator = Terminator { };
    builder.moveToEndOf(entryBlock);
    ValueID nothing = append(IRBuilder::make(Opcode::Undefined));
    ValueID isOwnNewTarget = append(IRBuilder::make(Opcode::StrictEq, { newTarget, callee }));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(isOwnNewTarget);
    builder.moveToEndOf(blocks.notTaken);
    builder.emitJump(body, Arguments { thisValue, nothing });

    BlockID construction = blocks.taken;
    builder.moveToEndOf(construction);
    Value read = IRBuilder::make(Opcode::GetConstructPrototype, { callee });
    read.setLayout(family.maker);
    ValueID prototype = append(WTF::move(read));

    auto addAllocation = [&](LayoutID layout) {
        BlockID block = builder.addBlock();
        builder.moveToEndOf(block);
        Value allocation = IRBuilder::make(Opcode::NewObject, { prototype });
        allocation.setLayout(layout);
        ValueID allocated = append(WTF::move(allocation));
        builder.emitJump(body, Arguments { allocated, allocated });
        return block;
    };
    Vector<std::pair<int64_t, Target>> cases;
    for (const TaggedLayout& member : family.members)
        cases.append({ member.tag, Target { addAllocation(member.layout), { }, { } } });
    BlockID otherTag = addAllocation(family.maker);
    builder.moveToEndOf(construction);
    builder.emitSwitch(tag, WTF::move(cases), Target { otherTag, { }, { } });
}

void allocateTaggedLayouts(Program& program, BuildContext& build, std::span<const TaggedFamily> families)
{
    for (const TaggedFamily& family : families) {
        FunctionID constructor = program.at(family.maker).constructor;
        const Function& function = program.at(constructor);
        ValueID thisValue;
        ValueID tag;
        if (canAllocateItsObjects(function)) {
            thisValue = function.at(entryBlock).parameters[*function.thisParameterIndex()];
            tag = findParameterStoredAsTag(function, thisValue, family.tagKey);
        }
        if (tag) {
            allocateInConstructor(program, build, family, constructor, thisValue, tag);
            compactFunction(program.at(constructor));
            continue;
        }
        for (const TaggedLayout& member : family.members)
            program.at(member.layout).flow.allocation = AllocationState::NeverAllocated;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
