/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTDerivedFunctions.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectsOfRules.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTHeldOperands.h"
#include "AOTIRProgram.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTIntegerRelations.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTProgramPasses.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTRuleSupport.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeTestSite.h"
#include "AOTTypedArrayKinds.h"
#include "AOTValueSubstitution.h"
#include "JSCJSValueInlines.h"
#include <algorithm>
#include <cmath>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// The answer is a prediction, and the creator and the readers of an array take the same one.
ArrayStorage arrayStorageHolding(const Type& elements)
{
    if (elements.hasObjects() || !elements.isJavaScriptValue())
        return ArrayStorage::Contiguous;
    uint16_t primitives = elements.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent);
    if (!primitives)
        return ArrayStorage::Other;
    if (primitives == Primitive::Integer && elements.integers.isSubsetOf(Range::int32()))
        return ArrayStorage::Int32;
    return primitives & ~Primitive::Number ? ArrayStorage::Contiguous : ArrayStorage::Double;
}

Expected<ArrayStorage> arrayStorageDeclaredBy(const Program& program, const ClassPart& array)
{
    if (array.arrayStorage != ArrayStorage::Other)
        return array.arrayStorage;
    const Type& elements = program.at(array.expectedContentsAt(program.builtins(), ContentsSlot::Element).predicted());
    return elements.isAnything() ? ArrayStorage::Other : arrayStorageHolding(elements);
}

Expected<ArrayStorage> arrayStorageDeclaredBy(const Program& program, const Type& array)
{
    const ClassPart* part = array.singleBuiltinClass();
    return part ? arrayStorageDeclaredBy(program, *part) : Expected(ArrayStorage::Other);
}

static ArrayStorage kindAtAllocation(ArrayStorage kind)
{
    return kind == ArrayStorage::Other ? ArrayStorage::Contiguous : kind;
}

ArrayStorage arrayStorageAtAllocation(const Type& elements)
{
    return kindAtAllocation(arrayStorageHolding(elements));
}

ArrayStorage arrayStorageAtAllocation(const Program& program, const Type& array)
{
    return kindAtAllocation(arrayStorageDeclaredBy(program, array).predicted());
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const Type& array, ArrayLiteralParts parts)
{
    // A literal without an element has the declared kind, or a more general one that the arrays of its site have turned into, and none is more general than Contiguous.
    if (!parts.hasElements && !parts.hasHoles)
        return arrayStorageDeclaredBy(program, array).predicted() == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
    // A list of numbers that does not fit its kind is found when code is generated or when the literal runs.
    return arrayStorageAtAllocation(program, array) == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const Value& literal)
{
    RELEASE_ASSERT(literal.opcode == Opcode::NewArray);
    return arrayStorageOfLiteral(program, program.at(literal.type), { !literal.elementsOperands().empty(), !literal.holes().empty(), !!literal.capacityOperand() });
}

ArrayStorage arrayStorageOfData(const Program& program, const DataNode::Array& array)
{
    bool areInt32 = true;
    bool areDoublesOrHoles = true;
    for (const DataNode& element : array.elements) {
        const auto* number = element.tryAs<DataNode::Number>();
        areInt32 &= number && canStoreWithoutConversion(ArrayStorage::Int32, number->value);
        areDoublesOrHoles &= number ? canStoreWithoutConversion(ArrayStorage::Double, number->value) : element.is<DataNode::Hole>();
    }
    // The readers of an array that is declared to hold numbers test for Double storage, also where every constant is a whole number.
    if (areInt32 && arrayStorageHolding(program.at(array.contents)) != ArrayStorage::Double)
        return ArrayStorage::Int32;
    return areDoublesOrHoles ? ArrayStorage::Double : ArrayStorage::Contiguous;
}

bool isStorageOfArray(const DataNode::Array& array)
{
    return !array.elements.isEmpty() && std::ranges::all_of(array.elements, [](const DataNode& element) {
        switch (element.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
            return true;
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
        case DataNode::Kind::Array:
        case DataNode::Kind::Object:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

std::optional<ArrayStorage> arrayStorageOfNewHoles(const Value& allocation)
{
    if (allocation.opcode == Opcode::NewDoubleArray)
        return ArrayStorage::Double;
    if (allocation.opcode == Opcode::NewArrayLike || allocation.opcode == Opcode::NewArrayWithCapacity)
        return kindAtAllocation(allocation.arrayStorage());
    if (allocation.opcode == Opcode::NewContiguousArray || allocation.opcode == Opcode::ArraySpeciesCreate)
        return ArrayStorage::Contiguous;
    return std::nullopt;
}

bool canStoreWithoutConversion(ArrayStorage kind, const Type& stored)
{
    if (kind == ArrayStorage::Double)
        return stored.isNumber() && !stored.mayBeNaN();
    return kind == ArrayStorage::Contiguous || (kind == ArrayStorage::Int32 && stored.isInt32());
}

bool canStoreWithoutConversion(ArrayStorage kind, double stored)
{
    if (kind == ArrayStorage::Double)
        return !std::isnan(stored);
    return kind == ArrayStorage::Contiguous || (kind == ArrayStorage::Int32 && jsNumber(purifyNaN(stored)).isInt32());
}

bool fitsStorageOfLiteral(ArrayStorage kind, const Type& element)
{
    if (element.isNever())
        return true;
    // A NaN among the numbers is found when the literal runs and takes the slow path.
    return kind == ArrayStorage::Double ? element.isNumber() : canStoreWithoutConversion(kind, element);
}

Type typeOfElementRead(ArrayStorage kind)
{
    if (kind == ArrayStorage::Int32)
        return Type::integer(Range::int32()).withPrimitives(Primitive::Undefined);
    return kind == ArrayStorage::Double ? Type::fromPrimitives(Primitive::Number | Primitive::Undefined) : Type::anything();
}

std::span<const ChainMethod> chainMethods()
{
    static constexpr std::array<ChainMethod, 12> methods { {
        { Builtins::ArrayPrototypeSlice, ChainMethodRole::Range, ChainLinkKind::None, ChainEndKind::Array, false, false },
        { Builtins::ArrayPrototypeMap, ChainMethodRole::Link, ChainLinkKind::Map, ChainEndKind::Array, true, false },
        { Builtins::ArrayPrototypeFilter, ChainMethodRole::Link, ChainLinkKind::Filter, ChainEndKind::Array, true, false },
        { Builtins::ArrayPrototypeFlatMap, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::FlatMap, true, false },
        { Builtins::ArrayPrototypeForEach, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::ForEach, true, false },
        { Builtins::ArrayPrototypeReduce, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Reduce, true, false },
        { Builtins::ArrayPrototypeSome, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Some, true, true },
        { Builtins::ArrayPrototypeEvery, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Every, true, true },
        { Builtins::ArrayPrototypeFind, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Find, true, true },
        { Builtins::ArrayPrototypeFindIndex, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::FindIndex, true, true },
        { Builtins::ArrayPrototypeJoin, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Join, false, false },
        { Builtins::ArrayPrototypeLength, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Length, false, false },
    } };
    return methods;
}

const ChainMethod* findChainMethod(BuiltinID id)
{
    std::span<const ChainMethod> methods = chainMethods();
    auto method = std::ranges::find(methods, id, &ChainMethod::builtin);
    return method == methods.end() ? nullptr : &*method;
}

static std::optional<ArrayMethodCall> libraryMethodCallOf(const Program& program, const Function& function, ValueID id)
{
    const BuiltinTable& table = program.builtins();
    // A version of the method for the callback that it is handed takes the operands of the method.
    FunctionID callee = function.at(id).function();
    while (program.at(callee).role == FunctionRole::CalleeVersion)
        callee = program.at(callee).derivation.source;
    const Function& target = program.at(callee);
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, target.enclosingRuntimeFunction);
    if (!entry || entry->function != callee)
        return std::nullopt;
    const ChainMethod* method = nullptr;
    for (BuiltinID builtin : table.at(target.enclosingRuntimeFunction).implements)
        method = method ? method : findChainMethod(table.canonicalID(builtin));
    if (!method)
        return std::nullopt;

    std::span<const ValueID> operands = function.at(id).argumentsOperands();
    RELEASE_ASSERT(operands.size() == target.numberOfEntryParameters());
    std::optional<unsigned> thisParameter = target.thisParameterIndex();
    if (!thisParameter || target.has(FunctionFlags::HasRestParameter))
        return std::nullopt;
    Vector<ValueID, 2> arguments(operands.subspan(target.firstParameterIndex(), target.numberOfPositionalParameters()));
    if (std::optional<unsigned> hiddenRest = target.hiddenRestParameterIndex()) {
        const Value& rest = function.at(sameObjectThroughCasts(function, operands[*hiddenRest]));
        if (rest.opcode != Opcode::NewArray || !rest.holes().empty() || rest.capacityOperand())
            return std::nullopt;
        arguments.append(rest.elementsOperands());
    }
    if (std::optional<unsigned> argumentCount = target.argumentCountParameterIndex()) {
        std::optional<int64_t> count = integerConstantThroughCasts(program, function, operands[*argumentCount]);
        if (!count || *count < 0 || static_cast<uint64_t>(*count) > arguments.size())
            return std::nullopt;
        arguments.shrink(static_cast<size_t>(*count));
    }
    return ArrayMethodCall { method, id, operands[*thisParameter], WTF::move(arguments) };
}

static bool isExpectedToBeArray(const Program& program, const Function& function, ValueID id)
{
    Type expected = program.at(function.at(id).expectedOrType().predicted()).withoutPrimitives(Primitive::UndefinedOrNull);
    const ClassPart* part = expected.singleBuiltinClass();
    return part && part->builtinClass == BuiltinClasses::Array;
}

// `list.filter(f)` where nobody has tested yet that the list has the methods of arrays: a list of a class that extends Array may come, or what a method in front has answered.
static std::optional<ArrayMethodCall> methodCallByNameOf(const Program& program, const Function& function, ValueID id)
{
    const Value& call = function.at(id);
    bool isRead = call.opcode == Opcode::PropGet;
    const Value& read = isRead ? call : function.at(sameObjectThroughCasts(function, call.calleeOperand()));
    if (read.opcode != Opcode::PropGet)
        return std::nullopt;
    ValueID receiver = isRead ? read.objectOperand() : call.thisValueOperand();
    if (!receiver || sameObjectThroughCasts(function, read.objectOperand()) != sameObjectThroughCasts(function, receiver) || !isExpectedToBeArray(program, function, receiver))
        return std::nullopt;
    const BuiltinTable& table = program.builtins();
    BuiltinMemberLookup member = table.findMember(Builtins::ArrayPrototype, program.builtinNameOf(read.key()));
    const ChainMethod* method = member.outcome == BuiltinMemberLookup::Outcome::Found ? findChainMethod(table.canonicalID(member.member)) : nullptr;
    // `length` is read, and every other one is called.
    if (!method || isRead != (method->end == ChainEndKind::Length))
        return std::nullopt;
    return ArrayMethodCall { method, id, receiver, isRead ? Vector<ValueID, 2>() : Vector<ValueID, 2>(call.argumentsOperands()) };
}

static ValueID readOfMethodCalledByName(const Function& function, ValueID call)
{
    return function.at(call).opcode == Opcode::Call ? sameObjectThroughCasts(function, function.at(call).calleeOperand()).valueInFrontOfFacts() : ValueID();
}

std::optional<ArrayMethodCall> arrayMethodCallOf(const Program& program, const Function& function, ValueID id, ChainOperandProofs proofs)
{
    const Value& value = function.at(id);
    if (value.opcode == Opcode::GetArrayLength)
        return ArrayMethodCall { findChainMethod(Builtins::ArrayPrototypeLength), id, value.arrayOperand(), { } };
    if ((value.opcode == Opcode::Call || value.opcode == Opcode::PropGet) && proofs == ChainOperandProofs::MayStillCome)
        return methodCallByNameOf(program, function, id);
    if (!arrayMethodCallOpcodes.contains(value.opcode))
        return std::nullopt;
    if (value.opcode == Opcode::CallDirect)
        return libraryMethodCallOf(program, function, id);

    BuiltinCall call = builtinCallOf(program, value);
    bool waitsForTestOfReceiver = call.form == BuiltinCallForm::AnyReceiver && proofs == ChainOperandProofs::MayStillCome;
    if ((call.form != BuiltinCallForm::Whole && !waitsForTestOfReceiver) || !call.receiver)
        return std::nullopt;
    const BuiltinOverload& overload = program.builtins().at(call.overload);
    bool isCallOrRead = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Get;
    const ChainMethod* method = isCallOrRead ? findChainMethod(program.builtins().canonicalID(overload.builtin)) : nullptr;
    if (!method)
        return std::nullopt;
    return ArrayMethodCall { method, id, call.receiver, Vector<ValueID, 2>(call.arguments) };
}

static bool answersOnlyNumbers(const BuiltinTable& table, BuiltinID function)
{
    if (!function)
        return false;
    bool isCalled = false;
    for (const BuiltinOverload& overload : table.overloadsOf(function)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const BuiltinType& result = table.at(overload.result.type);
        if (result.isAnything() || result.primitives != OptionSet<BuiltinPrimitive> { BuiltinPrimitive::Number } || !result.parts.empty())
            return false;
        isCalled = true;
    }
    return isCalled;
}

static bool returnsOnlyShortLists(const Program& program, FunctionID id)
{
    if (!id || !program.at(id).hasBody())
        return false;
    const Function& function = program.at(id);
    if (function.role == FunctionRole::EntryChecks) {
        const auto& record = function.creator.as<CreatorRecord::EntryChecks>();
        return returnsOnlyShortLists(program, record.body) && returnsOnlyShortLists(program, record.fallback);
    }
    auto isShortList = [&](ValueID id) {
        const Value& list = function.at(sameObjectThroughCasts(function, id));
        return list.opcode == Opcode::NewArray && list.holes().empty() && !list.capacityOperand() && list.elementsOperands().size() <= ChainShape::maxLengthOfShortList;
    };
    // `condition ? [x] : []` returns a parameter of the block where the two arms meet.
    auto isShortListOnEveryEdge = [&](BlockID join, ValueID id) {
        size_t position = function.at(join).parameters.find(id);
        if (position == notFound)
            return false;
        bool arrives = false;
        for (const Block& block : function.blocks) {
            for (const Target& target : block.terminator.targets) {
                if (target.block != join)
                    continue;
                if (position >= target.arguments.size() || !isShortList(target.arguments[position]))
                    return false;
                arrives = true;
            }
        }
        return arrives;
    };
    bool returns = false;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Terminator& terminator = function.blocks[index].terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return false;
        if (terminator.kind != TerminatorKind::Return)
            continue;
        if (!isShortList(terminator.valueOperand()) && !isShortListOnEveryEdge(BlockID(index), terminator.valueOperand()))
            return false;
        returns = true;
    }
    return returns;
}

FunctionID findChainTraversal(const Program& program)
{
    std::span<const BuiltinOverload> overloads = program.builtins().overloadsOf(Builtins::CompilerHelpersTraverseArray);
    RELEASE_ASSERT(!overloads.empty());
    const BuiltinImplementation& implementation = overloads.front().code.implementation;
    RELEASE_ASSERT(implementation.kind == BuiltinImplementationKind::Compiled);
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, implementation.function);
    return entry && entry->function && program.at(entry->function).hasBody() ? entry->function : FunctionID();
}

Function buildChainStep(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    static constexpr unsigned endCallbackSlot = 2 * ChainShape::maxNumberOfLinks;
    ChainShape shape = ChainShape::decode(key.parameter);
    const Function& traversal = inputs.program.at(key.target);

    Function step;
    step.name = traversal.name;
    step.module = traversal.module;
    step.position = traversal.position;
    step.text = traversal.text;
    step.kind = traversal.kind;
    step.role = FunctionRole::ChainStep;
    step.flags = traversal.flags & FunctionFlags::IsHiddenInStackTraces;
    step.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    step.results.append(DeclaredType(Proven::byPosition(TypeTable::anything)));
    step.capabilities = initialCapabilitiesOf(step);
    step.creator = CreatorRecord::ChainStep { };

    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, step, inputs.types);
    builder.setOrigin({ traversal.position, 0 });
    BlockID block = builder.addBlock();
    auto addParameter = [&] {
        step.parameters.append(DeclaredType(Proven::byPosition(TypeTable::anything)));
        return builder.addParameter(block, TypeTable::anything);
    };
    ValueID array = addParameter();
    ValueID start = addParameter();
    ValueID end = addParameter();
    std::array<ValueID, endCallbackSlot + 2> slots { };
    for (unsigned slot = 0; slot < 2 * shape.numberOfLinks(); ++slot)
        slots[slot] = addParameter();
    if (endTakesCallback(shape.end))
        slots[endCallbackSlot] = addParameter();
    if (endTakesThisArgument(shape.end))
        slots[endCallbackSlot + 1] = addParameter();
    ValueID operand = endTakesOperand(shape) ? addParameter() : ValueID();

    builder.moveToEndOf(block);
    ValueID undefined = builder.appendUndefined();
    std::ranges::replace(slots, ValueID(), undefined);
    Value operandList = IRBuilder::make(Opcode::NewArray);
    operandList.setContents(TypeTable::anything);
    operandList.setElementsOperands(slots);
    ValueID operands = builder.append(WTF::move(operandList));
    ValueID shapeWord = builder.appendInteger(key.parameter);
    std::array<ValueID, 6> arguments { array, start, end, shapeWord, operands, operand ? operand : undefined };
    ValueID call = appendDirectCall(builder, { .callee = key.target, .arguments = arguments });
    step.edit(call, ValueEdit::Definition).setIsAlwaysInlined(true);
    emitReturnOfCall(builder, call);
    return step;
}

