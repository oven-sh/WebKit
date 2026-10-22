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
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct OwnAllocation {
    BlockID block;
    uint32_t index { 0 };
    ValueID value;
};

class TaggedLayoutAllocator {
public:
    TaggedLayoutAllocator(Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
        , m_types(program.types)
    {
    }

    void run();

private:
    AnalysisContext contextOf(FunctionID id) const { return { m_build, m_program, nullptr, nullptr, id }; }
    bool isCalledDirectly(FunctionID);
    ValueID appendTestOfNewTarget(IRBuilder&, FunctionID constructor, ValueID newTarget);

    bool allocateInConstructorFunction(const TaggedFamily&);
    Function createAllocatingCopy(const TaggedFamily&, FunctionID constructor);
    void enterAllocatingCopy(FunctionID constructor, FunctionID copy);
    bool allocateInClassConstructor(const TaggedFamily&);

    Program& m_program;
    BuildContext& m_build;
    TypeArena m_types;
    std::optional<BitVector> m_isCalledDirectly;
};

} // anonymous namespace

// The values of a function as read have no types. TypeValues, which runs later, types all of them at once.
static ValueID appendAsRead(IRBuilder& builder, Value&& value)
{
    Function& function = builder.function();
    value.setOrigin(builder.origin());
    function.add(WTF::move(value));
    ValueID id(function.values.size() - 1);
    function.edit(builder.currentBlock(), BlockEdit::Values).values.append(id);
    return id;
}

static ValueID addProofAsRead(IRBuilder& builder, BlockID from, uint32_t targetIndex, uint32_t provenOperand)
{
    Function& function = builder.function();
    Target& target = function.edit(from, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets[targetIndex];
    target.proofs.append(provenOperand);

    Value parameter(Opcode::Parameter);
    parameter.setOrigin(builder.origin());
    function.add(WTF::move(parameter));
    ValueID id(function.values.size() - 1);
    function.edit(target.block, BlockEdit::Parameters).parameters.append(id);
    return id;
}

static void reopen(IRBuilder& builder, BlockID block)
{
    builder.function().edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    builder.moveToEndOf(block);
}

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

// `this` of a construction is an object, which ToThis answers with.
static ValueID skipRequestsOnConstructedObject(const Function& function, ValueID id)
{
    while (id) {
        const Value& value = function.at(id);
        if (value.opcode != Opcode::Convert && value.opcode != Opcode::Upcast && value.opcode != Opcode::ToThis)
            break;
        id = value.valueOperand();
    }
    return id;
}

static bool isApplicationOrLibraryCode(const Function& function)
{
    return function.role == FunctionRole::Source || function.role == FunctionRole::Library;
}

static bool isConstructorFunctionWithBody(const Function& function)
{
    return function.hasBody()
        && isApplicationOrLibraryCode(function)
        && function.kind == FunctionKind::Plain
        && function.constructorForm() == ConstructorForm::Base
        && function.has(FunctionFlags::HasThis)
        && function.results.size() == 1;
}

static bool isBaseClassConstructorWithBody(const Function& function)
{
    return function.hasBody()
        && isApplicationOrLibraryCode(function)
        && function.kind == FunctionKind::BaseConstructor
        && function.has(FunctionFlags::HasNewTarget)
        && !function.has(FunctionFlags::HasThis);
}

static BitVector findFunctionsCalledDirectly(const Program& program, const ParallelLoops& loops)
{
    Vector<FunctionID> callees = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::CallDirect)
                    found.append(value.function());
            }
        }
    });

    BitVector isCalledDirectly(program.functions.size());
    for (FunctionID callee : callees)
        isCalledDirectly.quickSet(callee.index());
    return isCalledDirectly;
}

// The one parameter of the function that it stores under the key of the tag in the object it constructs. The layout is chosen before the store,
// so what is stored has to be there at the entry.
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
            if (value.key() != tagKey || skipRequestsOnConstructedObject(function, value.objectOperand()) != object)
                continue;
            if (!addStore(value.valueOperand()))
                return { };
        }
    }
    return tag;
}

static ValueID ensureNewTargetParameter(IRBuilder& builder)
{
    Function& function = builder.function();
    auto& parameters = function.edit(entryBlock, BlockEdit::Parameters).parameters;
    if (function.has(FunctionFlags::HasNewTarget))
        return parameters[*function.newTargetParameterIndex()];

    function.flags.add(FunctionFlags::HasNewTarget);
    ValueID added = builder.addParameter(entryBlock, TypeTable::anything);
    RELEASE_ASSERT(parameters.last() == added);
    parameters.removeLast();
    parameters.insert(*function.newTargetParameterIndex(), added);
    return added;
}

