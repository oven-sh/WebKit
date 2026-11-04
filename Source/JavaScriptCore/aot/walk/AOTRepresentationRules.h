/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTCodeRole.h"
#include "AOTDeclaredType.h"
#include "AOTRepresentation.h"
#include <limits>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Plan;
class Program;
class Walker;

// In which form each operation takes its operands and gives its result. Pure functions of the operation, the proven types of its operands and Plan.
// An expected type never chooses a form. Three callers ask the same functions: the walk as it appends, verifyCode(), and the emitters.

struct RepresentationContext {
    const Program& program;
    const Plan& plan;
    TypeReader types;
    // The unit whose code holds the operation. Its parameters and results are positions of its own convention.
    CodeUnit unit;
};

// By the schema's operand index: named operands first, absent ones null and Unchosen, then the variadic ones.
struct OperandForms {
    std::span<const TypeID> provenTypes;
    // The forms that the operands have where they were built.
    std::span<const Representation> representations;

    TypeID typeAt(uint32_t operandIndex) const { return operandIndex < provenTypes.size() ? provenTypes[operandIndex] : TypeID(); }
    Representation representationAt(uint32_t operandIndex) const { return operandIndex < representations.size() ? representations[operandIndex] : Representation::Unchosen; }
};

inline constexpr RepresentationSet integerRepresentations { Representation::Int32, Representation::Int64 };
inline constexpr RepresentationSet numberRepresentations { Representation::Int32, Representation::Int64, Representation::Double };
inline constexpr RepresentationSet optionalRepresentations { Representation::OptionalInt64, Representation::OptionalDouble };

// A constant has no form of its own: each use gets it in the form the use wants. Walker::constant() builds the record in the form of its type, and Walker::use() makes a copy in the form wanted.
inline constexpr OpcodeSet constantsInFormOfUses {
    Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::KeyConstant
};

// A number beyond int64 is a multiple of 2^11, and what truncating it, or NaN, to an int64 gives ends in as many zeros. So the truncated number has that many of the lowest bits of ToInt32, whatever the number is.
inline constexpr unsigned numberOfLowestBitsOfInt32 = std::numeric_limits<int64_t>::digits - (std::numeric_limits<double>::digits - 1);

// `x & mask` looks at no other bit of x.
inline bool isMaskOfLowestBitsOfInt32(const Type& mask)
{
    return mask.isInteger() && mask.integers.minimum >= 0 && mask.integers.maximum < (int64_t { 1 } << numberOfLowestBitsOfInt32);
}

constexpr bool hasRegister(Representation representation)
{
    return representation != Representation::Unchosen && representation != Representation::Void;
}

constexpr bool isSameWord(Representation from, Representation to)
{
    if (from == to)
        return true;
    return (from == Representation::Cell && to == Representation::JSValue) || (from == Representation::JSValue && to == Representation::Cell);
}

// AOTRepresentationRules.cpp. representationOf() of the type, unless that is LiteralString and the type names a text without a number: LiteralStringTable::indexOf().
// The word of such a text is its cell, so whoever compares or switches on words would have to tell numbers from cells. Then the form of any other string: Cell, or JSValue beside undefined, null or the absent mark.
// Wherever the rules below go by a type they go by this. A position that Plan has given the form LiteralString keeps it.
Representation formOfTypeInWalk(const RepresentationContext&, TypeID);
// representationOfJoin() by the same rule.
Representation formOfJoinInWalk(const RepresentationContext&, TypeID united, std::span<const Representation> arguments);

