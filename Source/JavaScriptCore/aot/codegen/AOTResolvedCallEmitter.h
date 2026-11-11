/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpMatchEmitter.h"
#include "AOTStringEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

struct RegExpMatch {
    StringID pattern;
    OptionSet<RegExpFlags> flags;
    CellPointer string;
    CellPointer regExpObject { };
    bool isHeld { false };
    bool isArgument { false };
    bool resetsLastIndex { false };

    bool usesLastIndex() const { return !resetsLastIndex && flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }); }
};

enum class RegExpMatchAnswer : uint8_t { Boolean, Index, Array, Parts };

class ResolvedCallEmitter final : public OperationEmitter {
public:
    explicit ResolvedCallEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateResolveCallTarget(const ValueContext&);
    void generateCallResolved(const ValueContext&);
    void generateRegExpMatchLiteral(const ValueContext&);
    void generateRegExpMatchLiteralInParts(const ValueContext&);
    void generateBoundsOfRegExpMatch(const ValueContext&);
    void generateNewRegExpMatchArray(const ValueContext&);
    void generateMatchRegExpIntoFrame(const ValueContext&);
    void generateIsMatchInFrame(const ValueContext&);
    void generateLengthOfMatchInFrame(const ValueContext&);
    void generateBoundsOfMatchInFrame(const ValueContext&);
    void generateNewArrayOfRegExpMatch(const ValueContext&);

    const RegExpMatchPlan* matchPlanOf(BlockID, const RegExpMatch&);
    template<typename T> T matchInPlace(const OperationContext&, const RegExpMatchPlan&, const RegExpMatch&, RegExpMatchAnswer, T miss, const ScopedLambda<T()>& call);

private:
    CharacterView viewOfSubject(const OperationContext&, const RegExpMatch&, LBasicBlock calls);
    Pointer matchOutput();
    void storeBounds(RegExpMatchEmitter&, const RegExpMatchEmitter::Bounds&);
    Boxed createMatchArray(const OperationContext&, const RegExpMatch&);
    template<typename T> T matchByFirstCharacter(const ValueContext&, CellPointer string, T miss, NOESCAPE const Invocable<T()> auto& match);

    Vector<ValueID, 2> m_stringsAskedFirstCharacter;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
