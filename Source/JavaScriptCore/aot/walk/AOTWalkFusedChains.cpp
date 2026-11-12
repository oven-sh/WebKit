/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LinkOfFusedChain : uint8_t { Filter, Map, FilterTruthy };
enum class EndOfFusedChain : uint8_t { List, Reduce, Length, Search };

enum KindOfFusedChain : uint32_t { FirstLinkFilters = 1, HasSecondLink = 2, SecondLinkFilters = 4, FiltersTruthy = 8, KeepsEveryElement = 16 };

struct FusedChain {
    Vector<LinkOfFusedChain, 2> links;
    EndOfFusedChain end { EndOfFusedChain::List };
    Vector<ValueID, 8> operations;
    Vector<ValueID, 4> callbacks;
    FunctionID firstCallback;
    FunctionID lastMapping;
    BuiltinID search;
    ValueID start;
    ValueID lastCall;
    ValueID last;
};

} // anonymous namespace

static constexpr OpcodeSet operatorsOfFusedCallbacks {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Negate, Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement, Opcode::BitNot, Opcode::ToNumber, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
    Opcode::Eq, Opcode::StrictEq, Opcode::ToBoolean, Opcode::ToString, Opcode::Strcat, Opcode::PropGet, Opcode::ElemGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField,
};

static constexpr unsigned mostLinksOfFusedChain = 2;

static std::optional<LinkOfFusedChain> linkOfFusedChain(const BuiltinTable& table, BuiltinID method)
{
    if (method == table.canonicalID(Builtins::ArrayPrototypeFilter))
        return LinkOfFusedChain::Filter;
    if (method == table.canonicalID(Builtins::ArrayPrototypeMap))
        return LinkOfFusedChain::Map;
    return std::nullopt;
}

static bool isSearch(const BuiltinTable& table, BuiltinID method)
{
    return std::ranges::any_of(std::array { Builtins::ArrayPrototypeSome, Builtins::ArrayPrototypeEvery, Builtins::ArrayPrototypeFind, Builtins::ArrayPrototypeFindIndex }, [&](auto search) {
        return method == table.canonicalID(search);
    });
}

static bool mayStandInFusedCallback(Opcode opcode)
{
    return operatorsOfFusedCallbacks.contains(opcode) || opcodesThatStateTypes.contains(opcode) || (declaredBoundInTable(opcode).mayBeRunAgainUnseen() && !operandsMayRunCode(opcode));
}

static bool isBooleanFunction(const Walker& walker, ValueID passed)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID callback = index.stripCasts(passed);
    return callback && !index.isParameter(callback) && index.opcodeOf(callback) == Opcode::Builtin
        && walker.program().builtins().canonicalID(index.value(callback).as<Opcode::Builtin>().builtin()) == walker.program().builtins().canonicalID(Builtins::Boolean);
}

static bool callsMathFunction(const Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call)
        return false;
    ValueID callee = index.stripCasts(call.as<Opcode::Call>().calleeOperand());
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = table.canonicalID(index.value(callee).as<Opcode::Builtin>().builtin());
    return std::ranges::any_of(std::array { Builtins::MathMax, Builtins::MathMin, Builtins::MathFloor, Builtins::MathCeil, Builtins::MathRound, Builtins::MathTrunc, Builtins::MathAbs, Builtins::MathSqrt, Builtins::MathImul }, [&](auto function) {
        return builtin == table.canonicalID(function);
    });
}

bool isPureCallback(const Walker& walker, FunctionID function)
{
    const LinkedFunction& linked = walker.facts().linked().function(function);
    return linked.effectsOnReturn().mayBeRunAgainUnseen() && linked.effectsOnThrow().mayBeRunAgainUnseen();
}

static TypeID elementTypeOfCallback(const Walker& walker, FunctionID function, unsigned placeOfElement)
{
    const Function& callee = walker.program().at(function);
    if (placeOfElement >= callee.numberOfPositionalParameters())
        return { };
    std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(function);
    size_t place = callee.firstParameterIndex() + placeOfElement;
    return place < parameters.size() ? parameters[place].writtenOrSeenToArrive() : TypeID();
}

