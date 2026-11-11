/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTByteLiterals.h"
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

OperatorForm operatorFormFor(Walker&, const Draft&);

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
    if (!isLookupInTable(builtin) && builtin != InlineBuiltin::ArrayIndexOf && builtin != InlineBuiltin::ArrayIncludes)
        return std::nullopt;
    if (!searchedOrNotPassed)
        return std::nullopt;
    Type searched = types.at(searchedOrNotPassed);
    if (!searched.isJavaScriptValue() || (searched.primitives & Primitive::AnyString))
        return std::nullopt;
    if (isLookupInTable(builtin))
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
        ValueID read = index.stripCasts(call.calleeOperand());
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
    conversion.setForm(operatorFormFor(walker, conversion));
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
    if (!difference || !returned || index->stripCasts(returned) != difference)
        return { };
    ValueID left = index->stripCasts(index->value(difference).operand(0));
    ValueID right = index->stripCasts(index->value(difference).operand(1));
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

bool isBuiltinCalledWithoutReceiver(const Walker& walker, TypeID argumentType)
{
    if (!argumentType || !walker.has(Mechanism::BuiltinCalls))
        return false;
    Type type = walker.types().at(argumentType);
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

bool isLeafHandedToMethodOfLists(const Walker& walker, TypeID argumentType)
{
    if (!argumentType)
        return false;
    if (isBuiltinCalledWithoutReceiver(walker, argumentType))
        return true;
    Type type = walker.types().at(argumentType);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return only && walker.facts().linked().function(only).weightAsLeaf != LinkedFunction::notLeaf;
}

static bool passesOwnClosure(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID handed = index.stripCasts(call.argumentsOperands()[0]);
    return handed && !index.isParameter(handed) && index.opcodeOf(handed) == Opcode::NewClosure && index.hasOneUse(handed);
}

static bool chainBeginsAtKeptList(Walker& walker, ValueID list)
{
    static constexpr unsigned mostLinks = 8;
    const BodyIndex& index = walker.currentBodyIndex();
    for (unsigned link = 0; list && link < mostLinks; ++link) {
        if (isReadOutOfVariableOfFileThatStays(walker, list))
            return true;
        ValueID made = index.stripCasts(list);
        if (index.isParameter(made) || index.opcodeOf(made) != Opcode::Call)
            return false;
        list = index.value(made).as<Opcode::Call>().thisValueOperand();
    }
    return false;
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

static bool numbersAreSeenToArriveAtFunctionHandedForKnownRows(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto arguments = call.as<Opcode::Call>().argumentsOperands();
    ValueID handed = arguments.empty() ? ValueID() : index.stripCasts(arguments[0]);
    if (!handed || index.isParameter(handed) || index.opcodeOf(handed) != Opcode::NewClosure)
        return false;
    TypeArena& types = walker.types();
    return std::ranges::any_of(walker.facts().linked().parametersOf(index.value(handed).function()), [&](const LinkedParameter& parameter) {
        TypeID arriving = parameter.writtenOrSeenToArrive();
        TypeID there = arriving ? types.withoutNullish(arriving) : TypeID();
        return there && !isNeverType(there) && types.at(there).isNumber();
    });
}

static bool runOverBoxedValuesTakesResultFirstForKnownRows(Walker& walker, ValueID value)
{
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
        if (reader.opcode() != Opcode::PropGet || reader.operand(0) != value)
            return false;
        ValueID taker = index.onlyUsingValue(reader.id());
        if (!taker || index.opcodeOf(taker) != Opcode::Call || index.value(taker).as<Opcode::Call>().calleeOperand() != reader.id())
            return false;
        if (numbersAreSeenToArriveAtFunctionHandedForKnownRows(walker, index.value(taker)))
            return false;
        BuiltinID member = findMemberOnObjectAsMade(walker, walker.types().builtinClass(BuiltinClasses::Array), reader.key());
        FunctionID entered = member ? findLibraryFunctionsBehindBuiltin(walker, walker.types().forBuiltin(walker.program().builtins().canonicalID(member))).enteredByValue : FunctionID();
        const BodyIndex* text = entered ? walker.facts().bodies().find(entered) : nullptr;
        return text && text->opcodes().contains(Opcode::HasContiguousIndex);
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
    walker.count(StatisticID::LibraryResultsCheckedAgainstSignatures);
}

static std::optional<ByteLiteralKind> byteLiteralKindOf(const BuiltinTable& table, BuiltinID callee)
{
    auto isCallee = [&](BuiltinID named) {
        return table.contains(named) && table.canonicalID(named) == callee;
    };
    if (isCallee(Builtins::BufferFrom))
        return ByteLiteralKind::BufferFrom;
    if (isCallee(Builtins::Uint8ArrayFromBase64))
        return ByteLiteralKind::Uint8ArrayFromBase64;
    if (isCallee(Builtins::Uint8ArrayFromHex))
        return ByteLiteralKind::Uint8ArrayFromHex;
    return std::nullopt;
}

bool buildByteLiteralOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Folding))
        return false;
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.empty() || arguments.size() > 2)
        return false;
    BuiltinID callee = findBuiltinCalledForKnownRows(walker, call);
    std::optional<ByteLiteralKind> kind = callee ? byteLiteralKindOf(walker.program().builtins(), callee) : std::nullopt;
    bool hasEncoding = arguments.size() == 2;
    if (!kind || (hasEncoding && *kind != ByteLiteralKind::BufferFrom))
        return false;
    StringID string = textKnownForStrings(walker, walker.referenceOf(arguments[0]));
    StringID encoding = hasEncoding ? textKnownForStrings(walker, walker.referenceOf(arguments[1])) : StringID();
    if (!string || (hasEncoding && !encoding))
        return false;
    const HostStringEncoders* host = walker.stringEncoders();
    if (!encodeByteLiteral(walker.types(), host ? *host : HostStringEncoders { }, *kind, string, encoding))
        return false;
    BuiltinOverloadID signature = findMatchingOverload(walker, call);
    if (!signature)
        return false;

    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;
    Vector<Reference, 2> strings { walker.constant(PrimitiveKnownToFolding::text(string)) };
    if (hasEncoding)
        strings.append(walker.constant(PrimitiveKnownToFolding::text(encoding)));
    uint32_t parameters[] { static_cast<uint32_t>(*kind) };
    Draft literal(Opcode::CallInline, origin);
    walker.chooseConservativeFields(literal);
    literal.setInlineBuiltin(InlineBuiltin::ByteLiteral);
    literal.setInlineParameters(parameters);
    literal.setOverload(signature);
    literal.setArgumentsOperands(strings.span());
    literal.expected = written.expected;
    literal.inputValueGiven = call.id();
    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    Reference array = walker.build(WTF::move(literal), &effects);
    if (!array)
        return false;
    walker.bind(call.id(), array);
    walker.count(StatisticID::ByteLiteralsEncoded);
    return true;
}
static bool fillsArrayWithDoubles(Walker& walker, const BodyValue& call)
{
    std::span<const ValueID> arguments = call.argumentsOperands();
    const BodyIndex& index = walker.currentBodyIndex();
    if (arguments.size() != 2 || !arguments[0] || !arguments[1] || !walker.has(Mechanism::TypedOperations))
        return false;
    ValueID element = index.stripCasts(arguments[1]);
    Reference length = walker.referenceOf(arguments[0]);
    if (!element || index.isParameter(element) || index.opcodeOf(element) != Opcode::Integer || !length || !length.isRecord() || !walker.types().at(walker.typeOf(length)).isInteger())
        return false;
    if (findBuiltinCalledForKnownRows(walker, call) != walker.program().builtins().builtinFromToken("CompilerHelpersFilledArray"_s))
        return false;
    const LinkedContentsAtOperation* stored = walker.facts().linked().contentsAtOperation(walker.currentFunction(), call.id());
    return stored && !isNeverType(stored->value) && arrayStorageHolding(walker.types(), stored->value) == ArrayStorage::Double && (walker.types().at(stored->value).primitives & Primitive::FractionalNumber);
}