Vector<ArrayChain> findArrayChains(FunctionPassContext& context, const BitVector* callsTakenAlone, BlockID onlyBlock, ChainOperandProofs proofs, const BitVector* callsNotJoinedThroughTests)
{
    const Program& program = context.program;
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BitVector isInChain;
    Vector<ArrayChain> chains;

    auto typeOf = [&](ValueID id) -> const Type& {
        return program.typeOf(function, id);
    };
    auto calleeOf = [&](ValueID callback) {
        CalleeCandidates candidates = calleeCandidates(program, function, callback);
        std::optional<ProvenValue<FunctionID>> only = candidates.only();
        return only ? only->value() : FunctionID();
    };
    auto prependCallback = [&](ArrayChain& chain, const ArrayMethodCall& call, ValueID thisArgument) {
        ValueID callback = call.argumentOrNull(0);
        if (callback)
            chain.callbacks.insert(0, ChainCallback { call.call, callback, thisArgument, calleeOf(callback) });
        return !!callback;
    };
    // The loop of the traversal runs once for each element, however seldom the chain itself is reached. In cold code it pays where each callback goes into the loop.
    auto isWorthFusingIn = [&](const ArrayChain& chain) {
        if (analyses.blockFrequencies().classOf(chain.block).predicted() == BlockFrequency::Rare || context.runsAtMostOnce(chain.block))
            return false;
        if (!isCold(heatClassOf(context, chain.block)).predicted())
            return true;
        // Outside the loop the text is put together by calls, which cost what `join` costs and more.
        if (context.inputs.heat.isCompact(program, context.id).predicted() || chain.shape.end == ChainEndKind::Join)
            return false;
        return !chain.callbacks.isEmpty() && std::ranges::all_of(chain.callbacks, [](const ChainCallback& callback) {
            return !!callback.callee;
        });
    };
    // The traversal knows how long the array of a filter or of a flatMap can get, and gives the array of a map storage for the numbers that the callback returns.
    auto paysAlone = [&](const ArrayMethodCall& call) {
        if (!callsTakenAlone || !callsTakenAlone->get(call.call.index()))
            return false;
        if (call.method->role == ChainMethodRole::End)
            return call.method->end == ChainEndKind::FlatMap;
        switch (call.method->link) {
        case ChainLinkKind::None:
            return false;
        case ChainLinkKind::Filter:
            return true;
        case ChainLinkKind::Map:
            break;
        }
        FunctionID callee = call.arguments.isEmpty() ? FunctionID() : calleeOf(call.arguments[0]);
        if (!callee && !call.arguments.isEmpty())
            return answersOnlyNumbers(program.builtins(), typeOf(call.arguments[0]).singleBuiltin());
        return callee && program.at(callee).results.size() == 1 && arrayStorageAtAllocation(program.at(program.at(callee).results[0].expected().predicted())) != ArrayStorage::Contiguous;
    };
    auto takeEnd = [&](ArrayChain& chain, const ArrayMethodCall& last) {
        switch (last.method->end) {
        case ChainEndKind::Array:
        case ChainEndKind::Length:
            return true;
        case ChainEndKind::ForEach:
        case ChainEndKind::Some:
        case ChainEndKind::Every:
        case ChainEndKind::Find:
        case ChainEndKind::FindIndex:
            return prependCallback(chain, last, last.argumentOrNull(1));
        case ChainEndKind::FlatMap:
            if (!prependCallback(chain, last, last.argumentOrNull(1)))
                return false;
            chain.shape.flattensShortLists = returnsOnlyShortLists(program, chain.callbacks[0].callee);
            return true;
        case ChainEndKind::Reduce:
            if (last.arguments.size() >= 2) {
                chain.shape.hasInitialValue = true;
                chain.endOperand = last.arguments[1];
            }
            return prependCallback(chain, last, { });
        case ChainEndKind::Join:
            if (last.arguments.isEmpty())
                return true;
            if (last.arguments.size() > 1 || !typeOf(last.arguments[0]).isAnyString())
                return false;
            chain.endOperand = last.arguments[0];
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto isUsedOnlyBy = [&](ValueID receiver, const ArrayMethodCall& consumer) {
        CastChain casts(function, receiver);
        for (ValueID argument : consumer.arguments) {
            if (analyses.sameObjectThroughCasts(argument) == casts.last())
                return false;
        }
        auto isPassingCast = [&](ValueID cast) {
            const ClassPart* part = typeOf(cast).singleBuiltinClass();
            return !analyses.mayThrow(cast) || (part && part->builtinClass == BuiltinClasses::Array) || (proofs == ChainOperandProofs::MayStillCome && isExpectedToBeArray(program, function, cast));
        };
        const UseLists& useLists = analyses.useLists();
        ValueID readOfMethod = readOfMethodCalledByName(function, consumer.call);
        ValueID user = consumer.call;
        for (ValueID value : casts) {
            if (value != casts.last() && !isPassingCast(value))
                return false;
            for (const Use& use : useLists.usesOf(value)) {
                // A validating build looks at each array of the chain, and goes without the look where there is no array.
                bool isValidation = use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateType && use.block == useLists.blockOf(value);
                if (use.kind != Use::Kind::Operand || (!isValidation && use.user != user && use.user != consumer.call && use.user != readOfMethod))
                    return false;
            }
            user = value;
        }
        return true;
    };
    // What a method written in JavaScript answers is whatever flow has seen it answer, so the method behind it stands behind tests of that answer. Each block on the way up is the only way into the one below.
    // The traversal asks none of the tests. A list that a method has made of a plain list, with callbacks that do nothing the program can see, is a plain array: it passes a test for that,
    // and where a test also asks what the list holds, the other arm calls the same method by its name.
    auto goesOnWhereListPassesTest = [&](BlockID from, BlockID to, ValueID list) {
        const Terminator& terminator = function.at(from).terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return true;
        if (!terminator.isConditionalBranch() || terminator.taken().block != to || terminator.notTaken().block == to)
            return false;
        // Decided, and nobody has cleaned up yet: the test whether what was read under the name of a method is that method, behind the test of the names.
        if (typeOf(terminator.conditionOperand()).hasOnlyPrimitives(Primitive::True))
            return true;
        const Value& test = function.at(terminator.conditionOperand());
        if ((test.opcode != Opcode::IsPristine && test.opcode != Opcode::TypeTest) || analyses.identityOf(test.operand(0)) != list)
            return false;
        if (test.opcode == Opcode::IsPristine)
            return test.builtinClass() == BuiltinClasses::Array;
        const ClassPart* part = program.at(test.testedType()).singleBuiltinClass();
        return part && part->builtinClass == BuiltinClasses::Array;
    };
    auto appendBlocksUpTo = [&](BlockID upper, BlockID lower, ValueID list, Vector<BlockID, 4>& blocksFromLast) {
        const CFG& cfg = analyses.cfg();
        size_t sizeBefore = blocksFromLast.size();
        for (BlockID block = lower; block != upper;) {
            std::span<const BlockID> predecessors = cfg.predecessors(block);
            if (predecessors.size() != 1 || function.at(predecessors[0]).handler != function.at(lower).handler || !goesOnWhereListPassesTest(predecessors[0], block, list)) {
                blocksFromLast.shrink(sizeBefore);
                return false;
            }
            block = predecessors[0];
            blocksFromLast.append(block);
        }
        return true;
    };
    auto collectLinks = [&](ArrayChain& chain, const ArrayMethodCall& last, auto& linksFromLast) {
        ArrayMethodCall consumer = last;
        BlockID blockOfConsumer = chain.block;
        Vector<BlockID, 4> blocksFromLast;
        auto finish = [&](ValueID source) {
            if (!blocksFromLast.isEmpty()) {
                chain.blocksFromFirstCall.appendRange(blocksFromLast.rbegin(), blocksFromLast.rend());
                chain.blocksFromFirstCall.append(chain.block);
            }
            return source;
        };
        while (true) {
            size_t numberOfBlocksBelow = blocksFromLast.size();
            ValueID receiver = consumer.receiver;
            ValueID producer = analyses.sameObjectThroughCasts(receiver).valueInFrontOfFacts();
            std::optional<ArrayMethodCall> link = arrayMethodCallOf(program, function, producer, proofs);
            if (link && analyses.useLists().blockOf(producer) == blockOfConsumer) {
                // Of a chain that is split by tests nothing goes: whoever else looks at the list still finds it where the methods run as written. checkInterleaving() asks who reads it behind the chain.
                if (blocksFromLast.isEmpty() && !isUsedOnlyBy(receiver, consumer))
                    return finish(receiver);
            } else if (proofs == ChainOperandProofs::Required && !(callsNotJoinedThroughTests && callsNotJoinedThroughTests->get(consumer.call.index()))) {
                producer = analyses.identityOf(receiver);
                link = arrayMethodCallOf(program, function, producer, proofs);
                bool isHandedToConsumer = std::ranges::any_of(consumer.arguments, [&](ValueID argument) {
                    return analyses.identityOf(argument) == producer;
                });
                if (!link || link->method->role == ChainMethodRole::End || isHandedToConsumer || analyses.useLists().locationOf(producer).placement != ValuePlacement::InBlock)
                    return finish(receiver);
                BlockID blockOfProducer = analyses.useLists().blockOf(producer);
                if (blockOfProducer == blockOfConsumer || !analyses.dominators().strictlyDominates(blockOfProducer, blockOfConsumer) || !appendBlocksUpTo(blockOfProducer, blockOfConsumer, producer, blocksFromLast))
                    return finish(receiver);
                blockOfConsumer = blockOfProducer;
            } else
                return finish(receiver);
            // The blocks up to a call that is not taken after all go again.
            auto finishInFrontOf = [&](ValueID source) {
                blocksFromLast.shrink(numberOfBlocksBelow);
                return finish(source);
            };
            switch (link->method->role) {
            case ChainMethodRole::End:
                return finishInFrontOf(receiver);
            case ChainMethodRole::Range: {
                ValueID from = link->argumentOrNull(0);
                ValueID to = link->argumentOrNull(1);
                if ((from && !typeOf(from).isInteger()) || (to && !typeOf(to).isInteger()))
                    return finishInFrontOf(receiver);
                chain.rangeCall = link->call;
                chain.from = from;
                chain.to = to;
                return finish(link->receiver);
            }
            case ChainMethodRole::Link:
                if (linksFromLast.size() == ChainShape::maxNumberOfLinks)
                    return finishInFrontOf(receiver);
                linksFromLast.append(*link);
                consumer = WTF::move(*link);
                break;
            }
        }
    };
    // No copy of the traversal goes into a copy of the traversal. A traversal that is called costs more than the methods that it stands for, and a trace names none of them.
    FunctionID traversal = findChainTraversal(program);
    auto standsInTraversal = [&](ValueID call) {
        for (uint32_t frame = function.at(call).inlineFrame; frame; frame = function.inlineFrames[frame].parent) {
            if (function.inlineFrames[frame].callee == traversal)
                return true;
        }
        return false;
    };

    auto tryChain = [&](BlockID block, const ArrayMethodCall& last) {
        if (standsInTraversal(last.call))
            return;
        ArrayChain chain;
        chain.block = block;
        chain.lastCall = last.call;
        Vector<ArrayMethodCall, ChainShape::maxNumberOfLinks> linksFromLast;
        switch (last.method->role) {
        case ChainMethodRole::Range:
            return;
        case ChainMethodRole::Link:
            chain.shape.end = ChainEndKind::Array;
            linksFromLast.append(last);
            break;
        case ChainMethodRole::End:
            chain.shape.end = last.method->end;
            if (!takeEnd(chain, last))
                return;
            break;
        }
        chain.source = collectLinks(chain, last, linksFromLast);

        size_t numberOfLinks = linksFromLast.size();
        if (numberOfLinks + (chain.rangeCall ? 1 : 0) + (chain.shape.end != ChainEndKind::Array ? 1 : 0) < 2 && !paysAlone(last))
            return;
        for (size_t i = 0; i < numberOfLinks; ++i) {
            const ArrayMethodCall& link = linksFromLast[i];
            if (!prependCallback(chain, link, link.argumentOrNull(1)))
                return;
            chain.shape.links[numberOfLinks - 1 - i] = link.method->link;
        }
        if (!isWorthFusingIn(chain))
            return;

        isInChain.set(chain.lastCall.index());
        if (chain.rangeCall)
            isInChain.set(chain.rangeCall.index());
        for (const ArrayMethodCall& link : linksFromLast)
            isInChain.set(link.call.index());
        chains.append(WTF::move(chain));
    };

    auto searchBlock = [&](BlockID block) {
        const BlockValues& values = function.at(block).values;
        for (size_t index = values.size(); index--;) {
            if (isInChain.get(values[index].index()))
                continue;
            if (std::optional<ArrayMethodCall> call = arrayMethodCallOf(program, function, values[index], proofs))
                tryChain(block, *call);
        }
    };
    if (onlyBlock)
        searchBlock(onlyBlock);
    else {
        // From the end, as in a block: the call behind takes the calls in front of it into its chain, also those in blocks further up.
        const CFG& cfg = analyses.cfg();
        for (BlockID block : cfg.reversePostOrder() | std::views::reverse)
            searchBlock(block);
        AOT::stableSort(chains, { }, [&](const ArrayChain& chain) {
            return cfg.reversePostOrderIndex(chain.block);
        });
    }
    return chains;
}

static constexpr unsigned arrayArgumentIndex = 2;
static constexpr unsigned arrayArgumentIndexOfReducer = 3;
static constexpr AbstractHeapSet arrayHeaps { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
static constexpr uint16_t primitivesJoinedWithoutUserCode = Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::BigInt | Primitive::UndefinedOrNull | Primitive::Absent;

static bool canBeObserved(const Effects& effects)
{
    return effects.writes.containsAny(observableHeaps) || effects.mayRunOtherCode();
}

static bool mayStop(const Effects& effects)
{
    return effects.mayThrow || !effects.alwaysCompletes();
}

// A stack overflow is the same error whichever call hits it, and an omitted call hits none.
static bool mayStopOtherThanStackOverflow(const Effects& effects)
{
    return effects.mayThrowOtherThanStackOverflow() || !effects.alwaysCompletes();
}

// Every call of such a function from one loop needs the same stack space, so either the first call overflows or none does.
static bool entersNoFurtherFrame(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    auto runsNoCode = [](const EffectDescription& description) {
        return description.functions.isEmpty() && description.slowPathFunctions.isEmpty() && !description.callsUnknownCode;
    };
    return function.hasBody() && std::ranges::all_of(function.blocks, [&](const Block& block) {
        return runsNoCode(describeCodeRunBy(program, function, block.terminator)) && std::ranges::all_of(block.values, [&](ValueID value) {
            return runsNoCode(describeCodeRunBy(program, function, function.at(value)));
        });
    });
}

static bool mayAccessSameMemory(const Effects& a, const Effects& b)
{
    LocationMasks orderedWritesOfA = a.writes.restrictedTo(orderedHeaps);
    LocationMasks orderedWritesOfB = b.writes.restrictedTo(orderedHeaps);
    if (orderedWritesOfA.overlaps(b.writes) || orderedWritesOfA.overlaps(b.reads) || orderedWritesOfB.overlaps(a.reads))
        return true;
    return (a.mayRunOtherCode() && !b.reads.restrictedTo(orderedHeaps).isEmpty())
        || (b.mayRunOtherCode() && !a.reads.restrictedTo(orderedHeaps).isEmpty());
}

struct AnswersAboutCallbacksOfChains {
    HashMap<FunctionID, std::unique_ptr<Effects>> effectsOfVersionsThatGiveUp;
};

static const Effects* effectsOfVersionThatGivesUpAskedOnce(FunctionPassContext& context, FunctionID callback)
{
    if (!context.answersAboutCallbacksOfChains)
        context.answersAboutCallbacksOfChains = std::make_shared<AnswersAboutCallbacksOfChains>();
    auto& answers = context.answersAboutCallbacksOfChains->effectsOfVersionsThatGiveUp;
    if (auto known = answers.find(callback); known != answers.end())
        return known->value.get();
    std::optional<Effects> version = effectsOfVersionThatGivesUp(context, callback);
    return answers.add(callback, version ? makeUniqueWithoutFastMallocCheck<Effects>(*version) : nullptr).iterator->value.get();
}

static ValueID operandOfTraversal(FunctionAnalyses& analyses, ValueID value)
{
    return analyses.sameObjectThroughCasts(value).valueInFrontOfFacts();
}

static bool isClosureMadeHere(FunctionPassContext& context, const ChainCallback& callback)
{
    const Value& closure = context.function.at(operandOfTraversal(context.analyses, callback.function));
    return closure.opcode == Opcode::NewClosure && closure.function() == callback.callee;
}

// Each callback that can leave what the program would see to the methods as written. Its version is a closure of what the callback has captured:
// that stands here where the callback is made here, and else the type of the callback says which function it is a closure of, and so where its captures are read.
static uint8_t callbacksThatCanGiveUp(FunctionPassContext& context, const ArrayChain& chain, std::span<Effects> effectsOfVersions)
{
    uint8_t found = 0;
    for (size_t index = 0; index < chain.callbacks.size(); ++index) {
        const ChainCallback& callback = chain.callbacks[index];
        if (!callback.callee)
            continue;
        const Function& callee = context.program.at(callback.callee);
        if (callee.capabilities.contains(FunctionCapabilities::RecordsCallee))
            continue;
        if (!isClosureMadeHere(context, callback) && context.program.typeOf(context.function, operandOfTraversal(context.analyses, callback.function)).singleFunction() != callback.callee)
            continue;
        const Effects asWritten = CalleeSummary(context.analyses.effectContext(), callback.callee)->onCall().onAnyEdge();
        if (!canBeObserved(asWritten) && asWritten.alwaysCompletes())
            continue;
        if (const Effects* version = effectsOfVersionThatGivesUpAskedOnce(context, callback.callee)) {
            effectsOfVersions[index] = *version;
            found |= 1u << index;
        }
    }
    return found;
}

static bool standsInFrontOf(FunctionAnalyses& analyses, ProgramPoint point, ValueID value)
{
    ValueLocation location = analyses.useLists().locationOf(value);
    if (location.block == point.block)
        return location.placement == ValuePlacement::Parameter || (location.placement == ValuePlacement::InBlock && location.index < point.index);
    return location.placement != ValuePlacement::Detached && analyses.dominators().strictlyDominates(location.block, point.block);
}

// Not what makes an object: what fills the object are other values.
static bool onlyReads(FunctionAnalyses& analyses, ValueID value)
{
    Effects effects = analyses.effectsOf(value).onAnyEdge();
    return interpretedOf(analyses.function().at(value).opcode) != Interpretation::Allocation && effects.writes.isEmpty() && !effects.mayRunOtherCode() && !mayStop(effects);
}

// The traversal of a chain that is split by tests stands in front of the first call. What it is handed stands there too, or is made there once more: a constant, a closure of what stands there,
// or what is only read from there, as the variable of a function that is declared elsewhere: where the traversal runs to its end, nothing that the program can see has been written since.
static bool canBeMadeInFrontOf(FunctionAnalyses& analyses, ProgramPoint point, ValueID value)
{
    if (standsInFrontOf(analyses, point, value))
        return true;
    const Value& definition = analyses.function().at(value);
    if (isRematerializable(definition))
        return true;
    if (definition.opcode != Opcode::NewClosure && !onlyReads(analyses, value))
        return false;
    bool canBeMade = true;
    definition.forEachOperand([&](ValueID operand, uint32_t) {
        canBeMade &= standsInFrontOf(analyses, point, operand);
    });
    return canBeMade;
}

static void forEachOperandOfTraversal(const ArrayChain& chain, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    for (ValueID operand : { chain.from, chain.to, chain.endOperand }) {
        if (operand)
            functor(operand);
    }
    for (const ChainCallback& callback : chain.callbacks) {
        functor(callback.function);
        if (callback.thisArgument)
            functor(callback.thisArgument);
    }
}

// The methods stay as they are written, for the handler. Where the traversal has run, none of what stands between the first call and the last has: it may neither be missed there nor be read behind the chain.
static bool canLeaveOutWhatStandsBetweenCalls(FunctionPassContext& context, const ArrayChain& chain, NOESCAPE const ScopedLambda<bool(ValueID)>& isCallOfChain)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const UseLists& useLists = analyses.useLists();
    const CFG& cfg = analyses.cfg();
    ProgramPoint first { chain.blocksFromFirstCall.first(), useLists.locationOf(chain.firstCall()).index };
    uint32_t indexOfLastCall = useLists.locationOf(chain.lastCall).index;

    bool operandsCanBeMade = standsInFrontOf(analyses, first, chain.source);
    forEachOperandOfTraversal(chain, [&](ValueID operand) {
        operandsCanBeMade &= canBeMadeInFrontOf(analyses, first, operandOfTraversal(analyses, operand));
    });
    if (!operandsCanBeMade)
        return false;

    // Whatever control reaches behind the last call without coming by the first call again.
    BitVector isBehindChain(cfg.numNodes());
    Vector<BlockID, 16> worklist;
    auto reach = [&](BlockID block) {
        if (block != first.block && !isBehindChain.quickSet(block.index()))
            worklist.append(block);
    };
    for (BlockID successor : cfg.successors(chain.block))
        reach(successor);
    while (!worklist.isEmpty()) {
        for (BlockID successor : cfg.successors(worklist.takeLast()))
            reach(successor);
    }
    for (BlockID block : chain.blocksFromFirstCall) {
        if (isBehindChain.quickGet(block.index()))
            return false;
    }
    auto isReadBehindChain = [&](ValueID value) {
        return std::ranges::any_of(useLists.usesOf(value), [&](const Use& use) {
            if (use.block != chain.block)
                return isBehindChain.quickGet(use.block.index());
            return use.kind != Use::Kind::Operand || useLists.locationOf(use.user).index > indexOfLastCall;
        });
    };
    // A call of the chain, what it has answered under another type, or a test of that.
    static constexpr OpcodeSet testsOfLists { Opcode::TypeTest, Opcode::IsPristine };
    auto belongsToListOfChain = [&](ValueID value) {
        return isCallOfChain(analyses.identityOf(value)) || (testsOfLists.contains(function.at(value).opcode) && isCallOfChain(analyses.identityOf(function.at(value).operand(0))));
    };
    for (BlockID block : chain.blocksFromFirstCall) {
        if (block != first.block) {
            for (ValueID parameter : function.at(block).parameters) {
                if (isReadBehindChain(parameter))
                    return false;
            }
        }
        const BlockValues& values = function.at(block).values;
        uint32_t begin = block == first.block ? first.index : 0;
        uint32_t end = block == chain.block ? indexOfLastCall : values.size();
        for (uint32_t index = begin; index < end; ++index) {
            if (isReadBehindChain(values[index]))
                return false;
            if (belongsToListOfChain(values[index]))
                continue;
            Effects effects = analyses.effectsOf(values[index]).onAnyEdge();
            if (canBeObserved(effects) || mayStop(effects))
                return false;
        }
    }
    return true;
}

ArrayChainDeclineReason checkInterleaving(FunctionPassContext& context, const ArrayChain& chain, ChainOperandProofs proofs, bool* startsOverAfterThrow, uint8_t* callbacksThatGiveUp, bool* runsLinksOnEveryElement)
{
    const Program& program = context.program;
    FunctionAnalyses& analyses = context.analyses;
    std::optional<ArrayMethodCall> last = arrayMethodCallOf(program, context.function, chain.lastCall, proofs);
    RELEASE_ASSERT(last);

    ProgramPoint point { analyses.useLists().blockOf(chain.firstCall()), analyses.useLists().locationOf(chain.firstCall()).index };
    if (proofs == ChainOperandProofs::Required) {
        PristineQuery query { program, context.function, &analyses };
        PristineRequirement requirement { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
        if (pristineProofOf(query, chain.source, requirement, point) != PristineProof::Proven)
            return ArrayChainDeclineReason::SourceIsNotPristine;
        for (const ChainCallback& callback : chain.callbacks) {
            const Type& type = program.typeOf(context.function, callback.function);
            if (!type.isObject() || !type.isSubsetOf(Type::function(), program))
                return ArrayChainDeclineReason::CallbackIsNotAFunction;
        }
    }
    for (size_t step = chain.rangeCall ? 0 : 1; step < chain.callbacks.size(); ++step) {
        const ChainCallback& callback = chain.callbacks[step];
        if (!callback.callee)
            return ArrayChainDeclineReason::CalleeIsUnknown;
        const Function& callee = program.at(callback.callee);
        if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
            return ArrayChainDeclineReason::CallbackSeesIntermediateArray;

        bool isReducer = chain.shape.end == ChainEndKind::Reduce && callback.call == chain.lastCall;
        unsigned argumentIndex = isReducer ? arrayArgumentIndexOfReducer : arrayArgumentIndex;
        if (argumentIndex >= callee.numberOfPositionalParameters() && !callee.has(FunctionFlags::HasRestParameter))
            continue;
        unsigned parameter = callee.firstParameterIndex() + std::min(argumentIndex, callee.numberOfPositionalParameters());
        if (!context.inputs.effects.of(callback.callee).operand(parameter).uses.isEmpty())
            return ArrayChainDeclineReason::CallbackSeesIntermediateArray;
    }

    EffectContext effectContext { program, context.inputs.effects };
    Vector<Effects, ChainShape::maxNumberOfLinks + 2> effects;
    Vector<bool, ChainShape::maxNumberOfLinks + 2> mayStopAfterFirstCall;
    uint8_t givesUp = 0;
    std::array<Effects, ChainShape::maxNumberOfLinks + 2> effectsOfVersions;
    effectsOfVersions.fill(Effects::none());
    RELEASE_ASSERT(chain.callbacks.size() <= effectsOfVersions.size());
    bool differsOnlyInWhatIsThrown = false;
    bool skipsCallThatShows = false;
    auto judge = [&]() -> ArrayChainDeclineReason {
        effects.shrink(0);
        mayStopAfterFirstCall.shrink(0);
        for (size_t index = 0; index < chain.callbacks.size(); ++index) {
            const ChainCallback& callback = chain.callbacks[index];
            FunctionID callee = callback.callee;
            if (givesUp & 1u << index)
                effects.append(effectsOfVersions[index]);
            else {
                CalleeList callees = callee ? CalleeList::none() : CalleeList::unknown();
                if (callee)
                    callees.add(callee);
                effects.append(effectsOfCallees(effectContext, callees, BuiltinCallMode::Call).onAnyEdge());
            }
            mayStopAfterFirstCall.append(mayStopOtherThanStackOverflow(effects.last()) || (mayStop(effects.last()) && !(callee && entersNoFurtherFrame(program, callee))));
        }
        if (chain.shape.end == ChainEndKind::Join) {
            const ClassPart* part = program.typeOf(context.function, last->receiver).findClassPart(BuiltinClasses::Array);
            std::optional<ProvenValue<TypeID>> elements = part ? part->provenContentsAt(program.builtins(), ContentsSlot::Element) : std::nullopt;
            bool runsNoUserCode = elements && program.at(*elements).hasOnlyPrimitives(primitivesJoinedWithoutUserCode);
            effects.append(runsNoUserCode ? Effects::none() : context.inputs.effects.unknownCode());
        }
        if (chain.shape.end == ChainEndKind::FlatMap) {
            FunctionID mapper = chain.callbacks.last().callee;
            bool hasProvenResult = mapper && program.at(mapper).results.size() == 1 && !program.at(mapper).result().isExpected();
            const Type* parts = hasProvenResult ? &program.at(*program.at(mapper).result().established()) : nullptr;
            if (!parts || (parts->hasObjects() && !pristineAspectsProvenBy(*parts, BuiltinClasses::Array).contains(PristineAspects::Storage)))
                effects.last().merge(context.inputs.effects.unknownCode());
        }

        bool mayRunAgain = std::ranges::all_of(effects, [](const Effects& step) {
            return !canBeObserved(step) && step.alwaysCompletes();
        });
        differsOnlyInWhatIsThrown = false;
        // What a call has answered passes the tests behind it where no callback has done what the program can see: it is a list that the method has made of a plain list.
        if (chain.isSplitByTests()) {
            if (!mayRunAgain)
                return ArrayChainDeclineReason::CallbacksInterfere;
            differsOnlyInWhatIsThrown = true;
        }
        if (givesUp) {
            if (!mayRunAgain)
                return ArrayChainDeclineReason::CallbacksInterfere;
            differsOnlyInWhatIsThrown = true;
        }
        for (size_t earlier = 0; earlier < effects.size(); ++earlier) {
            for (size_t later = earlier + 1; later < effects.size(); ++later) {
                const Effects& a = effects[earlier];
                const Effects& b = effects[later];
                bool earlierStopsAmongLaterCalls = earlier < mayStopAfterFirstCall.size() ? mayStopAfterFirstCall[earlier] : mayStop(a);
                bool stopsDiffer = (mayStopOtherThanStackOverflow(a) && mayStop(b)) || (earlierStopsAmongLaterCalls && mayStopOtherThanStackOverflow(b));
                if (stopsDiffer && mayRunAgain) {
                    differsOnlyInWhatIsThrown = true;
                    stopsDiffer = false;
                }
                if ((mayAccessSameMemory(a, b) && !context.build.isSabotaged(Sabotage::CallbacksOfOneChainShareNoMemory)) || (earlierStopsAmongLaterCalls && canBeObserved(b)) || stopsDiffer)
                    return ArrayChainDeclineReason::CallbacksInterfere;
                if (mayStop(b) && canBeObserved(a))
                    return ArrayChainDeclineReason::SkippedCallIsObservable;
            }
        }
        // Where one method runs after the other, a link has run on every element before the end looks at the first. What it does on an element behind the answer shows: what the program can see, what is thrown,
        // and the test that such an element fails, behind which may be either. So the traversal goes on behind the answer, with the links alone.
        bool linkGivesUp = givesUp & ((1u << chain.shape.numberOfLinks()) - 1);
        skipsCallThatShows = last->method->stopsEarly && (linkGivesUp || std::ranges::any_of(effects.span().first(chain.shape.numberOfLinks()), [](const Effects& step) {
            return canBeObserved(step) || mayStopOtherThanStackOverflow(step);
        }));
        return ArrayChainDeclineReason::None;
    };
    auto judgeWithSource = [&]() -> ArrayChainDeclineReason {
        ArrayChainDeclineReason verdict = judge();
        if (verdict != ArrayChainDeclineReason::None)
            return verdict;
        bool mayWriteArrays = std::ranges::any_of(effects.span().subspan(std::min<size_t>(chain.rangeCall ? 0 : 1, effects.size())), [](const Effects& step) {
            return step.writes.containsAny(arrayHeaps);
        });
        if (!mayWriteArrays)
            return ArrayChainDeclineReason::None;
        // The caller of this function holds an entry parameter, so other code reaches it whatever happens to it here.
        const EscapeAnalysis& escape = analyses.escape();
        std::optional<ObjectID> source = escape.singleObject(chain.source);
        if (!source || escape.at(*source).kind != ObjectKind::Allocation || escape.at(*source).state != EscapeState::DoesNotEscape || !escape.containersOf(*source).empty())
            return ArrayChainDeclineReason::CallbackWritesSource;
        return ArrayChainDeclineReason::None;
    };
    ArrayChainDeclineReason verdict = judgeWithSource();
    if (verdict == ArrayChainDeclineReason::CallbacksInterfere || verdict == ArrayChainDeclineReason::SkippedCallIsObservable || verdict == ArrayChainDeclineReason::CallbackWritesSource) {
        givesUp = callbacksThatCanGiveUp(context, chain, effectsOfVersions);
        if (givesUp) {
            ArrayChainDeclineReason behindChecks = judgeWithSource();
            if (behindChecks == ArrayChainDeclineReason::None)
                verdict = behindChecks;
        }
    }
    if (verdict != ArrayChainDeclineReason::None)
        return verdict;

    Effects moved = listReadEffects;
    for (const Effects& step : effects)
        moved.merge(step);
    auto isCallOfChain = [&](ValueID id) {
        return id == chain.rangeCall || id == chain.lastCall || chain.callbacks.containsIf([&](const ChainCallback& callback) {
            return callback.call == id;
        });
    };
    // Behind its tests the method is a constant.
    auto isReadOfMethodOfChain = [&](ValueID id) {
        return proofs == ChainOperandProofs::MayStillCome && (id == readOfMethodCalledByName(context.function, chain.lastCall) || chain.callbacks.containsIf([&](const ChainCallback& callback) {
            return id == readOfMethodCalledByName(context.function, callback.call);
        }));
    };
    if (chain.isSplitByTests()) {
        if (!canLeaveOutWhatStandsBetweenCalls(context, chain, isCallOfChain))
            return ArrayChainDeclineReason::ValueBetweenLinksInterferes;
    } else {
        const BlockValues& values = context.function.at(chain.block).values;
        uint32_t end = analyses.useLists().locationOf(chain.lastCall).index;
        for (uint32_t index = point.index + 1; index < end; ++index) {
            if (!isCallOfChain(analyses.sameObjectThroughCasts(values[index]).valueInFrontOfFacts()) && !isReadOfMethodOfChain(values[index]) && analyses.effectsOf(values[index]).onAnyEdge().interferesWith(moved))
                return ArrayChainDeclineReason::ValueBetweenLinksInterferes;
        }
    }
    if (startsOverAfterThrow)
        *startsOverAfterThrow = differsOnlyInWhatIsThrown;
    if (callbacksThatGiveUp)
        *callbacksThatGiveUp = givesUp;
    if (runsLinksOnEveryElement)
        *runsLinksOnEveryElement = skipsCallThatShows;
    return ArrayChainDeclineReason::None;
}

static void rewriteArrayChain(FunctionPassContext& context, IRBuilder& builder, const ArrayChain& chain)
{
    Function& function = context.function;
    const ChainShape& shape = chain.shape;
    FunctionID step = context.derivedFunction(derivedFunctionKey(FunctionRole::ChainStep, findChainTraversal(context.program), shape.encode()));

    const UseLists& useLists = context.analyses.useLists();
    // All that goes with the chain but its last call.
    BitVector erasedIndexes;
    auto indexInBlock = [&](ValueID value) {
        ValueLocation location = useLists.locationOf(value);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && location.block == chain.block);
        return location.index;
    };
    auto eraseUnsharedHiddenRest = [&](ValueID call) {
        const Value& value = function.at(call);
        std::optional<unsigned> hiddenRest = value.opcode == Opcode::CallDirect ? context.program.at(value.function()).hiddenRestParameterIndex() : std::nullopt;
        if (!hiddenRest)
            return;
        Vector<uint32_t, 2> unsharedIndexes;
        ValueID user = call;
        for (ValueID id : CastChain(function, value.argumentsOperands()[*hiddenRest])) {
            std::optional<Use> use = useLists.singleUse(id);
            ValueLocation location = useLists.locationOf(id);
            bool isUnshared = use && use->kind == Use::Kind::Operand && use->user == user;
            if (!isUnshared || location.placement != ValuePlacement::InBlock || location.block != chain.block)
                return;
            unsharedIndexes.append(location.index);
            user = id;
        }
        for (uint32_t index : unsharedIndexes)
            erasedIndexes.set(index);
    };

    Vector<ValueID, ChainShape::maxNumberOfLinks + 2> calls;
    if (chain.rangeCall)
        calls.append(chain.rangeCall);
    for (const ChainCallback& callback : chain.callbacks) {
        if (callback.call != chain.lastCall)
            calls.append(callback.call);
    }
    calls.append(chain.lastCall);
    ValueID producer;
    for (ValueID call : calls) {
        if (chain.isSplitByTests())
            break;
        if (producer) {
            std::optional<ArrayMethodCall> consumer = arrayMethodCallOf(context.program, function, call);
            RELEASE_ASSERT(consumer);
            CastChain receiver(function, consumer->receiver);
            RELEASE_ASSERT(receiver.last() == producer);
            for (ValueID value : receiver) {
                erasedIndexes.set(indexInBlock(value));
                for (const Use& use : useLists.usesOf(value)) {
                    if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateType)
                        erasedIndexes.set(indexInBlock(use.user));
                }
            }
        }
        eraseUnsharedHiddenRest(call);
        producer = call;
    }
    ProgramPoint point { chain.block, indexInBlock(chain.lastCall) };

    // The traversal stands for calls of built-ins that are written in JavaScript: a trace that is taken in a callback names the built-in between the callback and the caller, at the place of its own call, as it does where the text of the built-in takes the place of the call.
    // `flatMap` ends in a tail call of the helper that calls the callback, and a trace names neither.
    Vector<InlineFrame>& frames = function.inlineFrames;
    auto appendFrame = [&](const InlineFrame& frame) {
        if (frames.isEmpty())
            frames.append(InlineFrame { });
        frames.append(frame);
        return static_cast<uint32_t>(frames.size() - 1);
    };
    // A method that is not written in JavaScript has the frame of a built-in whose steps stand in its caller.
    auto calledBuiltinOf = [&](const Value& call) -> BuiltinID {
        if (call.opcode != Opcode::CallBuiltin || (call.inlineFrame && frames[call.inlineFrame].calledBuiltin))
            return { };
        const BuiltinTable& builtins = context.program.builtins();
        const BuiltinOverload& overload = builtins.at(call.overload());
        return overload.mode == BuiltinCallMode::Call ? builtins.canonicalID(overload.builtin) : BuiltinID();
    };
    auto appendFrameOfBuiltin = [&](ValueID id) -> std::optional<Origin> {
        const Value& call = function.at(id);
        if (BuiltinID builtin = calledBuiltinOf(call))
            return function.enterFrameOfBuiltin(context.id, call.origin(), builtin);
        if (call.opcode != Opcode::CallDirect)
            return std::nullopt;
        const Function& builtin = context.program.at(call.function());
        return Origin { builtin.position, appendFrame({ .callee = call.function(), .callPosition = call.position, .parent = call.inlineFrame, .isTailCall = !!call.isTailCall, .module = builtin.module }) };
    };
    Origin origin = function.at(chain.lastCall).origin();
    if (shape.end != ChainEndKind::FlatMap)
        origin = appendFrameOfBuiltin(chain.lastCall).value_or(origin);
    std::span<const ChainCallback> links = chain.callbacks.span().first(shape.numberOfLinks());
    bool linksHavePlacesOfTheirOwn = std::ranges::any_of(links, [&](const ChainCallback& link) {
        return link.call != chain.lastCall;
    });
    bool linksHaveFrames = std::ranges::all_of(links, [&](const ChainCallback& link) {
        const Value& call = function.at(link.call);
        return call.opcode == Opcode::CallDirect || calledBuiltinOf(call);
    });
    if (linksHavePlacesOfTheirOwn && linksHaveFrames) {
        // The frames of the links stand in a row in front of the frame of the site, which is where the copy of the traversal looks for them.
        for (const ChainCallback& link : links) {
            std::optional<Origin> frameOfLink = appendFrameOfBuiltin(link.call);
            RELEASE_ASSERT(frameOfLink);
        }
        const Function& traversal = context.program.at(findChainTraversal(context.program));
        origin = Origin { traversal.position, appendFrame({ .callee = step, .callPosition = origin.position, .parent = origin.inlineFrame, .isTailCall = origin.isTailCall, .module = traversal.module }) };
    }
    TypeID resultType = function.at(chain.lastCall).type;
    size_t numberOfValuesBeforeInsertions = function.at(chain.block).values.size();

    // The methods stay where they are, with the tests between them, and the handler goes on at the first of them. The traversal stands in front of them and goes on behind the last.
    std::optional<IRBuilder::Expansion> behindLastCall;
    BitVector isMadeAnewForTraversal;
    if (chain.isSplitByTests()) {
        ProgramPoint first { chain.blocksFromFirstCall.first(), useLists.locationOf(chain.firstCall()).index };
        forEachOperandOfTraversal(chain, [&](ValueID operand) {
            ValueID definition = operandOfTraversal(context.analyses, operand);
            if (!standsInFrontOf(context.analyses, first, definition))
                isMadeAnewForTraversal.set(definition.index());
        });
        Expected<BlockFrequency> frequency = function.at(chain.block).frequency;
        Value lastCall = function.at(chain.lastCall);
        behindLastCall = builder.beginExpansion(point);
        builder.setOrigin(lastCall.origin());
        ValueID written = builder.append(WTF::move(lastCall));
        if (behindLastCall->results.isEmpty())
            builder.endExpansion(*behindLastCall, std::span<const ValueID>());
        else
            builder.endExpansion(*behindLastCall, written);

        std::optional<Target> outerHandler = builder.handler();
        BlockID startsOver = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
        builder.setHandler(Target { startsOver, { }, { } });
        BlockID traversal = builder.addBlock(frequency);
        builder.setHandler(WTF::move(outerHandler));
        builder.moveToEndOf(traversal);
        builder.setOrigin(function.at(chain.firstCall()).origin());
        BlockID asWritten = builder.splitBlock(first);
        builder.redirectEdge(Edge { first.block, 0 }, traversal, { });
        // Only the handler comes here now.
        function.edit(asWritten, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        for (BlockID block : chain.blocksFromFirstCall.span().subspan(1))
            function.edit(block, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        builder.moveToEndOf(startsOver);
        builder.append(Opcode::CatchValue, { });
        builder.emitJump(asWritten);
        builder.moveToEndOf(traversal);
    }

    builder.setOrigin(origin);
    // A ScopedLambda hands its functor each argument as an lvalue.
    auto insertBeforeLastCall = [&](auto&& value) -> ValueID {
        if (behindLastCall)
            return builder.append(WTF::move(value));
        return builder.insertBefore(point, WTF::move(value));
    };
    InsertValue insert(insertBeforeLastCall);
    ValueInsertion insertion { context.analyses.context(), function, context.types, origin, insert };

    unsigned numberOfLinks = shape.numberOfLinks();
    RELEASE_ASSERT(chain.callbacks.size() == numberOfLinks + endTakesCallback(shape.end));
    RELEASE_ASSERT(chain.endOperand || !endTakesOperand(shape) || shape.end == ChainEndKind::Join);
    ValueID undefined;
    auto valueOrUndefined = [&](ValueID value) {
        if (!value && !undefined)
            undefined = insert(IRBuilder::make(Opcode::Undefined));
        return value ? value : undefined;
    };
    auto forTraversal = [&](ValueID value) {
        if (!value || !behindLastCall)
            return value;
        ValueID definition = operandOfTraversal(context.analyses, value);
        if (!isMadeAnewForTraversal.get(definition.index()))
            return definition;
        // It is the program's own, made a few lines earlier: no part of the traversal, whose frame is that of a body that was put in here.
        builder.setOrigin(function.at(definition).origin());
        ValueID madeAnew = insert(Value(function.at(definition)));
        builder.setOrigin(origin);
        return madeAnew;
    };
    Vector<ValueID, 3 + 2 * (ChainShape::maxNumberOfLinks + 1) + 1> arguments;
    arguments.append(chain.source);
    arguments.append(chain.from ? forTraversal(chain.from) : insert(makeInteger(0)));
    arguments.append(valueOrUndefined(forTraversal(chain.to)));
    auto closureThatGivesUp = [&](const ChainCallback& callback) {
        Value closure = isClosureMadeHere(context, callback) ? function.at(operandOfTraversal(context.analyses, callback.function)) : IRBuilder::make(Opcode::NewClosure);
        if (!isClosureMadeHere(context, callback)) {
            ValueID written = behindLastCall ? forTraversal(callback.function) : operandOfTraversal(context.analyses, callback.function);
            Vector<ValueID, 8> captures;
            for (uint32_t index = 0; index < context.program.at(callback.callee).captures.size(); ++index) {
                Value capture = IRBuilder::make(Opcode::GetCapture, { written });
                capture.setFunction(callback.callee);
                capture.setIndex(index);
                captures.append(insert(WTF::move(capture)));
            }
            closure.setCapturesOperands(captures.span());
        }
        FunctionID checks = context.derivedFunction(derivedFunctionKey(FunctionRole::ChecksThatGiveUp, callback.callee));
        closure.setFunction(checks);
        closure.type = context.types.intern(Type::forFunction(checks));
        return insert(WTF::move(closure));
    };
    for (unsigned index = 0; index < chain.callbacks.size(); ++index) {
        arguments.append(chain.callbacksThatGiveUp & 1u << index ? closureThatGivesUp(chain.callbacks[index]) : forTraversal(chain.callbacks[index].function));
        if (index < numberOfLinks || endTakesThisArgument(shape.end))
            arguments.append(valueOrUndefined(forTraversal(chain.callbacks[index].thisArgument)));
    }
    if (endTakesOperand(shape) && chain.endOperand)
        arguments.append(forTraversal(chain.endOperand));
    else if (endTakesOperand(shape)) {
        Value separator = IRBuilder::make(Opcode::String);
        separator.setString(context.strings.intern(StringView(","_s)));
        arguments.append(insert(WTF::move(separator)));
    }

    if (behindLastCall) {
        ValueID fused = appendDirectCall(builder, { .callee = step, .arguments = arguments.span(), .isAlwaysInlined = true });
        if (behindLastCall->results.isEmpty())
            builder.endExpansion(*behindLastCall, std::span<const ValueID>());
        else
            builder.endExpansion(*behindLastCall, convertTo(builder, fused, DeclaredType(Proven::byPosition(resultType))));
        return;
    }

    if (chain.startsOverAfterThrow) {
        builder.applyInsertions();
        const BlockValues& valuesOfChain = function.at(chain.block).values;
        Vector<std::pair<ValueID, Value>, 8> written;
        for (size_t index : erasedIndexes)
            written.append({ valuesOfChain[index], function.at(valuesOfChain[index]) });
        written.append({ chain.lastCall, function.at(chain.lastCall) });
        Expected<BlockFrequency> frequency = function.at(chain.block).frequency;
        builder.eraseValues(chain.block, erasedIndexes);

        IRBuilder::Expansion expansion = builder.beginExpansion({ chain.block, static_cast<uint32_t>(function.at(chain.block).values.find(chain.lastCall)) });
        auto finish = [&](ValueID result) {
            if (expansion.results.isEmpty())
                builder.endExpansion(expansion, std::span<const ValueID>());
            else
                builder.endExpansion(expansion, convertTo(builder, result, DeclaredType(Proven::byPosition(resultType))));
        };
        std::optional<Target> outerHandler = builder.handler();
        BlockID asWritten = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
        builder.setHandler(Target { asWritten, { }, { } });
        BlockID traversal = builder.addBlock(frequency);
        builder.setHandler(WTF::move(outerHandler));
        builder.emitJump(traversal);

        builder.moveToEndOf(traversal);
        builder.setOrigin(origin);
        ValueID fused = appendDirectCall(builder, { .callee = step, .arguments = arguments.span(), .isAlwaysInlined = true });
        finish(fused);

        builder.moveToEndOf(asWritten);
        builder.setOrigin(written.last().second.origin());
        builder.append(Opcode::CatchValue, { });
        Vector<ValueID, 8> copies;
        for (auto& [original, value] : written) {
            value.forEachOperand([&](ValueID& operand, uint32_t) {
                for (size_t index = 0; index < copies.size(); ++index) {
                    if (written[index].first == operand)
                        operand = copies[index];
                }
            });
            builder.setOrigin(value.origin());
            copies.append(builder.append(WTF::move(value)));
        }
        finish(copies.last());
        return;
    }

    Value call = makeDirectCall(insertion, { .callee = step, .arguments = arguments.span() });
    // A step that is called calls each callback by value, and costs more than the methods that it stands for.
    call.setIsAlwaysInlined(true);
    ValueID fused = insert(WTF::move(call));
    ValueID converted = convertTo(insertion, fused, DeclaredType(Proven::byPosition(resultType)));
    builder.applyInsertions();

    ValueSubstitution substitution(function);
    substitution.replace(chain.lastCall, converted);
    substitution.apply(function, &context.analyses);

    const BlockValues& values = function.at(chain.block).values;
    size_t indexOfLastCall = point.index + (values.size() - numberOfValuesBeforeInsertions);
    RELEASE_ASSERT(values[indexOfLastCall] == chain.lastCall);
    erasedIndexes.set(indexOfLastCall);
    builder.eraseValues(chain.block, erasedIndexes);
}

static bool endsGuardInUse(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        GuardAvailability::Cursor cursor(analyses.guards(), block);
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (reliesOf(value.opcode).isEmpty())
                continue;
            cursor.advanceTo(index);
            for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                ValueID guard = value.operand(operandIndex);
                if (guard && value.operandKind(operandIndex) == OperandKind::Guard && cursor.stateOf(guard, value) == GuardState::Unavailable)
                    return true;
            }
        }
    }
    return false;
}