static bool elementIsSeenToArriveAtFusedCallback(const Walker& walker, FunctionID function, unsigned placeOfElement)
{
    if (placeOfElement >= walker.program().at(function).numberOfPositionalParameters())
        return true;
    TypeID arriving = elementTypeOfCallback(walker, function, placeOfElement);
    return arriving && !isNeverType(arriving) && !walker.types().isUninformative(arriving);
}

static bool callbackMayBeFused(const Walker& walker, FunctionID function, unsigned numberOfArguments, bool storageTellsElements)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee, FunctionFlags::HasThis };
    const Function& callee = walker.program().at(function);
    const BodyIndex* index = walker.facts().bodies().find(function);
    if (!index || !callee.hasBody() || callee.hasAny(otherForms) || isClassConstructorOf(callee.kind) || callee.numberOfPositionalParameters() > numberOfArguments || index->hasLoops())
        return false;
    bool isPure = isPureCallback(walker, function);
    if ((!isPure && walker.facts().linked().function(function).weightAsLeaf == LinkedFunction::notLeaf) || !copyOfCalleeCalledInLoopPays(walker, function, numberOfArguments) || (!storageTellsElements && !elementIsSeenToArriveAtFusedCallback(walker, function, numberOfArguments - 2)))
        return false;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        BodyBlock block = index->block(BlockID(number));
        TerminatorKind end = block.terminator().kind();
        if (block.hasHandler() || (end != TerminatorKind::Jump && end != TerminatorKind::Branch && end != TerminatorKind::Return && end != TerminatorKind::ReturnCall && end != TerminatorKind::Throw))
            return false;
        for (BodyValue value : block.values()) {
            if (!mayStandInFusedCallback(value.opcode()) && !(isPure && value.opcode() == Opcode::Call) && !callsMathFunction(walker, *index, value))
                return false;
        }
    }
    return true;
}

static FunctionID functionOfFusedCallback(Walker& walker, ValueID passed, ValueID firstCall)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID callback = index.stripCasts(passed);
    if (!callback)
        return { };
    if (!index.isParameter(callback) && index.opcodeOf(callback) == Opcode::NewClosure)
        return index.value(callback).function();
    if (!index.isParameter(callback) && index.opcodeOf(callback) == Opcode::GetModuleVariable) {
        const Variable& variable = walker.program().at(index.value(callback).as<Opcode::GetModuleVariable>().variable());
        return variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable) ? variable.function : FunctionID();
    }
    if (!(callback < firstCall))
        return { };
    Reference reference = walker.referenceOf(callback);
    Type type = walker.types().at(reference && reference.isRecord() ? walker.typeOf(reference) : TypeID(TypeTable::anything));
    return type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
}

static ValueID findReadAfterCall(const BodyIndex& index, ValueID call, uint32_t behindStretch, ValueID& result)
{
    result = call;
    uint32_t next = call.index() + 1;
    while (next < behindStretch && index.opcodeOf(ValueID(next)) == Opcode::Convert && index.value(ValueID(next)).operand(0) == result && index.onlyUsingValue(result) == ValueID(next))
        result = ValueID(next++);
    if (next >= behindStretch || index.opcodeOf(ValueID(next)) != Opcode::PropGet)
        return { };
    BodyValue read = index.value(ValueID(next));
    return !read.isFreeName() && read.key() && read.objectOperand() == result ? read.id() : ValueID();
}