static bool buildArrayFilledWithDoubles(Walker& walker, const BodyValue& call, FunctionID function, const Draft& written)
{
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Draft list(Opcode::NewArrayLike, origin);
    walker.chooseConservativeFields(list);
    list.setLengthOperand(walker.referenceOf(call.argumentsOperands()[0]));
    list.setArrayStorage(ArrayStorage::Double);
    Reference made = walker.build(WTF::move(list));
    if (!made || !made.isValue() || walker.isUnreachable() || walker.hasFailed())
        return true;
    Draft fill(Opcode::TryFillNewArray, origin);
    walker.chooseConservativeFields(fill);
    fill.setArrayOperand(made);
    fill.setValueOperand(walker.referenceOf(call.argumentsOperands()[1]));
    Reference isFilled = walker.build(WTF::move(fill));
    if (!isFilled)
        return true;
    BlockID filled = walker.addBlock();
    BlockID otherwise = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isFilled, filled, otherwise);
    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(filled);
    if (!walker.isUnreachable())
        arms.append(Walker::JoinArm { filled, { made } });
    walker.continueIn(otherwise);
    if (!walker.isUnreachable()) {
        if (!callFunctionDirectly(walker, call, function, written.thisValueOperand(), written.argumentsOperands(), MayCopyFunctionCalledDirectly::No))
            return false;
        if (Reference called = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id()))
            arms.append(Walker::JoinArm { walker.currentBlock(), { called } });
    }
    Reference array;
    if (!arms.isEmpty() && walker.joinArms(arms.span(), walker.addBlock(), singleElementSpan(array)))
        walker.bind(call.id(), array);
    walker.count(StatisticID::ArraysFilledWithDoubles);
    return true;
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
            Reference firstArgument = call.argumentsOperands().empty() ? Reference() : walker.referenceOf(call.argumentsOperands()[0]);
            Reference receiver = call.thisValueOperand() ? walker.referenceOf(call.thisValueOperand()) : Reference();
            bool isListAsItWasMade = receiver && (isKnownPristine(walker, receiver) || receiverStaysAsMadeByLink(walker, call));
            mayCopy = ((walker.isInLoop() || walker.isCurrentFrameRecursive()) ? isListAsItWasMade || walker.walksOwnBodyOfUnit() : isListAsItWasMade && chainBeginsAtKeptList(walker, call.thisValueOperand())) && firstArgument && (isLeafHandedToMethodOfLists(walker, walker.typeOf(firstArgument)) || (walker.isInLoop() && passesOwnClosure(walker, call))) ? MayCopyFunctionCalledDirectly::Yes : MayCopyFunctionCalledDirectly::No;
            if (mayCopy == MayCopyFunctionCalledDirectly::Yes && fuseChainOfCallbacks(walker, call, findBuiltinCalledForKnownRows(walker, call)))
                return true;
            if (mayCopy == MayCopyFunctionCalledDirectly::Yes && !isLeafHandedToMethodOfLists(walker, walker.typeOf(firstArgument)))
                walker.count(StatisticID::ArrayCallbacksInlinedWithCalls);
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
            walker.count(StatisticID::LibraryFunctionsCalledDirectly);
            walker.count(StatisticID::SortComparatorCallsElided);
            return true;
        }
    }
    if (fillsArrayWithDoubles(walker, call) && buildArrayFilledWithDoubles(walker, call, function, written))
        return true;
    TypeID resultByRows = loopReadsOffResultFirstForKnownRows(walker, call.id()) ? findTypeOfResultOfRowsReachedBy(walker, call) : TypeID();
    FunctionID reached = enteredByValue && mayCopy == MayCopyFunctionCalledDirectly::Yes ? findLibraryFunctionOfRowsReachedBy(walker, call) : FunctionID();
    bool makesNewList = reached && reached != function;
    FunctionID calledWhereNoCopyIsMade;
    if (const BodyIndex* ofRows = makesNewList ? walker.facts().bodies().find(reached) : nullptr; ofRows && ofRows->opcodes().contains(Opcode::HasContiguousIndex))
        calledWhereNoCopyIsMade = std::exchange(function, reached);
    walker.noteThatCopyMakesNewList(makesNewList ? function : FunctionID(), written.thisValueOperand(), runOverBoxedValuesTakesResultFirstForKnownRows(walker, call.id()));
    bool isBuilt = callFunctionDirectly(walker, call, function, written.thisValueOperand(), written.argumentsOperands(), mayCopy, calledWhereNoCopyIsMade);
    walker.noteThatCopyMakesNewList({ }, { });
    if (isBuilt) {
        walker.count(StatisticID::LibraryFunctionsCalledDirectly);
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
