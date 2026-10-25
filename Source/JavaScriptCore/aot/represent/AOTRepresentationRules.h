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
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

class CallingConventions;
class Placement;

struct RepresentationInputs {
    const Program& program;
    FunctionID id;
    const Function& function;
    const Placement& placement;
    const CallingConventions& conventions;

    const Type& typeOf(ValueID value) const { return program.typeOf(function, value); }
};

inline constexpr RepresentationSet integerRepresentations { Representation::Int32, Representation::Int64 };
inline constexpr RepresentationSet numberRepresentations { Representation::Int32, Representation::Int64, Representation::Double };
inline constexpr RepresentationSet optionalRepresentations { Representation::OptionalInt64, Representation::OptionalDouble };

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

inline constexpr OpcodeSet operationsThatMayRunBarrier = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (runsOwnerBarrierOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

bool isEqualityOfNumbers(const Program&, const Function&, const Value&);

bool picksOwnRepresentation(const Function&, ValueID);
bool isForwardedOperand(const Value& user, uint32_t operandIndex);

Representation wantedRepresentationOf(const RepresentationInputs&, const Value& user, uint32_t operandIndex);
Representation wantedRepresentationOf(const RepresentationInputs&, const Terminator& user, uint32_t operandIndex);

Representation resultRepresentationOf(const RepresentationInputs&, const Value&);
Representation resultRepresentationOf(const RepresentationInputs&, const Value& producer, uint32_t resultIndex);
Representation edgeResultRepresentationOf(const RepresentationInputs&, EdgeResultPosition);

struct ConversionStep {
    Opcode opcode { Opcode::Box };
    Representation to { Representation::JSValue };

    friend bool operator==(const ConversionStep&, const ConversionStep&) = default;
};

using ConversionPath = Vector<ConversionStep, 2>;

std::optional<ConversionPath> findConversion(const Type&, Representation from, Representation to);

void checkRepresentations(const RepresentationInputs&, Vector<InternalError>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