static bool collectFusedChain(Walker& walker, const BodyValue& firstCall, BuiltinID firstMethod, size_t fewestLinks, FusedChain& chain)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstCall.id()));
    uint32_t behindStretch = block.firstValue().index() + std::min<uint32_t>(block.numberOfValues(), walker.endOfStretchBeingWalked());
    ValueID callID = firstCall.id();
    BuiltinID method = firstMethod;
    Reference receiver = walker.referenceOf(firstCall.thisValueOperand());
    bool isOfSubclass = receiver && receiver.isValue() && !isKnownPristine(walker, receiver) && isArraySubclassInstance(walker, walker.typeOf(receiver));
    for (;;) {
        std::span<const ValueID> arguments = index.value(callID).as<Opcode::Call>().argumentsOperands();
        std::optional<LinkOfFusedChain> link = linkOfFusedChain(table, method);
        bool isReduce = method == table.canonicalID(Builtins::ArrayPrototypeReduce) && chain.links.size() + 1 >= fewestLinks;
        bool searches = chain.links.isEmpty() && fewestLinks <= 1 && isSearch(table, method);
        bool reducesFromFirst = isReduce && arguments.size() == 1 && chain.links.isEmpty();
        bool isTaken = isReduce ? arguments.size() == 2 || reducesFromFirst : searches ? arguments.size() == 1 : link && arguments.size() == 1 && chain.links.size() < mostLinksOfFusedChain;
        if (isTaken && link == LinkOfFusedChain::Filter && chain.links.size() == 1 && chain.links[0] == LinkOfFusedChain::Map && isBooleanFunction(walker, arguments[0])) {
            chain.links.append(LinkOfFusedChain::FilterTruthy);
            chain.operations.append(callID);
            chain.lastCall = callID;
            chain.last = callID;
            return true;
        }
        ValueID result;
        if (isTaken && link == LinkOfFusedChain::Filter && chain.links.isEmpty() && fewestLinks <= 1 && isBooleanFunction(walker, arguments[0])) {
            chain.links.append(LinkOfFusedChain::FilterTruthy);
            chain.operations.append(callID);
            chain.lastCall = callID;
            chain.last = callID;
            if (ValueID read = findReadAfterCall(index, callID, behindStretch, result); read && index.value(read).key() == program.keyOf(BuiltinNames::length) && index.onlyUsingValue(result) == read) {
                for (uint32_t number = callID.index() + 1; number <= read.index(); ++number)
                    chain.operations.append(ValueID(number));
                chain.end = EndOfFusedChain::Length;
                chain.last = read;
            }
            return true;
        }
        if (chain.links.size() + 1 < fewestLinks && !findReadAfterCall(index, callID, behindStretch, result))
            return false;
        FunctionID callback = isTaken ? functionOfFusedCallback(walker, arguments[0], firstCall.id()) : FunctionID();
        if (!callback || !callbackMayBeFused(walker, callback, isReduce ? 3 : 2, isOfSubclass && (isReduce || searches)))
            return chain.links.size() >= fewestLinks;
        if (chain.callbacks.isEmpty())
            chain.firstCallback = callback;
        chain.callbacks.append(arguments[0]);
        chain.operations.append(callID);
        chain.lastCall = callID;
        chain.last = callID;
        if (isReduce) {
            chain.start = reducesFromFirst ? ValueID() : arguments[1];
            chain.end = EndOfFusedChain::Reduce;
            return true;
        }
        if (searches) {
            chain.search = method;
            chain.end = EndOfFusedChain::Search;
            return true;
        }
        chain.links.append(*link);
        if (*link == LinkOfFusedChain::Map)
            chain.lastMapping = callback;
        ValueID readAfterCall = findReadAfterCall(index, callID, behindStretch, result);
        if (!readAfterCall)
            return chain.links.size() >= fewestLinks;
        BodyValue read = index.value(readAfterCall);
        auto takeResult = [&] {
            for (uint32_t number = callID.index() + 1; number <= readAfterCall.index(); ++number)
                chain.operations.append(ValueID(number));
        };
        if (read.key() == program.keyOf(BuiltinNames::length) && index.onlyUsingValue(result) == read.id() && chain.links.size() == 1 && chain.links[0] == LinkOfFusedChain::Filter) {
            takeResult();
            chain.end = EndOfFusedChain::Length;
            chain.last = read.id();
            return true;
        }
        BuiltinID member = index.useCount(result) == 2 ? findOwnMember(program, table.at(BuiltinClassID(BuiltinClasses::Array)).prototype, read.key()) : BuiltinID();
        ValueID user = member ? index.onlyUsingValue(read.id()) : ValueID();
        if (!user || index.isParameter(user) || !block.holds(user) || index.opcodeOf(user) != Opcode::Call || user.index() >= behindStretch)
            return chain.links.size() >= fewestLinks;
        auto nextCall = index.value(user).as<Opcode::Call>();
        if (nextCall.calleeOperand() != read.id() || nextCall.thisValueOperand() != result || index.value(user).isDoneInPlace())
            return chain.links.size() >= fewestLinks;
        takeResult();
        callID = user;
        method = table.canonicalID(member);
    }
}

