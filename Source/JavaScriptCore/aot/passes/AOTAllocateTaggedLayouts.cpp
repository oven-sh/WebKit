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
#include "AOTSort.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static ValueID skipRequests(const Function& function, ValueID id)
{
    while (id) {
        const Value& value = function.at(id);
        if (value.opcode != Opcode::Convert && value.opcode != Opcode::Upcast)
            break;
        id = value.valueOperand();
    }
    return id;
}

// The values of a function as read have no types. TypeValues, which runs later, types all of them at once.
static ValueID appendAsRead(IRBuilder& builder, Value&& value)
{
    value.setOrigin(builder.origin());
    ValueID id = builder.function().add(WTF::move(value));
    builder.function().edit(builder.currentBlock(), BlockEdit::Values).values.append(id);
    return id;
}

static ValueID appendTypeTestAsRead(IRBuilder& builder, ValueID value, TypeID type)
{
    Value test = IRBuilder::make(Opcode::TypeTest, { value });
    test.setTestedType(type);
    return appendAsRead(builder, WTF::move(test));
}

static void reopen(IRBuilder& builder, BlockID block)
{
    builder.function().edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    builder.moveToEndOf(block);
}

// The one parameter of the function that it stores under the key of the tag in the object it constructs.
static ValueID findParameterStoredAsTag(const Program& program, const Function& function, ValueID object, PropertyKeyID tagKey)
{
    const auto& entryParameters = function.at(entryBlock).parameters;
    ValueID tag;
    auto addStore = [&](ValueID stored) {
        stored = skipRequests(function, stored);
        size_t index = function.at(stored).opcode == Opcode::Parameter ? entryParameters.find(stored) : notFound;
        if (tag || index == notFound || function.entryParameterKind(static_cast<unsigned>(index)) != EntryParameter::Parameter)
            return false;
        tag = stored;
        return true;
    };

    const Value& definition = function.at(object);
    if (definition.opcode == Opcode::NewObject) {
        const Vector<Field>& fields = program.at(definition.layout()).fields;
        std::span<const uint32_t> initialFields = definition.fields();
        for (size_t index = 0; index < initialFields.size(); ++index) {
            bool isTag = initialFields[index] < fields.size() && fields[initialFields[index]].key == tagKey;
            if (isTag && !addStore(definition.valuesOperands()[index]))
                return { };
        }
    }

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::PropSet && value.opcode != Opcode::PropDefine)
                continue;
            if (value.key() != tagKey || sameObjectThroughConversionsToObject(function, value.objectOperand()) != object)
                continue;
            if (!addStore(value.valueOperand()))
                return { };
        }
    }
    return tag;
}

// In a block of its own, which goes on to the rest of the function with the object.
static BlockID addAllocationBlock(IRBuilder& builder, const Value& allocationOfMaker, LayoutID layout, BlockFrequency frequency, BlockID rest)
{
    BlockID block = builder.addBlock(frequency);
    builder.moveToEndOf(block);
    Value allocation = allocationOfMaker;
    allocation.setLayout(layout);
    builder.emitJump(rest, Arguments { appendAsRead(builder, WTF::move(allocation)) });
    return block;
}

// In the block of the builder. Each member is allocated as the maker is, and every other tag goes on to the given block.
static void emitSwitchOnTag(IRBuilder& builder, const TaggedFamily& family, ValueID tag, BlockID otherTag, const Value& allocationOfMaker, BlockFrequency frequency, BlockID rest)
{
    BlockID start = builder.currentBlock();
    Vector<std::pair<int64_t, Target>> integerCases;
    Vector<std::pair<StringID, Target>> stringCases;
    for (const TaggedLayout& member : family.members) {
        BlockID allocation = addAllocationBlock(builder, allocationOfMaker, member.layout, frequency, rest);
        for (const LayoutTag& tag : member.tags) {
            WTF::switchOn(tag,
                [&](int64_t integer) {
                    integerCases.append({ integer, Target { allocation, { }, { } } });
                },
                [&](StringID string) {
                    stringCases.append({ string, Target { allocation, { }, { } } });
                });
        }
    }
    AOT::sort(integerCases, { }, &std::pair<int64_t, Target>::first);
    AOT::sort(stringCases, { }, &std::pair<StringID, Target>::first);

    BlockID noInteger = otherTag;
    if (integerCases.isEmpty() || !stringCases.isEmpty()) {
        noInteger = integerCases.isEmpty() ? start : builder.addBlock(builder.function().at(start).frequency);
        builder.moveToEndOf(noInteger);
        builder.emitSwitchString(tag, WTF::move(stringCases), Target { otherTag, { }, { } });
    }
    if (integerCases.isEmpty())
        return;
    builder.moveToEndOf(start);
    builder.emitSwitch(tag, WTF::move(integerCases), Target { noInteger, { }, { } });
}

