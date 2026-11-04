/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A call whose result is made by a written type: `JSON.parse(text) as Config`, `await response.json()`, the rows of a query.
// ParsedText: the parser of the written type at the call. ParsedBody: nothing at the call, the host's parser makes records that fit. Rows: the call as written, and behind it the word to the host which layout its rows have.
// Every refusal stands in front of the first build(). At a suspension: none lies between what is built here. At a throw: as the call. Withheld with BuiltinCalls, TypedOperations or Guesses.

#include "config.h"
#include "AOTWalkStringParsers.h"

#if ENABLE(AOT)

#include "AOTLinkStringParsers.h"

namespace JSC { namespace AOT { namespace Typed {

// A parse by the type saves the generic parser's look at each name and each value, and every read behind it finds a compiled object. As a declared guess outside a loop has to save.
static constexpr uint32_t numberOfOperationsThatParseByTypeStandsFor = 2;

static bool typedResultsAreWithheld(const Walker& walker)
{
    return !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses);
}

static ExpectedResultID expectedResultWrittenAtCallForTypedResults(const BodyValue& call)
{
    if (call.opcode() == Opcode::Call)
        return call.as<Opcode::Call>().expectedResult();
    if (call.opcode() == Opcode::CallList)
        return call.as<Opcode::CallList>().expectedResult();
    return { };
}

TypeID typeParsedAtCall(const Walker& walker, const BodyValue& call)
{
    TypeID found = typeParsedAt(walker.facts().summaries().at(walker.currentFunction()), call.id());
    if (!found)
        return { };
    ExpectedResultID written = expectedResultWrittenAtCallForTypedResults(call);
    return !written || walker.program().at(written).type == found ? found : TypeID();
}

Reference buildCallByOneRowAsWritten(Walker& walker, const CallByOneRow& row)
{
    Draft call(Opcode::CallBuiltin, row.origin);
    walker.chooseConservativeFields(call);
    call.setBuiltinCallForm(BuiltinCallForm::Whole);
    call.setOverload(row.overload);
    if (row.receiver)
        call.setReceiverOperand(row.receiver);
    call.setArgumentsOperands(row.arguments);
    call.setIsDoneInPlace(row.call.opcode() == Opcode::Call && row.call.isDoneInPlace());
    call.expected = row.expected;
    call.inputValueGiven = row.call.id();
    return walker.build(WTF::move(call));
}

// The parser that the call would enter. What it may make is asked layout by layout (hasTypedPartForParser()); then whether the queue comes to its unit from this one, and whether there is such a unit.
static FunctionID parserToCallForTypedResults(const Walker& walker, const BodyValue& call)
{
    const Program& program = walker.program();
    TypeID type = typeParsedAtCall(walker, call);
    FunctionID parser = type ? parserFunctionOf(program, type) : FunctionID();
    if (!parser || !hasTypedPartForParser({ program, walker.facts(), walker.plan() }, ParserNodeRef { type }))
        return { };
    const LinkTables& linked = walker.facts().linked();
    if (!linked.function(parser).isReached || !linked.mayBeMentionedBy(walker.unit().function, parser) || !walker.plan().conventions().exists(CodeUnit { parser, CodeRole::Body }))
        return { };
    return parser;
}

TypeID typeOfTextParsedByTypeAtCall(const Walker& walker, const BodyValue& call, BuiltinID builtin, BuiltinOperand position)
{
    if (typedResultsAreWithheld(walker))
        return { };
    bool isTextOfSomeRow = false;
    for (const BuiltinOverload& overload : walker.program().builtins().overloadsOf(builtin))
        isTextOfSomeRow |= overload.mode == BuiltinCallMode::Call && overload.result.typedResultForm == TypedResultForm::ParsedText && overload.result.typedResultSource == position;
    return isTextOfSomeRow && parserToCallForTypedResults(walker, call) ? TypeID(TypeTable::string) : TypeID();
}

static bool buildParseOfTextForTypedResults(Walker& walker, const CallByOneRow& row, const BuiltinResult& result)
{
    if (!walker.opensGuesses() || !result.typedResultSource)
        return false;
    BuiltinOperand source = *result.typedResultSource;
    Reference text = row.receiver;
    if (!source.isReceiver())
        text = source.argumentIndex() < row.arguments.size() ? row.arguments[source.argumentIndex()] : Reference();
    // The run time also opens the bytes of a Uint8Array, which JSON.parse() converts to a string like any other object. The row has converted the operand.
    if (!text || !walker.types().isSubset(walker.typeOf(text), TypeTable::string))
        return false;

    FunctionID parser = parserToCallForTypedResults(walker, row.call);
    if (!parser || !shouldOpenGuess(walker, text, typeParsedAtCall(walker, row.call), true, numberOfOperationsThatParseByTypeStandsFor))
        return false;

    walker.noteGuessOpened();
    walker.noteTypedResultSelected();
    if (Reference parsed = buildParseByTypeAtCall(walker, row, text, parser))
        walker.bind(row.call.id(), parsed);
    return true;
}

bool buildTypedResultOfCallByOneRow(Walker& walker, const CallByOneRow& row)
{
    if (typedResultsAreWithheld(walker) || row.mode != BuiltinCallMode::Call || walker.isUnreachable())
        return false;
    const BuiltinResult& result = walker.program().builtins().at(row.overload).result;
    switch (result.typedResultForm) {
    case TypedResultForm::ParsedText:
        return buildParseOfTextForTypedResults(walker, row, result);
    // The host's parser makes records that fit, and callQueryThatExpectsRows() has had its look at the call.
    case TypedResultForm::None:
    case TypedResultForm::ParsedBody:
    case TypedResultForm::Rows:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `await sql<User[]>..`: the record of the front end says what the result settles to. The layout of the rows, where the host may make them. Null: it makes plain objects, as without the word.
static LayoutID layoutOfExpectedRowsForTypedResults(const Walker& walker, const BodyValue& call)
{
    const Program& program = walker.program();
    ExpectedResultID written = call.as<Opcode::Call>().expectedResult();
    if (!written || !program.at(written).flags.contains(ExpectedResultFlags::IsRowsOfQuery))
        return { };
    const ExpectedResult& record = program.at(written);
    if (typeParsedAtCall(walker, call) != record.type)
        return { };
    LayoutID layout = layoutOfObjectsInOnlyArrayOf(program, record.type);
    if (!layout || !record.layouts.contains(layout))
        return { };
    return parsersMayAllocate({ program, walker.facts(), walker.plan() }, layout) ? layout : LayoutID();
}

// Whoever is called under the signature may answer with anything: the run time looks at what it is handed. The word stands behind the call, so the call is no tail call.
bool callQueryThatExpectsRowsOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || !call.as<Opcode::Call>().expectedResult() || typedResultsAreWithheld(walker))
        return false;
    LayoutID layout = layoutOfExpectedRowsForTypedResults(walker, call);
    if (!layout)
        return false;

    Draft query = walker.draftOf(call);
    if (walker.isUnreachable())
        return true;
    walker.chooseConservativeFields(query);
    query.isTailCall = false;
    query.resultIsReturnedAtOnce = false;
    Origin origin = query.origin();
    Reference target = walker.build(WTF::move(query));
    if (!target)
        return true;
    Draft expectation(Opcode::ExpectRows, origin);
    walker.chooseConservativeFields(expectation);
    expectation.setTargetOperand(target);
    expectation.setLayout(layout);
    walker.build(WTF::move(expectation));
    walker.bind(call.id(), target);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