// The proof of a test that nobody has relied on is no value, and whoever asks what is proven at a place asks about values.
static bool takeProofsOfPristineTests(FunctionPassContext& context, ValueID array)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    ValueID subject = analyses.identityOf(array);
    Vector<Edge, 2> passingEdges;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch() || !terminator.taken().proofs.isEmpty())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode == Opcode::IsPristine && analyses.identityOf(test.operand(0)) == subject)
            passingEdges.append(Edge { block, 0 });
    }
    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    for (Edge edge : passingEdges)
        ensureEdgeResult(builder, analyses, edge, 0);
    return !passingEdges.isEmpty();
}

// Taken only where the chain goes in with them: a chain that stays as it is would take them again in every round, behind whoever drops what nobody uses.
static bool wouldFuseWithProofsOfPristineTests(FunctionPassContext& context, const ArrayChain& chain)
{
    if (checkInterleaving(context, chain, ChainOperandProofs::MayStillCome) != ArrayChainDeclineReason::None)
        return false;
    Function trial = context.function;
    FunctionAnalyses analyses(context.analyses.context(), trial);
    FunctionPassContext trialContext { context.program, context.inputs, context.id, trial, analyses, context.types, context.strings, context.build };
    trialContext.answersAboutCallbacksOfChains = context.answersAboutCallbacksOfChains;
    if (!takeProofsOfPristineTests(trialContext, chain.source))
        return false;
    return checkInterleaving(trialContext, chain) == ArrayChainDeclineReason::None;
}

