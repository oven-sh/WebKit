/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSelectionContext.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class InlineForm : uint8_t { Call, TwoWay, Whole };

struct InlineBuiltinSite {
    SelectionContext& context;
    ValueID call;
    const BuiltinOverload& overload;
    InlineBuiltin builtin;
    bool hasSlowPath { false };
    CodeShape shape { CodeShape::Balanced };

    const Value& value() const { return context.function().at(call); }
    ValueID receiver() const;
    std::span<const ValueID> arguments() const LIFETIME_BOUND;
    const Type& typeOf(ValueID id) const { return context.typeOf(id); }
};

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

#define AOT_DECLARE_INLINE_FORM_CHOOSER(family) InlineForm chooseFormOf##family(const InlineBuiltinSite&);
FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(AOT_DECLARE_INLINE_FORM_CHOOSER)
#undef AOT_DECLARE_INLINE_FORM_CHOOSER

struct MembersReadByHost {
    Vector<BuiltinID, 1> inEveryCall;
    Vector<BuiltinID, 1> whereInlineCodeAnswers;
};
MembersReadByHost membersReadByHostIn(const BuiltinTable&, const BuiltinOverload&);

std::optional<InlineBuiltin> inlineImplementationOf(const Program&, const Value& builtinCall);
std::optional<InlineBuiltinSite> findInlineBuiltinSite(SelectionContext&, ValueID call);
InlineForm chooseInlineForm(const InlineBuiltinSite&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
