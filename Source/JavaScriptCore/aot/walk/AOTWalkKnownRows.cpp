/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkKnown.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

Effects effectsOfLibraryFunctionBehindRow(const Walker& walker, const BuiltinOverload& row)
{
    const Program& program = walker.program();
    const LinkTables& linked = walker.facts().linked();
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, row.libraryFunction());
    if (!entry || !entry->function || entry->function.index() >= linked.functions.size() || !program.at(entry->function).hasBody())
        return Effects::unknownCode();
    const LinkedFunction& entered = linked.function(entry->function);
    if (!entered.isReached || entered.everyCallerIsKnown)
        return Effects::unknownCode();

    Effects effects = entered.effectsOnReturn();
    effects.merge(entered.effectsOnThrow());
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    if (program.builtins().has(BuiltinMark::MayTerminateProcess, row.builtin))
        effects.mayBeTerminated = true;
    effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    return effects;
}

std::optional<Effects> effectsOfSearchForWhatIsNoString(const TypeArena& types, InlineBuiltin builtin, TypeID searchedOrNotPassed)
{
    bool isLookupInTable = builtin == InlineBuiltin::MapGet || builtin == InlineBuiltin::MapHas || builtin == InlineBuiltin::SetHas;
    if (!isLookupInTable && builtin != InlineBuiltin::ArrayIndexOf && builtin != InlineBuiltin::ArrayIncludes)
        return std::nullopt;
    if (!searchedOrNotPassed)
        return std::nullopt;
    Type searched = types.at(searchedOrNotPassed);
    if (!searched.isJavaScriptValue() || (searched.primitives & Primitive::AnyString))
        return std::nullopt;
    if (isLookupInTable)
        return searched.primitives & Primitive::BigInt ? std::nullopt : std::optional<Effects>(declaredBoundOf(InlineBuiltin::WeakMapGet));
    return declaredBoundOf(InlineBuiltin::ArrayAt);
}

static BuiltinID findBuiltinCalledForKnownRows(Walker& walker, const BodyValue& call)
{
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!named) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID read = index.valueBehindCasts(call.calleeOperand());
        bool isReadByName = read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName();
        if (isReadByName)
            named = findConstantMemberReadOff(walker, walker.referenceOf(index.value(read).objectOperand()), index.value(read).key());
    }
    return named ? walker.program().builtins().canonicalID(named) : BuiltinID();
}

static bool usesDecimalRadixForKnownRows(Walker& walker, std::span<const ValueID> arguments)
{
    if (arguments.empty())
        return true;
    Reference radix = arguments[0] ? walker.referenceOf(arguments[0]) : Reference();
    TypeID proven = radix ? walker.typeOf(radix) : TypeID();
    if (!proven)
        return false;
    if (proven == TypeTable::undefined)
        return true;
    Type type = walker.types().at(proven);
    return type.isInt32() && type.integers.isSubsetOf({ 10, 10 });
}

static bool buildToStringOfPrimitiveForKnownRows(Walker& walker, const BodyValue& call, BuiltinID callee)
{
    const BuiltinTable& table = walker.program().builtins();
    uint16_t receivers = 0;
    bool takesRadix = true;
    if (callee == table.canonicalID(Builtins::NumberPrototypeToString))
        receivers = Primitive::Number;
    else if (callee == table.canonicalID(Builtins::BigIntPrototypeToString))
        receivers = Primitive::BigInt;
    else if (callee == table.canonicalID(Builtins::BooleanPrototypeToString)) {
        receivers = Primitive::Boolean;
        takesRadix = false;
    } else
        return false;

    Reference receiver = call.thisValueOperand() ? walker.referenceOf(call.thisValueOperand()) : Reference();
    TypeID proven = receiver ? walker.typeOf(receiver) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    if (type.isNever() || !type.hasOnlyPrimitives(receivers))
        return false;
    if (takesRadix && !usesDecimalRadixForKnownRows(walker, call.argumentsOperands()))
        return false;

    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Draft conversion(Opcode::ToString, origin);
    conversion.setValueOperand(receiver);
    walker.chooseConservativeFields(conversion);
    conversion.setForm(formAllowedForTypedOperator(walker, conversion));
    conversion.inputValueGiven = call.id();
    if (Reference text = walker.build(WTF::move(conversion)))
        walker.bind(call.id(), text);
    return true;
}

bool buildToStringOfPrimitiveByRows(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    BuiltinID callee = findBuiltinCalledForKnownRows(walker, call);
    return callee && buildToStringOfPrimitiveForKnownRows(walker, call, callee);
}

