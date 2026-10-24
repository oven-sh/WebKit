/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include "AOTStringBuiltinOperationData.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct BuiltinCall {
    BuiltinOverloadID overload;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    uint32_t prefixLength { 0 };
    ValueID receiver;
    std::span<const ValueID> arguments;

    ValueID operandAt(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return receiver;
        RELEASE_ASSERT(operand.kind() == BuiltinOperand::Kind::Argument);
        return operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : ValueID();
    }
};

inline constexpr OpcodeSet builtinCallOpcodes { Opcode::CallBuiltin, Opcode::CallInline };

BuiltinCall builtinCallOf(const Program&, const Value&);
BuiltinCall builtinCallOf(const Program&, const Terminator&);

struct BuiltinOperandTypes {
    TypeID receiver;
    Vector<TypeID, 4> arguments;

    TypeID at(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return receiver;
        RELEASE_ASSERT(operand.kind() == BuiltinOperand::Kind::Argument);
        return operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : TypeID();
    }
};

BuiltinOperandTypes operandTypesOf(const TypingContext&, const BuiltinCall&);
BuiltinOperandTypes operandTypesOf(const TypingContext&, ValueID receiver, std::span<const ValueID> arguments);

// A part is a field of the layout or an element of the prefix.
struct BuiltinResultIndexes {
    std::optional<uint32_t> presence;
    std::optional<uint32_t> length;
    uint32_t firstProperty { 0 };
    uint32_t numberOfProperties { 0 };
    uint32_t firstPart { 0 };
    uint32_t numberOfParts { 0 };
    bool partsAreBounds { false };

    uint32_t resultsPerPart() const { return partsAreBounds ? 2 : 1; }
    uint32_t resultOfPart(uint32_t part) const { return firstPart + part * resultsPerPart(); }
    uint32_t numberOfResults() const { return resultOfPart(numberOfParts); }
};

std::optional<BuiltinResultIndexes> resultIndexesOf(const BuiltinTable&, BuiltinOverloadID, BuiltinCallForm, uint32_t prefixLength);

struct BuiltinResultQuery {
    BuiltinOverloadID overload;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    uint32_t prefixLength { 0 };
    const BuiltinOperandTypes& operands;
};

Type resultTypeOfBuiltinCall(const TypingContext&, const BuiltinResultQuery&, uint32_t resultIndex);
// What a use of the built-in gives when nothing has chosen an overload: the union over the overloads that operands of these types can reach.
Type resultTypeOfReachableOverloads(const TypingContext&, BuiltinID, BuiltinCallMode, const BuiltinOperandTypes&);
Type resultTypeOfOverload(const TypingContext&, const Value& call, BuiltinOverloadID, uint32_t resultIndex);
Type resultTypeOfOverload(const TypingContext&, const Terminator& call, BuiltinOverloadID, uint32_t resultIndex);
std::optional<uint32_t> numberOfResultsOfOverload(const Program&, const Value& call);

Type typeOfBuiltinLayoutField(const Program&, const BuiltinLayoutField&, TypeArena&);
Type parameterTypeOfOverload(const Program&, BuiltinOverloadID, BuiltinOperand);

// The overload is the one for a call without this argument, and undefined in its place would select another.
bool distinguishesOmittedFromUndefined(const BuiltinTable&, const BuiltinParameter&);

bool overloadHasForm(const BuiltinTable&, BuiltinOverloadID, BuiltinCallForm);

// A call whose result is validated, and a call of what is in no realm: a helper of the compiler, a function of bun:bundle. There is no code for either, so one that is left stops the build.
bool isCallThatRequiredPassRemoves(const Program&, const Value& call);

bool overloadTakesOperands(const TypingContext&, const Value& call);
bool overloadTakesOperands(const TypingContext&, const Terminator& call);
bool overloadTakesOperands(const Program&, const Function&, const Value& call);
bool overloadCanAppend(const TypingContext&, const Value& append);
bool overloadCanAppend(const Program&, const Function&, const Value& append);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