// `list.filter(f).map(g).reduce(h, 0)` where tests have just shown what `reduce` is: it is still called by value, and ResolveCalls makes a call of the method of it in the next round.
// A chain that ends in front of such a call is longer by that method then.
static BitVector findListsWhoseMethodIsNotResolvedYet(FunctionPassContext& context)
{
    const Function& function = context.function;
    const BuiltinTable& table = context.program.builtins();
    BitVector lists;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& call = function.at(id);
            if (call.opcode != Opcode::Call || !call.thisValueOperand())
                continue;
            const Value& callee = function.at(sameObjectThroughCasts(function, call.calleeOperand()));
            if (callee.opcode == Opcode::Builtin && findChainMethod(table.canonicalID(callee.builtin())))
                lists.set(context.analyses.identityOf(call.thisValueOperand()).index());
        }
    }
    return lists;
}

PassOutcome fuseArrayMethodsTakenAlone(FunctionPassContext& context, const BitVector& calls)
{
    if (context.function.role == FunctionRole::ChainStep || context.function.enclosingRuntimeFunction)
        return PassOutcome::Unchanged;
    FunctionID traversal = findChainTraversal(context.program);
    if (!traversal)
        return PassOutcome::Unchanged;

    Vector<ArrayChain> accepted;
    PassOutcome outcomeWithoutChain = PassOutcome::Unchanged;
    // The calls of a chain through tests that stays as it is are looked at once more, each with what stands in its own block: a shorter chain, or a method that pays alone.
    BitVector callsNotJoinedThroughTests;
    BitVector isDecided;
    std::optional<BitVector> listsWhoseMethodIsNotResolvedYet;
    for (bool searchesAgain = true; std::exchange(searchesAgain, false);) {
        for (ArrayChain& chain : findArrayChains(context, &calls, { }, ChainOperandProofs::Required, &callsNotJoinedThroughTests)) {
            if (isDecided.get(chain.lastCall.index()))
                continue;
            // Not where the text of a method is about to take the place of its call: no round comes in which the chain could still be taken.
            if (calls.isEmpty() && chain.shape.end == ChainEndKind::Array) {
                if (!listsWhoseMethodIsNotResolvedYet)
                    listsWhoseMethodIsNotResolvedYet = findListsWhoseMethodIsNotResolvedYet(context);
                if (listsWhoseMethodIsNotResolvedYet->get(chain.lastCall.index())) {
                    isDecided.set(chain.lastCall.index());
                    continue;
                }
            }
            ArrayChainDeclineReason obstacle = checkInterleaving(context, chain, ChainOperandProofs::Required, &chain.startsOverAfterThrow, &chain.callbacksThatGiveUp, &chain.shape.runsLinksOnEveryElement);
            bool takesProofs = obstacle == ArrayChainDeclineReason::SourceIsNotPristine && (calls.get(chain.lastCall.index()) || wouldFuseWithProofsOfPristineTests(context, chain));
            if (takesProofs && takeProofsOfPristineTests(context, chain.source)) {
                outcomeWithoutChain = PassOutcome::Changed;
                obstacle = checkInterleaving(context, chain, ChainOperandProofs::Required, &chain.startsOverAfterThrow, &chain.callbacksThatGiveUp, &chain.shape.runsLinksOnEveryElement);
            }
            if (obstacle != ArrayChainDeclineReason::None && chain.isSplitByTests()) {
                callsNotJoinedThroughTests.set(chain.lastCall.index());
                for (const ChainCallback& callback : chain.callbacks)
                    callsNotJoinedThroughTests.set(callback.call.index());
                searchesAgain = true;
                continue;
            }
            isDecided.set(chain.lastCall.index());
            if (obstacle == ArrayChainDeclineReason::None)
                accepted.append(WTF::move(chain));
            else
                context.addDecline(Wire::DeclineKind::ArrayChainNotFused, chain.lastCall, obstacle);
        }
    }
    if (accepted.isEmpty())
        return outcomeWithoutChain;
    auto declineAll = [&] {
        for (const ArrayChain& chain : accepted)
            context.addDecline(Wire::DeclineKind::ArrayChainNotFused, chain.lastCall, ArrayChainDeclineReason::Unknown);
        return outcomeWithoutChain;
    };
    // The traversal is library code.
    const EffectContext& effects = context.analyses.effectContext();
    if (!CalleeSummary(effects, traversal)->onCall().isSubsetOf(CalleeSummary(effects, context.id)->onCall()))
        return declineAll();

    // An ArrayChain names its operands by id, so a chain that reads the result of another one is rewritten before that result is replaced.
    AOT::sort(accepted, std::ranges::greater { }, [&, &cfg = context.analyses.cfg(), &useLists = context.analyses.useLists()](const ArrayChain& chain) {
        return std::pair { cfg.reversePostOrderIndex(chain.block), useLists.locationOf(chain.lastCall).index };
    });
    // A test in front of a chain may hold behind it only because the calls of the chain say how little they write. Whether it still holds shows on the fused body.
    Function fused = context.function;
    {
        FunctionAnalyses analyses(context.analyses.context(), fused);
        FunctionPassContext fusedContext { context.program, context.inputs, context.id, fused, analyses, context.types, context.strings, context.build };
        IRBuilder builder(analyses.context(), fused, context.types, &analyses);
        for (const ArrayChain& chain : accepted) {
            const Terminator& terminator = fused.at(chain.block).terminator;
            if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == chain.lastCall) {
                builder.demoteReturnCall(chain.block);
                context.build.statistics().count(StatisticID::ReturnCallsDemoted);
            }
            rewriteArrayChain(fusedContext, builder, chain);
        }
    }
    FunctionAnalyses analysesOfFused(context.analyses.context(), fused);
    if (endsGuardInUse(analysesOfFused))
        return declineAll();

    context.function = WTF::move(fused);
    context.build.statistics().count(StatisticID::ArrayChainsFused, accepted.size());
    // A chain in one block that never starts over is rewritten inside that block: the fused body has the blocks, the targets and the handlers of the body that it replaces.
    // InlineCalls asks more than once in one run, so what an earlier answer kept does not stand for this one.
    bool addsBlocks = std::ranges::any_of(accepted, [](const ArrayChain& chain) {
        return chain.startsOverAfterThrow || chain.isSplitByTests();
    });
    context.keptAnalyses = addsBlocks ? AnalysisSet { } : controlFlowAnalyses;
    return PassOutcome::Changed;
}