static BuiltinID sortForComparatorThatIsDifferenceForKnownRows(const Walker& walker, FunctionID comparator)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest };
    const Function& function = walker.program().at(comparator);
    const BodyIndex* index = walker.facts().bodies().find(comparator);
    if (!index || !function.hasBody() || function.numberOfPositionalParameters() != 2 || function.hasAny(otherForms))
        return { };
    BodyBlock entry = index->block(entryBlock);
    unsigned firstParameter = function.firstParameterIndex();
    if (entry.hasHandler() || entry.terminator().kind() != TerminatorKind::Return || entry.numberOfParameters() < firstParameter + 2)
        return { };

    ValueID difference;
    for (BodyValue value : entry.values()) {
        if (opcodesThatStateTypes.contains(value.opcode()))
            continue;
        if (value.opcode() != Opcode::Sub || difference)
            return { };
        difference = value.id();
    }
    ValueID returned = entry.terminator().operand(0);
    if (!difference || !returned || index->valueBehindCasts(returned) != difference)
        return { };
    ValueID left = index->valueBehindCasts(index->value(difference).operand(0));
    ValueID right = index->valueBehindCasts(index->value(difference).operand(1));
    if (left == entry.parameter(firstParameter) && right == entry.parameter(firstParameter + 1))
        return Builtins::CompilerHelpersSortByDifference;
    if (left == entry.parameter(firstParameter + 1) && right == entry.parameter(firstParameter))
        return Builtins::CompilerHelpersSortByReversedDifference;
    return { };
}

static FunctionID findSortThatCallsNoComparatorForKnownRows(Walker& walker, const BodyValue& call, BuiltinID callee)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (callee != table.canonicalID(Builtins::ArrayPrototypeSort) || call.argumentsOperands().size() != 1 || !call.thisValueOperand())
        return { };
    Reference passed = walker.referenceOf(call.argumentsOperands()[0]);
    TypeID proven = passed ? walker.typeOf(passed) : TypeID();
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    FunctionID comparator = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    BuiltinID helper = comparator ? sortForComparatorThatIsDifferenceForKnownRows(walker, comparator) : BuiltinID();
    if (!helper || !table.contains(helper))
        return { };
    for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(helper))) {
        const RuntimeFunctionEntry* held = row.mode == BuiltinCallMode::Call ? findRuntimeFunction(program, row.libraryFunction()) : nullptr;
        if (held && held->function && program.at(held->function).hasBody())
            return held->function;
    }
    return { };
}

bool isBuiltinCalledWithoutReceiver(const Walker& walker, TypeID handedOver)
{
    if (!handedOver || !walker.has(Mechanism::BuiltinCalls))
        return false;
    Type type = walker.types().at(handedOver);
    BuiltinID builtin = type.isObject() && type.isJavaScriptValue() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    if (!builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    bool hasRow = false;
    for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(builtin))) {
        if (row.mode != BuiltinCallMode::Call)
            continue;
        if (row.receiver || row.isUndeclared())
            return false;
        hasRow = true;
    }
    return hasRow;
}

bool isLeafHandedToMethodOfLists(const Walker& walker, TypeID handedOver)
{
    if (!handedOver)
        return false;
    if (isBuiltinCalledWithoutReceiver(walker, handedOver))
        return true;
    Type type = walker.types().at(handedOver);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return only && walker.facts().linked().function(only).weightAsLeaf != LinkedFunction::notLeaf;
}

static bool loopReadsOffResultFirstForKnownRows(Walker& walker, ValueID value)
{
    static constexpr OpcodeSet readersOfFirstOperand { Opcode::PropGet, Opcode::PropHas, Opcode::ElemGet, Opcode::ElemHas };
    const BodyIndex& index = walker.currentBodyIndex();
    for (;;) {
        std::optional<BodyUser> user = index.firstUser(value);
        if (!user || user->kind != BodyUseKind::Operand)
            return false;
        BodyValue reader = index.value(user->value);
        if (opcodesThatStateTypes.contains(reader.opcode())) {
            value = user->value;
            continue;
        }
        if ((reader.opcode() == Opcode::IteratorOpen && reader.operand(0) == value) || (reader.opcode() == Opcode::AppendSpread && reader.operand(1) == value))
            return true;
        return walker.isInLoop() && readersOfFirstOperand.contains(reader.opcode()) && reader.operand(0) == value;
    }
}

static void holdResultToRowsForKnownRows(Walker& walker, const BodyValue& call, TypeID byRows)
{
    TypeArena& types = walker.types();
    Reference result = walker.isUnreachable() || walker.hasFailed() ? Reference() : walker.referenceOf(call.id());
    if (!byRows || !result || !result.isValue())
        return;
    TypeID own = walker.typeOf(result);
    if (!types.at(byRows).isJavaScriptValue() || !types.at(own).isJavaScriptValue() || types.at(byRows).anyObject || types.isSubset(own, byRows))
        return;
    TypeID tested = walker.typeThatUnitMayTestFor(types.testablePartOf(byRows));
    if (types.isSubset(own, tested))
        return;
    Reference held = walker.proveOrTrap(result, tested, ProofSource::ResultOfCall, walker.originOf(call));
    if (!held)
        return;
    walker.bind(call.id(), held);
    walker.count(StatisticID::ResultsOfLibraryFunctionsHeldToRows);
}

