/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTOpcode.h"
#include "AOTResultRules.h"
#include "AOTType.h"
#include "AOTVector.h"
#include "TypedArrayType.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class Program;

struct BuiltinTypeContext {
    const Program& program;
    TypeArena& types;
    const PositionTypes* positions { nullptr };
    TypeLevel level { TypeLevel::Proven };
};

struct BuiltinOperandTypes {
    TypeID receiver;
    Vector<TypeID, 4> arguments;
    PristineAspectSet aspectsOfReceiver;
    Vector<PristineAspectSet, 4> aspectsOfArguments;

    TypeID at(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return receiver;
        RELEASE_ASSERT(operand.kind() == BuiltinOperand::Kind::Argument);
        return operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : TypeID();
    }

    PristineAspectSet aspectsOfArgument(size_t index) const { return index < aspectsOfArguments.size() ? aspectsOfArguments[index] : PristineAspectSet(); }
};

struct PristineRequirement {
    BuiltinClassID builtinClass;
    PristineAspectSet aspects;

    explicit operator bool() const { return !aspects.isEmpty(); }

    friend bool operator==(const PristineRequirement&, const PristineRequirement&) = default;
};

enum class TypeFit : uint8_t { Unknown, Outside, Inside };

struct OperandFit {
    TypeFit fit { TypeFit::Unknown };
    PristineRequirement unprovenPristine;
};

TypeID typeOfBuiltinType(const BuiltinTypeContext&, BuiltinTypeID, const BuiltinOperandTypes* operands = nullptr);
TypeID typeOfBuiltinLayoutField(const BuiltinTypeContext&, const BuiltinLayoutField&);
TypeID typeOfBuiltinConstant(const BuiltinTypeContext&, BuiltinID);
TypeID parameterTypeOfOverload(const BuiltinTypeContext&, BuiltinOverloadID, BuiltinOperand);
TypeID typeExpectedByLibraryFunction(const Program&, TypeArena&, FunctionID, uint32_t entryParameter);
uint16_t primitivesOfBuiltinType(const BuiltinType&);
Range integerRangeWrittenBy(const BuiltinType&);
TypedArrayType typedArrayTypeOf(const BuiltinTypeContext&, TypeID);
uint32_t kindsOfViewsOfNumbersIn(const Program&, TypeReader, TypeID);
BuiltinClassID classAboveViewsOfSeveralKinds(const Program&, TypeReader, TypeID);

OperandFit fitOf(const BuiltinTypeContext&, TypeID operandOrNotPassed, BuiltinTypeID tested);
bool fitsIfPristine(const BuiltinTypeContext&, TypeID operandOrNotPassed, BuiltinTypeID tested);
TypeFit fitOfPrimitiveInBuiltinType(const BuiltinType& tested, PrimitiveSet, Range integers);
TypeFit fitOfOmittedOperandInBuiltinType(const BuiltinType& tested);

struct BuiltinResultQuery {
    BuiltinOverloadID overload;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    const BuiltinOperandTypes& operands;
};

Range computeResultRange(BuiltinRangeRule, std::span<const Range> operandRanges);
TypeID resultTypeOfBuiltinCall(const BuiltinTypeContext&, const BuiltinResultQuery&);
TypeID resultTypeOfBuiltinCallOnList(const BuiltinTypeContext&, BuiltinOverloadID, const BuiltinOperandTypes& receiverAlone);
TypeID resultTypeOfReachableOverloads(const BuiltinTypeContext&, BuiltinID, BuiltinCallMode, const BuiltinOperandTypes&);
Vector<BuiltinOverloadID, 4> overloadsReachedBy(const BuiltinTypeContext&, BuiltinID, BuiltinCallMode, const BuiltinOperandTypes&);
bool rowsSpeakOfCallOnReceiver(const BuiltinTypeContext&, BuiltinID, TypeID receiverOrNotPassed);
bool overloadTakesOperands(const BuiltinTypeContext&, BuiltinOverloadID, BuiltinCallForm, const BuiltinOperandTypes&);
bool distinguishesOmittedFromUndefined(const BuiltinTable&, const BuiltinParameter&);
bool requiresWalkLowering(const BuiltinTable&, BuiltinID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
