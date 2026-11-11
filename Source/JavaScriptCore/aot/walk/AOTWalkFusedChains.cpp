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
enum class EndOfFusedChain : uint8_t { List, Reduce, Length };

enum KindOfFusedChain : uint32_t { FirstLinkFilters = 1, HasSecondLink = 2, SecondLinkFilters = 4, FiltersTruthy = 8, CountsAlone = 16 };

struct FusedChain {
    Vector<LinkOfFusedChain, 2> links;
    EndOfFusedChain end { EndOfFusedChain::List };
    Vector<ValueID, 8> operations;
    Vector<ValueID, 4> callbacks;
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

static bool elementIsSeenToArriveAtFusedCallback(const Walker& walker, FunctionID function, unsigned placeOfElement)
{
    const Function& callee = walker.program().at(function);
    if (placeOfElement >= callee.numberOfPositionalParameters())
        return true;
    std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(function);
    size_t place = callee.firstParameterIndex() + placeOfElement;
    TypeID arriving = place < parameters.size() ? parameters[place].writtenOrSeenToArrive() : TypeID();
    return arriving && !isNeverType(arriving) && !walker.types().isUninformative(arriving);
}

static bool callbackMayBeFused(const Walker& walker, FunctionID function, unsigned numberOfArguments)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee, FunctionFlags::HasThis };
    const Function& callee = walker.program().at(function);
    const BodyIndex* index = walker.facts().bodies().find(function);
    if (!index || !callee.hasBody() || callee.hasAny(otherForms) || isClassConstructorOf(callee.kind) || callee.numberOfPositionalParameters() > numberOfArguments || index->hasLoops())
        return false;
    if (walker.facts().linked().function(function).weightAsLeaf == LinkedFunction::notLeaf || !copyOfCalleeCalledInLoopPays(walker, function, numberOfArguments) || !elementIsSeenToArriveAtFusedCallback(walker, function, numberOfArguments - 1))
        return false;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        BodyBlock block = index->block(BlockID(number));
        TerminatorKind end = block.terminator().kind();
        if (block.hasHandler() || (end != TerminatorKind::Jump && end != TerminatorKind::Branch && end != TerminatorKind::Return && end != TerminatorKind::Throw))
            return false;
        for (BodyValue value : block.values()) {
            if (!mayStandInFusedCallback(value.opcode()))
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

static bool collectFusedChain(Walker& walker, const BodyValue& firstCall, BuiltinID firstMethod, FusedChain& chain)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstCall.id()));
    uint32_t behindStretch = block.firstValue().index() + std::min<uint32_t>(block.numberOfValues(), walker.endOfStretchBeingWalked());
    ValueID callID = firstCall.id();
    BuiltinID method = firstMethod;
    for (;;) {
        std::span<const ValueID> arguments = index.value(callID).as<Opcode::Call>().argumentsOperands();
        std::optional<LinkOfFusedChain> link = linkOfFusedChain(table, method);
        bool isReduce = method == table.canonicalID(Builtins::ArrayPrototypeReduce) && !chain.links.isEmpty();
        bool isTaken = isReduce ? arguments.size() == 2 : link && arguments.size() == 1 && chain.links.size() < mostLinksOfFusedChain;
        if (isTaken && link == LinkOfFusedChain::Filter && chain.links.size() == 1 && chain.links[0] == LinkOfFusedChain::Map && isBooleanFunction(walker, arguments[0])) {
            chain.links.append(LinkOfFusedChain::FilterTruthy);
            chain.operations.append(callID);
            chain.lastCall = callID;
            chain.last = callID;
            return true;
        }
        FunctionID callback = isTaken ? functionOfFusedCallback(walker, arguments[0], firstCall.id()) : FunctionID();
        if (!callback || !callbackMayBeFused(walker, callback, isReduce ? 2 : 1))
            return chain.links.size() > 1;
        chain.callbacks.append(arguments[0]);
        chain.operations.append(callID);
        chain.lastCall = callID;
        chain.last = callID;
        if (isReduce) {
            chain.start = arguments[1];
            chain.end = EndOfFusedChain::Reduce;
            return true;
        }
        chain.links.append(*link);
        ValueID result = callID;
        uint32_t next = result.index() + 1;
        while (next < behindStretch && index.opcodeOf(ValueID(next)) == Opcode::Convert && index.value(ValueID(next)).operand(0) == result && index.onlyUsingValue(result) == ValueID(next))
            result = ValueID(next++);
        if (next >= behindStretch || index.opcodeOf(ValueID(next)) != Opcode::PropGet)
            return chain.links.size() > 1;
        BodyValue read = index.value(ValueID(next));
        if (read.isFreeName() || !read.key() || read.objectOperand() != result)
            return chain.links.size() > 1;
        auto takeResult = [&] {
            for (uint32_t number = callID.index() + 1; number <= next; ++number)
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
            return chain.links.size() > 1;
        auto nextCall = index.value(user).as<Opcode::Call>();
        if (nextCall.calleeOperand() != read.id() || nextCall.thisValueOperand() != result || index.value(user).isDoneInPlace())
            return chain.links.size() > 1;
        takeResult();
        callID = user;
        method = table.canonicalID(member);
    }
}

