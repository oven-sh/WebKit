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
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct RegExpMatch {
    StringID pattern;
    OptionSet<RegExpFlags> flags;
    CellPointer string;
    CellPointer regExpObject { };
    bool isHeld { false };
    bool isPrivate { false };
    bool isExpected { false };
    bool isArgument { false };
    bool resetsLastIndex { false };
    ValueID subject { };
    BuiltinID calledBuiltin { };
    std::optional<Integer> lastIndexStored { };

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
    void generateMatchHeldRegExpParts(const ValueContext&);
    void generateGetRegExpLastIndex(const ValueContext&);
    void generateBoundsOfRegExpMatch(const ValueContext&);
    void generateNewRegExpMatchArray(const ValueContext&);
    void generateGetMatchArrayGroups(const ValueContext&);
    void generateMatchRegExpIntoFrame(const ValueContext&);
    void generateStringLengthWithoutMatches(const ValueContext&);
    void generateIsMatchInFrame(const ValueContext&);
    void generateFrameMatchLength(const ValueContext&);
    void generateFrameMatchBounds(const ValueContext&);
    void generateNewArrayOfRegExpMatch(const ValueContext&);

    bool isDeferredLastIndexStore(const ValueContext& store);

    const RegExpMatchPlan* matchPlanOf(BlockID, const RegExpMatch&);
    template<typename T> T matchInPlace(const OperationContext&, const RegExpMatchPlan&, const RegExpMatch&, RegExpMatchAnswer, T miss, const ScopedLambda<T()>& call);
    OwnedCharacters loadCarriedSubstring(const OperationContext&, const CodeGenerator::CarriedSubstring&, bool isBeforeBranches = true);

private:
    bool canTestByMatcher(const RegExpMatchPlan&, const RegExpMatch&, RegExpMatchAnswer);
    Boxed testByMatcher(const OperationContext&, const RegExpMatch&);
    struct LoadedSubject {
        BlockID block;
        uint32_t suspensionFreeRegion;
        LValue string;
        OwnedCharacters characters;
    };
    bool isMatchOf(ValueID operation, ValueID string) const;
    bool isMatchedSeveralTimes(ValueID string);
    std::optional<OwnedCharacters> findLoadedSubject(const OperationContext&, CellPointer string);
    const CodeGenerator::CarriedSubstring* carriedSubjectOf(const RegExpMatch&) const;
    CellPointer subjectOf(const OperationContext&, const RegExpMatch&);
    std::optional<OwnedCharacters> loadSubject(const OperationContext&, const RegExpMatch&);
    CharacterView viewOfSubject(const OperationContext&, const RegExpMatch&, const std::optional<OwnedCharacters>&, LBasicBlock calls);
    ValueID findDeferredLastIndexStore(ValueID match);
    void recordMatch(const OperationContext&, const RegExpMatch&, const std::optional<OwnedCharacters>& part, Word32 start, Word32 end);
    Pointer matchOutput();
    Pointer matchIntoFrameByCall(const ValueContext&, LValue slot);
    RegExpMatchEmitter::Begin beginOfMatch(const RegExpMatch&, RegExpMatchEmitter&, const CharacterView&, LBasicBlock calls, LValue& first);
    void storeBounds(RegExpMatchEmitter&, const RegExpMatchEmitter::Bounds&, LValue output, ptrdiff_t offset = 0);
    Boxed createMatchArray(const OperationContext&, const RegExpMatch&);
    Condition isMadeByLiteral(const RegExpMatch&);
    void clearAbsentGroupBounds(const RegExpMatch&, unsigned count);
    template<typename T> T matchByFirstCharacter(const ValueContext&, const RegExpMatch&, T miss, NOESCAPE const Invocable<T()> auto& call);

    Vector<ValueID, 2> m_stringsAskedFirstCharacter;
    Vector<LoadedSubject, 2> m_loadedSubjects;
    std::optional<Vector<ValueID>> m_subjectsInLoops;
    BlockID m_blockOfDeferredStores;
    Vector<ValueID, 8> m_deferredLastIndexStores;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