PassOutcome fuseArrayChains(FunctionPassContext& context)
{
    return fuseArrayMethodsTakenAlone(context, { });
}

namespace {

struct ChainPoint {
    bool isEarlierThan(ChainPoint other) const { return chainIndex != other.chainIndex ? chainIndex > other.chainIndex : index < other.index; }

    uint32_t chainIndex { 0 };
    uint32_t index { 0 };
};

struct ElementStore {
    ProgramPoint point;
    uint32_t arrayIndex;
    ValueID element;
};

struct LiteralPlan {
    ValueID literal;
    uint32_t length;
    ProgramPoint allocation;
    Vector<ElementStore> stores { };
    ValueID array { };
    ValueID lengthOfArray { };
    // The elements are appended in their order to an empty array of the literal's type. A store without an element appends a hole.
    bool appendsElements { false };
    // Nothing but the literal reads them.
    Vector<ValueID> ownElements { };
};

struct DominatorChain {
    bool isUsable(uint32_t chainIndex) const { return chainIndex < nearestUsablePoints.size() && nearestUsablePoints[chainIndex].chainIndex == chainIndex; }
    ProgramPoint programPointOf(ChainPoint point) const { return { blocks[point.chainIndex], point.index }; }
    ChainPoint moveToUsableBlock(ChainPoint point) const { return point.chainIndex >= nearestUsablePoints.size() || isUsable(point.chainIndex) ? point : nearestUsablePoints[point.chainIndex]; }