// Instantiated for Draft and Operation, and for TerminatorDraft and Terminator.
// Unchosen: the operation takes the operand in the form it has. Both operands of Narrowed are taken so, and the operand of Box, Unbox, IntegerToNumber, NumberToInteger, WidenInteger, NarrowInteger, Decode and Compress.
// ToInt32 and ToUint32 take a number that is not proven to be an integer and is held as a JSValue so too.
// The element of AppendElement and of InlineBuiltin::ArrayPush is taken so where it is held as Int32, Double or OptionalDouble. Any other proven number is taken as a Double where arrayStorageDeclaredBy() the array is Double.
// That is a guess: the code tests the storage, and boxes the number for storage of another kind.
// Of an argument of CallDirect and AwaitCallBySavingFrame: representationOfArgument() for CallingConventions::unitEnteredBy() the call.
// Of an operation that testsItsOwnGuess() (ir/AOTResultRules.h): JSValue for every operand and for the result. But for the position where readTakesIndexOfProvenList(): the form of the integer.
// And where storeTakesIndexOfProvenList(): the position likewise, and the value as the element of AppendElement.
// The result of such a GetArrayElement may be an unboxed number where that is what is proven to come out of it: the walk says so in the draft (formOfNumberThatLinkHasTypedOfTypedElements()).
// A unit returns one value where CallingConventions::numberOfResults() of it is 1 and either its function has no split result or it is a generic entry.
// Of the value of Return: representationOfResult(unit, 0) where the unit returns one value. Else Void: what it names is a Results or a call that holds none of its results.
// Each value of a Results is taken as it is: the record does not show who reads it. Whoever builds the Results that a unit returns brings value i into representationOfResult(unit, i) first.
// A deferred closure has no value. PutSlot, PutSlotOrAdd, NewObject and PutEnvironmentField take an operand that was built in Void, and whose type does not allow Void, as it is.
// Add, Sub, Increment and Decrement in OperatorForm::Numbers give a Double. A handler asks for a sum of boxed numbers by setting the draft's form to JSValue in front of append():
// where every operand that is no int32 may be an integer and is held as a JSValue, the operands are taken as Int32 | JSValue and the result is a JSValue. Where the operands are not so, no emitter serves the record.
template<typename Operation> Representation wantedRepresentationOf(const RepresentationContext&, const Operation&, const OperandForms&, uint32_t operandIndex);
// `result` is the proven type of the result asked for.
// A record holds none of its results where numberOfResultsOf() answers, and where it is a CallDirect of a unit that does not return one value. Of such a record:
//   a null `result` asks about the record itself: Void
//   any other `result` asks about result `resultIndex`: by the convention of unitEnteredBy() for CallDirect, the form in which value `resultIndex` was built for a Results, JSValue where the schema's results(..) says `unknown`, else by the type
// A ResultAt alone does not show its producer: the answer is the record's own form where that is chosen.
// Of NewClosure with ClosureForm::Deferred: Void.
// Of a constant of constantsInFormOfUses whose form nobody has chosen: the form of its type. A conversion or a Parameter that is left Unchosen stays Unchosen, which fails the walk.
template<typename Operation> Representation resultRepresentationOf(const RepresentationContext&, const Operation&, const OperandForms&, TypeID result, uint32_t resultIndex = 0);
// StrictEq or Eq in a form that compares numbers.
template<typename Operation> bool isEqualityOfNumbers(const RepresentationContext&, const Operation&, const OperandForms&);
// The same for Walker::append(): they gather the context, the operands' forms and the result's proven type from the walker and ask the functions above.
// By operand index. Unchosen in the place of an operand that is absent.
using WantedRepresentations = Vector<Representation, 8>;
WantedRepresentations wantedRepresentationsOf(Walker&, const Draft&);
// What wantedRepresentationsOf() goes by, by the opcode alone, so that whoever converts the operands of a draft need not ask it.
enum class OperandsAreTaken : uint8_t {
    // Each in the form that it has.
    AsBuilt,
    // Each by representationWantedByKind() of the kind that the schema gives it and of its own type, unless the operation testsItsOwnGuess().
    ByKind,
    // By more than that.
    ByRule,
};
extern const std::array<OperandsAreTaken, numberOfOpcodes> howOperandsAreTakenByOpcode;
inline OperandsAreTaken howOperandsAreTakenBy(Opcode opcode) { return howOperandsAreTakenByOpcode[static_cast<unsigned>(opcode)]; }
// `type` is not null. Void for a Guard and for Results, Unchosen for Declared and Convertible, an integer at the width of its type, else representationWantedBy().
Representation representationWantedByKind(TypeReader, OperandKind, TypeID type);
WantedRepresentations wantedRepresentationsOf(Walker&, const TerminatorDraft&);
// Void for a record that holds none of its results. For a ResultAt it asks the producer's record in Walker::code() for result index().
Representation resultRepresentationOf(Walker&, const Draft&);
// `proven` is Walker::resultTypeOf() of the draft, which the caller has asked for already.
Representation resultRepresentationOf(Walker&, const Draft&, TypeID proven);
// What the function above answers by the opcode alone, whatever the words, the operands and the type of the draft are. Unchosen: it has to be asked.
extern const std::array<Representation, numberOfOpcodes> formsOfResultsByOpcode;
inline Representation formOfResultOfEvery(Opcode opcode) { return formsOfResultsByOpcode[static_cast<unsigned>(opcode)]; }
// It answers formOfTypeInWalk() of the proven type, whatever the words and the operands of the draft are.
extern const OpcodeSet opcodesWhoseResultsHaveFormsOfTheirTypes;

// AOTRepresentationRulesOfPositions.cpp
Representation representationStoredInSlot(const RepresentationContext&, PropertyKeyID, LayoutSetID receivers);
Representation representationLoadedFromSlot(const RepresentationContext&, PropertyKeyID, LayoutSetID receivers, TypeID loaded);
Representation representationOfCapture(const RepresentationContext&, FunctionID, uint32_t capture);
Representation representationOfVariable(const RepresentationContext&, VariableID);
Representation representationOfArrayElement(ArrayStorage);
// Of an argument of a direct call of `callee`, by the callee's entry parameters, and of its results.
Representation representationOfArgument(const RepresentationContext&, CodeUnit callee, uint32_t entryParameter);
Representation representationOfResult(const RepresentationContext&, CodeUnit callee, uint32_t resultIndex);

// AOTRepresentationConversions.cpp
struct ConversionStep {
    // Box, Unbox, Compress, Decode, IntegerToNumber, NumberToInteger, NarrowInteger or WidenInteger.
    Opcode opcode { Opcode::Box };
    Representation to { Representation::JSValue };

    friend bool operator==(const ConversionStep&, const ConversionStep&) = default;
};

using ConversionPath = Vector<ConversionStep, 2>;

// Empty where the two are the same. Nothing where a value of the type has no such form, or no two steps lead there.
std::optional<ConversionPath> findConversion(TypeReader, TypeID proven, Representation from, Representation to);

// The length of a string that is one of the written literals is the length that most of them have, unless its number is that of a literal of another length: up to this many are compared.
inline constexpr size_t maxLiteralsComparedForLength = 3;
unsigned commonestOf(std::span<const unsigned> lengths);

// A text of up to this many code units is compared where it is asked about, as immediates. StringStartsWithLiteral and StringEndsWithLiteral have no other code, so the walk builds neither for a longer text.
inline constexpr unsigned maxLengthOfTextComparedInPlace = 32;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
