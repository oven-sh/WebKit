/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class StringBuilderPartEmitter final : public OperationEmitter {
public:
    explicit StringBuilderPartEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateStringBuilderAppendConstant(const ValueContext&);
    void generateStringBuilderAppendInteger(const ValueContext&);
    void generateStringBuilderAppendNumber(const ValueContext&);
    void generateStringBuilderAppendBoolean(const ValueContext&);
    void generateStringBuilderAppendString(const ValueContext&);
    void generateStringBuilderAppendStringRange(const ValueContext&);
    void generateStringBuilderAppendStringOrNullish(const ValueContext&);
    void generateStringBuilderAppendValue(const ValueContext&);

private:
    struct Builder {
        CellPointer storage;
        const StringBuilderEmitter::StackBuilder* onStack { nullptr };
    };

    StringBuilderEmitter& builders() { return emitter<StringBuilderEmitter>(); }
    Builder builderOf(const ValueContext&);
    void appendString(const OperationContext&, Builder, CellPointer string);
    void appendDigits(const OperationContext&, Builder, Word32 value);
    void appendNullish(const OperationContext&, Builder, Condition isUndefined);
    void appendPrimitive(const OperationContext&, Builder, Boxed primitive, bool isOnRarePath = false);
    void appendInWordsOrByCall(const OperationContext&, Builder, int32_t bytesWritten, NOESCAPE const Invocable<Pointer(Pointer cursor)> auto& write, NOESCAPE const Invocable<Boxed()> auto& box);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
