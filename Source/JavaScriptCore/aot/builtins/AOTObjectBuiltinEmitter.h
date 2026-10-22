/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ObjectBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ObjectBuiltinEmitter(CodeGenerator&);

    void generateObjectCreate(const InlineCall&);
    void generateGetPrototypeOf(const InlineCall&);
    void generateSetPrototypeOf(const InlineCall&);
    void generateCallObjectConstructor(const InlineCall&);
    void generateObjectFreeze(const InlineCall&);
    void generateObjectPrototypeToString(const InlineCall&);
    void generateObjectIs(const InlineCall&);
    void generateReturnThis(const InlineCall&);

    CellPointer allocateEmptyObject(InlineAllocation, std::optional<Integer> header, LBasicBlock slowPath);

private:
    CellPointer createObjectWithPrototype(const InlineCall&, CellPointer prototype);
    CellPointer createObjectWithoutPrototype(const InlineCall&);
    Condition isSameValue(const OperationContext&, ValueID, ValueID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