static bool holdsNoNumbers(const Walker& walker, const FusedChain& chain)
{
    if (!chain.firstCallback)
        return false;
    TypeID arriving = elementTypeOfCallback(walker, chain.firstCallback, chain.end == EndOfFusedChain::Reduce && chain.links.isEmpty() ? 1 : 0);
    return arriving && !isNeverType(arriving) && !walker.types().isUninformative(arriving) && !walker.types().overlap(arriving, TypeTable::number);
}

static Draft draftOfUnusedStart(const Walker& walker, const FusedChain& chain, Origin origin)
{
    TypeID arriving = elementTypeOfCallback(walker, chain.firstCallback, 1);
    if (!arriving || isNeverType(arriving) || !walker.types().at(arriving).isNumber())
        return Draft(Opcode::Undefined, origin);
    Draft zero(Opcode::Integer, origin);
    zero.setInteger(0);
    return zero;
}

static ArrayStorage storageOfAppendedArray(const Walker& walker, const FusedChain& chain)
{
    TypeID mapped = chain.lastMapping && chain.end == EndOfFusedChain::List ? walker.facts().linked().function(chain.lastMapping).result : TypeID();
    return mapped && !isNeverType(mapped) && walker.types().at(mapped).isNumber() && (walker.types().at(mapped).primitives & Primitive::FractionalNumber) ? ArrayStorage::Double : ArrayStorage::Other;
}

static uint32_t bitsOfFusedChain(const FusedChain& chain)
{
    uint32_t word = chain.links[0] == LinkOfFusedChain::Filter ? FirstLinkFilters : 0;
    if (chain.links.size() == 2 && chain.links[1] != LinkOfFusedChain::FilterTruthy)
        word |= chain.links[1] == LinkOfFusedChain::Filter ? HasSecondLink | SecondLinkFilters : HasSecondLink;
    if (chain.links.last() == LinkOfFusedChain::FilterTruthy)
        word |= FiltersTruthy;
    if (std::ranges::all_of(chain.links, [](LinkOfFusedChain link) { return link == LinkOfFusedChain::Map; }))
        word |= KeepsEveryElement;
    return word;
}

static bool isOneMethod(const FusedChain& chain)
{
    return chain.end == EndOfFusedChain::Search || chain.end == EndOfFusedChain::Length || chain.links.size() == (chain.end == EndOfFusedChain::Reduce ? 0 : 1);
}

enum CopiedForFilter : uint8_t { FilterChainToList, CountInArray, CountTruthyInArray, FilterArray, FilterTruthyArray };
enum CopiedForMap : uint8_t { MapChainToList, MapArray };
enum CopiedForReduce : uint8_t { ChainToValue, ReduceArray, ReduceArrayFromFirst };

static RuntimeFunctionID copiedFunctionAt(const BuiltinTable& table, BuiltinID method, size_t place)
{
    std::span<const BuiltinCalledFunction> copied = table.calledFunctionsOf(method);
    return place < copied.size() ? copied[place].function : RuntimeFunctionID();
}