// In the block of the builder. Each member is allocated in a block of its own, which the functor adds, and every other tag goes on to the given block.
static void emitSwitchOnTag(IRBuilder& builder, const TaggedFamily& family, ValueID tag, BlockID otherTag, NOESCAPE const Invocable<BlockID(LayoutID)> auto& addAllocation)
{
    BlockID start = builder.currentBlock();
    Vector<std::pair<int64_t, Target>> integerCases;
    Vector<std::pair<StringID, Target>> stringCases;
    for (const TaggedLayout& member : family.members) {
        BlockID allocation = addAllocation(member.layout);
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
    std::ranges::sort(integerCases, { }, &std::pair<int64_t, Target>::first);
    std::ranges::sort(stringCases, { }, &std::pair<StringID, Target>::first);

    if (integerCases.isEmpty()) {
        builder.moveToEndOf(start);
        builder.emitSwitchString(tag, WTF::move(stringCases), Target { otherTag, { }, { } });
        return;
    }

    BlockID noInteger = otherTag;
    if (!stringCases.isEmpty()) {
        noInteger = builder.addBlock(builder.function().at(start).frequency);
        builder.moveToEndOf(noInteger);
        builder.emitSwitchString(tag, WTF::move(stringCases), Target { otherTag, { }, { } });
    }
    builder.moveToEndOf(start);
    builder.emitSwitch(tag, WTF::move(integerCases), Target { noInteger, { }, { } });
}

static void removeConversionsOfThis(IRBuilder& builder, ValueID thisValue)
{
    Function& function = builder.function();
    if (!function.has(FunctionFlags::IsSloppy))
        return;

    ValueSubstitution substitution(function);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        BitVector erasedIndexes(values.size());
        for (size_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::ToThis || skipRequestsOnConstructedObject(function, values[index]) != thisValue)
                continue;
            substitution.replace(values[index], value.valueOperand());
            erasedIndexes.set(index);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), erasedIndexes);
    }
    substitution.apply(function, nullptr);
}

// [[Construct]] answers with what the function returns if that is an object, and with the object it allocated otherwise. No call is in tail position before that test.
// Answers whether the function may return another object.
static bool returnConstructedObject(IRBuilder& builder, ValueID object)
{
    Function& function = builder.function();
    bool mayReturnOtherObject = false;
    uint32_t numberOfBlocks = function.blocks.size();
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Return && terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID returned = terminator.kind == TerminatorKind::Return ? terminator.valueOperand() : terminator.callOperand();
        builder.setOrigin(terminator.origin());
        reopen(builder, block);
        if (!returned || function.at(skipRequests(function, returned)).opcode == Opcode::Undefined) {
            builder.emitReturn(object);
            continue;
        }

        mayReturnOtherObject = true;
        Value test = IRBuilder::make(Opcode::TypeTest, { returned });
        test.setTestedType(TypeTable::object);
        ValueID isObject = appendAsRead(builder, WTF::move(test));
        IRBuilder::BranchBlocks blocks = builder.emitBranch(isObject);
        builder.emitReturn(addProofAsRead(builder, block, 0, OperandIndex::TypeTest::value));
        builder.moveToEndOf(blocks.notTaken);
        builder.emitReturn(object);
    }
    return mayReturnOtherObject;
}

static std::optional<OwnAllocation> findOwnAllocation(const Function& function, LayoutID maker)
{
    ValueID newTarget = function.at(entryBlock).parameters[*function.newTargetParameterIndex()];
    std::optional<OwnAllocation> found;
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
            found = OwnAllocation { BlockID(blockIndex), index, values[index] };
        }
    }
    return found;
}

// As read, an initializer takes the allocation itself as its `this`.
static bool callsInitializer(const Program& program, const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            const Value& value = function.at(id);
            return value.opcode == Opcode::CallDirect && program.at(value.function()).kind == FunctionKind::Initializer;
        });
    });
}

// A direct call as read lists an argument for each entry parameter that the front end gave the function.
bool TaggedLayoutAllocator::isCalledDirectly(FunctionID function)
{
    // This pass adds direct calls of the copies that it makes, and of nothing else.
    if (const Program::BodiesAsRead* bodies = m_program.bodiesAsRead())
        return bodies->functionsCalledDirectly.quickGet(function.index());
    if (!m_isCalledDirectly)
        m_isCalledDirectly = findFunctionsCalledDirectly(m_program, m_build.parallelLoops());
    return m_isCalledDirectly->quickGet(function.index());
}