    // blocks[0] holds the literal and blocks[j + 1] is the immediate dominator of blocks[j], so a greater chain index is earlier.
    Vector<BlockID, 8> blocks;
    Vector<ChainPoint, 8> nearestUsablePoints;
};

} // anonymous namespace

PassOutcome expandLongArrayLiterals(FunctionPassContext& context)
{
    Function& function = context.function;
    // The code generator makes a constant that is no number where it is defined, so the constants of a very long literal are stored one by one too.
    AOT_THRESHOLD(size_t, maxConstantsMadeBeforeAllocation, 4095);
    auto isStoredWhenMade = [&](const Value& literal, ValueID element) {
        bool countsConstants = literal.elementsOperands().size() > maxConstantsMadeBeforeAllocation;
        return (countsConstants || !isRematerializable(function.at(element))) && !context.program.typeOf(function, element).isNumber();
    };
    // Numbers decide the form of the array that holds them, so a very long list of computed numbers is appended to an array that starts in the form of the literal.
    auto isComputed = [&](ValueID element) {
        return !isRematerializable(function.at(element));
    };
    Vector<LiteralPlan> plans;
    for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::NewArray)
                continue;
            uint64_t numberOfComputedElements = std::ranges::count_if(value.elementsOperands(), [&](ValueID element) {
                return isStoredWhenMade(value, element);
            });
            bool storesElements = context.exceeds(LimitID::MaxArrayLiteralBatch, numberOfComputedElements, values[index]);
            bool appendsElements = !storesElements && !value.capacityOperand() && value.literalStorage() == ArrayLiteralStorage::Fresh && value.storage() == AllocationStorage::Heap
                && static_cast<size_t>(std::ranges::count_if(value.elementsOperands(), isComputed)) > maxConstantsMadeBeforeAllocation;
            if (storesElements || appendsElements) {
                plans.append({ values[index], static_cast<uint32_t>(value.elementsOperands().size() + value.holes().size()), { BlockID(blockIndex), index } });
                plans.last().appendsElements = appendsElements;
            }
        }
    }
    if (plans.isEmpty())
        return PassOutcome::Unchanged;

    FunctionAnalyses& analyses = context.analyses;
    const CFG& cfg = analyses.cfg();
    const Dominators& dominators = analyses.dominators();
    const UseLists& useLists = analyses.useLists();
    BitVector visitedElements(function.values.size());

    // Nothing where the search has gone on for too long. The blocks between the elements grow with the literal and do not count.
    // An array that is made early is made in vain where control leaves on a rarer path, as where a caller ends a generator at one of its yields.
    auto computePostDominanceBound = [&](std::span<const BlockID> chainBlocks, ValueID literal) -> std::optional<uint32_t> {
        BitVector visited(cfg.numNodes());
        visited.quickSet(chainBlocks.front().index());
        BitVector isOnChain(cfg.numNodes());
        for (BlockID block : chainBlocks)
            isOnChain.quickSet(block.index());
        Expected<BlockFrequency> frequencyOfLiteral = function.at(chainBlocks.front()).frequency;
        uint64_t numberOfBlocksSearched = 0;
        Vector<BlockID, 16> worklist;
        for (uint32_t chainIndex = 1; chainIndex < chainBlocks.size(); ++chainIndex) {
            if (!visited.quickSet(chainBlocks[chainIndex].index()))
                worklist.append(chainBlocks[chainIndex]);
            while (!worklist.isEmpty()) {
                BlockID block = worklist.takeLast();
                const Terminator& terminator = function.at(block).terminator;
                if (returnsToCaller(terminator.kind))
                    return chainIndex - 1;
                if (!isOnChain.quickGet(block.index()) && context.exceeds(LimitID::MaxBlocksSearchedForLiteralStores, ++numberOfBlocksSearched, literal))
                    return std::nullopt;
                for (const Target& target : terminator.targets) {
                    if ((frequencyOfLiteral < function.at(target.block).frequency).predicted())
                        continue;
                    if (!visited.quickSet(target.block.index()))
                        worklist.append(target.block);
                }
            }
        }
        return static_cast<uint32_t>(chainBlocks.size() - 1);
    };
    auto buildChain = [&](ValueID literal, BlockID literalBlock, std::span<const ChainPoint> ownAvailabilities) -> std::optional<DominatorChain> {
        DominatorChain chain;
        chain.blocks.append(literalBlock);
        chain.nearestUsablePoints.append({ 0, indexAfterCaughtValues(function, function.at(literalBlock)) });
        uint32_t top = 0;
        for (ChainPoint availability : ownAvailabilities)
            top = std::max(top, availability.chainIndex);
        for (uint32_t chainIndex = 1; chainIndex <= top; ++chainIndex)
            chain.blocks.append(dominators.idom(chain.blocks.last()));
        if (analyses.isInCycle(literalBlock)) {
            // A cycle through the literal that avoids one of its dominators avoids every earlier dominator.
            std::span<const BlockID> dominatorsOfLiteral = chain.blocks.span().subspan(1);
            auto firstAvoided = std::ranges::partition_point(dominatorsOfLiteral, [&](BlockID dominator) {
                return !cfg.isInCycleAvoiding(literalBlock, dominator);
            });
            chain.blocks.shrink(static_cast<size_t>(firstAvoided - dominatorsOfLiteral.begin()) + 1);
        }
        std::optional<uint32_t> last = computePostDominanceBound(chain.blocks.span(), literal);
        if (!last)
            return std::nullopt;
        if (!*last)
            return chain;

        const NaturalLoops& loops = analyses.naturalLoops();
        for (uint32_t chainIndex = 1; chainIndex <= *last; ++chainIndex) {
            BlockID block = chain.blocks[chainIndex];
            bool isUsable = loops.innerMostLoopOf(block) == loops.innerMostLoopOf(literalBlock) && function.at(block).handler == function.at(literalBlock).handler && dominators.terminatorDominates(block, literalBlock);
            chain.nearestUsablePoints.append(isUsable ? ChainPoint { chainIndex, indexAfterCaughtValues(function, function.at(block)) } : chain.nearestUsablePoints.last());
        }
        return chain;
    };
    for (LiteralPlan& plan : plans) {
        ProgramPoint point = plan.allocation;
        const Value& literal = function.at(plan.literal);
        RELEASE_ASSERT(!literal.capacityOperand() && literal.literalStorage() == ArrayLiteralStorage::Fresh);
        auto availabilityOf = [&](ValueID element) {
            ProgramPoint available = useLists.pointAfter(element);
            RELEASE_ASSERT(dominators.dominates(available.block, point.block));
            ChainPoint availability { dominators.depthOf(point.block) - dominators.depthOf(available.block), available.index };
            RELEASE_ASSERT(!(ChainPoint { 0, point.index }).isEarlierThan(availability));
            return availability;
        };

        ChainPoint allocation { 0, point.index };
        std::optional<DominatorChain> chain;
        if (cfg.isReachable(point.block)) {
            Vector<ChainPoint, 8> ownAvailabilities;
            for (ValueID element : literal.elementsOperands()) {
                bool isOwnElement = !visitedElements.quickSet(element.index()) && (plan.appendsElements ? isComputed(element) : isStoredWhenMade(literal, element)) && std::ranges::all_of(useLists.usesOf(element), [&](const Use& use) {
                    if (use.kind == Use::Kind::Operand && (use.user == plan.literal || isHeldOperand(function.at(use.user), use.operandIndex)))
                        return true;
                    // What a yield is sent is also thrown or returned, on the rare arms right behind it.
                    return (function.at(point.block).frequency < function.at(use.block).frequency).predicted();
                });
                if (isOwnElement) {
                    ownAvailabilities.append(availabilityOf(element));
                    plan.ownElements.append(element);
                }
            }
            for (ValueID element : literal.elementsOperands())
                visitedElements.quickClear(element.index());
            if ((chain = buildChain(plan.literal, point.block, ownAvailabilities.span()))) {
                for (ChainPoint availability : ownAvailabilities) {
                    ChainPoint candidate = chain->moveToUsableBlock(availability);
                    if (chain->isUsable(candidate.chainIndex) && candidate.isEarlierThan(allocation))
                        allocation = candidate;
                }
                plan.allocation = chain->programPointOf(allocation);
            }
        }

        std::span<const uint32_t> holes = literal.holes();
        uint32_t arrayIndex = 0;
        ChainPoint lastAppend = allocation;
        auto programPointOf = [&](ChainPoint storePoint) {
            return allocation.isEarlierThan(storePoint) ? chain->programPointOf(storePoint) : plan.allocation;
        };
        for (ValueID element : literal.elementsOperands()) {
            for (; !holes.empty() && holes.front() == arrayIndex; holes = holes.subspan(1)) {
                if (plan.appendsElements)
                    plan.stores.append({ programPointOf(lastAppend), arrayIndex, ValueID() });
                ++arrayIndex;
            }
            ChainPoint availability = chain ? chain->moveToUsableBlock(availabilityOf(element)) : allocation;
            if (plan.appendsElements) {
                if (lastAppend.isEarlierThan(availability))
                    lastAppend = availability;
                availability = lastAppend;
            }
            plan.stores.append({ programPointOf(availability), arrayIndex++, element });
        }
        if (plan.appendsElements) {
            for (; !holes.empty(); holes = holes.subspan(1))
                plan.stores.append({ programPointOf(lastAppend), arrayIndex++, ValueID() });
        }
    }

    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    auto insertStores = [&](const LiteralPlan& plan, bool standBehindAllocation) {
        builder.setOrigin(function.at(plan.literal).origin());
        for (const ElementStore& store : plan.stores) {
            if ((store.point == plan.allocation) != standBehindAllocation)
                continue;
            if (!plan.appendsElements)
                builder.insertBefore(store.point, IRBuilder::make(Opcode::PutContiguousIndex, { plan.array, builder.insertBefore(store.point, makeInteger(store.arrayIndex)), store.element }));
            else if (store.element)
                builder.insertBefore(store.point, IRBuilder::make(Opcode::AppendElement, { plan.array, store.element }));
            else
                builder.insertBefore(store.point, IRBuilder::make(Opcode::AppendHole, { plan.array }));
        }
    };
    for (LiteralPlan& plan : plans) {
        builder.setOrigin(function.at(plan.literal).origin());
        plan.lengthOfArray = builder.insertBefore(plan.allocation, makeInteger(plan.length));
        Value allocation = plan.appendsElements ? IRBuilder::make(Opcode::NewArray) : IRBuilder::make(Opcode::NewContiguousArray, { plan.lengthOfArray });
        if (plan.appendsElements) {
            allocation.setContents(function.at(plan.literal).contents());
            allocation.setContentsAreProven(function.at(plan.literal).contentsAreProven());
            allocation.setArrayStorage(function.at(plan.literal).arrayStorage());
            allocation.setCapacityOperand(plan.lengthOfArray);
        }
        allocation.setAllocationSite(function.at(plan.literal).allocationSite());
        plan.array = builder.insertBefore(plan.allocation, WTF::move(allocation));
        insertStores(plan, true);
    }
    for (const LiteralPlan& plan : plans)
        insertStores(plan, false);

    // A suspension behind the store of an element holds the array, which holds the element. Listed by itself, the element would stay alive across every suspension up to that one.
    BitVector isOwnElementOfPlan(function.values.size());
    BitVector isHeldByArray(function.values.size());
    for (const LiteralPlan& plan : plans) {
        for (ValueID element : plan.ownElements)
            isOwnElementOfPlan.quickSet(element.index());
        Vector<std::pair<ValueID, ValueID>> heldElements;
        for (const ElementStore& store : plan.stores) {
            if (!store.element || !isOwnElementOfPlan.quickGet(store.element.index()))
                continue;
            for (const Use& use : useLists.usesOf(store.element)) {
                if (use.kind != Use::Kind::Operand || use.user == plan.literal || !isHeldOperand(function.at(use.user), use.operandIndex))
                    continue;
                ProgramPoint suspension = useLists.pointOf(use.user);
                if (store.point.block == suspension.block ? store.point.index <= suspension.index : dominators.dominates(store.point.block, suspension.block))
                    heldElements.append({ use.user, store.element });
            }
        }
        for (ValueID element : plan.ownElements)
            isOwnElementOfPlan.quickClear(element.index());
        AOT::sort(heldElements);
        for (std::span<const std::pair<ValueID, ValueID>> rest = heldElements.span(); !rest.empty();) {
            ValueID suspension = rest.front().first;
            size_t count = std::ranges::find_if(rest, [&](const auto& entry) { return entry.first != suspension; }) - rest.begin();
            for (const auto& [ignored, element] : rest.first(count))
                isHeldByArray.quickSet(element.index());
            Vector<ValueID, 8> held;
            for (ValueID listed : heldOperandsOf(function.at(suspension))) {
                if (!isHeldByArray.quickGet(listed.index()))
                    held.append(listed);
            }
            held.append(plan.array);
            setHeldOperands(function.edit(suspension), held.span());
            for (const auto& [ignored, element] : rest.first(count))
                isHeldByArray.quickClear(element.index());
            rest = rest.subspan(count);
        }
    }
    builder.applyInsertions();
    for (const LiteralPlan& plan : plans) {
        Value& literal = function.edit(plan.literal);
        literal.become(Opcode::TruncateNewArray);
        literal.setArrayOperand(plan.array);
        literal.setLengthOperand(plan.lengthOfArray);
    }
    // Values are put into the blocks that are there.
    context.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

static_assert(OperandIndex::IsElementPresent::array == testedOperandIndex && OperandIndex::IsInsideView::view == testedOperandIndex);

using ElementAccessTests = Vector<Value, 6>;

static constexpr unsigned maxValuesFollowed = 8;

// ResolveAccesses has rewritten these: the types of their operands needed no test. Whether the position holds an element is still open.
static constexpr OpcodeSet resolvedAccessesWithPresenceTest { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement };

// SinkAllocations takes such an array apart by its accesses, which stay as they are. The result of `x.map(f)` is an allocation too, and its reads get the test.
static constexpr OpcodeSet localArrayAllocations { Opcode::NewArray, Opcode::NewArrayLike, Opcode::NewArrayWithCapacity, Opcode::NewContiguousArray, Opcode::NewDoubleArray, Opcode::CreateRest };

static constexpr OpcodeSet equalityComparisons { Opcode::Eq, Opcode::StrictEq };

// Each applies ToNumeric to an operand that is a primitive.
static constexpr OpcodeSet operatorsOnNumericOperands {
    Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow,
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Negate, Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement, Opcode::BitNot,
    Opcode::ToNumber, Opcode::ToInt32, Opcode::ToUint32,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
};