// The body of the constructor function with an object of its own in the place of `this`: nothing else is ever `this` here, so all that holds for an object that was just allocated holds for it.
static Function createAllocatingCopy(const Program& program, BuildContext& build, TypeArena& types, const TaggedFamily& family, FunctionID constructor)
{
    const Function& original = program.at(constructor);

    Function copy = original;
    copy.capabilities = initialCapabilitiesOf(copy) | OptionSet<FunctionCapabilities> { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };
    copy.derivation.source = constructor;

    IRBuilder builder(AnalysisContext { build, program, nullptr, nullptr, constructor }, copy, types);
    builder.setOrigin({ original.position, 0 });
    ValueID thisValue = copy.at(entryBlock).parameters[*copy.thisParameterIndex()];
    if (copy.has(FunctionFlags::IsSloppy)) {
        ValueSubstitution substitution(copy);
        for (uint32_t blockIndex = 0; blockIndex < copy.blocks.size(); ++blockIndex) {
            const BlockValues& values = copy.at(BlockID(blockIndex)).values;
            BitVector erasedIndexes(values.size());
            for (size_t index = 0; index < values.size(); ++index) {
                const Value& value = copy.at(values[index]);
                if (value.opcode != Opcode::ToThis || sameObjectThroughConversionsToObject(copy, values[index]) != thisValue)
                    continue;
                substitution.replace(values[index], value.valueOperand());
                erasedIndexes.set(index);
            }
            if (!erasedIndexes.isEmpty())
                builder.eraseValues(BlockID(blockIndex), erasedIndexes);
        }
        substitution.apply(copy, nullptr);
    }
    ValueID tag = findParameterStoredAsTag(program, copy, thisValue, family.tagKey);
    RELEASE_ASSERT(tag);

    BlockID rest = builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    ValueID object = builder.addParameter(rest, TypeID());
    ValueSubstitution substitution(copy);
    substitution.replace(thisValue, object);
    substitution.apply(copy, nullptr);

    BitVector removedParameters(copy.numberOfEntryParameters());
    removedParameters.set(*copy.thisParameterIndex());
    builder.removeParameters(entryBlock, removedParameters);
    copy.flags.remove(FunctionFlags::HasThis);
    copy.flags.add(FunctionFlags::AllocatesOwnThis);
    copy.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));

    reopen(builder, entryBlock);
    Value read = IRBuilder::make(Opcode::GetConstructPrototype, { copy.at(entryBlock).parameters[*copy.newTargetParameterIndex()] });
    read.setLayout(family.maker);
    Value allocation = IRBuilder::make(Opcode::NewObject, { appendAsRead(builder, WTF::move(read)) });
    BlockID otherTag = addAllocationBlock(builder, allocation, family.maker, BlockFrequency::Normal, rest);
    builder.moveToEndOf(entryBlock);
    emitSwitchOnTag(builder, family, tag, otherTag, allocation, BlockFrequency::Normal, rest);

    // [[Construct]] answers with what the function returns if that is an object, and with the object it allocated otherwise.
    bool mayReturnOtherObject = false;
    uint32_t numberOfBlocks = copy.blocks.size();
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        const Terminator& terminator = copy.at(block).terminator;
        if (terminator.kind != TerminatorKind::Return && terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID returned = terminator.kind == TerminatorKind::Return ? terminator.valueOperand() : terminator.callOperand();
        builder.setOrigin(terminator.origin());
        reopen(builder, block);
        if (!returned || copy.at(skipRequests(copy, returned)).opcode == Opcode::Undefined || build.isSabotaged(Sabotage::TaggedMakersAnswerWithTheirOwnObject)) {
            builder.emitReturn(object);
            continue;
        }

        mayReturnOtherObject = true;
        IRBuilder::BranchBlocks blocks = builder.emitBranch(appendTypeTestAsRead(builder, returned, TypeTable::object));
        copy.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets[0].proofs.append(OperandIndex::TypeTest::value);
        Value provenObject(Opcode::Parameter);
        provenObject.setOrigin(builder.origin());
        ValueID provenObjectID = copy.add(WTF::move(provenObject));
        copy.edit(blocks.taken, BlockEdit::Parameters).parameters.append(provenObjectID);
        builder.emitReturn(provenObjectID);
        builder.moveToEndOf(blocks.notTaken);
        builder.emitReturn(object);
    }
    TypeID constructed = types.intern(Type::forLayoutSet(program.declaredMembersOf(family.maker)));
    copy.results[0] = DeclaredType(Proven::byPosition(mayReturnOtherObject ? TypeTable::object : constructed)).with(PositionFlags::IsFromCompiler);

    compactFunction(copy);
    return copy;
}

