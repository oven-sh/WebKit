/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPrimitiveEmitter.h"
#include "AOTRegExpMatchPlan.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class RegExpMatchEmitter final : public PrimitiveEmitter {
public:
    using Bounds = Vector<LValue, 8>;

    enum class Groups : bool { Ignored, Kept };

    RegExpMatchEmitter(CodeGenerator&, const RegExpMatchPlan&, Pointer characters, Word32 length, Groups);

    enum class Begin : uint8_t {
        AtStart,
        AtOrAfterPosition,
        AtPlace,
    };

    Bounds match(LBasicBlock misses, Begin = Begin::AtStart, LValue first = nullptr);
    LValue addressAt(Word32 index);
    bool isSearched(Begin) const;
    bool testsFirstCharacterAtPlace() const;
    Word32 indexOf(LValue bound);
    Word32 indexOfGroupBound(LValue boundOrNull);

private:
    using Node = RegExpMatchNode;
    using Sequence = RegExpMatchNode::Sequence;

    struct State {
        LValue base { nullptr };
        unsigned offset { 0 };
        unsigned available { 0 };
        std::optional<Latin1Set> next;
        std::optional<Latin1Set> previous;
        Bounds bounds;
    };

    using Continuation = ScopedLambda<void(State&, LBasicBlock fails)>;

    struct Join {
        LBasicBlock block;
        unsigned firstChangedBound { 0 };
        unsigned endOfChangedBounds { 0 };
        unsigned available { std::numeric_limits<unsigned>::max() };
        Vector<Vector<ValueFromBlock, 8>, 4> incoming { };
    };

    void emitSequence(std::span<const Node>, unsigned minimumLengthAfter, State&&, LBasicBlock fails, const Continuation&);
    void collectSetsOfTable(std::span<const Node>);
    void addSetOfTable(const Latin1Set&);
    void emitLiterals(std::span<const Node>, State&, LBasicBlock fails);
    void emitFixedCharacters(const Node&, State&, LBasicBlock fails);
    void emitRun(const Node&, std::span<const Node> rest, unsigned minimumLengthAfter, State&&, LBasicBlock fails, const Continuation&);
    void emitGroup(const Node&, std::span<const Node> rest, unsigned minimumLengthAfter, State&&, LBasicBlock fails, const Continuation&);
    void emitGroupLoop(const Node&, std::span<const Node> rest, unsigned minimumLengthAfter, State&&, LBasicBlock fails, const Continuation&);
    void emitAlternatives(std::span<const Sequence>, std::span<const Latin1Set> beginnings, bool mayBeLeftOut, unsigned minimumLengthAfter, const State&, LBasicBlock fails, const Continuation& matches, const Continuation& isLeftOut);
    void emitAlternativesInOrder(std::span<const Sequence>, uint64_t chosen, unsigned minimumLengthAfter, const State&, LBasicBlock fails, const Continuation& matches);
    void emitChoiceOfAlternatives(std::span<const Sequence>, std::span<const Latin1Set> beginnings, unsigned minimumLengthAfter, const State&, LBasicBlock fails, const Continuation& matches);
    void emitLookahead(const Node&, State&, LBasicBlock fails);
    void emitWordBoundary(const Node&, std::span<const Node> rest, State&, LBasicBlock fails);

    void requireCharacters(std::span<const Node> sequence, unsigned minimumLengthAfter, State&, LBasicBlock fails);
    void branchOnCharacter(LValue character, const Latin1Set&, LBasicBlock isIn, LBasicBlock isOut);
    Condition isInTable(LValue character, const Latin1Set&);
    LValue loadCharacter(const State&, unsigned offset);
    LValue loadCharacterAt(LValue address, ptrdiff_t offset = 0);
    LValue addressOf(const State&);
    void continueIf(LValue condition, LBasicBlock otherwise);
    void resetInnerGroups(const Node&, Bounds&);

    Join createJoin(unsigned firstGroup, unsigned lastGroup);
    void addTo(Join&, const State&);
    State finish(const Join&, const Bounds& boundsInFront);
    std::optional<bool> isWordCharacter(const std::optional<Latin1Set>&) const;

    const RegExpMatchPlan& m_plan;
    const bool m_keepsGroups;
    LValue m_start;
    LValue m_end;
    Latin1Set m_wordCharacters { };
    Latin1Set m_newlines { };
    Vector<Latin1Set, 8> m_setsOfTable { };
    Vector<uint32_t, 2> m_tables { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