namespace {

// The test in front of an element access that has no arm for an absent element: where it fails, the access is made by the generic copy.
enum class PresenceTest : uint8_t { InBounds, InBoundsWithHolesAsNaN, ElementPresent, InsideView };

struct PresenceQuery {
    ValueID access;
    BlockID block;
    ValueID receiver;
    Access kind { Access::Read };
    ElementAccessForm form { ElementAccessForm::ByKey };
    ArrayStorage arrayStorage { ArrayStorage::Other };
    TypedArrayType typedArrayType { NotTypedArray };
    bool keyIsInteger { false };
    bool isDense { false };
    bool isNeverPristine { false };
};

// What the function itself does with an array says what to expect of it: a position that it compares with the length lies inside, and an array that it stores into has storage of its own.
struct ArrayUses {
    FunctionAnalyses& analyses;
    bool isCollected { false };
    HashMap<ValueID, Vector<ValueID, 2>> lengthReads { };
    Vector<std::pair<ValueID, BlockID>, 2> creations { };
    HashSet<ValueID> storedArrays { };
    std::optional<IntegerRelations> relations { };

    void collectOnce()
    {
        if (std::exchange(isCollected, true))
            return;
        auto addLength = [&](ValueID array, ValueID length) {
            lengthReads.ensure(analyses.identityOf(array), [] { return Vector<ValueID, 2>(); }).iterator->value.append(length);
        };
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (ValueID id : analyses.function().at(block).values) {
                const Value& value = analyses.function().at(id);
                std::optional<MeasuredStorage> measured = storageMeasuredBy(analyses.program(), value);
                if (measured && measured->heap == AbstractHeap::ArrayStorage && measured->owner)
                    addLength(measured->owner, id);
                // An array of holes has its length from the start.
                ValueID length;
                if (value.opcode == Opcode::NewArrayLike || value.opcode == Opcode::NewContiguousArray || value.opcode == Opcode::NewDoubleArray)
                    length = value.lengthOperand();
                else if (value.opcode == Opcode::CallBuiltin && inlineImplementationOf(analyses.program(), value) == InlineBuiltin::ArrayWithLength && !value.argumentsOperands().empty())
                    length = value.argumentsOperands()[0];
                if (length) {
                    addLength(id, length);
                    creations.append({ length, block });
                }
                if (value.opcode == Opcode::PutArrayElement)
                    storedArrays.add(analyses.identityOf(value.arrayOperand()));
                else if (std::optional<Access> access = elementAccessOf(value.opcode); access == Access::Store || access == Access::Define)
                    storedArrays.add(analyses.identityOf(value.objectOperand()));
            }
        }
    }

    bool isStoredInto(ValueID array)
    {
        collectOnce();
        return storedArrays.contains(analyses.identityOf(array));
    }

    bool isComparedWithLength(ValueID array, ValueID position, BlockID at)
    {
        collectOnce();
        auto lengths = lengthReads.find(analyses.identityOf(array));
        if (lengths == lengthReads.end())
            return false;
        if (!relations) {
            relations.emplace(analyses);
            // Any other number has thrown a RangeError.
            for (auto [length, block] : creations)
                relations->assumeRange(length, Range::uint32(), block);
        }
        if (relations->isAtLeast(position, 0, at) != IntegerRelations::Answer::Proven)
            return false;

        // A position is compared with a length that was read shortly before: the reads are asked about from the last one in front of the access to the first, and then those behind it.
        const CFG& cfg = analyses.cfg();
        const UseLists& useLists = analyses.useLists();
        auto isAbovePosition = [&](ValueID length) {
            return relations->isAtMost(position, length, -1, at) == IntegerRelations::Answer::Proven;
        };
        std::span<const ValueID> reads = lengths->value.span();
        size_t numberInFront = std::ranges::partition_point(reads, [&](ValueID length) {
            return cfg.reversePostOrderIndex(useLists.blockOf(length)) <= cfg.reversePostOrderIndex(at);
        }) - reads.begin();
        return std::ranges::any_of(reads.first(numberInFront) | std::views::reverse, isAbovePosition) || std::ranges::any_of(reads.subspan(numberInFront), isAbovePosition);
    }
};

} // anonymous namespace

static bool asksWhetherValueIsAbsent(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::ToBoolean || user.opcode == Opcode::TypeOf)
        return true;
    // `??` and `?.` are tests for undefined and null. A test for a number is what a check of the element has become, and it folds once the read is typed.
    if (user.opcode == Opcode::TypeTest)
        return program.at(user.testedType()).isUndefinedOrNull();
    // A comparison with what is never undefined or null looks for a value.
    return equalityComparisons.contains(user.opcode) && program.typeOf(function, user.operand(1 - operandIndex)).mayBeUndefinedOrNull();
}

// A wrong "no" costs speed only: the failed test takes the generic copy.
static bool isTestedForAbsence(FunctionAnalyses& analyses, ValueID read)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    Vector<ValueID, maxValuesFollowed> followed { read };
    auto follow = [&](ValueID value) {
        if (followed.size() < maxValuesFollowed && !followed.contains(value) && program.typeOf(function, value).mayBeUndefinedOrNull())
            followed.append(value);
    };
    // What passes a test of its type goes on under the narrower type, as behind a cast. What fails it has left for other code.
    auto followWhatPassed = [&](ValueID test, uint32_t operandIndex) {
        for (const Use& use : useLists.usesOf(test)) {
            const Terminator& terminator = function.at(use.block).terminator;
            if (use.kind != Use::Kind::TerminatorOperand || !terminator.isConditionalBranch())
                continue;
            const Target& taken = terminator.taken();
            uint32_t firstResult = firstEdgeResultParameter(function, use.block, 0);
            for (uint32_t resultIndex = 0; resultIndex < taken.proofs.size(); ++resultIndex) {
                if (taken.proofs[resultIndex] == operandIndex)
                    follow(function.at(taken.block).parameters[firstResult + resultIndex]);
            }
        }
    };
    for (size_t index = 0; index < followed.size(); ++index) {
        for (const Use& use : useLists.usesOf(followed[index])) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = function.at(use.user);
                if (asksWhetherValueIsAbsent(program, function, user, use.operandIndex))
                    return true;
                if (isCast(user.opcode) && !use.operandIndex)
                    follow(use.user);
                else if (user.opcode == Opcode::TypeTest)
                    followWhatPassed(use.user, use.operandIndex);
                break;
            }
            // The cases of a switch are numbers or strings: what is absent goes where every other value goes.
            case Use::Kind::TerminatorOperand:
                if (function.at(use.block).terminator.isConditionalBranch())
                    return true;
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
                follow(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]);
                break;
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return false;
}

// Undefined converts to NaN, and the hole of Double storage is a NaN.
static bool everyUseConvertsToNumber(const Program& program, FunctionAnalyses& analyses, ValueID read)
{
    const Function& function = analyses.function();
    bool isUsed = false;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;
            // A cast that throws tells undefined from NaN.
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode) && !use.operandIndex) {
                worklist.append(use.user);
                continue;
            }
            // Beside a string the sum would hold the text "undefined".
            if (!operatorsOnNumericOperands.contains(user.opcode) && (user.opcode != Opcode::Add || !program.typeOf(function, user.operand(1 - use.operandIndex)).isNumber()))
                return false;
            isUsed = true;
        }
    }
    return isUsed;
}

static bool isMadeInFrontOfLoopAround(FunctionPassContext& context, ValueID array, BlockID block)
{
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(block);
    return loop && std::ranges::any_of(CastChain(context.function, array), [&](ValueID candidate) {
        ValueLocation location = context.analyses.useLists().locationOf(candidate);
        return location.placement != ValuePlacement::Detached && !loops.belongsTo(location.block, *loop);
    });
}

// Such a test fails for an absent element alone, so the test of the position takes its place. Of a number in storage for any value it also tells how the number is held, which whoever reckons with it asks anyway.
static bool isTestedForTypeOfElements(FunctionAnalyses& analyses, ValueID read, ArrayStorage arrayStorage)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    Type element = program.typeOf(function, read).withoutPrimitives(Primitive::Undefined);
    if (arrayStorage == ArrayStorage::Contiguous && element.isNumber())
        return false;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand || use.operandIndex)
                continue;
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode))
                worklist.append(use.user);
            else if (user.opcode == Opcode::TypeTest && !program.at(user.testedType()).mayBeUndefinedOrNull() && element.isSubsetOf(program.at(user.testedType()), program))
                return true;
        }
    }
    return false;
}

static bool hasFormWithoutAbsentElement(FunctionPassContext& context, const PresenceQuery& query)
{
    const Program& program = context.program;
    const Function& function = context.function;
    // A store outside a typed array does nothing, and no program relies on that.
    if (query.form == ElementAccessForm::TypedArray)
        return (query.kind == Access::Read || query.kind == Access::Store) && hasInlineElementAccess(query.typedArrayType);
    if (query.form != ElementAccessForm::Array || query.kind != Access::Read || (!hasArrayStorageForm(query.arrayStorage) && !query.isNeverPristine))
        return false;
    // The first elements of such a result are delivered without the array, each as the element or undefined. An array that lasts through the turns of a loop stays an array.
    // In a dense one only a position outside finds nothing, and the arm for that is one comparison with the length, which the loop mostly makes itself.
    const Value& array = function.at(sameObjectThroughCasts(function, query.receiver));
    if (localArrayAllocations.contains(array.opcode))
        return isMadeInFrontOfLoopAround(context, query.receiver, query.block) && (!query.isDense || isTestedForTypeOfElements(context.analyses, query.access, query.arrayStorage));
    return array.opcode != Opcode::CallBuiltin || !program.builtins().at(array.overload()).code.prefixForm;
}

// The read keeps its type less undefined, which has to admit the NaN. An operand of any other type is tested by its operator in the same round, and a test is no conversion.
static bool hasTypesToReadHolesAsNaN(FunctionPassContext& context, const PresenceQuery& query)
{
    const Type& type = context.program.typeOf(context.function, query.access);
    return query.arrayStorage == ArrayStorage::Double && query.keyIsInteger && type.isNumberOrNullish() && type.mayBeNaN();
}

// That a hole reads as undefined is a fact about the prototypes, whose test pays only in front of a loop.
static bool canReadHolesAsNaN(FunctionPassContext& context, const PresenceQuery& query)
{
    return hasTypesToReadHolesAsNaN(context, query) && isMadeInFrontOfLoopAround(context, query.receiver, query.block) && everyUseConvertsToNumber(context.program, context.analyses, query.access);
}

// Nothing where the access keeps its arm for an absent element.
static std::optional<PresenceTest> choosePresenceTest(FunctionPassContext& context, const PresenceQuery& query)
{
    if (!hasFormWithoutAbsentElement(context, query) || context.analyses.blockFrequencies().classOf(query.block).predicted() == BlockFrequency::Rare)
        return std::nullopt;
    if (query.isNeverPristine)
        return PresenceTest::ElementPresent;
    if (query.kind == Access::Read && isTestedForAbsence(context.analyses, query.access))
        return std::nullopt;
    if (query.form == ElementAccessForm::TypedArray)
        return PresenceTest::InsideView;
    if (query.isDense && query.keyIsInteger)
        return PresenceTest::InBounds;
    return canReadHolesAsNaN(context, query) ? PresenceTest::InBoundsWithHolesAsNaN : PresenceTest::ElementPresent;
}

static bool isDenseArray(const Type& array)
{
    return pristineAspectsProvenBy(array, BuiltinClasses::Array).contains(PristineAspects::Dense);
}

static TypeID testedTypeOfPrimitive(const TestSiteReporter& reporter, ValueID value)
{
    std::optional<Type> expected = reporter.expectedPartOf(value);
    if (!expected)
        return { };
    return expected->isNumber() ? testedNumberTypeContaining(*expected) : reporter.types().intern(*expected);
}

// Where nothing is expected of the key itself, the plan tests it because its receiver has elements: an index is expected.
static TypeID testedTypeOfKey(const TestSiteReporter& reporter, ValueID key)
{
    if (TypeID tested = testedTypeOfPrimitive(reporter, key))
        return tested;
    bool mayBeIndex = !reporter.function().at(key).expected().predicted() && reporter.typeOf(key).overlaps(reporter.types().at(TypeTable::int32), reporter.program());
    return mayBeIndex ? TypeTable::int32 : TypeID();
}

static TypeID testedTypeOfStoredValue(const TestSiteReporter& reporter, ValueID value, const ElementAccessPlan& plan)
{
    if (TypeID tested = testedTypeOfPrimitive(reporter, value))
        return tested;
    if (plan.form != ElementAccessForm::TypedArray || reporter.function().at(value).expected().predicted())
        return { };
    Type number = reporter.typeOf(value).withoutPrimitives(Primitive::UndefinedOrNull);
    return number.isNumber() ? testedNumberTypeContaining(number) : TypeID();
}

static std::optional<ElementAccessTests> createTests(const TestSiteReporter& reporter, ArrayUses& arrays, const Value& use, const ElementAccessPlan& plan)
{
    ValueID receiver = use.objectOperand();
    ValueID key = use.propertyOperand();
    ElementAccessTests tests;

    // IsPristine proves a guard and leaves the type of the receiver as it is, and the element operations take an operand typed as the class.
    if (plan.testsReceiverClass) {
        std::optional<Type> expected = reporter.expectedPartOf(receiver);
        if (!expected)
            return std::nullopt;
        tests.append(makeTypeTest(receiver, reporter.types().intern(typeOfInstances(reporter.program().builtins(), plan.builtinClass, *expected))));
    } else if (plan.checksNullish) {
        // The tests behind this one look into the receiver. Undefined and null fail it, and the access by key throws what it always throws.
        tests.append(makeTypeTest(receiver, TypeTable::object));
    }
    if (plan.testsReceiverIsString)
        tests.append(makeTypeTest(receiver, TypeTable::string));
    if (!plan.requiredAspects.isEmpty())
        tests.append(makePristineTest(receiver, plan.builtinClass, plan.requiredAspects));
    if (plan.testsArrayStorage)
        tests.append(makeArrayStorageTest(receiver, plan.arrayStorage, arrays.isStoredInto(receiver)));
    auto appendTestOfPrimitive = [&](ValueID value, TypeID tested) {
        if (tested)
            tests.append(makeTypeTest(value, tested));
        return !!tested;
    };
    if (plan.testsKeyIsNumber && !appendTestOfPrimitive(key, testedTypeOfKey(reporter, key)))
        return std::nullopt;
    if (plan.testsStoredValue && !appendTestOfPrimitive(use.valueOperand(), testedTypeOfStoredValue(reporter, use.valueOperand(), plan)))
        return std::nullopt;
    if (plan.testsInBounds)
        tests.append(makeBoundsTest(receiver, key));
    // Last: a test with a guard operand gets the proofs of the earlier tests of its site, and this one relies on the kind of storage.
    if (plan.testsPresence)
        tests.append(plan.form == ElementAccessForm::Array ? makePresenceTest(receiver, key, plan.arrayStorage, plan.keyIsIntegral) : makeViewBoundsTest(receiver, key, plan.typedArrayType, plan.keyIsIntegral));
    return tests;
}

static Type keyTypeAfterTests(const TestSiteReporter& reporter, const Value& use, const ElementAccessPlan& plan)
{
    ValueID key = use.propertyOperand();
    TypeID tested = plan.testsKeyIsNumber ? testedTypeOfKey(reporter, key) : TypeID();
    return tested ? reporter.typeOf(key).intersectWith(reporter.types().at(tested), reporter.types()) : reporter.typeOf(key);
}