// The function keeps its body for a `this` that somebody hands in: a call, and a construction for another new.target.
static void enterAllocatingCopy(Program& program, BuildContext& build, TypeArena& types, FunctionID constructor, FunctionID copyID)
{
    Function& function = program.at(constructor);
    Function& copy = program.at(copyID);
    IRBuilder builder(AnalysisContext { build, program, nullptr, nullptr, constructor }, function, types);
    builder.setOrigin({ function.position, 0 });

    BlockID rest = builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    Vector<ValueID, 8> entryParameters(function.at(entryBlock).parameters.span());

    reopen(builder, entryBlock);
    // Whether new.target is a closure of the constructor: then its prototype property belongs to the layout of the constructor.
    ValueID isOwnNewTarget = appendTypeTestAsRead(builder, entryParameters[*function.newTargetParameterIndex()], types.intern(Type::forFunction(constructor)));
    BlockID construction = builder.addBlock();
    builder.emitBranch(isOwnNewTarget, Target { construction, { }, { } }, Target { build.isSabotaged(Sabotage::TaggedMakersAreOnlyConstructed) ? construction : rest, { }, { } });

    unsigned thisParameter = *function.thisParameterIndex();
    Vector<ValueID, 8> arguments;
    for (unsigned index = 0; index < entryParameters.size(); ++index) {
        if (index == thisParameter)
            continue;
        arguments.append(entryParameters[index]);
        copy.derivation.parameterMap.append(ParameterOrigin { index });
    }
    RELEASE_ASSERT(arguments.size() == copy.numberOfEntryParameters());

    builder.moveToEndOf(construction);
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(copyID);
    call.setArgumentsOperands(arguments.span());
    call.setIsConstruction(true);
    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { appendAsRead(builder, WTF::move(call)) });
    terminator.setRequirement(TailCallRequirement::Allowed);
    builder.emitTerminator(WTF::move(terminator));

    DeclaredType& result = function.results[0];
    result.changeType(Proven::byPosition(types.unionOf(result.written(), copy.results[0].written())));
    result.remove(PositionFlags::IsInt32Promise);
    compactFunction(function);
}

static bool isApplicationOrLibraryCodeWithBody(const Function& function)
{
    return function.hasBody() && (function.role == FunctionRole::Source || function.role == FunctionRole::Library);
}

static bool allocateInConstructorFunction(Program& program, BuildContext& build, TypeArena& types, const TaggedFamily& family, NOESCAPE const Invocable<bool(FunctionID)> auto& isCalledDirectly)
{
    FunctionID constructor = program.at(family.maker).constructor;
    {
        Function& function = program.at(constructor);
        bool isConstructorFunction = isApplicationOrLibraryCodeWithBody(function) && function.kind == FunctionKind::Plain && function.constructorForm() == ConstructorForm::Base && function.has(FunctionFlags::HasThis) && function.results.size() == 1;
        if (!isConstructorFunction || !build.isEnabled(PassID::AllocateTaggedLayouts, constructor))
            return false;
        if (!function.has(FunctionFlags::HasNewTarget) && isCalledDirectly(constructor))
            return false;
        if (!findParameterStoredAsTag(program, function, function.at(entryBlock).parameters[*function.thisParameterIndex()], family.tagKey))
            return false;

        IRBuilder builder(AnalysisContext { build, program, nullptr, nullptr, constructor }, function, types);
        builder.setOrigin({ function.position, 0 });
        auto& parameters = function.edit(entryBlock, BlockEdit::Parameters).parameters;
        if (!function.has(FunctionFlags::HasNewTarget)) {
            function.flags.add(FunctionFlags::HasNewTarget);
            ValueID added = builder.addParameter(entryBlock, TypeTable::anything);
            RELEASE_ASSERT(parameters.last() == added);
            parameters.removeLast();
            parameters.insert(*function.newTargetParameterIndex(), added);
        }
    }

    FunctionID copy(program.functions.size());
    program.functions.append(createAllocatingCopy(program, build, types, family, constructor));
    enterAllocatingCopy(program, build, types, constructor, copy);
    return true;
}

static std::optional<ProgramPoint> findOwnAllocation(const Function& function, LayoutID maker)
{
    ValueID newTarget = function.at(entryBlock).parameters[*function.newTargetParameterIndex()];
    std::optional<ProgramPoint> found;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::NewObject || value.layout() != maker)
                continue;
            ValueID prototype = skipRequests(function, value.prototypeOperand());
            if (found || !prototype)
                return std::nullopt;
            const Value& read = function.at(prototype);
            if (read.opcode != Opcode::GetConstructPrototype || read.layout() != maker || skipRequests(function, read.newTargetOperand()) != newTarget)
                return std::nullopt;
            found = ProgramPoint { BlockID(blockIndex), index };
        }
    }
    return found;
}

