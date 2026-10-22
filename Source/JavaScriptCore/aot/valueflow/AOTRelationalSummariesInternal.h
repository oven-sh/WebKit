/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTRelationalSummaries.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// What the parts of the link share.
class RelationalLink {
public:
    RelationalLink(const Program& program, const FlowResults& flow, const RelationalSummaries& linked)
        : program(program)
        , flow(flow)
        , linked(linked)
    {
    }

    const Program& program;
    const FlowResults& flow;
    // Holds the answers for every function that was visited before. Of a function of the cycle that is being visited it holds what the last visit found, which only grows.
    const RelationalSummaries& linked;
};

// The origins of all the results of one function.
bool linkResultOrigins(const RelationalLink&, FunctionID, std::span<const LinkedFate> fatesOfSubjects, Vector<ResultOrigin>& origins);
// When every function has been visited for the last time: keeps what a reader at a call can learn that flow does not say of every call at once: a result that names a parameter, and a result that may be an object which nobody else holds.
void keepOriginsThatDependOnTheCall(Vector<ResultOrigin>& origins);
// One result before flow has run.
ResultOrigin originOfResultBeforeFlow(const Program&, FunctionID, uint32_t result);
// A class is not called and a function that is no constructor is not constructed: the run time throws in front of the body.
bool throwsBeforeEntry(const Function& callee, bool isConstruction);
// A call by value fills the parameters that it has no argument for with undefined.
bool isParameterWithoutArgument(const Function& callee, const Value& call, uint32_t entryParameter);

// From here on: defined in AOTRelationalSummariesAtCalls.cpp.

// Where the operand stands at a call by value.
std::optional<PlaceAtCall> placeAtCall(const Value& call, uint32_t operandIndex);

// What a value that stands at a place is to a function that is entered by value.
struct EntryOfPlace {
    enum class Kind : uint8_t {
        // The function finds it in `arguments`.
        Unknown,
        Parameter,
        // It is an element of the array that the entry makes for the rest parameter.
        ElementOfParameter,
    };

    Kind kind { Kind::Unknown };
    uint32_t parameter { 0 };
};

// No entry: the function cannot see the value.
Vector<EntryOfPlace, 4> entriesAt(const Function& callee, PlaceAtCall);
// The entry parameter where the value is exactly that.
std::optional<uint32_t> entryParameterAt(const Function& callee, PlaceAtCall);

enum class Callbacks : uint8_t {
    // The function of the closure that the call passes answers for what native code hands to it. The type of the operand lists the functions, or the answer is "anything".
    AreFollowed,
    // The answer leaves out what the functions do that native code calls. forEachCallbackOfBuiltin() lists those calls.
    AreLeftOut,
};

// What a built-in does with the operand of a call by value that may enter it, whichever overload the call selects. A bit of storedIntoParameters speaks of an operand of the call.
LinkedFate fateOfOperandOfBuiltin(const RelationalLink&, const Function& caller, BuiltinID, const Value& call, uint32_t operandIndex, Callbacks);

// Native code calls what the operand `called` holds and passes the operand `handed`, or something read through it, at `place`.
struct CallbackOfBuiltin {
    uint32_t called { 0 };
    uint32_t handed { 0 };
    bool isReadThroughHanded { false };
    PlaceAtCall place;
    OptionSet<ValueFate> resultOfCall { everyValueFate };
};

// With the caller only the overloads count that what is proven of the operands leaves open, the same that fateOfOperandOfBuiltin() counts. Without it: every overload of the mode.
void forEachCallbackOfBuiltin(const Program&, const Function* caller, BuiltinID, const Value& call, const ScopedLambda<void(const CallbackOfBuiltin&)>&);
void forEachCallbackOfBuiltin(const Program&, BuiltinID, const Value& call, const ScopedLambda<void(const CallbackOfBuiltin&)>&);

// Who answers for the entry parameters of functions: the finished table behind FlowResults, which finds the function as read behind a version, the table that the link is filling, or one that a test has written.
class FatesOfParameters {
public:
    virtual ~FatesOfParameters() = default;
    virtual LinkedFate whole(FunctionID, uint32_t entryParameter) const = 0;
    virtual LinkedFate apartFromCalledParameters(FunctionID, uint32_t entryParameter) const = 0;
    // Answers false where the rows cannot be put into the terms of the function. Then whole() is all there is.
    virtual bool listRows(FunctionID, Vector<HandedToCalledParameter, 4>&) const = 0;
};

// FlowResults::fateOfOperandAt() with the table given.
LinkedFate fateOfOperandAt(const Program&, const FlowResults&, const FatesOfParameters&, const Function& caller, const Value& user, uint32_t operandIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
