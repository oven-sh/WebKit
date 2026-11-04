/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTOpcodeSets.h"
#include "AOTThinSummary.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndexTable;

// Parsing by a written type is a guess with a test of its own in front of a call of a built-in, and what it builds are typed operations. With one of the three withheld the program gets no parser and no summary lists a type.
inline bool parsersAreWrittenUnder(Mechanisms mechanisms)
{
    return mechanisms.containsAll({ Mechanism::BuiltinCalls, Mechanism::TypedOperations, Mechanism::Guesses });
}

// The type by which what the call gives is parsed. Null: none. Behind appendParsersOfTypesThatScanHasFound() a call stands once in the list.
TypeID typeParsedAt(const ThinSummary&, ValueID site);

// `const config: Config = JSON.parse(text)`, `use(await response.json())`: no record of the front end says what is parsed, and the users of the result do.
// One for each ScanOfBody. It follows what a call that may enter a parser gives through the operations that hand a value on and through one await, within the body, in the order of the stream.
class ResultsOfParsersInScan {
public:
    explicit ResultsOfParsersInScan(Mechanisms mechanisms)
        : m_isWithheld(!parsersAreWrittenUnder(mechanisms))
    {
    }

    void reset();
    // Asked of every operation of every body.
    bool looksAt(Opcode opcode) const { return !m_isWithheld && (opcode == Opcode::Call || (!m_calls.isEmpty() && operationsThatUseResults.contains(opcode))); }
    // OF EVERY CALL OF EVERY BODY, so in line and by the record of the call alone: it names a key under which a parser is found, a built-in, or a record of what is expected. A plain `f(x)` names none of the three.
    ALWAYS_INLINE bool mayFollowCall(const Program& program, const BodyOperation<Opcode::Call>& call)
    {
        if (!m_hasListedKeysOfParsers) [[unlikely]]
            listKeysOfParsers(program);
        PropertyKeyID key = call.calleeKey();
        return (key && m_keysOfParsers.contains(key)) || call.expectedResult() || call.calledBuiltin();
    }
    // Of a Call that mayFollowCall() lets by, in front of readOperation(). The record of the call says which built-in is called or under which name. Where it says neither: the callee.
    // Under a name that finds a parser on one object alone (`JSON.parse`): the receiver. Null: nothing has to be looked for.
    ValueID operandWhoseMakerIsAsked(const Program&, const BodyOperation<Opcode::Call>&);
    // `definitionOfCallee`: of a Call, what makes the operand that operandWhoseMakerIsAsked() has named, behind casts. Null: a parameter, or none was named.
    void readOperation(const Program&, const BodyValue&, const BodyValue& definitionOfCallee);
    void readReturnCall(const Program&, const BodyTerminator&);
    // Leaves ThinSummary::typesParsedAtCalls, ascending by site. A call of which several types are asked stands once for each.
    void finish(const Program&, const Function&, ThinSummary&);

private:
    static constexpr OpcodeSet operationsThatUseResults = inputOperationsThatHandOn | OpcodeSet { Opcode::Await, Opcode::PropGet, Opcode::TypeTest, Opcode::CallDirect };

    struct FollowedValue {
        ValueID value;
        uint32_t call { 0 };
        // Which built-in is called shows in Link, so what the call answers may be a promise of what is parsed.
        bool mayBePromise { false };
    };

    struct FollowedCall {
        ValueID call;
        ExpectedResultID expected;
        bool isAwaited { false };
        Vector<TypeID, 2> requestedTypes;
        Vector<PropertyKeyID, 4> namesRead;
        Vector<PropertyKeyID, 4> namesReadInFrontOfAwait;
    };

    void listKeysOfParsers(const Program&);
    bool mayCallParserByType(const Program&, const BodyOperation<Opcode::Call>&, const BodyValue& definitionOfCallee);
    const FollowedValue* find(ValueID) const;
    void request(const Program&, uint32_t call, TypeID);

    bool m_isWithheld;
    // The keys under which the program may find a parser. They are the program's, so they outlast reset().
    bool m_hasListedKeysOfParsers { false };
    Vector<PropertyKeyID, 8> m_keysOfParsers;
    Vector<PropertyKeyID, 8> m_keysFoundOnOneObjectAlone;
    // Ascending by value.
    Vector<FollowedValue, 8> m_values;
    Vector<FollowedCall, 2> m_calls;
};

// Between the scan and Link, on one thread. Of several types that are asked of one call the most specific stays, where every other takes all that it takes, and else none.
// The program gets a parser for each container type below what is left, and each parser an entry in the two tables. The summary of a parser lists what its allocations list, so that the plan has an order for it.
void appendParsersOfTypesThatScanHasFound(Program&, BodyIndexTable&, ThinSummaries&, Diagnostics&, Mechanisms);

// For WriterOfLink::forEachNodeNamedBy(). Code of a body may mention the parser of each type that it parses by, and code of a parser those of the types below its own.
void forEachParserNamedBy(const Program&, const ThinSummary&, FunctionID referrer, NOESCAPE const ScopedLambda<void(FunctionID)>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