static uint32_t linksAsStringWord(const FusedChain& chain)
{
    uint32_t word = chain.links[0] == LinkOfFusedChain::Filter ? FirstLinkFilters : 0;
    if (chain.links.size() == 2 && chain.links[1] != LinkOfFusedChain::FilterTruthy)
        word |= chain.links[1] == LinkOfFusedChain::Filter ? HasSecondLink | SecondLinkFilters : HasSecondLink;
    if (chain.links.last() == LinkOfFusedChain::FilterTruthy)
        word |= FiltersTruthy;
    return chain.end == EndOfFusedChain::Length ? word | CountsAlone : word;
}

static FunctionID findTextOfFusedChain(const Walker& walker, const FusedChain& chain)
{
    const Program& program = walker.program();
    ASCIILiteral name = chain.end == EndOfFusedChain::Reduce ? "ArrayPrototypeChainToValue"_s : "ArrayPrototypeChainToList"_s;
    std::span<const RuntimeFunctionData> functions = program.builtins().runtimeFunctions();
    for (size_t place = 0; place < functions.size(); ++place) {
        std::span<const char> token = builtinTableRuntimeFunctionTokens[place];
        if (!std::ranges::equal(token, std::span<const char>(name.characters(), name.length())))
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, functions[place].id);
        return held && held->function && program.at(held->function).hasBody() ? held->function : FunctionID();
    }
    return { };
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

bool fuseChainOfCallbacks(Walker& walker, const BodyValue& firstCall, BuiltinID firstMethod)
{
    if (!walker.opensGuesses() || !walker.has(Mechanism::Inlining) || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::EscapeAnalysis) || walker.program().admitsReplacedBuiltins || walker.standsUnderHandler() || walker.hasStringsInBuilders()
        || walker.isInFusedChain() || walker.program().at(walker.unit().function).isCoroutine())
        return false;
    FusedChain chain;
    if (!collectFusedChain(walker, firstCall, firstMethod, chain))
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
    Draft links(Opcode::Integer, origin);
    links.setInteger(linksAsStringWord(chain));
    Vector<Reference, 8> operands { walker.referenceOf(firstCall.thisValueOperand()), walker.constant(WTF::move(links)) };
    for (ValueID callback : chain.callbacks)
        operands.append(walker.referenceOf(callback));
    bool isReduce = chain.end == EndOfFusedChain::Reduce;
    if (chain.callbacks.size() == (isReduce ? 2 : 1))
        operands.insert(3, walker.constant(Draft(Opcode::Undefined, origin)));
    if (isReduce)
        operands.append(walker.referenceOf(chain.start));
    Walker::ArmsThatMeet arms;
    BlockID asWritten;
    bool isCopied = !operands.contains(Reference()) && copyFusedChain(walker, index.value(chain.lastCall), text, operands.span(), asWritten);
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
        walker.walkStretch(block.id(), begin, behind);
        walker.listArm(stretch, arms);
    }
    if (!walker.hasFailed() && walker.joinArms(stretch, arms, walker.addBlock()))
        walker.walkStretch(block.id(), behind, behind);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