static RuntimeFunctionID functionOfFusedChain(const BuiltinTable& table, const FusedChain& chain)
{
    BuiltinID filter = table.canonicalID(Builtins::ArrayPrototypeFilter);
    if (chain.end == EndOfFusedChain::Search)
        return copiedFunctionAt(table, chain.search, 0);
    if (chain.end == EndOfFusedChain::Reduce)
        return copiedFunctionAt(table, table.canonicalID(Builtins::ArrayPrototypeReduce), !isOneMethod(chain) ? ChainToValue : chain.start ? ReduceArray : ReduceArrayFromFirst);
    if (chain.end == EndOfFusedChain::Length)
        return copiedFunctionAt(table, filter, chain.links[0] == LinkOfFusedChain::FilterTruthy ? CountTruthyInArray : CountInArray);
    if (!isOneMethod(chain))
        return copiedFunctionAt(table, filter, FilterChainToList);
    if (chain.links[0] == LinkOfFusedChain::Map)
        return copiedFunctionAt(table, table.canonicalID(Builtins::ArrayPrototypeMap), MapArray);
    return copiedFunctionAt(table, filter, chain.links[0] == LinkOfFusedChain::FilterTruthy ? FilterTruthyArray : FilterArray);
}

static FunctionID findTextOfFusedChain(const Walker& walker, const FusedChain& chain)
{
    const Program& program = walker.program();
    RuntimeFunctionID function = functionOfFusedChain(program.builtins(), chain);
    const RuntimeFunctionEntry* held = function ? findRuntimeFunction(program, function) : nullptr;
    return held && held->function && program.at(held->function).hasBody() ? held->function : FunctionID();
}

static bool mayStandInsideFusedChain(const BodyIndex&, ValueID);

static uint32_t startOfUnfusedChain(const BodyIndex& index, const BodyBlock& block, ValueID firstCallback, ValueID firstCall)
{
    ValueID closure = firstCallback ? index.stripCasts(firstCallback) : ValueID();
    if (!closure || index.isParameter(closure) || index.opcodeOf(closure) != Opcode::NewClosure || !block.holds(closure) || !(closure < firstCall))
        return firstCall.index() - block.firstValue().index();
    for (uint32_t number = closure.index(); number < firstCall.index(); ++number) {
        ValueID user = index.onlyUsingValue(ValueID(number));
        if (!mayStandInsideFusedChain(index, ValueID(number)) || !user || firstCall < user)
            return firstCall.index() - block.firstValue().index();
    }
    return closure.index() - block.firstValue().index();
}

static bool mayStandInsideFusedChain(const BodyIndex& index, ValueID value)
{
    Opcode opcode = index.opcodeOf(value);
    if (opcode == Opcode::NewClosure || opcode == Opcode::Upcast || opcode == Opcode::GetModuleVariable || opcode == Opcode::Builtin)
        return true;
    if (opcode == Opcode::Convert) {
        DeclaredType declared = index.value(value).as<Opcode::Convert>().declared();
        return !declared.has(PositionFlags::IsClaim) && !declared.has(PositionFlags::IsInt32Promise);
    }
    return !opcodesThatStateTypes.contains(opcode) && declaredBoundInTable(opcode).isPure() && !operandsMayRunCode(opcode);
}

