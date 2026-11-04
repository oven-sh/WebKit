/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTMemberLookup.h"
#include "AOTOpcode.h"
#include "AOTSchemaInlineBuiltins.h"
#include <array>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// The operations that make the conversion in front of a call. None: the built-in converts for itself.
inline std::span<const Opcode> conversionOpcodesOf(BuiltinConversion conversion)
{
    static constexpr std::array toBoolean { Opcode::ToBoolean };
    static constexpr std::array toNumber { Opcode::ToNumber };
    static constexpr std::array toNumeric { Opcode::ToNumeric };
    static constexpr std::array toString { Opcode::ToString };
    static constexpr std::array toPropertyKey { Opcode::ToPropertyKey };
    static constexpr std::array toObject { Opcode::ToObject };
    static constexpr std::array toIntegerOrInfinity { Opcode::ToNumber, Opcode::ToIntegerOrInfinity };
    static constexpr std::array toInt32 { Opcode::ToNumber, Opcode::ToInt32 };
    static constexpr std::array toUint32 { Opcode::ToNumber, Opcode::ToUint32 };

    switch (conversion) {
    case BuiltinConversion::ToBoolean:
        return toBoolean;
    case BuiltinConversion::ToNumber:
        return toNumber;
    case BuiltinConversion::ToNumeric:
        return toNumeric;
    case BuiltinConversion::ToString:
        return toString;
    case BuiltinConversion::ToPropertyKey:
        return toPropertyKey;
    case BuiltinConversion::ToObject:
        return toObject;
    case BuiltinConversion::ToIntegerOrInfinity:
        return toIntegerOrInfinity;
    case BuiltinConversion::ToInt32:
        return toInt32;
    case BuiltinConversion::ToUint32:
        return toUint32;
    case BuiltinConversion::None:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// AOTInlineBuiltinForms.cpp and its siblings by family. WHAT THE EMITTER OF EACH PIECE OF CODE IN LINE TAKES: it converts nothing and tests only what its case leaves open.
// Every member of InlineBuiltin has its case, so whoever adds one says what its emitter takes.

enum class InlineForm : uint8_t {
    // CallBuiltin stays.
    Call,
    // TryCallInline, and CallBuiltin where the code in line does not answer.
    TwoWay,
    // CallInline.
    Whole,
};

struct InlineBuiltinQuery {
    const AccessContext& context;
    const BuiltinOverload& overload;
    InlineBuiltin builtin;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    // As the built-in receives them.
    const BuiltinOperandTypes& operands;
    // The table names a function to call where the code in line gives up.
    bool hasSlowPath { false };
    // The call is written in the function whose frame must be on top when the built-in runs: f.apply(), Reflect.construct().
    bool isDoneInPlace { false };
    // The function in whose text the call stands is left out of stack traces.
    bool isHiddenInStackTraces { false };
    // Nothing says that the operands lie in the parameters of the row: the code in line has to refuse and convert them itself, in the order of the function.
    bool operandsAreAsWritten { false };

    TypeID receiver() const { return operands.receiver; }
    TypeID argument(unsigned index) const { return index < operands.arguments.size() ? operands.arguments[index] : TypeID(); }
    unsigned numberOfArguments() const { return operands.arguments.size(); }
};

inline BuiltinTypeContext builtinTypeContextOf(const AccessContext& context) { return { context.program, context.types, &*context.facts }; }
// No emitter stands behind it in this tree: the form is Call, whatever the table says.
bool hasEmitter(InlineBuiltin);
InlineForm chooseInlineForm(const InlineBuiltinQuery&);

#define FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(v) \
    v(ArrayBuiltin) \
    v(ArrayStorageBuiltin) \
    v(ArrayBufferViewBuiltin) \
    v(OwnPropertyBuiltin) \
    v(ObjectBuiltin) \
    v(HashTableBuiltin) \
    v(MapSetBuiltin) \
    v(PromiseBuiltin) \
    v(BufferBuiltin) \
    v(MathBuiltin) \
    v(DateBuiltin) \
    v(FunctionBuiltin) \
    v(StringBuiltin) \
    v(RegExpBuiltin) \
    v(ErrorBuiltin) \
    v(IntlBuiltin)

#define AOT_DECLARE_INLINE_FORM_CHOOSER(family) InlineForm chooseFormOf##family(const InlineBuiltinQuery&);
FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(AOT_DECLARE_INLINE_FORM_CHOOSER)
#undef AOT_DECLARE_INLINE_FORM_CHOOSER

// The host function reads members for the first time, which their holders add at that moment. Code in line does not, so listed keys would come out otherwise.
bool hostReadsMembersFirstIn(const BuiltinTable&, const BuiltinOverload&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
