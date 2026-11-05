/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Known rows: what the table of built-ins says of a call that no one row was matched for: a row whose code is a function of the library, a conversion to a text, a sort.
// At a suspension: no row suspends the frame of its caller.
// Withheld: every door here stands behind BuiltinCalls. The direct call of a function of the library asks DirectCalls as well: callFunctionDirectly().

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
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkKnownCalls.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

// ---- A function of the library behind a row

// Link has not seen the call. What it says of a function whose callers it has counted does not hold of one more: callFunctionDirectly().
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

    Effects effects = entered.effectsOnReturn;
    effects.merge(entered.effectsOnThrow);
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    if (program.builtins().has(BuiltinMark::MayTerminateProcess, row.builtin))
        effects.mayBeTerminated = true;
    // The error is allocated on the way to the handler.
    effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    addVariablesThatStandForFields(walker.plan(), effects);
    return effects;
}

// Only a string among the elements is compared by its characters, and only with a string.
std::optional<Effects> effectsOfSearchForWhatIsNoString(const TypeArena& types, InlineBuiltin builtin, TypeID searchedOrNotPassed)
{
    if (builtin != InlineBuiltin::ArrayIndexOf && builtin != InlineBuiltin::ArrayIncludes)
        return std::nullopt;
    if (!searchedOrNotPassed)
        return std::nullopt;
    Type searched = types.at(searchedOrNotPassed);
    if (!searched.isJavaScriptValue() || (searched.primitives & Primitive::AnyString))
        return std::nullopt;
    return declaredBoundOf(InlineBuiltin::ArrayAt);
}

// ---- Calls that become something cheaper

// As CallInWalkBuiltins::findCallee(). Canonical.
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

// The operator folds over a constant, has a form for each kind of primitive and is a part of a concatenation.
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

// A look at a loop types the call by the rows of the built-in. The direct call is typed by what Link has proven of the function, which need not lie within that: widestTypeOfOperation().
static bool directCallLiesWithinLooksForKnownRows(Walker& walker, const BodyValue& call, FunctionID function)
{
    if (!walker.isInLoop())
        return true;
    Draft direct(Opcode::CallDirect);
    direct.setFunction(function);
    direct.setRole(walker.plan().conventions().directEntryOf(function).role);
    TypeID given = walker.resultTypeOf(direct);
    TypeID widest = widestTypeOfOperation(walker, call);
    return !widest || (given && walker.types().isSubset(given, widest));
}

// ---- A sort that calls no comparator

// `(a, b) => a - b`, `(a, b) => b - a`: the body is casts, one subtraction of one parameter from the other, and the return of that. Null: it is something else.
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

// `list.sort((a, b) => a - b)`. The library has a function that sorts a short list of numbers where it lies, without one call: on numbers nobody sees such a comparator run. It looks at the list itself, and hands every other list to sort().
// It takes the list and the comparator as its arguments. Null: the call is no such call, or the program does not hold the function.
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

bool isLeafHandedToMethodOfLists(const Walker& walker, TypeID handedOver)
{
    if (!handedOver)
        return false;
    Type type = walker.types().at(handedOver);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return only && walker.facts().linked().function(only).weightAsLeaf != LinkedFunction::notLeaf;
}

bool mayBeLeafHandedToMethodOfLists(const Walker& walker, TypeID handedOver)
{
    if (!handedOver)
        return true;
    Type type = walker.types().at(handedOver);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return !only || walker.facts().linked().function(only).weightAsLeaf != LinkedFunction::notLeaf;
}

// The function object of such a built-in is the closure of that function, so the call enters the same code with the same operands, and spares the way through the value.
bool callLibraryFunctionBehindRows(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    FunctionID function = callee ? findLibraryFunctionBehindBuiltin(walker, walker.typeOf(callee)) : FunctionID();
    // `list.map(f)`: the rows name two texts, and what the function object runs takes whatever comes. Outside of loops it is called: copied at every such call, the code of a large application grew by a quarter.
    // In a loop the text repeats as the loop does. Where `f` calls nobody the rules of copies decide: `f` goes into the run over the vector, and no closure is made on the way through it.
    // `list.sort(f)`: the function object is native code, and the function that the rows name which these operands reach does what native code does on them.
    MayCopyFunctionCalledDirectly mayCopy = MayCopyFunctionCalledDirectly::Yes;
    bool isFoundByRowsReached = false;
    bool enteredByValue = false;
    if (!function && callee) {
        LibraryFunctionsBehindBuiltin behind = findLibraryFunctionsBehindBuiltin(walker, walker.typeOf(callee));
        if (behind.enteredByValue) {
            function = behind.enteredByValue;
            enteredByValue = true;
            Reference handedOver = call.argumentsOperands().empty() ? Reference() : walker.referenceOf(call.argumentsOperands()[0]);
            // At a call that the unit's own body writes: in the copy of a callee the text would stand once more for each caller.
            mayCopy = walker.isInLoop() && !walker.depthOfCopies() && handedOver && isLeafHandedToMethodOfLists(walker, walker.typeOf(handedOver)) ? MayCopyFunctionCalledDirectly::Yes : MayCopyFunctionCalledDirectly::No;
        } else if (!behind.named.isEmpty())
            function = findLibraryFunctionOfRowsReachedBy(walker, call);
        isFoundByRowsReached = function && !behind.enteredByValue;
    }
    if (!function)
        return false;
    if (!directCallLiesWithinLooksForKnownRows(walker, call, function))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    // The rows that the call reaches are those of a list as its class makes it and a comparator that is a function.
    if (FunctionID withoutCalls = isFoundByRowsReached ? findSortThatCallsNoComparatorForKnownRows(walker, call, findBuiltinCalledForKnownRows(walker, call)) : FunctionID(); withoutCalls && directCallLiesWithinLooksForKnownRows(walker, call, withoutCalls)) {
        Reference listAndComparator[] { written.thisValueOperand(), written.argumentsOperands()[0] };
        if (callFunctionDirectly(walker, call, withoutCalls, Reference(), listAndComparator)) {
            walker.count(StatisticID::LibraryFunctionsEnteredBehindRows);
            walker.count(StatisticID::SortsThatCallNoComparator);
            return true;
        }
    }
    // The rows name the text on a list for the row of lists alone.
    FunctionID reached = enteredByValue && mayCopy == MayCopyFunctionCalledDirectly::Yes ? findLibraryFunctionOfRowsReachedBy(walker, call) : FunctionID();
    walker.noteThatCopyMakesNewList(reached && reached != function ? function : FunctionID(), written.thisValueOperand());
    bool isBuilt = callFunctionDirectly(walker, call, function, written.thisValueOperand(), written.argumentsOperands(), mayCopy);
    walker.noteThatCopyMakesNewList({ }, { });
    if (isBuilt)
        walker.count(StatisticID::LibraryFunctionsEnteredBehindRows);
    return isBuilt;
}

// ---- Reads

bool isPrivateAliasOfBuiltinMember(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::EnginePrivateName)
        return false;
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    return name && name.isPrivateAlias;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
