/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <utility>
#include "AOTBitVector.h"
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// One run of a pass on one function. Rules read the function as it stands when the run begins and leave their edits for the end of the walk. Nobody but commit() talks to the analyses, types a value or says what the run has kept.
class Rewrite {
    WTF_MAKE_NONCOPYABLE(Rewrite);
public:
    explicit Rewrite(FunctionPassContext& context)
        : context(context)
        , program(context.program)
        , function(context.function)
        , analyses(context.analyses)
        , m_numberOfEditsAtStart(context.function.numberOfEdits())
    {
    }
    ~Rewrite()
    {
        ASSERT(m_edits.isEmpty());
        for (void* room : m_furtherRooms)
            fastFree(room);
    }

    FunctionPassContext& context;
    const Program& program;
    Function& function;
    FunctionAnalyses& analyses;

    const Value& at(ValueID id) const { return function.at(id); }
    const Block& at(BlockID id) const { return function.at(id); }
    const Type& typeOf(ValueID id) const { return program.typeOf(function, id); }
    const CFG& cfg() { return analyses.cfg(); }
    const UseLists& uses() { return analyses.useLists(); }
    std::span<const BlockID> blocks() { return analyses.cfg().reversePostOrder(); }

    // A block that a rule has claimed is shown to no other rule in this walk.
    void claim(BlockID block)
    {
        if (!std::exchange(m_hasClaimedBlocks, true))
            m_claimedBlocks.ensureSize(function.blocks.size());
        m_claimedBlocks.quickSet(block.index());
    }
    bool isClaimed(BlockID block) const { return m_hasClaimedBlocks && m_claimedBlocks.get(block.index()); }

    // Runs in commit(): the edits of the first rule of the table in the order in which it left them, then those of the second. An edit asks no analysis: what it needs it captures.
    template<typename Functor>
    void later(Functor&& functor)
    {
        using Stored = std::decay_t<Functor>;
        Stored* stored = new (NotNull, allocate(sizeof(Stored), alignof(Stored))) Stored(std::forward<Functor>(functor));
        m_edits.append({ m_rule, stored, [](void* edit) {
            (*static_cast<Stored*>(edit))();
            static_cast<Stored*>(edit)->~Stored();
        } });
    }
    // A copy of the list that lives as long as the run, for an edit to capture.
    template<typename T>
    std::span<const T> keep(std::span<const T> list)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T* copy = static_cast<T*>(allocate(list.size_bytes(), alignof(T)));
        memcpy(static_cast<void*>(copy), list.data(), list.size_bytes());
        return { copy, list.size() };
    }
    void setRule(unsigned rule) { m_rule = rule; }

    // For edits.
    IRBuilder& builder();
    Value& edit(ValueID id, OptionSet<ValueEdit> edits = anyValueEdit) { return function.edit(id, edits); }
    Block& edit(BlockID, OptionSet<BlockEdit> = anyBlockEdit);
    void replaceUse(const Use&, ValueID replacement);
    ValueID insertBefore(ProgramPoint, Origin, Value&&);
    // The value keeps its number, its place and its origin, and is otherwise the other operation, typed by its rule.
    void become(ValueID, Value&&);
    void erase(BlockID, ValueID);
    void removeParameter(BlockID, uint32_t parameterIndex);
    void replaceUsesOf(ValueID, ValueID replacement);
    void retype(ValueID value) { m_retypedValues.append(value); }
    void count(StatisticID, uint64_t amount = 1);

    PassOutcome commit();

private:
    struct Edit {
        unsigned rule;
        void* functor;
        void (*runAndDestroy)(void*);
    };

    void* allocate(size_t size, size_t alignment)
    {
        uintptr_t start = roundUpToMultipleOf(alignment, reinterpret_cast<uintptr_t>(m_free));
        if (start + size > reinterpret_cast<uintptr_t>(m_endOfRoom)) [[unlikely]]
            return allocateInNewRoom(size);
        m_free = reinterpret_cast<std::byte*>(start + size);
        return reinterpret_cast<void*>(start);
    }
    void* allocateInNewRoom(size_t);

    unsigned m_rule { 0 };
    uint32_t m_numberOfEditsAtStart;
    bool m_didEditEdges { false };
    bool m_hasClaimedBlocks { false };
    std::optional<IRBuilder> m_builder;
    BitVector m_claimedBlocks;
    BitVector m_erasedValues;
    Vector<BlockID, 4> m_blocksOfErasedValues;
    Vector<Edit, 4> m_edits;
    Vector<std::pair<BlockID, uint32_t>, 4> m_removedParameters;
    std::optional<ValueSubstitution> m_substitution;
    Vector<ValueID, 8> m_retypedValues;
    Vector<void*, 1> m_furtherRooms;
    std::byte* m_free { m_firstRoom };
    std::byte* m_endOfRoom { m_firstRoom + sizeof(m_firstRoom) };
    alignas(16) std::byte m_firstRoom[1024];
};

