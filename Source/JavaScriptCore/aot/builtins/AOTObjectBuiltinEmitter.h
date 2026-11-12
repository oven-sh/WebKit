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

    FOR_EACH_AOT_INLINE_BUILTIN_IN_OBJECT_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    Boxed cachedToStringTagBeforeClosedCall(Boxed receiver, LBasicBlock closed);
    CellPointer allocateEmptyObject(InlineAllocation, std::optional<Integer> header, LBasicBlock slowPath, unsigned inlineCapacity);

private:
    template<typename LeaveUnless>
    CellPointer cachedToStringTag(CellPointer object, const LeaveUnless&);
    CellPointer createObjectWithPrototype(const InlineCall&, CellPointer prototype);
    CellPointer createObjectWithoutPrototype(const InlineCall&);
    Condition isSameValue(const OperationContext&, ValueID, ValueID);
    void finishWithCachedString(const InlineCall&, CellPointer symbol, CellPointer cached, RuntimeOperation create);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