// Under the prototype of a class below this one the run time answers with an instance of that class, whatever the allocation names.
static bool allocateInClassConstructor(Program& program, BuildContext& build, TypeArena& types, const TaggedFamily& family)
{
    FunctionID constructor = program.at(family.maker).constructor;
    Function& function = program.at(constructor);
    if (!isApplicationOrLibraryCodeWithBody(function) || function.kind != FunctionKind::BaseConstructor || !function.has(FunctionFlags::HasNewTarget) || function.has(FunctionFlags::HasThis))
        return false;
    // As read, an initializer takes the allocation itself as its `this`.
    bool callsInitializer = std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            const Value& value = function.at(id);
            return value.opcode == Opcode::CallDirect && program.at(value.function()).kind == FunctionKind::Initializer;
        });
    });
    if (callsInitializer || !build.isEnabled(PassID::AllocateTaggedLayouts, constructor))
        return false;

    std::optional<ProgramPoint> own = findOwnAllocation(function, family.maker);
    if (!own)
        return false;
    ValueID ownValue = function.at(own->block).values[own->index];
    ValueID tag = findParameterStoredAsTag(program, function, ownValue, family.tagKey);
    if (!tag)
        return false;

    IRBuilder builder(AnalysisContext { build, program, nullptr, nullptr, constructor }, function, types);
    const Value allocationOfMaker = function.at(ownValue);
    builder.setOrigin(allocationOfMaker.origin());

    BlockID rest = builder.splitBlock(ProgramPoint { own->block, own->index + 1 });
    ValueID object = builder.addParameter(rest, TypeID());
    ValueSubstitution substitution(function);
    substitution.replace(ownValue, object);
    substitution.apply(function, nullptr);
    RELEASE_ASSERT(function.at(own->block).values.last() == ownValue);
    function.edit(own->block, BlockEdit::Values).values.removeLast();

    BlockFrequency frequency = function.at(own->block).frequency;
    builder.setHandler(function.at(own->block).handler);
    BlockID otherTag = addAllocationBlock(builder, allocationOfMaker, family.maker, frequency, rest);
    BlockID ownConstruction = builder.addBlock(frequency);

    reopen(builder, own->block);
    ValueID isOwnNewTarget = appendTypeTestAsRead(builder, function.at(entryBlock).parameters[*function.newTargetParameterIndex()], types.intern(Type::forFunction(constructor)));
    builder.emitBranch(isOwnNewTarget, Target { ownConstruction, { }, { } }, Target { otherTag, { }, { } });
    builder.moveToEndOf(ownConstruction);
    emitSwitchOnTag(builder, family, tag, otherTag, allocationOfMaker, frequency, rest);
    builder.setHandler(std::nullopt);

    compactFunction(function);
    return true;
}

void allocateTaggedLayouts(Program& program, BuildContext& build)
{
    TypeArena types(program.types);
    std::optional<BitVector> functionsCalledDirectly;
    // A direct call as read lists an argument for each entry parameter that the front end gave the function.
    auto isCalledDirectly = [&](FunctionID function) {
        // This pass adds direct calls of the copies that it makes, and of nothing else.
        if (const Program::BodiesAsRead* bodies = program.bodiesAsRead())
            return bodies->functionsCalledDirectly.quickGet(function.index());
        if (!functionsCalledDirectly) {
            Vector<FunctionID> callees = collectInParallel<FunctionID>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
                const Function& caller = program.functions[index];
                for (const Block& block : caller.blocks) {
                    for (ValueID id : block.values) {
                        if (caller.at(id).opcode == Opcode::CallDirect)
                            found.append(caller.at(id).function());
                    }
                }
            });
            functionsCalledDirectly.emplace(program.functions.size());
            for (FunctionID callee : callees)
                functionsCalledDirectly->quickSet(callee.index());
        }
        return functionsCalledDirectly->quickGet(function.index());
    };

    unsigned numberAllocated = 0;
    unsigned numberNeverAllocated = 0;
    for (const TaggedFamily& family : program.taggedFamilies) {
        bool isConstructorFunction = program.at(family.maker).has(LayoutFlags::IsConstructorFunction);
        if (isConstructorFunction ? allocateInConstructorFunction(program, build, types, family, isCalledDirectly) : allocateInClassConstructor(program, build, types, family)) {
            numberAllocated += family.members.size();
            continue;
        }
        for (const TaggedLayout& member : family.members)
            program.at(member.layout).flow.allocation = AllocationState::NeverAllocated;
        numberNeverAllocated += family.members.size();
    }

    // The tables by class and by name leave out the layouts that are never allocated.
    if (numberNeverAllocated) {
        program.indexClasses();
        program.indexNames(build.parallelLoops());
    }
    build.statistics().count(StatisticID::TaggedLayoutsAllocated, numberAllocated);
    build.statistics().count(StatisticID::TaggedLayoutsNeverAllocated, numberNeverAllocated);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
