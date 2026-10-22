/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include "AOTRepresentation.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallingConventions;
class Heat;
class Placement;

struct RepresentationInputs {
    const Program& program;
    FunctionID id;
    const Function& function;
    const Placement& placement;
    const CallingConventions& conventions;

    const Type& typeOf(ValueID value) const { return program.typeOf(function, value); }
};

// Nothing says that the function runs often, so its code is made without the optimizing back end.
bool isGeneratedWithoutOptimizing(const BuildContext&, const Heat&, const Program&, FunctionID);
// For such a function ChooseRepresentations writes the forms of values and leaves the conversions to the code generator, which makes them where an operand is fetched.
bool leavesConversionsToCodeGenerator(const BuildContext&, const Heat&, const Program&, FunctionID);

inline constexpr RepresentationSet integerRepresentations { Representation::Int32, Representation::Int64 };
inline constexpr RepresentationSet numberRepresentations { Representation::Int32, Representation::Int64, Representation::Double };
inline constexpr RepresentationSet optionalRepresentations { Representation::OptionalInt64, Representation::OptionalDouble };
inline constexpr RepresentationSet cellRepresentations { Representation::Cell, Representation::Cell32, Representation::LiteralString };

inline constexpr OpcodeSet constantsInFormOfUses {
    Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::KeyConstant
};

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

constexpr OpcodeSet sumsOfNumbers { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };

bool isSumOfBoxedNumbers(const Value&);
bool isEqualityOfNumbers(const Program&, const Function&, const Value&);

Representation integerRepresentationOf(const Type&);

bool picksOwnRepresentation(const Function&, ValueID);
bool isForwardedOperand(const Value& user, uint32_t operandIndex);

Representation wantedRepresentationOf(const Program&, const Function&, const Value& user, uint32_t operandIndex);
Representation wantedRepresentationOf(const Program&, const Function&, const Terminator& user, uint32_t operandIndex);

Representation wantedRepresentationOf(const RepresentationInputs&, const Value& user, uint32_t operandIndex);
Representation wantedRepresentationOf(const RepresentationInputs&, const Terminator& user, uint32_t operandIndex);

Representation resultRepresentationOf(const RepresentationInputs&, const Value&);
Representation resultRepresentationOf(const RepresentationInputs&, const Value& producer, uint32_t resultIndex);
Representation edgeResultRepresentationOf(const RepresentationInputs&, EdgeResultPosition);

Representation wantedByOperator(const Program&, const Function&, const Value& user, uint32_t operandIndex);
Representation resultOfOperator(const Program&, const Function&, const Value&);
Representation wantedByConcatenation(const Type& part);

Representation wantedByDeclaredPosition(const RepresentationInputs&, const Value& user, uint32_t operandIndex);
Representation wantedByDeclaredPosition(const RepresentationInputs&, const Terminator& user, uint32_t operandIndex);
Representation resultOfDeclaredPosition(const RepresentationInputs&, const Value&);
Representation resultOfDeclaredPosition(const RepresentationInputs&, const Value& producer, uint32_t resultIndex);
Representation representationOfEntryParameter(const RepresentationInputs&, unsigned entryParameterIndex);

void checkRepresentations(const RepresentationInputs&, Vector<InternalError>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
