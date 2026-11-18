/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTResolvedCallEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class RegExpBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit RegExpBuiltinEmitter(CodeGenerator&);

    void generateNewRegExp(const InlineCall&);
    void generateStringMatch(const InlineCall&);
    void generateStringSearch(const InlineCall&);
    void generateRegExpExec(const InlineCall&);
    void generateRegExpTest(const InlineCall&);
    void generateRegExpFlags(const InlineCall&);
    void generateRegExpSource(const InlineCall&);
    void generateStringReplace(const InlineCall&);
    void generateStringReplaceAll(const InlineCall&);
    bool storesElementIntoResult(const InlineCall&);
    void generateStringSplit(const InlineCall&);
    void generateRegExpFlag(const InlineCall&);
    void generateRegExpGetLastIndex(const InlineCall&);

private:
    template<typename T> T matchInPlace(const InlineCall&, RegExpMatch, RegExpMatchAnswer, T miss, const ScopedLambda<T()>& matchByCall);

    TypedPointer fieldAt(LValue cell, ptrdiff_t offset);
    Pointer regExpOf(CellPointer regExpObject);
    Word32 flagsOf(CellPointer regExpObject);
};

static_assert(sizeof(RegExpBuiltinEmitter) == sizeof(BuiltinFamilyEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