// Whether new.target is a closure of the constructor: then its prototype property belongs to the layout of the constructor. The type of new.target decides the test
// wherever the construction is known: a call passes undefined, and a direct construction passes the closure.
ValueID TaggedLayoutAllocator::appendTestOfNewTarget(IRBuilder& builder, FunctionID constructor, ValueID newTarget)
{
    Value test = IRBuilder::make(Opcode::TypeTest, { newTarget });
    test.setTestedType(m_types.intern(Type::forFunction(constructor)));
    return appendAsRead(builder, WTF::move(test));
}

// The body of the constructor function with an object of its own in the place of `this`: nothing else is ever `this` here, so all that holds for an object that was just allocated holds for it.
Function TaggedLayoutAllocator::createAllocatingCopy(const TaggedFamily& family, FunctionID constructor)
{
    const Function& original = m_program.at(constructor);

    Function copy = original;
    copy.capabilities = initialCapabilitiesOf(copy) | OptionSet<FunctionCapabilities> { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };
    copy.derivation.source = constructor;
    // `this` is no entry parameter of the copy, so what the relations say of the parameters by their indices is not true of it.
    copy.relations = { };

    IRBuilder builder(contextOf(constructor), copy, m_types);
    builder.setOrigin({ original.position, 0 });
    ValueID thisValue = copy.at(entryBlock).parameters[*copy.thisParameterIndex()];
    removeConversionsOfThis(builder, thisValue);
    ValueID tag = findParameterStoredAsTag(m_program, copy, thisValue, family.tagKey);
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
    copy.thisType = DeclaredType { .type = TypeTable::anything };

    reopen(builder, entryBlock);
    Value read = IRBuilder::make(Opcode::GetConstructPrototype, { copy.at(entryBlock).parameters[*copy.newTargetParameterIndex()] });
    read.setLayout(family.maker);
    ValueID prototype = appendAsRead(builder, WTF::move(read));
    auto addAllocation = [&](LayoutID layout) {
        BlockID block = builder.addBlock();
        builder.moveToEndOf(block);
        Value allocation = IRBuilder::make(Opcode::NewObject, { prototype });
        allocation.setLayout(layout);
        builder.emitJump(rest, Arguments { appendAsRead(builder, WTF::move(allocation)) });
        return block;
    };
    BlockID otherTag = addAllocation(family.maker);
    builder.moveToEndOf(entryBlock);
    emitSwitchOnTag(builder, family, tag, otherTag, addAllocation);

    bool mayReturnOtherObject = returnConstructedObject(builder, object);
    TypeID constructed = m_types.intern(Type::forLayoutSet(m_program.declaredMembersOf(family.maker)));
    copy.results[0] = DeclaredType { .type = mayReturnOtherObject ? TypeTable::object : constructed, .flags = { PositionFlags::IsFromCompiler } };

    compactFunction(copy);
    return copy;
}

// The function keeps its body for a `this` that somebody hands in: a call, and a construction for another new.target.
// A construction by value hands in an object too, which the run time allocated. It goes unused, and the caller takes the one that is returned.
void TaggedLayoutAllocator::enterAllocatingCopy(FunctionID constructor, FunctionID copyID)
{
    Function& function = m_program.at(constructor);
    Function& copy = m_program.at(copyID);
    IRBuilder builder(contextOf(constructor), function, m_types);
    builder.setOrigin({ function.position, 0 });

    BlockID rest = builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    Vector<ValueID, 8> entryParameters(function.at(entryBlock).parameters.span());

    reopen(builder, entryBlock);
    ValueID isOwnNewTarget = appendTestOfNewTarget(builder, constructor, entryParameters[*function.newTargetParameterIndex()]);
    BlockID construction = builder.addBlock();
    builder.emitBranch(isOwnNewTarget, Target { construction, { }, { } }, Target { rest, { }, { } });

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
    result.type = m_types.unionOf(result.type, copy.results[0].type);
    result.flags.remove(PositionFlags::IsInt32Promise);
    compactFunction(function);
}