struct Site {
    ProgramPoint point;
    ValueID id;
    const Value& value;
};

enum class BlocksShown : uint8_t { All, WithParameters, WithHandler };

struct Rule {
    using OnValue = void (*)(Rewrite&, const Site&);
    using OnBlock = void (*)(Rewrite&, BlockID, const Block&);

    constexpr Rule(const OpcodeSet& opcodes, OnValue onValue)
        : opcodes(opcodes)
        , onValue(onValue)
    {
    }
    constexpr Rule(const TerminatorKindSet& terminators, OnBlock onBlock, BlocksShown blocks = BlocksShown::All)
        : terminators(terminators)
        , onBlock(onBlock)
        , blocks(blocks)
    {
    }

    OpcodeSet opcodes;
    TerminatorKindSet terminators;
    OnValue onValue { nullptr };
    OnBlock onBlock { nullptr };
    BlocksShown blocks { BlocksShown::All };
};

inline constexpr TerminatorKindSet allTerminators = [] {
    TerminatorKindSet result;
    for (TerminatorKind kind : allMembersOf<TerminatorKind>)
        result.add(kind);
    return result;
}();

enum class Rounds : uint8_t { One, UntilUnchanged };

template<size_t... indexes>
ALWAYS_INLINE void forEachRuleIndex(std::index_sequence<indexes...>, const auto& functor)
{
    (functor(std::integral_constant<size_t, indexes> { }), ...);
}

// A pass is a table of rules. The table is an argument of the template so that the walk calls each rule by name.
template<const auto& rules, Rounds rounds = Rounds::One>
PassOutcome runRules(FunctionPassContext& context)
{
    constexpr auto indexes = std::make_index_sequence<std::size(rules)> { };
    static constexpr OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (const Rule& rule : rules)
            result |= rule.opcodes;
        return result;
    }();

    PassOutcome outcome = PassOutcome::Unchanged;
    do {
        Rewrite rewrite(context);
        for (BlockID id : rewrite.blocks()) {
            const Block& block = rewrite.at(id);
            forEachRuleIndex(indexes, [&](auto index) ALWAYS_INLINE_LAMBDA {
                if constexpr (rules[index].onBlock) {
                    constexpr BlocksShown shown = rules[index].blocks;
                    if ((shown != BlocksShown::WithParameters || !block.parameters.isEmpty()) && (shown != BlocksShown::WithHandler || block.handler)
                        && (rules[index].terminators == allTerminators || rules[index].terminators.contains(block.terminator.kind)) && !rewrite.isClaimed(id)) {
                        rewrite.setRule(index);
                        constexpr auto apply = rules[index].onBlock;
                        [[clang::always_inline]] apply(rewrite, id, block);
                    }
                }
            });
            if constexpr (!opcodes.isEmpty()) {
                for (uint32_t position = 0; position < block.values.size(); ++position) {
                    const Value& value = rewrite.at(block.values[position]);
                    if (!opcodes.contains(value.opcode))
                        continue;
                    forEachRuleIndex(indexes, [&](auto index) ALWAYS_INLINE_LAMBDA {
                        if constexpr (rules[index].onValue) {
                            if (rules[index].opcodes.contains(value.opcode)) {
                                rewrite.setRule(index);
                                constexpr auto apply = rules[index].onValue;
                                [[clang::always_inline]] apply(rewrite, Site { { id, position }, block.values[position], value });
                            }
                        }
                    });
                }
            }
        }
        rewrite.setRule(std::size(rules));
        if (rewrite.commit() == PassOutcome::Unchanged)
            return outcome;
        outcome = PassOutcome::Changed;
    } while (rounds == Rounds::UntilUnchanged);
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