static PresenceQuery presenceQueryOf(ValueID access, BlockID block, const Value& use, const ElementAccessPlan& plan, const Type& receiver, const Type& key)
{
    return { access, block, use.objectOperand(), *elementAccessOf(use.opcode), plan.form, plan.arrayStorage, plan.typedArrayType, key.isInteger(), isDenseArray(receiver), plan.receiverIsNeverPristine };
}

// Where nothing asks whether the element is there, an absent element leaves typed code: the access behind the test has no arm for it.
static bool expectPresentElement(const TestSiteReporter& reporter, const Value& use, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array && plan.form != ElementAccessForm::TypedArray)
        return false;
    PresenceQuery query = presenceQueryOf(reporter.use(), reporter.point().block, use, plan, reporter.typeOf(use.objectOperand()), keyTypeAfterTests(reporter, use, plan));
    std::optional<PresenceTest> test = choosePresenceTest(reporter.context(), query);
    if (!test)
        return false;
    switch (*test) {
    case PresenceTest::InBoundsWithHolesAsNaN:
        plan.readsHoleAsNaN = true;
        [[fallthrough]];
    case PresenceTest::InBounds:
        plan.testsInBounds = true;
        return true;
    case PresenceTest::ElementPresent:
        // An own element that is present answers whatever the prototypes hold, and a key that is no index fails the test.
        plan.testsInBounds = false;
        plan.requiredAspects = { };
        [[fallthrough]];
    case PresenceTest::InsideView:
        plan.testsPresence = true;
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void countPresenceTest(const TestSiteReporter& reporter)
{
    if (reporter.didReport())
        reporter.context().build.statistics().count(StatisticID::ElementPresenceTestsPlaced);
}

static void reportBoundsTestOfResolvedStore(TestSiteReporter& reporter, ArrayUses& arrays, const Value& use)
{
    if (!hasArrayStorageForm(use.arrayStorage()))
        return;
    const Type& positionType = reporter.typeOf(use.positionOperand());
    if (!positionType.isInteger() || !canStoreWithoutConversion(use.arrayStorage(), reporter.typeOf(use.valueOperand())))
        return;
    if (!arrays.isComparedWithLength(use.arrayOperand(), use.positionOperand(), reporter.point().block))
        return;
    Effects effects = declaredBoundOf(use.opcode);
    refineElementEffects(use.opcode, positionType, ElementPosition::InsideLength, effects);
    // As for a read: where the store has the kind of storage from its own guard, the test of the kind is asked for again, and the proof that still holds answers.
    ElementAccessTests tests;
    if (proofsNeededFromGuard(reporter.program(), reporter.function(), use).contains(Proof::ArrayStorage))
        tests.append(makeArrayStorageTest(use.arrayOperand(), use.arrayStorage(), true));
    tests.append(makeBoundsTest(use.arrayOperand(), use.positionOperand()));
    reporter.report(tests.span(), effects);
}

static void reportPresenceTestOfResolvedAccess(TestSiteReporter& reporter, ArrayUses& arrays, const Value& use)
{
    bool isArrayRead = use.opcode == Opcode::GetArrayElement;
    if (isArrayRead ? use.isPresent() || use.isInBounds() : use.opcode == Opcode::PutArrayElement ? use.isInBounds() : use.isInsideView())
        return;
    if (use.opcode == Opcode::PutArrayElement)
        return reportBoundsTestOfResolvedStore(reporter, arrays, use);

    ValueID receiver = isArrayRead ? use.arrayOperand() : use.viewOperand();
    ValueID position = use.positionOperand();
    const Type& positionType = reporter.typeOf(position);
    std::optional<PresenceTest> chosen = choosePresenceTest(reporter.context(), PresenceQuery {
        .access = reporter.use(),
        .block = reporter.point().block,
        .receiver = receiver,
        .kind = use.opcode == Opcode::PutTypedArrayElement ? Access::Store : Access::Read,
        .form = isArrayRead ? ElementAccessForm::Array : ElementAccessForm::TypedArray,
        .arrayStorage = isArrayRead ? use.arrayStorage() : ArrayStorage::Other,
        .typedArrayType = isArrayRead ? NotTypedArray : use.typedArrayType(),
        .keyIsInteger = positionType.isInteger(),
        .isDense = isArrayRead && isDenseArray(reporter.typeOf(receiver)),
    });
    if (!chosen)
        return;

    ElementAccessTests tests;
    switch (*chosen) {
    case PresenceTest::InBounds:
    case PresenceTest::InBoundsWithHolesAsNaN:
        tests.append(makeBoundsTest(receiver, position));
        break;
    case PresenceTest::ElementPresent: {
        // A test is reported without a guard. Where the read has the kind of storage from its own guard, the test of the kind is asked for again, and the proof that still holds answers.
        Value test = makePresenceTest(receiver, position, use.arrayStorage(), use.keyIsIntegral());
        if (proofsNeededFromGuard(reporter.program(), reporter.function(), test).contains(Proof::ArrayStorage))
            tests.append(makeArrayStorageTest(receiver, use.arrayStorage(), arrays.isStoredInto(receiver)));
        tests.append(WTF::move(test));
        break;
    }
    case PresenceTest::InsideView:
        tests.append(makeViewBoundsTest(receiver, position, use.typedArrayType(), use.keyIsIntegral()));
        break;
    }
    Effects effects = declaredBoundOf(use.opcode);
    if (isArrayRead)
        refineElementEffects(use.opcode, positionType, ElementPosition::InsideLength, effects);
    reporter.report(tests.span(), effects);
    countPresenceTest(reporter);
}

static ArrayStorage arrayStorageExpectedOfRead(const TestSiteReporter& reporter)
{
    const Function& function = reporter.function();
    auto storageHolding = [&](TypeID type) {
        return type && !reporter.types().at(type).isAnything() ? arrayStorageHolding(reporter.types().at(type)) : ArrayStorage::Other;
    };
    Vector<ValueID, maxValuesFollowed> followed { reporter.use() };
    for (size_t index = 0; index < followed.size(); ++index) {
        const Value& value = function.at(followed[index]);
        for (TypeID type : { value.type, value.expected().predicted() }) {
            if (ArrayStorage expected = storageHolding(type); expected != ArrayStorage::Other)
                return expected == ArrayStorage::Int32 ? ArrayStorage::Double : expected;
        }
        for (const Use& use : reporter.analyses().useLists().usesOf(followed[index])) {
            ValueID next;
            if (use.kind == Use::Kind::Operand && !use.operandIndex && isCast(function.at(use.user).opcode))
                next = use.user;
            else if (use.kind == Use::Kind::EdgeArgument)
                next = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
            if (next && followed.size() < maxValuesFollowed && !followed.contains(next))
                followed.append(next);
        }
    }
    return ArrayStorage::Other;
}

void reportElementAccessTests(TestSiteReporter& reporter)
{
    ArrayUses arrays { reporter.analyses() };
    static constexpr OpcodeSet operations = resolvedAccessesWithPresenceTest | OpcodeSet { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemHas };
    reporter.forEachCandidate(operations, [&](const Value& use) {
        if (resolvedAccessesWithPresenceTest.contains(use.opcode))
            return reportPresenceTestOfResolvedAccess(reporter, arrays, use);
        ProgramPoint point = reporter.point();
        std::optional<ElementAccessSite> site = elementAccessOf(use.opcode) ? planElementAccessSite(reporter.context(), ValueSite { point.block, point.index, reporter.use() }) : std::nullopt;
        if (!site)
            return;
        ElementAccessPlan& plan = site->plan;
        // An array of holes is allocated with the kind that its allocation names, and has its length from the start: a store into it is a store inside the length, which the forms with a kind have inline.
        std::optional<ArrayStorage> allocated = plan.form == ElementAccessForm::Array && plan.arrayStorage == ArrayStorage::Other ? arrayStorageOfNewHoles(reporter.function().at(reporter.analyses().identityOf(use.objectOperand()))) : std::nullopt;
        if (allocated) {
            plan.arrayStorage = *allocated;
            plan.testsArrayStorage = true;
            plan.level = KnowledgeLevel::Expected;
        }
        if (plan.receiverIsNeverPristine && plan.arrayStorage == ArrayStorage::Other) {
            plan.arrayStorage = arrayStorageExpectedOfRead(reporter);
            plan.testsArrayStorage = hasArrayStorageForm(plan.arrayStorage);
        }
        // The test is placed only where the comparison says how it will come out: an append by index, `a[n++] = x`, fails it every time.
        if (plan.form == ElementAccessForm::Array && hasArrayStorageForm(plan.arrayStorage) && Value::opcodesWithIsInBounds.contains(plan.operation) && keyTypeAfterTests(reporter, use, plan).isInteger()) {
            // A store that converts the storage is no store of one element.
            if (!OperandPosition::value(plan.operation) || canStoreWithoutConversion(plan.arrayStorage, reporter.typeOf(use.valueOperand())))
                plan.testsInBounds = arrays.isComparedWithLength(use.objectOperand(), use.propertyOperand(), reporter.point().block);
        }
        bool expectsPresentElement = expectPresentElement(reporter, use, plan);
        bool storesIntoTestedStorage = plan.operation == Opcode::PutArrayElement && plan.testsArrayStorage;
        if (plan.receiverIsNeverPristine && !plan.testsPresence && !storesIntoTestedStorage)
            return;
        if (plan.receiverIsNeverPristine && storesIntoTestedStorage)
            plan.requiredAspects = { };
        bool hasGuardedForm = plan.isCharacterReadAfterTests() || (plan.isResolved() && !plan.needsNoTest() && (plan.form != ElementAccessForm::Array || plan.arrayStorage == ArrayStorage::Other || hasArrayStorageForm(plan.arrayStorage)));
        std::optional<ElementAccessTests> tests = hasGuardedForm ? createTests(reporter, arrays, use, plan) : std::nullopt;
        if (!tests)
            return;
        reporter.report(tests->span(), plan.isCharacterReadAfterTests() ? declaredBoundOf(Opcode::StringRangeCharacterOrUndefined) : effectsAfterRewrite(plan, keyTypeAfterTests(reporter, use, plan)));
        if (expectsPresentElement)
            countPresenceTest(reporter);
        if (plan.readsHoleAsNaN && reporter.didReport())
            reporter.noteThatHoleMayBeReadAsNaN();
    });
}

static void markPositionAsTested(TestSiteRewriter& rewriter)
{
    ValueID guard = rewriter.guard();
    RELEASE_ASSERT(guard);
    if (ValueID earlier = rewriter.useValue().guardOperand())
        guard = rewriter.insertBeforeUse(IRBuilder::make(Opcode::AllGuards, { earlier, guard }));

    Value& use = rewriter.useValue();
    use.setGuardOperand(guard);
    for (const Value& test : rewriter.tests()) {
        if (test.opcode == Opcode::IsElementPresent)
            use.setIsPresent(true);
        else if (test.opcode == Opcode::IsInBounds) {
            use.setIsInBounds(true);
            if (Value::opcodesWithReadsHoleAsNaN.contains(use.opcode))
                use.setReadsHoleAsNaN(!isDenseArray(rewriter.program().typeOf(rewriter.function(), use.arrayOperand())));
        } else if (test.opcode == Opcode::IsInsideView)
            use.setIsInsideView(true);
        else
            RELEASE_ASSERT(test.opcode == Opcode::HasArrayStorage);
    }
}

void rewriteTestedElementAccess(TestSiteRewriter& rewriter)
{
    if (resolvedAccessesWithPresenceTest.contains(rewriter.useValue().opcode))
        return markPositionAsTested(rewriter);

    ProgramPoint point = rewriter.point();
    std::optional<ElementAccessSite> site = planElementAccessSite(rewriter.context(), ValueSite { point.block, point.index, rewriter.use() });
    RELEASE_ASSERT(site);
    ElementAccessPlan& plan = site->plan;
    // A character read has no operation of its own before selection. Where the access behind its tests asks for more than they found, it stays the access by key, which is right whatever they have found.
    ASSERT(plan.isResolved() || plan.form == ElementAccessForm::String);
    if (!plan.isResolved())
        return;

    for (const Value& test : rewriter.tests()) {
        RELEASE_ASSERT(rewriter.guard() || provesForever(provesOf(test.opcode)));
        if (test.opcode == Opcode::HasArrayStorage) {
            plan.arrayStorage = test.arrayStorage();
            plan.testsArrayStorage = false;
        } else if (test.opcode == Opcode::IsPristine)
            plan.requiredAspects = plan.requiredAspects - test.aspects();
        else if (test.opcode == Opcode::IsInBounds) {
            plan.isInBounds = true;
            plan.testsInBounds = false;
        } else if (test.opcode == Opcode::IsElementPresent || test.opcode == Opcode::IsInsideView) {
            (test.opcode == Opcode::IsElementPresent ? plan.isPresent : plan.isInsideView) = true;
            plan.testsPresence = false;
        } else
            RELEASE_ASSERT(test.opcode == Opcode::TypeTest);
    }
    // Sites that share the test of the class share the value that passed it, which has the expected type of one of them. A storage that this site did not test is not relied on.
    if (plan.testsArrayStorage) {
        plan.arrayStorage = ArrayStorage::Other;
        plan.testsArrayStorage = false;
        plan.isInBounds = false;
        plan.isPresent = false;
    }
    // The plan is made again here and knows nothing of what was expected of the position when the tests were reported.
    if (plan.isPresent || (plan.receiverIsNeverPristine && plan.operation == Opcode::PutArrayElement && hasArrayStorageForm(plan.arrayStorage)))
        plan.requiredAspects = { };
    // A comparison with the length also puts the test of the bounds, so the test alone does not say what was expected of a hole. The site does. What the tests have found since is asked here:
    // nothing that takes the lists of uses, which every rewrite before this one has made stale.
    plan.readsHoleAsNaN = false;
    if (plan.operation == Opcode::GetArrayElement && plan.isInBounds && rewriter.mayReadHoleAsNaN()) {
        const Value& read = rewriter.useValue();
        const Type& receiver = rewriter.program().typeOf(rewriter.function(), read.objectOperand());
        PresenceQuery query = presenceQueryOf(rewriter.use(), point.block, read, plan, receiver, rewriter.program().typeOf(rewriter.function(), read.propertyOperand()));
        plan.readsHoleAsNaN = !query.isNeverPristine && !query.isDense && hasTypesToReadHolesAsNaN(rewriter.context(), query);
    }
    ASSERT(plan.needsNoTest());
    if (!plan.needsNoTest())
        return;
    // A test of the position is no test of a type: the compilers of typed languages make it too.
    bool testsTypeOfOperand = std::ranges::any_of(rewriter.tests(), [](const Value& test) {
        return provesOf(test.opcode) != Proof::InBounds;
    });
    plan.level = testsTypeOfOperand ? KnowledgeLevel::Expected : KnowledgeLevel::Proven;

    const Value& use = rewriter.useValue();
    Access access = *elementAccessOf(use.opcode);
    bool storesValue = access == Access::Store || access == Access::Define;
    AccessOperands operands { use.objectOperand(), storesValue ? use.valueOperand() : ValueID(), rewriter.guard() };
    // A ScopedLambda hands its functor each argument as an lvalue.
    auto insertBeforeUse = [&](auto&& value) -> ValueID {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertValue insert(insertBeforeUse);
    AccessRewriter(rewriter.context(), insert).rewrite(*site, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