bool callLibraryFunctionBehindRows(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    FunctionID function = callee ? findLibraryFunctionBehindBuiltin(walker, walker.typeOf(callee)) : FunctionID();
    MayCopyFunctionCalledDirectly mayCopy = MayCopyFunctionCalledDirectly::Yes;
    bool isFoundByRowsReached = false;
    bool enteredByValue = false;
    if (!function && callee) {
        LibraryFunctionsBehindBuiltin behind = findLibraryFunctionsBehindBuiltin(walker, walker.typeOf(callee));
        bool mayGoBySpecialCase = behind.enteredByValue && behind.someSpecialCaseNamesFunction;
        FunctionID ofSpecialCase = mayGoBySpecialCase ? findLibraryFunctionOfRowsReachedBy(walker, call, RowsThatNameLibraryFunction::SpecialCasesAlone) : FunctionID();
        if (ofSpecialCase && ofSpecialCase != behind.enteredByValue)
            function = ofSpecialCase;
        else if (behind.enteredByValue) {
            if (mayGoBySpecialCase && openGuessAtOperandThatSpecialCaseTakesAsMade(walker, call, behind.enteredByValue))
                return true;
            function = behind.enteredByValue;
            enteredByValue = true;
            Reference handedOver = call.argumentsOperands().empty() ? Reference() : walker.referenceOf(call.argumentsOperands()[0]);
            Reference receiver = call.thisValueOperand() ? walker.referenceOf(call.thisValueOperand()) : Reference();
            bool isListAsItWasMade = receiver && (isListAsMade(walker, receiver) || receiverStaysAsMadeByLink(walker, call));
            mayCopy = ((walker.isInLoop() || walker.bodyAtWorkCallsItself()) ? isListAsItWasMade || walker.walksOwnBodyOfUnit() : isListAsItWasMade && isReadOutOfVariableOfFileThatStays(walker, call.thisValueOperand())) && handedOver && isLeafHandedToMethodOfLists(walker, walker.typeOf(handedOver)) ? MayCopyFunctionCalledDirectly::Yes : MayCopyFunctionCalledDirectly::No;
        } else if (!behind.named.isEmpty())
            function = findLibraryFunctionOfRowsReachedBy(walker, call);
        isFoundByRowsReached = function && function != behind.enteredByValue;
    }
    if (!function)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    if (FunctionID withoutCalls = isFoundByRowsReached ? findSortThatCallsNoComparatorForKnownRows(walker, call, findBuiltinCalledForKnownRows(walker, call)) : FunctionID()) {
        Reference listAndComparator[] { written.thisValueOperand(), written.argumentsOperands()[0] };
        if (callFunctionDirectly(walker, call, withoutCalls, Reference(), listAndComparator)) {
            walker.count(StatisticID::LibraryFunctionsEnteredBehindRows);
            walker.count(StatisticID::SortsThatCallNoComparator);
            return true;
        }
    }
    TypeID resultByRows = loopReadsOffResultFirstForKnownRows(walker, call.id()) ? findTypeOfResultOfRowsReachedBy(walker, call) : TypeID();
    FunctionID reached = enteredByValue && mayCopy == MayCopyFunctionCalledDirectly::Yes ? findLibraryFunctionOfRowsReachedBy(walker, call) : FunctionID();
    bool makesNewList = reached && reached != function;
    FunctionID calledWhereNoCopyIsMade;
    if (const BodyIndex* ofRows = makesNewList ? walker.facts().bodies().find(reached) : nullptr; ofRows && ofRows->opcodes().contains(Opcode::HasContiguousIndex))
        calledWhereNoCopyIsMade = std::exchange(function, reached);
    walker.noteThatCopyMakesNewList(makesNewList ? function : FunctionID(), written.thisValueOperand());
    bool isBuilt = callFunctionDirectly(walker, call, function, written.thisValueOperand(), written.argumentsOperands(), mayCopy, calledWhereNoCopyIsMade);
    walker.noteThatCopyMakesNewList({ }, { });
    if (isBuilt) {
        walker.count(StatisticID::LibraryFunctionsEnteredBehindRows);
        holdResultToRowsForKnownRows(walker, call, resultByRows);
    }
    return isBuilt;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool isPrivateAliasOfBuiltinMember(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::EnginePrivateName)
        return false;
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    return name && name.isPrivateAlias;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