bool fuseChainOfCallbacks(Walker& walker, const BodyValue& firstCall, BuiltinID firstMethod, size_t fewestLinks)
{
    if (!walker.opensGuesses() || !walker.has(Mechanism::Inlining) || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::EscapeAnalysis) || walker.program().admitsReplacedBuiltins || walker.standsUnderHandler() || walker.hasStringsInBuilders()
        || walker.isInFusedChain() || walker.program().at(walker.unit().function).isCoroutine())
        return false;
    if (!firstCall.thisValueOperand())
        return false;
    FusedChain chain;
    if (!collectFusedChain(walker, firstCall, firstMethod, fewestLinks, chain))
        return false;
    Reference receiver = walker.referenceOf(firstCall.thisValueOperand());
    if (!chain.links.isEmpty() && receiver && receiver.isValue() && !isKnownPristine(walker, receiver) && isArraySubclassInstance(walker, walker.typeOf(receiver)))
        return false;
    FunctionID text = findTextOfFusedChain(walker, chain);
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstCall.id()));
    uint32_t firstValue = block.firstValue().index();
    for (uint32_t number = firstCall.id().index() + 1; number < chain.last.index(); ++number) {
        if (!chain.operations.contains(ValueID(number)) && !mayStandInsideFusedChain(index, ValueID(number)))
            return false;
    }
    if (!mayCopyFusedChain(walker, text))
        return false;

    uint32_t begin = firstCall.id().index() - firstValue;
    uint32_t behind = chain.last.index() + 1 - firstValue;
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, behind);
    for (uint32_t number = firstCall.id().index() + 1; number < chain.last.index() && !walker.isUnreachable() && !walker.hasFailed(); ++number) {
        if (!chain.operations.contains(ValueID(number)))
            walker.walkStretch(block.id(), number - firstValue, number - firstValue + 1);
    }
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    Origin origin = walker.originOf(firstCall);
    origin.isTailCall = false;
    Vector<Reference, 8> operands { walker.referenceOf(firstCall.thisValueOperand()) };
    bool takesLinks = !isOneMethod(chain);
    if (takesLinks) {
        Draft links(Opcode::Integer, origin);
        links.setInteger(bitsOfFusedChain(chain));
        operands.append(walker.constant(WTF::move(links)));
    }
    for (ValueID callback : chain.callbacks)
        operands.append(walker.referenceOf(callback));
    bool isReduce = chain.end == EndOfFusedChain::Reduce;
    if (takesLinks && operands.size() == (isReduce ? 4 : 3))
        operands.insert(3, walker.constant(Draft(Opcode::Undefined, origin)));
    if (isReduce && chain.start)
        operands.append(walker.referenceOf(chain.start));
    else if (isReduce && !takesLinks)
        operands.append(walker.constant(draftOfUnusedStart(walker, chain, origin)));
    Walker::ArmsThatMeet arms;
    BlockID asWritten;
    SetForScope exitsAtGenericLoop(walker.exitsChainAtGenericLoops(), !isReduce && (!walker.isInLoop() || holdsNoNumbers(walker, chain)));
    SetForScope storageOfResult(walker.storageOfFusedChain(), storageOfAppendedArray(walker, chain));
    walker.noteThatCopyMakesNewList(text, operands[0]);
    bool isCopied = !operands.contains(Reference()) && copyFusedChain(walker, index.value(chain.lastCall), text, operands.span(), asWritten);
    walker.noteThatCopyMakesNewList({ }, { });
    if (walker.hasFailed())
        return true;
    if (isCopied) {
        if (!walker.isUnreachable() && chain.last != chain.lastCall)
            walker.bind(chain.last, walker.referenceOf(chain.lastCall));
        walker.count(StatisticID::CallbackChainsFused);
        walker.listArm(stretch, arms);
    }
    for (ValueID operation : chain.operations) {
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation))
            walker.leaveAnythingAt(callee);
    }
    if (asWritten || !isCopied) {
        std::optional<Walker::ClosedArm> closedArm;
        if (asWritten) {
            walker.continueIn(asWritten);
            closedArm.emplace(walker, true);
        }
        SetForScope fusesNoChain(walker.fusesNoChain(), true);
        walker.walkStretch(block.id(), startOfUnfusedChain(index, block, chain.callbacks.isEmpty() ? ValueID() : chain.callbacks[0], firstCall.id()), behind);
        walker.listArm(stretch, arms);
    }
    if (!walker.hasFailed() && walker.joinArms(stretch, arms, walker.addBlock()))
        walker.walkStretch(block.id(), behind, behind);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
