/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirReuseConditionFlags.h"

#if ENABLE(B3_JIT)

#include "AirCFG.h"
#include "AirCode.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"
#include <algorithm>
#include <utility>
#include <wtf/GraphOrdering.h>
#include <wtf/IndexMap.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

struct Comparison {
    Opcode definerOpcode;
    Arg left;
    Arg right;
    MacroAssembler::RelationalCondition condition;
};

struct AvailableFlags {
    bool matches(const Comparison& comparison) const
    {
        return definerOpcode == comparison.definerOpcode && left == comparison.left && right == comparison.right;
    }

    Opcode definerOpcode;
    Arg left;
    Arg right;
    BasicBlock* definerBlock;
    unsigned definerIndex;
};

struct PendingDefiner {
    BasicBlock* block;
    unsigned index;
    Inst definer;
};

enum class FlagsProducer : uint8_t { Compare, Test };
enum class FlagsConsumer : uint8_t { Branch, ConditionalMove };

struct OpcodeParts {
    Width width;
    FlagsProducer producer;
    FlagsConsumer consumer;
};

std::optional<OpcodeParts> partsOf(Opcode opcode)
{
    switch (opcode) {
    case Branch32:
        return OpcodeParts { Width32, FlagsProducer::Compare, FlagsConsumer::Branch };
    case Branch64:
        return OpcodeParts { Width64, FlagsProducer::Compare, FlagsConsumer::Branch };
    case BranchTest32:
        return OpcodeParts { Width32, FlagsProducer::Test, FlagsConsumer::Branch };
    case BranchTest64:
        return OpcodeParts { Width64, FlagsProducer::Test, FlagsConsumer::Branch };
    case MoveConditionally32:
        return OpcodeParts { Width32, FlagsProducer::Compare, FlagsConsumer::ConditionalMove };
    case MoveConditionally64:
        return OpcodeParts { Width64, FlagsProducer::Compare, FlagsConsumer::ConditionalMove };
    case MoveConditionallyTest32:
        return OpcodeParts { Width32, FlagsProducer::Test, FlagsConsumer::ConditionalMove };
    case MoveConditionallyTest64:
        return OpcodeParts { Width64, FlagsProducer::Test, FlagsConsumer::ConditionalMove };
    default:
        return std::nullopt;
    }
}

Opcode compareOnFlagsOpcode(Width width)
{
    return width == Width32 ? CompareOnFlags32 : CompareOnFlags64;
}

Opcode testOnFlagsOpcode(Width width)
{
    return width == Width32 ? TestOnFlags32 : TestOnFlags64;
}

bool definesFlagsFromIntegers(Opcode opcode)
{
    switch (opcode) {
    case CompareOnFlags32:
    case CompareOnFlags64:
    case TestOnFlags32:
    case TestOnFlags64:
        return true;
    default:
        return false;
    }
}

std::optional<MacroAssembler::RelationalCondition> conditionAgainstZero(MacroAssembler::ResultCondition condition)
{
    switch (condition) {
    case MacroAssembler::Zero:
        return MacroAssembler::Equal;
    case MacroAssembler::NonZero:
        return MacroAssembler::NotEqual;
    case MacroAssembler::Signed:
        return MacroAssembler::LessThan;
    case MacroAssembler::PositiveOrZero:
        return MacroAssembler::GreaterThanOrEqual;
    case MacroAssembler::Overflow:
    case MacroAssembler::Carry:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// MacroAssemblerARM64 emits cbz, cbnz, tbz or tbnz for these branches, so they have no flags to share.
bool isFlaglessBranch(const OpcodeParts& parts, const Comparison& comparison)
{
#if CPU(ARM64)
    if (parts.consumer != FlagsConsumer::Branch || comparison.right.isTmp())
        return false;

    switch (parts.producer) {
    case FlagsProducer::Compare:
        if (parts.width == Width32)
            return MacroAssembler::canBranchWithoutFlags<32>(comparison.condition, comparison.right.value());
        return MacroAssembler::canBranchWithoutFlags<64>(comparison.condition, comparison.right.value());
    case FlagsProducer::Test: {
        if (comparison.condition != MacroAssembler::Equal && comparison.condition != MacroAssembler::NotEqual)
            return false;
        uint64_t mask = parts.width == Width32 ? static_cast<uint32_t>(comparison.right.value()) : static_cast<uint64_t>(comparison.right.value());
        return comparison.right.isImm() || hasOneBitSet(mask);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
#else
    UNUSED_PARAM(parts);
    UNUSED_PARAM(comparison);
    return false;
#endif
}

bool clobbersOperand(Inst& inst, const AvailableFlags& flags)
{
    bool result = false;
    inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
        result |= Arg::isAnyDef(role) && (flags.left.usesTmp(tmp) || flags.right.usesTmp(tmp));
    });
    if (flags.left.isMemory() || flags.right.isMemory()) {
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            result |= Arg::isAnyDef(role) && arg.isMemory();
        });
    }
    return result;
}

class ConditionFlagsReuse {
public:
    explicit ConditionFlagsReuse(Code& code)
        : m_code(code)
        , m_flagsAtTail(code.size())
    {
    }

