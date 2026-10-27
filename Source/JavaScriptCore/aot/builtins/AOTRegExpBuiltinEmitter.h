/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

class RegExpBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit RegExpBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_REG_EXP_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

private:
    using Results = Vector<Operand, 36>;

    enum class SecondPath : uint8_t { Unsure, Rare };

    TypedPointer fieldAt(LValue cell, ptrdiff_t offset);
    Pointer regExpOf(CellPointer regExpObject);
    Word32 flagsOf(CellPointer regExpObject);
    void storeLastIndex(CellPointer regExpObject, Boxed value, LBasicBlock ifNotWritable);
    uint32_t prefixLengthOf(const InlineCall&) const;
    Integer numberOfGroupsOf(CellPointer regExpObject);
    Vector<CodeUnitRange, 16> loadMatchedRanges(CellPointer regExpObject, Integer numberOfGroups, unsigned count);
    Results execBoundsByOperation(const InlineCall&, CellPointer regExpObject, CellPointer string, unsigned count);
    Results execBoundsInline(const InlineCall&, CellPointer regExpObject, CellPointer string, unsigned count);
    Results matchPrefixByOperation(const InlineCall&, CellPointer regExpObject, CellPointer string, unsigned count, const CallOptions& = { });
    Results matchPrefixInline(const InlineCall&, CellPointer regExpObject, CellPointer string, unsigned count);
    template<typename First, typename Second> Results eitherResults(Condition, SecondPath, const First&, const Second&);
};

static_assert(sizeof(RegExpBuiltinEmitter) == sizeof(BuiltinFamilyEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