bool TaggedLayoutAllocator::allocateInConstructorFunction(const TaggedFamily& family)
{
    FunctionID constructor = m_program.at(family.maker).constructor;
    {
        const Function& function = m_program.at(constructor);
        if (!isConstructorFunctionWithBody(function) || !m_build.isEnabled(PassID::AllocateTaggedLayouts, constructor))
            return false;
        if (!function.has(FunctionFlags::HasNewTarget) && isCalledDirectly(constructor))
            return false;
        ValueID thisValue = function.at(entryBlock).parameters[*function.thisParameterIndex()];
        if (!findParameterStoredAsTag(m_program, function, thisValue, family.tagKey))
            return false;
    }

    {
        Function& function = m_program.at(constructor);
        IRBuilder builder(contextOf(constructor), function, m_types);
        builder.setOrigin({ function.position, 0 });
        ensureNewTargetParameter(builder);
        // The relations name the entry parameters by their places, which have moved, and know nothing of the call that hands them to the copy.
        function.relations = { };
    }

    FunctionID copy(m_program.functions.size());
    m_program.functions.append(createAllocatingCopy(family, constructor));
    enterAllocatingCopy(constructor, copy);
    return true;
}

// Under the prototype of a class below this one the run time answers with an instance of that class, whatever the allocation names.
// So the layout of the tag is allocated only where new.target is the class itself.
bool TaggedLayoutAllocator::allocateInClassConstructor(const TaggedFamily& family)
{
    FunctionID constructor = m_program.at(family.maker).constructor;
    Function& function = m_program.at(constructor);
    if (!isBaseClassConstructorWithBody(function) || callsInitializer(m_program, function) || !m_build.isEnabled(PassID::AllocateTaggedLayouts, constructor))
        return false;

    std::optional<OwnAllocation> own = findOwnAllocation(function, family.maker);
    ValueID tag = own ? findParameterStoredAsTag(m_program, function, own->value, family.tagKey) : ValueID();
    if (!tag)
        return false;

    IRBuilder builder(contextOf(constructor), function, m_types);
    const Value allocationOfMaker = function.at(own->value);
    builder.setOrigin(allocationOfMaker.origin());

    BlockID rest = builder.splitBlock(ProgramPoint { own->block, own->index + 1 });
    ValueID object = builder.addParameter(rest, TypeID());
    ValueSubstitution substitution(function);
    substitution.replace(own->value, object);
    substitution.apply(function, nullptr);
    RELEASE_ASSERT(function.at(own->block).values.last() == own->value);
    function.edit(own->block, BlockEdit::Values).values.removeLast();

    BlockFrequency frequency = function.at(own->block).frequency;
    builder.setHandler(function.at(own->block).handler);
    auto addAllocation = [&](LayoutID layout) {
        BlockID block = builder.addBlock(frequency);
        builder.moveToEndOf(block);
        Value allocation = allocationOfMaker;
        allocation.setLayout(layout);
        builder.emitJump(rest, Arguments { appendAsRead(builder, WTF::move(allocation)) });
        return block;
    };
    BlockID otherTag = addAllocation(family.maker);
    BlockID ownConstruction = builder.addBlock(frequency);

    reopen(builder, own->block);
    ValueID isOwnNewTarget = appendTestOfNewTarget(builder, constructor, function.at(entryBlock).parameters[*function.newTargetParameterIndex()]);
    builder.emitBranch(isOwnNewTarget, Target { ownConstruction, { }, { } }, Target { otherTag, { }, { } });
    builder.moveToEndOf(ownConstruction);
    emitSwitchOnTag(builder, family, tag, otherTag, addAllocation);
    builder.setHandler(std::nullopt);

    compactFunction(function);
    return true;
}

void TaggedLayoutAllocator::run()
{
    unsigned numberAllocated = 0;
    unsigned numberNeverAllocated = 0;
    for (const TaggedFamily& family : m_program.taggedFamilies) {
        bool isConstructorFunction = m_program.at(family.maker).has(LayoutFlags::IsConstructorFunction);
        if (isConstructorFunction ? allocateInConstructorFunction(family) : allocateInClassConstructor(family)) {
            numberAllocated += family.members.size();
            continue;
        }
        for (const TaggedLayout& member : family.members)
            m_program.at(member.layout).flow.allocation = AllocationState::NeverAllocated;
        numberNeverAllocated += family.members.size();
    }

    // The tables by class and by name leave out the layouts that are never allocated.
    if (numberNeverAllocated) {
        m_program.indexClasses();
        m_program.indexNames(m_build.parallelLoops());
    }
    m_build.statistics().count(StatisticID::TaggedLayoutsAllocated, numberAllocated);
    m_build.statistics().count(StatisticID::TaggedLayoutsNeverAllocated, numberNeverAllocated);
}

void allocateTaggedLayouts(Program& program, BuildContext& build)
{
    TaggedLayoutAllocator(program, build).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
