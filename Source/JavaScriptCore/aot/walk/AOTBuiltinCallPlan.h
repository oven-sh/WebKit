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

enum class InlineForm : uint8_t {
    Call,
    TwoWay,
    Whole,
};

struct InlineBuiltinQuery {
    const AccessContext& context;
    const BuiltinOverload& overload;
    InlineBuiltin builtin;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    const BuiltinOperandTypes& operands;
    bool hasSlowPath { false };
    bool isDoneInPlace { false };
    bool isHiddenInStackTraces { false };
    bool operandsAreAsWritten { false };
    bool standsInLoop { false };

    TypeID receiver() const { return operands.receiver; }
    TypeID argument(unsigned index) const { return index < operands.arguments.size() ? operands.arguments[index] : TypeID(); }
    unsigned numberOfArguments() const { return operands.arguments.size(); }
};

inline BuiltinTypeContext builtinTypeContextOf(const AccessContext& context) { return { context.program, context.types }; }
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

bool hostReadsMembersFirstIn(const BuiltinTable&, const BuiltinOverload&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