    bool run()
    {
        m_code.resetReachability();

        Vector<BasicBlock*, 4> roots;
        for (const FrequentedBlock& entrypoint : m_code.entrypoints())
            roots.append(entrypoint.block());
        if (roots.isEmpty())
            roots.append(m_code[0]);

        Vector<BasicBlock*, 64> blocks;
        appendNodesInOrder(m_code.cfg(), roots, GraphOrder::PreOrder, blocks);
        for (BasicBlock* block : blocks)
            processBlock(block);

        insertPendingDefiners();
        return m_changed;
    }

private:
    std::optional<Comparison> comparisonOf(Inst& inst) const
    {
        if (inst.kind.effects)
            return std::nullopt;
        std::optional<OpcodeParts> parts = partsOf(inst.kind.opcode);
        if (!parts)
            return std::nullopt;

        std::span<const Arg> args = inst.args();
        Opcode definerOpcode = compareOnFlagsOpcode(parts->width);
        Arg right = args[2];
        MacroAssembler::RelationalCondition condition;
        if (parts->producer == FlagsProducer::Test) {
            std::optional<MacroAssembler::RelationalCondition> relationalCondition = conditionAgainstZero(args[0].asResultCondition());
            if (!relationalCondition)
                return std::nullopt;
            condition = *relationalCondition;
            if (right == args[1])
                right = Arg::imm(0);
            else {
                definerOpcode = testOnFlagsOpcode(parts->width);
                if (right.isImm())
                    right = Arg::bitImm(right.value());
            }
        } else
            condition = args[0].asRelationalCondition();

        Comparison comparison { definerOpcode, args[1], right, condition };
        if (isFlaglessBranch(*parts, comparison))
            return std::nullopt;
        CPUFeatureSet features = m_code.targetFeatures();
        if (!definerFor(inst, comparison).isValidForm(features) || !onFlagsFormOf(inst, comparison).isValidForm(features))
            return std::nullopt;
        return comparison;
    }

    static Inst definerFor(const Inst& user, const Comparison& comparison)
    {
        return Inst(comparison.definerOpcode, user.origin, comparison.left, comparison.right);
    }

    static Inst onFlagsFormOf(const Inst& user, const Comparison& comparison)
    {
        Arg condition = Arg::relCond(comparison.condition);
        std::span<const Arg> args = user.args();
        switch (args.size()) {
        case 3:
            return Inst(BranchOnFlags, user.origin, condition);
        case 5:
            return Inst(MoveConditionallyOnFlags64, user.origin, condition, args[3], args[4]);
        case 6:
            return Inst(MoveConditionallyOnFlags64, user.origin, condition, args[3], args[4], args[5]);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    std::optional<AvailableFlags> flagsAtHead(BasicBlock* block) const
    {
        if (block->numPredecessors() != 1 || block->predecessor(0) == block || m_code.isEntrypoint(block))
            return std::nullopt;
        return m_flagsAtTail[block->predecessor(0)];
    }

    void processBlock(BasicBlock* block)
    {
        std::optional<AvailableFlags> flags = flagsAtHead(block);
        for (unsigned index = 0; index < block->size(); ++index) {
            Inst& inst = block->at(index);
            if (std::optional<Comparison> comparison = comparisonOf(inst)) {
                if (flags && flags->matches(*comparison)) {
                    makeExplicit(*flags);
                    inst = onFlagsFormOf(inst, *comparison);
                    m_changed = true;
                } else
                    flags = AvailableFlags { comparison->definerOpcode, comparison->left, comparison->right, block, index };
            } else if (definesFlagsFromIntegers(inst.kind.opcode))
                flags = AvailableFlags { inst.kind.opcode, inst.args()[0], inst.args()[1], block, index };
            else if (!preservesFlags(inst.kind.opcode))
                flags = std::nullopt;

            if (flags && clobbersOperand(inst, *flags))
                flags = std::nullopt;
        }
        m_flagsAtTail[block] = flags;
    }

    void makeExplicit(const AvailableFlags& flags)
    {
        Inst& definer = flags.definerBlock->at(flags.definerIndex);
        std::optional<Comparison> comparison = comparisonOf(definer);
        if (!comparison)
            return;
        m_pendingDefiners.append(PendingDefiner { flags.definerBlock, flags.definerIndex, definerFor(definer, *comparison) });
        definer = onFlagsFormOf(definer, *comparison);
    }

    void insertPendingDefiners()
    {
        std::ranges::sort(m_pendingDefiners, { }, [](const PendingDefiner& pending) {
            return std::pair { pending.block->index(), pending.index };
        });

        InsertionSet insertionSet(m_code);
        BasicBlock* block = nullptr;
        for (PendingDefiner& pending : m_pendingDefiners) {
            if (block && block != pending.block)
                insertionSet.execute(block);
            block = pending.block;
            insertionSet.insertInst(pending.index, WTF::move(pending.definer));
        }
        if (block)
            insertionSet.execute(block);
    }

    Code& m_code;
    IndexMap<BasicBlock*, std::optional<AvailableFlags>> m_flagsAtTail;
    Vector<PendingDefiner> m_pendingDefiners;
    bool m_changed { false };
};

} // anonymous namespace

bool reuseConditionFlags(Code& code)
{
    PhaseScope phaseScope(code, "reuseConditionFlags"_s);
    return ConditionFlagsReuse(code).run();
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
