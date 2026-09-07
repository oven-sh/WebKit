/*
 * Copyright (C) 2026 Anthropic PBC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BytecodeOptimizer.h"

#include "BytecodeGenerator.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "ExpressionInfoInlines.h"
#include "JSCJSValueInlines.h"
#include "PreciseJumpTargetsInlines.h"
#include "UnlinkedCodeBlockGenerator.h"
#include "UnlinkedMetadataTableInlines.h"
#include <wtf/DataLog.h>
#include <wtf/FastBitVector.h>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

namespace {

constexpr bool verbose = false;
constexpr unsigned noTarget = UINT_MAX;

struct Insn {
    const JSInstruction* instruction { nullptr }; // Points into the original stream. Null for synthesized instructions.
    unsigned oldOffset { 0 };
    unsigned newOffset { UINT_MAX };
    OpcodeID opcode { op_nop };
    OpcodeSize minimumSize { OpcodeSize::Narrow };
    bool live { true };
    bool hasCheckpoints { false };
    enum Kind : uint8_t { Original, SynthMov, SynthJmp, SynthRet };
    Kind kind { Original };
    VirtualRegister synthDst;
    VirtualRegister synthSrc;
    // Jump targets as instruction indices, in the order extractStoredJumpTargetsForInstruction() reports them.
    // For switches these mirror the jump table (noTarget for empty dense-table slots) followed by the default.
    Vector<unsigned, 1> targets;
    // Operand substitutions, keyed by the original operand value. useMap applies to source operands (any operand
    // not named dst/srcDst), defMap to the operand named dst. Roles are told apart by operand name, so an
    // instruction like `add loc7, loc7, loc8` can have its lhs and its dst renamed independently.
    Vector<std::pair<VirtualRegister, VirtualRegister>, 2> useMap;
    Vector<std::pair<VirtualRegister, VirtualRegister>, 1> defMap;
    // Effective uses/defs after substitution (locals, arguments and constants as reported by BytecodeUseDef).
    Vector<VirtualRegister, 4> uses;
    Vector<VirtualRegister, 2> defs;
    // Registers that appear as explicit source operands / as the dst operand in the encoded instruction.
    Vector<VirtualRegister, 4> explicitUses;
    Vector<VirtualRegister, 1> explicitDefs;
    // Registers read through a register range (call arguments, new_array/strcat operands); never renamed.
    Vector<VirtualRegister, 4> implicitUses;
    // Calls build the callee frame on top of the caller's registers: every local with index >= clobberFrom is
    // garbage afterwards (frame header, arguments the callee may overwrite, and the callee's own locals).
    unsigned clobberFrom { UINT_MAX };
    unsigned clobberEnd { UINT_MAX }; // locals at or beyond this index are fresh registers added by the optimizer, placed below all frames
    bool hasSrcDst { false };
    // If valid, "mov copyTo, dst" is emitted right after this instruction (a fresh register caching its result).
    VirtualRegister copyTo;

    bool clobbers(VirtualRegister r) const { return r.isLocal() && static_cast<unsigned>(r.toLocal()) >= clobberFrom && static_cast<unsigned>(r.toLocal()) < clobberEnd; }

    OpcodeID effectiveOpcode() const
    {
        switch (kind) {
        case Original:
            return opcode;
        case SynthMov:
            return op_mov;
        case SynthJmp:
            return op_jmp;
        case SynthRet:
            return op_ret;
        }
        return opcode;
    }
    bool isBranchOrSwitch() const { return isBranch(effectiveOpcode()); }
    bool isJmp() const { return effectiveOpcode() == op_jmp; }
    bool isMov() const { return effectiveOpcode() == op_mov; }
    bool endsBlock() const { return isBranch(effectiveOpcode()) || isTerminal(effectiveOpcode()) || isThrow(effectiveOpcode()); }
};

struct Block {
    unsigned start { 0 }; // instruction index range [start, end)
    unsigned end { 0 };
    bool reachable { false };
    Vector<unsigned, 2> successors;
    Vector<unsigned, 2> predecessors;
    FastBitVector liveIn;
    FastBitVector liveOut;
};

static bool isValueProfileOperand(BytecodeOperandName name)
{
    switch (name) {
    case BytecodeOperandName::valueProfile:
    case BytecodeOperandName::iterableValueProfile:
    case BytecodeOperandName::iteratorValueProfile:
    case BytecodeOperandName::nextValueProfile:
    case BytecodeOperandName::nextResultValueProfile:
    case BytecodeOperandName::doneValueProfile:
    case BytecodeOperandName::valueValueProfile:
    case BytecodeOperandName::hasInstanceValueProfile:
    case BytecodeOperandName::prototypeValueProfile:
        return true;
    default:
        return false;
    }
}

// Instructions with no effects besides defining their destination: removable when the destination is dead.
static bool isPure(const Insn& insn)
{
    if (insn.kind == Insn::SynthMov)
        return true;
    if (insn.kind != Insn::Original)
        return false;
    switch (insn.opcode) {
    case op_mov:
    case op_get_scope:
    case op_resolve_scope:
    case op_is_empty:
    case op_typeof_is_undefined:
    case op_typeof_is_object:
    case op_typeof_is_function:
    case op_is_undefined_or_null:
    case op_is_boolean:
    case op_is_number:
    case op_is_big_int:
    case op_is_object:
    case op_is_callable:
    case op_is_constructor:
    case op_is_cell_with_type:
    case op_has_structure_with_flags:
    case op_not:
    case op_stricteq:
    case op_nstricteq:
    case op_typeof:
    case op_new_object:
    case op_new_reg_exp:
    case op_new_func:
    case op_new_func_exp:
    case op_new_generator_func:
    case op_new_generator_func_exp:
    case op_new_async_func:
    case op_new_async_func_exp:
    case op_new_async_generator_func:
    case op_new_async_generator_func_exp:
    case op_get_argument:
    case op_argument_count:
        return true;
    case op_get_from_scope:
        return insn.instruction->as<OpGetFromScope>().m_getPutInfo.resolveType() == ResolvedClosureVar;
    default:
        return false;
    }
}

// Instructions whose register operands we never rewrite (checkpointed ops are read through
// BytecodeOperandsForCheckpoint; op_yield and friends are rewritten by generatorification).
static bool allowsOperandSubstitution(const Insn& insn)
{
    if (insn.kind != Insn::Original)
        return insn.kind == Insn::SynthMov || insn.kind == Insn::SynthRet;
    if (insn.hasCheckpoints)
        return false;
    switch (insn.opcode) {
    case op_enter:
    case op_yield:
    case op_create_generator_frame_environment:
    case op_catch:
    case op_create_direct_arguments:
    case op_create_scoped_arguments:
    case op_create_cloned_arguments:
    case op_create_rest:
    case op_profile_type:
    case op_profile_control_flow:
    case op_debug:
    case op_log_shadow_chicken_prologue:
    case op_log_shadow_chicken_tail:
    case op_call_direct_eval:
    case op_unreachable:
    case op_enumerator_next:
    case op_async_iterator_open:
    case op_async_iterator_next:
        return false;
    default:
        return true;
    }
}

} // anonymous namespace

class BytecodeOptimizerAccess {
public:
    BytecodeOptimizerAccess(BytecodeGenerator& generator, UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& writer)
        : m_generator(generator)
        , m_codeBlock(codeBlock)
        , m_writer(writer)
        , m_numLocals(codeBlock->numCalleeLocals())
        , m_originalNumLocals(codeBlock->numCalleeLocals())
        , m_originalNumVars(codeBlock->numVars())
    {
    }

    void run();
    static void runIfAppropriate(BytecodeGenerator&);

private:
    void decode();
    void computeImplicitUses(Insn&);
    void computeUseDef(Insn&);
    void applySubstitutions(Insn&);
    void buildBlocks();
    unsigned blockIndexForInsn(unsigned insnIndex) const;
    bool removeUnreachable();
    std::optional<JSValue> constantValue(VirtualRegister) const;
    std::optional<bool> evaluateConstantBranch(const Insn&) const;
    bool simplifyJumps();
    void stepLiveness(FastBitVector&, const Insn&) const;
    void computeLiveness();
    bool eliminateDeadStores();
    bool propagateCopies();
    bool coalesceDestinations();
    bool eliminateRedundantTDZChecks();
    unsigned resolveJumpChain(unsigned target) const;
    unsigned nextLiveInsn(unsigned index) const;
    const UnlinkedHandlerInfo* handlerForInsn(const Insn& insn) const
    {
        unsigned index = m_handlerForInsn[&insn - m_insns.begin()];
        return index == UINT_MAX ? nullptr : &m_handlers[index];
    }
    void computeHandlerMap();
    bool isLocal(VirtualRegister r) const { return r.isLocal() && static_cast<unsigned>(r.toLocal()) < m_numLocals; }
    void emit();
    void dumpIR(const char* title);

    struct Mapper;
    friend struct Mapper;

    BytecodeGenerator& m_generator;
    UnlinkedCodeBlockGenerator* m_codeBlock;
    JSInstructionStreamWriter& m_writer;
    Vector<UnlinkedHandlerInfo> m_handlers; // Copy in original offsets, used for coverage queries throughout.
    Vector<Insn> m_insns;
    Vector<unsigned> m_handlerForInsn; // innermost handler covering each instruction (by original offset), or UINT_MAX
    Vector<unsigned> m_offsetToIndex;
    Vector<Block> m_blocks;
    Vector<unsigned> m_blockForInsn;
    unsigned m_numLocals; // in analysis numbering: original locals, then fresh registers
    unsigned m_originalNumLocals;
    unsigned m_originalNumVars;
    unsigned m_numFreshRegisters { 0 }; // allocated by the optimizer; physically placed right after the original vars
    bool m_changedControlFlow { true };

    VirtualRegister allocateFreshRegister()
    {
        VirtualRegister result = virtualRegisterForLocal(m_numLocals);
        ++m_numLocals;
        ++m_numFreshRegisters;
        return result;
    }
    // Analysis register -> register in the emitted code block.
    VirtualRegister physicalRegister(VirtualRegister r) const
    {
        if (!r.isLocal() || !m_registerShift)
            return r;
        unsigned index = r.toLocal();
        if (index < m_originalNumVars)
            return r;
        if (index < m_originalNumLocals)
            return virtualRegisterForLocal(index + m_registerShift);
        return virtualRegisterForLocal(m_originalNumVars + (index - m_originalNumLocals));
    }
    unsigned m_registerShift { 0 };
    struct ScopeValue {
        int base;
        unsigned via;
    };
    UncheckedKeyHashMap<int, ScopeValue, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> m_scopeCacheContents; // cache register -> what it holds
    bool cacheScopeResolutions();
    bool isStableScopeName(unsigned identifierIndex) const;
};

void BytecodeOptimizerAccess::dumpIR(const char* title)
{
    dataLogLn("BytecodeOptimizer: ", title);
    for (unsigned i = 0; i < m_insns.size(); ++i) {
        auto& insn = m_insns[i];
        dataLog("  ", insn.live ? " " : "X", " #", i, " @", insn.oldOffset, " ");
        switch (insn.kind) {
        case Insn::Original:
            dataLog(opcodeNames[insn.opcode]);
            break;
        case Insn::SynthMov:
            dataLog("mov* ", insn.synthDst, ", ", insn.synthSrc);
            break;
        case Insn::SynthJmp:
            dataLog("jmp*");
            break;
        case Insn::SynthRet:
            dataLog("ret* ", insn.synthSrc);
            break;
        }
        if (!insn.targets.isEmpty()) {
            dataLog(" ->");
            for (unsigned t : insn.targets)
                dataLog(" #", t == noTarget ? -1 : static_cast<int>(t));
        }
        if (!insn.useMap.isEmpty() || !insn.defMap.isEmpty()) {
            dataLog(" {");
            for (auto& pair : insn.useMap)
                dataLog(" use ", pair.first, "=>", pair.second);
            for (auto& pair : insn.defMap)
                dataLog(" def ", pair.first, "=>", pair.second);
            dataLog(" }");
        }
        dataLog(" uses[");
        for (auto r : insn.uses)
            dataLog(" ", r);
        dataLog(" ] defs[");
        for (auto r : insn.defs)
            dataLog(" ", r);
        dataLogLn(" ]");
    }
}

void BytecodeOptimizerAccess::decode()
{
    unsigned size = m_writer.size();
    m_offsetToIndex.fill(UINT_MAX, size + 1);
    m_insns.reserveInitialCapacity(size / 3);
    for (const auto& instruction : m_writer) {
        Insn insn;
        insn.instruction = instruction.ptr();
        insn.oldOffset = instruction.offset();
        insn.opcode = instruction->opcodeID();
        insn.hasCheckpoints = static_cast<unsigned>(insn.opcode) < NUMBER_OF_BYTECODE_WITH_CHECKPOINTS;
        insn.clobberEnd = m_numLocals;
        m_offsetToIndex[insn.oldOffset] = m_insns.size();
        m_insns.append(WTF::move(insn));
    }
    m_offsetToIndex[size] = m_insns.size();

    for (unsigned i = 0; i < m_insns.size(); ++i) {
        auto& insn = m_insns[i];
        if (isBranch(insn.opcode)) {
            auto ref = m_writer.ref(insn.oldOffset);
            extractStoredJumpTargetsForInstruction(m_codeBlock, ref, [&](int32_t relativeOffset) {
                if (!relativeOffset) {
                    // Empty dense switch table slot.
                    insn.targets.append(noTarget);
                    return;
                }
                unsigned absolute = insn.oldOffset + relativeOffset;
                RELEASE_ASSERT(absolute <= size && m_offsetToIndex[absolute] != UINT_MAX);
                insn.targets.append(m_offsetToIndex[absolute]);
            });
        }
        auto visitor = [&](BytecodeOperandName name, auto operand) {
            if constexpr (std::is_same_v<decltype(operand), VirtualRegister>) {
                switch (name) {
                case BytecodeOperandName::firstFree:
                    break;
                case BytecodeOperandName::dst:
                    insn.explicitDefs.append(operand);
                    break;
                case BytecodeOperandName::srcDst:
                    insn.hasSrcDst = true;
                    break;
                default:
                    // Operands that name the start of a register range are not individually renameable.
                    if ((insn.opcode == op_new_array || insn.opcode == op_new_array_with_spread) && name == BytecodeOperandName::argv)
                        break;
                    if (insn.opcode == op_strcat && name == BytecodeOperandName::src)
                        break;
                    if (!insn.explicitUses.contains(operand))
                        insn.explicitUses.append(operand);
                    break;
                }
            }
        };
        visitInstructionOperands(insn.instruction, visitor);
        computeImplicitUses(insn);
        computeUseDef(insn);
    }
}

void BytecodeOptimizerAccess::computeImplicitUses(Insn& insn)
{
    auto range = [&](VirtualRegister first, int step, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            insn.implicitUses.append(VirtualRegister { first.offset() + step * static_cast<int>(i) });
    };
    auto clobberFrame = [&](unsigned argv, unsigned argc) {
        // The callee frame starts at offset -argv and is sized in whole stack-alignment units, so the padding slot
        // above the last argument (reserved by CallArguments) belongs to it too: tail calls and arity fixup in the
        // callee may write it. Everything at or below that is garbage after the call.
        int end = -static_cast<int>(argv) + static_cast<int>(WTF::roundUpToMultipleOf(stackAlignmentRegisters(), CallFrame::headerSizeInRegisters + argc));
        insn.clobberFrom = static_cast<unsigned>(VirtualRegister { end - 1 }.toLocal());
    };
    auto callLike = [&](auto op) {
        int lastArg = -static_cast<int>(op.m_argv) + CallFrame::thisArgumentOffset();
        range(VirtualRegister { lastArg }, 1, op.m_argc);
        clobberFrame(op.m_argv, op.m_argc);
    };
    auto varargsLike = [&](auto op) {
        if (op.m_firstFree.isLocal())
            insn.clobberFrom = static_cast<unsigned>(op.m_firstFree.toLocal());
    };
    switch (insn.opcode) {
    case op_call:
        callLike(insn.instruction->as<OpCall>());
        break;
    case op_tail_call:
        callLike(insn.instruction->as<OpTailCall>());
        break;
    case op_call_ignore_result:
        callLike(insn.instruction->as<OpCallIgnoreResult>());
        break;
    case op_construct:
        callLike(insn.instruction->as<OpConstruct>());
        break;
    case op_super_construct:
        callLike(insn.instruction->as<OpSuperConstruct>());
        break;
    case op_call_direct_eval:
        callLike(insn.instruction->as<OpCallDirectEval>());
        break;
    case op_iterator_open: {
        auto op = insn.instruction->as<OpIteratorOpen>();
        range(VirtualRegister { -static_cast<int>(op.m_stackOffset) + CallFrame::thisArgumentOffset() }, 1, 1);
        clobberFrame(op.m_stackOffset, 1);
        break;
    }
    case op_iterator_next: {
        auto op = insn.instruction->as<OpIteratorNext>();
        range(VirtualRegister { -static_cast<int>(op.m_stackOffset) + CallFrame::thisArgumentOffset() }, 1, 1);
        clobberFrame(op.m_stackOffset, 1);
        break;
    }
    case op_async_iterator_open: {
        auto op = insn.instruction->as<OpAsyncIteratorOpen>();
        range(VirtualRegister { -static_cast<int>(op.m_stackOffset) + CallFrame::thisArgumentOffset() }, 1, 1);
        clobberFrame(op.m_stackOffset, 1);
        break;
    }
    case op_async_iterator_next: {
        // The iterator operand aliases the call frame's |this| slot; the resume value sits in argument 1.
        auto op = insn.instruction->as<OpAsyncIteratorNext>();
        range(VirtualRegister { -static_cast<int>(op.m_stackOffset) + CallFrame::thisArgumentOffset() }, 1, op.m_hasValue ? 2 : 1);
        clobberFrame(op.m_stackOffset, op.m_hasValue ? 2 : 1);
        break;
    }
    case op_call_varargs:
        varargsLike(insn.instruction->as<OpCallVarargs>());
        break;
    case op_tail_call_varargs:
        varargsLike(insn.instruction->as<OpTailCallVarargs>());
        break;
    case op_construct_varargs:
        varargsLike(insn.instruction->as<OpConstructVarargs>());
        break;
    case op_super_construct_varargs:
        varargsLike(insn.instruction->as<OpSuperConstructVarargs>());
        break;

    case op_new_array: {
        auto op = insn.instruction->as<OpNewArray>();
        range(op.m_argv, -1, op.m_argc);
        break;
    }
    case op_new_array_with_spread: {
        auto op = insn.instruction->as<OpNewArrayWithSpread>();
        range(op.m_argv, -1, op.m_argc);
        break;
    }
    case op_strcat: {
        auto op = insn.instruction->as<OpStrcat>();
        range(op.m_src, -1, op.m_count);
        break;
    }
    default:
        break;
    }
}

void BytecodeOptimizerAccess::computeUseDef(Insn& insn)
{
    insn.uses.shrink(0);
    insn.defs.shrink(0);
    if (insn.kind == Insn::SynthMov) {
        insn.uses.append(insn.synthSrc);
        insn.defs.append(insn.synthDst);
        return;
    }
    if (insn.kind == Insn::SynthJmp)
        return;
    if (insn.kind == Insn::SynthRet) {
        insn.uses.append(insn.synthSrc);
        return;
    }

    auto addUse = [&](VirtualRegister r) {
        if (!insn.uses.contains(r))
            insn.uses.append(r);
    };
    auto addDef = [&](VirtualRegister r) {
        if (!insn.defs.contains(r))
            insn.defs.append(r);
    };
    ScopedLambda<void(VirtualRegister)> useFunctor(addUse);
    ScopedLambda<void(VirtualRegister)> defFunctor(addDef);
    unsigned checkpoints = insn.hasCheckpoints ? bytecodeCheckpointCountTable[insn.opcode] : 1;
    for (unsigned checkpoint = 0; checkpoint < checkpoints; ++checkpoint) {
        computeUsesForBytecodeIndexImpl(insn.instruction, checkpoint, useFunctor);
        computeDefsForBytecodeIndexImpl(m_originalNumVars, insn.instruction, checkpoint, defFunctor);
    }
    applySubstitutions(insn);
    if (insn.copyTo.isValid())
        addDef(insn.copyTo);
}

void BytecodeOptimizerAccess::applySubstitutions(Insn& insn)
{
    if (insn.useMap.isEmpty() && insn.defMap.isEmpty())
        return;
    Vector<VirtualRegister, 4> uses;
    auto add = [&](Vector<VirtualRegister, 4>& list, VirtualRegister r) {
        if (!list.contains(r))
            list.append(r);
    };
    for (auto r : insn.uses) {
        bool isExplicit = insn.explicitUses.contains(r);
        bool isImplicit = !isExplicit || insn.implicitUses.contains(r) || (insn.hasSrcDst && insn.defs.contains(r));
        if (isExplicit) {
            VirtualRegister mapped = r;
            for (auto& pair : insn.useMap) {
                if (pair.first == r)
                    mapped = pair.second;
            }
            add(uses, mapped);
        }
        if (isImplicit)
            add(uses, r);
    }
    insn.uses = WTF::move(uses);
    for (auto& r : insn.defs) {
        if (!insn.explicitDefs.contains(r))
            continue;
        for (auto& pair : insn.defMap) {
            if (r == pair.first) {
                r = pair.second;
                break;
            }
        }
    }
}

void BytecodeOptimizerAccess::computeHandlerMap()
{
    // Handlers are ordered innermost first: the first handler in the list that covers an offset wins. Assign in
    // list order, skipping already-assigned runs with a "next unassigned" forest so this stays near-linear.
    unsigned count = m_insns.size();
    m_handlerForInsn.fill(UINT_MAX, count);
    Vector<unsigned> nextUnassigned(count + 1);
    for (unsigned i = 0; i <= count; ++i)
        nextUnassigned[i] = i;
    auto find = [&](unsigned i) {
        unsigned root = i;
        while (nextUnassigned[root] != root)
            root = nextUnassigned[root];
        while (nextUnassigned[i] != root) {
            unsigned next = nextUnassigned[i];
            nextUnassigned[i] = root;
            i = next;
        }
        return root;
    };
    for (unsigned h = 0; h < m_handlers.size(); ++h) {
        auto& handler = m_handlers[h];
        unsigned begin = m_offsetToIndex[handler.start];
        unsigned end = m_offsetToIndex[handler.end];
        RELEASE_ASSERT(begin != UINT_MAX && end != UINT_MAX);
        for (unsigned i = find(begin); i < end; i = find(i + 1)) {
            m_handlerForInsn[i] = h;
            nextUnassigned[i] = i + 1;
        }
    }
}

unsigned BytecodeOptimizerAccess::nextLiveInsn(unsigned index) const
{
    while (index < m_insns.size() && !m_insns[index].live)
        ++index;
    return index;
}

void BytecodeOptimizerAccess::buildBlocks()
{
    unsigned count = m_insns.size();
    Vector<bool> isLeader;
    isLeader.fill(false, count + 1);
    isLeader[0] = true;
    isLeader[count] = true;
    bool nextIsLeader = false;
    for (unsigned i = 0; i < count; ++i) {
        auto& insn = m_insns[i];
        if (!insn.live)
            continue;
        if (nextIsLeader) {
            isLeader[i] = true;
            nextIsLeader = false;
        }
        if (insn.kind == Insn::Original && insn.opcode == op_catch)
            isLeader[i] = true;
        for (unsigned target : insn.targets) {
            if (target != noTarget)
                isLeader[nextLiveInsn(target)] = true;
        }
        if (insn.endsBlock())
            nextIsLeader = true;
    }
    // Exception handler boundaries and targets start blocks too, so that handler coverage is uniform per block.
    for (auto& handler : m_handlers) {
        for (unsigned offset : { handler.start, handler.end, handler.target }) {
            unsigned index = nextLiveInsn(m_offsetToIndex[offset]);
            isLeader[index] = true;
        }
    }

    m_blocks.shrink(0);
    m_blockForInsn.fill(UINT_MAX, count);
    unsigned start = UINT_MAX;
    for (unsigned i = 0; i <= count; ++i) {
        if (i < count && !m_insns[i].live)
            continue;
        if (isLeader[i]) {
            if (start != UINT_MAX) {
                Block block;
                block.start = start;
                block.end = i;
                m_blocks.append(WTF::move(block));
            }
            start = i;
        }
        if (i < count)
            m_blockForInsn[i] = m_blocks.size();
    }
    // Dead instructions between blocks belong to no block; blockIndexForInsn() is only asked about live ones.

    auto blockStartingAt = [&](unsigned insnIndex) -> unsigned {
        unsigned live = nextLiveInsn(insnIndex);
        RELEASE_ASSERT(live < count);
        unsigned blockIndex = m_blockForInsn[live];
        RELEASE_ASSERT(m_blocks[blockIndex].start == live);
        return blockIndex;
    };

    for (unsigned b = 0; b < m_blocks.size(); ++b) {
        auto& block = m_blocks[b];
        // Find the last live instruction of the block.
        unsigned last = block.end;
        while (last > block.start && !m_insns[last - 1].live)
            --last;
        RELEASE_ASSERT(last > block.start);
        auto& insn = m_insns[last - 1];
        bool fallsThrough = true;
        auto addSuccessor = [&](unsigned s) {
            if (!block.successors.contains(s))
                block.successors.append(s);
        };
        if (isTerminal(insn.effectiveOpcode()))
            fallsThrough = false;
        else if (isThrow(insn.effectiveOpcode())) {
            fallsThrough = false;
            if (auto* handler = handlerForInsn(insn))
                addSuccessor(blockStartingAt(m_offsetToIndex[handler->target]));
        } else if (insn.isBranchOrSwitch()) {
            for (unsigned target : insn.targets) {
                if (target != noTarget)
                    addSuccessor(blockStartingAt(target));
            }
            if (insn.isJmp())
                fallsThrough = false;
        }
        if (fallsThrough) {
            unsigned next = nextLiveInsn(block.end);
            if (next < count)
                addSuccessor(m_blockForInsn[next]);
            // Falling off the end can only happen after op_unreachable-style artifacts; treat as exit.
        }
    }
    for (unsigned b = 0; b < m_blocks.size(); ++b) {
        for (unsigned s : m_blocks[b].successors)
            m_blocks[s].predecessors.append(b);
    }

    // Reachability from the entry block; exception handlers are reachable if any covered block is.
    Vector<unsigned, 16> worklist;
    m_blocks[0].reachable = true;
    worklist.append(0);
    while (!worklist.isEmpty()) {
        unsigned b = worklist.takeLast();
        auto& block = m_blocks[b];
        auto visit = [&](unsigned s) {
            if (!m_blocks[s].reachable) {
                m_blocks[s].reachable = true;
                worklist.append(s);
            }
        };
        for (unsigned s : block.successors)
            visit(s);
        unsigned offset = m_insns[block.start].oldOffset;
        for (auto& handler : m_handlers) {
            if (handler.start <= offset && handler.end > offset)
                visit(blockStartingAt(m_offsetToIndex[handler.target]));
        }
    }
}

bool BytecodeOptimizerAccess::removeUnreachable()
{
    bool changed = false;
    for (auto& block : m_blocks) {
        if (block.reachable)
            continue;
        for (unsigned i = block.start; i < block.end; ++i) {
            if (m_insns[i].live) {
                m_insns[i].live = false;
                changed = true;
            }
        }
    }
    return changed;
}

unsigned BytecodeOptimizerAccess::resolveJumpChain(unsigned target) const
{
    // Follow jmp -> jmp chains (bounded, to stay clear of pathological cycles).
    for (unsigned hops = 0; hops < 8; ++hops) {
        unsigned live = nextLiveInsn(target);
        if (live >= m_insns.size())
            break;
        auto& insn = m_insns[live];
        if (!insn.isJmp())
            return live;
        unsigned next = insn.targets[0];
        if (nextLiveInsn(next) == live)
            return live; // Self loop.
        // Do not thread through a jmp that sits inside a different exception handler range than the target:
        // jumping directly is still correct (jmp cannot throw), so no restriction is needed here.
        target = next;
    }
    return nextLiveInsn(target);
}

// The compile-time value of a constant register, if it is a plain value (link-time constants are placeholders).
std::optional<JSValue> BytecodeOptimizerAccess::constantValue(VirtualRegister reg) const
{
    if (!reg.isConstant())
        return std::nullopt;
    if (m_codeBlock->constantSourceCodeRepresentation(reg) == SourceCodeRepresentation::LinkTimeConstant)
        return std::nullopt;
    JSValue value = m_codeBlock->getConstant(reg);
    if (!value)
        return std::nullopt;
    if (value.isCell() && !value.isString())
        return std::nullopt;
    return value;
}

static std::optional<bool> constantToBoolean(JSValue value)
{
    if (value.isUndefinedOrNull())
        return false;
    if (value.isBoolean())
        return value.asBoolean();
    if (value.isInt32())
        return !!value.asInt32();
    if (value.isDouble())
        return value.asDouble() > 0.0 || value.asDouble() < 0.0; // false for 0, -0 and NaN
    if (value.isString())
        return !!asString(value)->length();
    return std::nullopt;
}

std::optional<bool> BytecodeOptimizerAccess::evaluateConstantBranch(const Insn& insn) const
{
    auto operand = [&](VirtualRegister original) -> std::optional<JSValue> {
        VirtualRegister reg = original;
        for (auto& pair : insn.useMap) {
            if (pair.first == original)
                reg = pair.second;
        }
        return constantValue(reg);
    };
    auto numbers = [&](VirtualRegister lhsReg, VirtualRegister rhsReg, auto compare) -> std::optional<bool> {
        auto lhs = operand(lhsReg);
        auto rhs = operand(rhsReg);
        if (!lhs || !rhs || !lhs->isNumber() || !rhs->isNumber())
            return std::nullopt;
        return compare(lhs->asNumber(), rhs->asNumber());
    };
    auto strictEqual = [&](VirtualRegister lhsReg, VirtualRegister rhsReg) -> std::optional<bool> {
        auto lhs = operand(lhsReg);
        auto rhs = operand(rhsReg);
        if (!lhs || !rhs)
            return std::nullopt;
        if (lhs->isNumber() && rhs->isNumber())
            return lhs->asNumber() == rhs->asNumber();
        if (lhs->isString() || rhs->isString()) {
            if (lhs->isString() && rhs->isString()) {
                auto a = asString(*lhs)->tryGetValue();
                auto b = asString(*rhs)->tryGetValue();
                return WTF::equal(a.data.impl(), b.data.impl());
            }
            return false;
        }
        return *lhs == *rhs; // undefined, null, booleans: encoded identically iff strictly equal.
    };
    switch (insn.opcode) {
    case op_jtrue:
    case op_jfalse: {
        auto value = operand(insn.instruction->as<OpJtrue>().m_condition); // OpJfalse has the same layout.
        if (!value)
            return std::nullopt;
        auto truth = constantToBoolean(*value);
        if (!truth)
            return std::nullopt;
        return insn.opcode == op_jtrue ? *truth : !*truth;
    }
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null: {
        auto value = operand(insn.instruction->as<OpJeqNull>().m_value);
        if (!value)
            return std::nullopt;
        bool nullish = value->isUndefinedOrNull();
        return (insn.opcode == op_jeq_null || insn.opcode == op_jundefined_or_null) ? nullish : !nullish;
    }
    case op_jstricteq:
    case op_jnstricteq: {
        auto bytecode = insn.instruction->as<OpJstricteq>();
        auto equal = strictEqual(bytecode.m_lhs, bytecode.m_rhs);
        if (!equal)
            return std::nullopt;
        return insn.opcode == op_jstricteq ? *equal : !*equal;
    }
    case op_jless:
        return numbers(insn.instruction->as<OpJless>().m_lhs, insn.instruction->as<OpJless>().m_rhs, [](double a, double b) { return a < b; });
    case op_jlesseq:
        return numbers(insn.instruction->as<OpJlesseq>().m_lhs, insn.instruction->as<OpJlesseq>().m_rhs, [](double a, double b) { return a <= b; });
    case op_jgreater:
        return numbers(insn.instruction->as<OpJgreater>().m_lhs, insn.instruction->as<OpJgreater>().m_rhs, [](double a, double b) { return a > b; });
    case op_jgreatereq:
        return numbers(insn.instruction->as<OpJgreatereq>().m_lhs, insn.instruction->as<OpJgreatereq>().m_rhs, [](double a, double b) { return a >= b; });
    case op_jnless:
        return numbers(insn.instruction->as<OpJnless>().m_lhs, insn.instruction->as<OpJnless>().m_rhs, [](double a, double b) { return !(a < b); });
    case op_jnlesseq:
        return numbers(insn.instruction->as<OpJnlesseq>().m_lhs, insn.instruction->as<OpJnlesseq>().m_rhs, [](double a, double b) { return !(a <= b); });
    case op_jngreater:
        return numbers(insn.instruction->as<OpJngreater>().m_lhs, insn.instruction->as<OpJngreater>().m_rhs, [](double a, double b) { return !(a > b); });
    case op_jngreatereq:
        return numbers(insn.instruction->as<OpJngreatereq>().m_lhs, insn.instruction->as<OpJngreatereq>().m_rhs, [](double a, double b) { return !(a >= b); });
    default:
        return std::nullopt;
    }
}

bool BytecodeOptimizerAccess::simplifyJumps()
{
    bool changed = false;
    for (unsigned i = 0; i < m_insns.size(); ++i) {
        auto& insn = m_insns[i];
        if (!insn.live || !insn.isBranchOrSwitch())
            continue;
        for (unsigned& target : insn.targets) {
            if (target == noTarget)
                continue;
            unsigned resolved = resolveJumpChain(target);
            if (resolved != target) {
                target = resolved;
                changed = true;
                m_changedControlFlow = true;
            }
        }
        if (insn.isJmp()) {
            // jmp to the next live instruction is a no-op.
            if (nextLiveInsn(insn.targets[0]) == nextLiveInsn(i + 1)) {
                insn.live = false;
                changed = true;
                m_changedControlFlow = true;
                continue;
            }
            // jmp to a ret: return directly (same size, one dispatch less, and the ret block may become dead).
            unsigned target = nextLiveInsn(insn.targets[0]);
            if (target < m_insns.size() && m_insns[target].effectiveOpcode() == op_ret && m_insns[target].uses.size() == 1) {
                insn.kind = Insn::SynthRet;
                insn.synthSrc = m_insns[target].uses[0];
                insn.targets.shrink(0);
                insn.useMap.shrink(0);
                insn.defMap.shrink(0);
                computeUseDef(insn);
                changed = true;
                m_changedControlFlow = true;
            }
            continue;
        }
        if (insn.kind == Insn::Original && insn.targets.size() == 1) {
            if (auto known = evaluateConstantBranch(insn)) {
                if (*known) {
                    insn.kind = Insn::SynthJmp;
                    insn.useMap.shrink(0);
                    insn.defMap.shrink(0);
                    computeUseDef(insn);
                } else
                    insn.live = false;
                changed = true;
                m_changedControlFlow = true;
                continue;
            }
        }
        if (insn.kind == Insn::Original && insn.targets.size() == 1) {
            // A conditional branch whose target is the fall-through successor is a no-op if evaluating the
            // condition has no effects. That holds for the jumps that test a value without conversions.
            switch (insn.opcode) {
            case op_jtrue:
            case op_jfalse:
            case op_jeq_null:
            case op_jneq_null:
            case op_jundefined_or_null:
            case op_jnundefined_or_null:
            case op_jstricteq:
            case op_jnstricteq:
            case op_jeq_ptr:
                if (nextLiveInsn(insn.targets[0]) == nextLiveInsn(i + 1)) {
                    insn.live = false;
                    changed = true;
                    m_changedControlFlow = true;
                }
                break;
            default:
                break;
            }
        }
    }
    return changed;
}

unsigned BytecodeOptimizerAccess::blockIndexForInsn(unsigned insnIndex) const
{
    return m_blockForInsn[insnIndex];
}

void BytecodeOptimizerAccess::stepLiveness(FastBitVector& live, const Insn& insn) const
{
    for (auto r : insn.defs) {
        if (isLocal(r))
            live[r.toLocal()] = false;
    }
    if (insn.clobberFrom < insn.clobberEnd)
        live.clearRange(insn.clobberFrom, std::min(insn.clobberEnd, m_numLocals));
    for (auto r : insn.uses) {
        if (isLocal(r))
            live[r.toLocal()] = true;
    }
    if (auto* handler = handlerForInsn(insn)) {
        unsigned target = nextLiveInsn(m_offsetToIndex[handler->target]);
        live |= m_blocks[m_blockForInsn[target]].liveIn;
    }
}

void BytecodeOptimizerAccess::computeLiveness()
{
    unsigned numBits = m_numLocals;
    for (auto& block : m_blocks) {
        block.liveIn = FastBitVector();
        block.liveIn.resize(numBits);
        block.liveOut = FastBitVector();
        block.liveOut.resize(numBits);
    }

    bool changed;
    FastBitVector live;
    live.resize(numBits);
    do {
        changed = false;
        for (unsigned b = m_blocks.size(); b--;) {
            auto& block = m_blocks[b];
            if (!block.reachable)
                continue;
            live.resize(numBits);
            live.clearAll();
            for (unsigned s : block.successors)
                live |= m_blocks[s].liveIn;
            block.liveOut = live;
            for (unsigned i = block.end; i-- > block.start;) {
                auto& insn = m_insns[i];
                if (!insn.live)
                    continue;
                stepLiveness(live, insn);
            }
            if (live != block.liveIn) {
                block.liveIn = live;
                changed = true;
            }
        }
    } while (changed);
}

bool BytecodeOptimizerAccess::eliminateDeadStores()
{
    bool changed = false;
    FastBitVector live;
    for (auto& block : m_blocks) {
        if (!block.reachable)
            continue;
        live = block.liveOut;
        for (unsigned i = block.end; i-- > block.start;) {
            auto& insn = m_insns[i];
            if (!insn.live)
                continue;
            if (insn.copyTo.isValid() && !live[insn.copyTo.toLocal()]) {
                insn.copyTo = VirtualRegister();
                computeUseDef(insn);
                changed = true;
            }
            if (isPure(insn) && !insn.defs.isEmpty()) {
                bool allDead = true;
                for (auto r : insn.defs) {
                    if (!isLocal(r) || live[r.toLocal()]) {
                        allDead = false;
                        break;
                    }
                }
                if (allDead) {
                    insn.live = false;
                    changed = true;
                    continue;
                }
            }
            if (insn.isMov() && insn.uses.size() == 1 && insn.defs.size() == 1 && insn.uses[0] == insn.defs[0]) {
                // Self move after substitution.
                insn.live = false;
                changed = true;
                continue;
            }
            stepLiveness(live, insn);
        }
    }
    return changed;
}

bool BytecodeOptimizerAccess::propagateCopies()
{
    // Forward "available copies" dataflow: after `mov d, s`, uses of d can read s instead until either is
    // redefined. Meet is intersection; exception handler entry blocks start empty.
    using CopyMap = UncheckedKeyHashMap<int, int, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>>; // d.offset() -> s.offset()
    struct State {
        bool top { true }; // Not yet computed: acts as the universal set for intersection.
        CopyMap copies;
    };
    Vector<State> outStates(m_blocks.size());
    Vector<bool> isHandlerEntry;
    isHandlerEntry.fill(false, m_blocks.size());
    for (auto& handler : m_handlers) {
        unsigned target = nextLiveInsn(m_offsetToIndex[handler.target]);
        if (target < m_insns.size() && m_blockForInsn[target] != UINT_MAX)
            isHandlerEntry[m_blockForInsn[target]] = true;
    }

    auto killRegister = [](CopyMap& copies, VirtualRegister r) {
        if (copies.isEmpty())
            return;
        copies.remove(r.offset());
        Vector<int, 4> toRemove;
        for (auto& entry : copies) {
            if (entry.value == r.offset())
                toRemove.append(entry.key);
        }
        for (int key : toRemove)
            copies.remove(key);
    };

    auto transfer = [&](Block& block, CopyMap& copies, bool apply) -> bool {
        bool changed = false;
        for (unsigned i = block.start; i < block.end; ++i) {
            auto& insn = m_insns[i];
            if (!insn.live)
                continue;
            if (apply && !copies.isEmpty() && allowsOperandSubstitution(insn)) {
                // Substitute explicit read-only operands.
                bool substituted = false;
                Vector<VirtualRegister, 4> candidates;
                if (insn.kind == Insn::SynthMov || insn.kind == Insn::SynthRet)
                    candidates.append(insn.synthSrc);
                else {
                    for (auto r : insn.explicitUses) {
                        // Map through existing substitutions first so we look at the effective register.
                        VirtualRegister effective = r;
                        for (auto& pair : insn.useMap) {
                            if (pair.first == r)
                                effective = pair.second;
                        }
                        if (!candidates.contains(effective))
                            candidates.append(effective);
                    }
                }
                for (auto r : candidates) {
                    if (!r.isLocal() && !r.isArgument())
                        continue;
                    if (!insn.uses.contains(r))
                        continue;
                    // A srcDst operand reads and writes through one operand; leave those registers alone.
                    if (insn.hasSrcDst && insn.defs.contains(r))
                        continue;
                    auto it = copies.find(r.offset());
                    if (it == copies.end())
                        continue;
                    VirtualRegister source { it->value };
                    // Frame-building instructions may write their callee frame before reading their operands, so an
                    // operand must never live inside the region the instruction clobbers.
                    if (insn.clobbers(source))
                        continue;
                    if (insn.kind == Insn::SynthMov || insn.kind == Insn::SynthRet)
                        insn.synthSrc = source;
                    else {
                        // Retarget original operands that currently map to r, and map literal r operands
                        // (unless r itself was already renamed away, in which case literal r means something else).
                        bool literalRenamed = false;
                        for (auto& pair : insn.useMap) {
                            if (pair.second == r)
                                pair.second = source;
                            if (pair.first == r)
                                literalRenamed = true;
                        }
                        if (!literalRenamed && insn.explicitUses.contains(r))
                            insn.useMap.append({ r, source });
                    }
                    substituted = true;
                }
                if (substituted) {
                    computeUseDef(insn);
                    changed = true;
                }
            }
            for (auto r : insn.defs)
                killRegister(copies, r);
            if (insn.kind == Insn::Original && insn.opcode == op_yield && !copies.isEmpty()) {
                // A generator body is re-entered with fresh arguments after each yield: copies of argument
                // registers do not survive it.
                Vector<int, 8> toRemove;
                for (auto& entry : copies) {
                    if (VirtualRegister { entry.key }.isArgument() || VirtualRegister { entry.value }.isArgument())
                        toRemove.append(entry.key);
                }
                for (int key : toRemove)
                    copies.remove(key);
            }
            if (insn.clobberFrom != UINT_MAX && !copies.isEmpty()) {
                Vector<int, 8> toRemove;
                for (auto& entry : copies) {
                    if (insn.clobbers(VirtualRegister { entry.key }) || insn.clobbers(VirtualRegister { entry.value }))
                        toRemove.append(entry.key);
                }
                for (int key : toRemove)
                    copies.remove(key);
            }
            if (insn.isMov() && insn.defs.size() == 1 && insn.uses.size() == 1) {
                VirtualRegister d = insn.defs[0];
                VirtualRegister s = insn.uses[0];
                if (isLocal(d) && d != s && (s.isLocal() || s.isArgument() || s.isConstant()))
                    copies.set(d.offset(), s.offset());
            }
        }
        return changed;
    };

    auto computeIn = [&](unsigned b, CopyMap& result) {
        result.clear();
        auto& block = m_blocks[b];
        if (isHandlerEntry[b] || !b)
            return;
        bool first = true;
        for (unsigned p : block.predecessors) {
            if (!m_blocks[p].reachable || outStates[p].top)
                continue;
            if (first) {
                result = outStates[p].copies;
                first = false;
                continue;
            }
            if (result.isEmpty())
                break;
            Vector<int, 8> toRemove;
            for (auto& entry : result) {
                auto it = outStates[p].copies.find(entry.key);
                if (it == outStates[p].copies.end() || it->value != entry.value)
                    toRemove.append(entry.key);
            }
            for (int key : toRemove)
                result.remove(key);
        }
    };

    // Iterate to a fixpoint in forward order without applying, then apply once with the stable in-states.
    bool changed;
    CopyMap state;
    unsigned iterations = 0;
    do {
        changed = false;
        for (unsigned b = 0; b < m_blocks.size(); ++b) {
            auto& block = m_blocks[b];
            if (!block.reachable)
                continue;
            computeIn(b, state);
            transfer(block, state, false);
            if (outStates[b].top || outStates[b].copies != state) {
                outStates[b].top = false;
                outStates[b].copies = state;
                changed = true;
            }
        }
        if (++iterations > 100) {
            // Give up on pathological CFGs; states are still sound over-approximations only if converged,
            // so bail out of the transformation entirely.
            return false;
        }
    } while (changed);

    bool result = false;
    for (unsigned b = 0; b < m_blocks.size(); ++b) {
        auto& block = m_blocks[b];
        if (!block.reachable)
            continue;
        computeIn(b, state);
        result |= transfer(block, state, true);
    }
    return result;
}

bool BytecodeOptimizerAccess::isStableScopeName(unsigned identifierIndex) const
{
    UniquedStringImpl* name = m_codeBlock->identifier(identifierIndex).impl();
    if (auto* link = m_generator.m_parentDeclaredNames.get())
        return link->isStablyDeclared(name);
    return false;
}

bool BytecodeOptimizerAccess::cacheScopeResolutions()
{
    // resolve_scope of a name declared by an enclosing function/module scope yields the same environment record
    // every time it runs against the same starting scope. Cache such resolutions in fresh registers (placed below
    // all call frames so calls do not clobber them) and turn repeats into movs. Names that would fall through to
    // the global object are left alone: a later global lexical binding can change what they resolve to.
    struct Candidate {
        int scope;
        unsigned identifier;
        unsigned count { 0 };
        VirtualRegister cache;
        bool operator==(const Candidate& other) const { return scope == other.scope && identifier == other.identifier; }
    };
    Vector<Candidate> candidates;
    auto candidateIndex = [&](const Insn& insn) -> std::optional<unsigned> {
        if (!insn.live || insn.kind != Insn::Original || insn.opcode != op_resolve_scope)
            return std::nullopt;
        auto bytecode = insn.instruction->as<OpResolveScope>();
        if (bytecode.m_resolveType != GlobalProperty || !insn.useMap.isEmpty() || !insn.defMap.isEmpty())
            return std::nullopt;
        Candidate key { bytecode.m_scope.offset(), bytecode.m_var, 0, { } };
        for (unsigned i = 0; i < candidates.size(); ++i) {
            if (candidates[i] == key)
                return i;
        }
        if (!isStableScopeName(bytecode.m_var))
            return std::nullopt;
        candidates.append(key);
        return candidates.size() - 1;
    };
    for (auto& insn : m_insns) {
        if (auto index = candidateIndex(insn))
            candidates[*index].count++;
    }
    Vector<unsigned> selected;
    for (unsigned i = 0; i < candidates.size(); ++i) {
        if (candidates[i].count >= 2)
            selected.append(i);
    }
    if (selected.isEmpty())
        return false;
    std::sort(selected.begin(), selected.end(), [&](unsigned a, unsigned b) { return candidates[a].count > candidates[b].count; });
    constexpr unsigned maxCached = 32;
    if (selected.size() > maxCached)
        selected.shrink(maxCached);
    Vector<int> slotOf;
    slotOf.fill(-1, candidates.size());
    for (unsigned i = 0; i < selected.size(); ++i) {
        slotOf[selected[i]] = i;
        candidates[selected[i]].cache = allocateFreshRegister();
        m_scopeCacheContents.add(candidates[selected[i]].cache.offset(), ScopeValue { candidates[selected[i]].scope, candidates[selected[i]].identifier });
    }
    // Keep the frame aligned: temporaries shift by an even amount.
    if (m_numFreshRegisters % stackAlignmentRegisters())
        allocateFreshRegister();
    m_registerShift = m_numFreshRegisters;
    for (auto& block : m_blocks) {
        block.liveIn.resize(m_numLocals);
        block.liveOut.resize(m_numLocals);
    }

    // Forward must-availability of each selected resolution in its cache register.
    using Bits = uint32_t;
    static_assert(maxCached <= 32);
    struct State {
        bool top { true };
        Bits available { 0 };
    };
    Vector<State> outStates(m_blocks.size());
    Vector<bool> isHandlerEntry;
    isHandlerEntry.fill(false, m_blocks.size());
    for (auto& handler : m_handlers) {
        unsigned target = nextLiveInsn(m_offsetToIndex[handler.target]);
        if (target < m_insns.size() && m_blockForInsn[target] != UINT_MAX)
            isHandlerEntry[m_blockForInsn[target]] = true;
    }
    auto killScope = [&](Bits& available, VirtualRegister r) {
        if (!available)
            return;
        for (unsigned i = 0; i < selected.size(); ++i) {
            if (candidates[selected[i]].scope == r.offset())
                available &= ~(1u << i);
        }
    };
    auto transfer = [&](Block& block, Bits& available, bool apply) -> bool {
        bool changed = false;
        for (unsigned i = block.start; i < block.end; ++i) {
            auto& insn = m_insns[i];
            if (!insn.live)
                continue;
            std::optional<unsigned> slot;
            if (auto index = candidateIndex(insn); index && slotOf[*index] >= 0)
                slot = slotOf[*index];
            if (slot && (available & (1u << *slot))) {
                if (apply) {
                    VirtualRegister dst = insn.defs[0];
                    insn.kind = Insn::SynthMov;
                    insn.synthDst = dst;
                    insn.synthSrc = candidates[selected[*slot]].cache;
                    insn.targets.shrink(0);
                    computeUseDef(insn);
                    changed = true;
                }
                continue;
            }
            for (auto r : insn.defs)
                killScope(available, r);
            if (insn.clobberFrom != UINT_MAX && available) {
                for (unsigned j = 0; j < selected.size(); ++j) {
                    if (insn.clobbers(VirtualRegister { candidates[selected[j]].scope }))
                        available &= ~(1u << j);
                }
            }
            // Do not keep caches alive across a yield/await: generatorification would spill and refill them at every
            // suspension point. Re-resolving after the resume is cheaper.
            if (insn.kind == Insn::Original && insn.opcode == op_yield)
                available = 0;
            if (slot) {
                if (apply && !insn.copyTo.isValid()) {
                    insn.copyTo = candidates[selected[*slot]].cache;
                    computeUseDef(insn);
                    changed = true;
                }
                available |= 1u << *slot;
            }
        }
        return changed;
    };
    auto computeIn = [&](unsigned b) -> Bits {
        if (isHandlerEntry[b] || !b)
            return 0;
        Bits result = ~0u;
        bool any = false;
        for (unsigned p : m_blocks[b].predecessors) {
            if (!m_blocks[p].reachable || outStates[p].top)
                continue;
            result &= outStates[p].available;
            any = true;
        }
        return any ? result : 0;
    };
    bool changed;
    unsigned iterations = 0;
    do {
        changed = false;
        for (unsigned b = 0; b < m_blocks.size(); ++b) {
            auto& block = m_blocks[b];
            if (!block.reachable)
                continue;
            Bits state = computeIn(b);
            transfer(block, state, false);
            if (outStates[b].top || outStates[b].available != state) {
                outStates[b].top = false;
                outStates[b].available = state;
                changed = true;
            }
        }
        if (++iterations > 100)
            return false;
    } while (changed);
    bool result = false;
    for (unsigned b = 0; b < m_blocks.size(); ++b) {
        auto& block = m_blocks[b];
        if (!block.reachable)
            continue;
        Bits state = computeIn(b);
        result |= transfer(block, state, true);
    }
    return result;
}

bool BytecodeOptimizerAccess::eliminateRedundantTDZChecks()
{
    // A binding leaves its temporal dead zone exactly once and never re-enters it. So a check_tdz on a value loaded
    // from a binding is redundant if every path to it already checked, or stored to, that same binding.
    //
    // Bindings are named by how they were reached, not by the register holding the scope: resolve_scope is a pure
    // function of (scope, name), so "get_from_scope(resolve_scope(base, X), Y)" denotes the same binding every time it
    // is evaluated while |base| holds the same value. Keys are (base register, resolved name X or none, name Y) and
    // are dropped when the base register is redefined. Forward "must" dataflow, meet is intersection.
    struct Key {
        int base { 0 };
        unsigned via { UINT_MAX }; // identifier index given to resolve_scope, or UINT_MAX if the scope is |base| itself
        unsigned name { UINT_MAX };
        bool operator==(const Key&) const = default;
        uint64_t packed() const { return (static_cast<uint64_t>(static_cast<uint16_t>(base)) << 48) ^ (static_cast<uint64_t>(via) << 24) ^ name; }
    };
    using KeySet = UncheckedKeyHashMap<uint64_t, Key, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    struct State {
        bool top { true };
        KeySet checked;
    };
    Vector<State> outStates(m_blocks.size());
    Vector<bool> isHandlerEntry;
    isHandlerEntry.fill(false, m_blocks.size());
    for (auto& handler : m_handlers) {
        unsigned target = nextLiveInsn(m_offsetToIndex[handler.target]);
        if (target < m_insns.size() && m_blockForInsn[target] != UINT_MAX)
            isHandlerEntry[m_blockForInsn[target]] = true;
    }

    auto mappedUse = [](const Insn& insn, VirtualRegister original) {
        for (auto& pair : insn.useMap) {
            if (pair.first == original)
                return pair.second;
        }
        return original;
    };

    auto transfer = [&](Block& block, KeySet& checked, bool apply) -> bool {
        bool changed = false;
        // Block-local knowledge about registers: which scope value / which binding's value they hold.
        UncheckedKeyHashMap<int, ScopeValue, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> regScope;
        UncheckedKeyHashMap<int, Key, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> regBinding;
        // Registers defined in this block by something that cannot produce the empty value.
        UncheckedKeyHashSet<int, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> neverEmpty;

        auto scopeValueOf = [&](VirtualRegister scope) -> ScopeValue {
            auto it = regScope.find(scope.offset());
            if (it != regScope.end())
                return it->value;
            // A scope-cache register only ever holds the resolution it was allocated for.
            auto cached = m_scopeCacheContents.find(scope.offset());
            if (cached != m_scopeCacheContents.end())
                return cached->value;
            return { scope.offset(), UINT_MAX };
        };
        auto killRegister = [&](VirtualRegister r) {
            regScope.remove(r.offset());
            regBinding.remove(r.offset());
            bool anyBase = false;
            for (auto& entry : regScope)
                anyBase |= entry.value.base == r.offset();
            for (auto& entry : regBinding)
                anyBase |= entry.value.base == r.offset();
            for (auto& entry : checked)
                anyBase |= entry.value.base == r.offset();
            if (!anyBase)
                return;
            regScope.removeIf([&](auto& entry) { return entry.value.base == r.offset(); });
            regBinding.removeIf([&](auto& entry) { return entry.value.base == r.offset(); });
            checked.removeIf([&](auto& entry) { return entry.value.base == r.offset(); });
        };

        for (unsigned i = block.start; i < block.end; ++i) {
            auto& insn = m_insns[i];
            if (!insn.live)
                continue;
            OpcodeID opcode = insn.effectiveOpcode();
            if (opcode == op_check_tdz && insn.kind == Insn::Original && insn.uses.size() == 1) {
                auto it = regBinding.find(insn.uses[0].offset());
                if (it != regBinding.end()) {
                    if (checked.contains(it->value.packed())) {
                        if (apply) {
                            insn.live = false;
                            changed = true;
                        }
                    } else
                        checked.add(it->value.packed(), it->value);
                }
                continue;
            }

            // Compute what this instruction establishes before killing its defs (sources may equal the dst).
            std::optional<std::pair<int, ScopeValue>> newScope;
            std::optional<std::pair<int, Key>> newBinding;
            std::optional<Key> stored;
            auto knownScope = [&](VirtualRegister r) -> std::optional<ScopeValue> {
                if (auto it = regScope.find(r.offset()); it != regScope.end())
                    return it->value;
                if (auto it = m_scopeCacheContents.find(r.offset()); it != m_scopeCacheContents.end())
                    return it->value;
                return std::nullopt;
            };
            if (insn.kind == Insn::SynthMov) {
                if (auto scope = knownScope(insn.synthSrc))
                    newScope = { { insn.synthDst.offset(), *scope } };
                if (auto it = regBinding.find(insn.synthSrc.offset()); it != regBinding.end())
                    newBinding = { { insn.synthDst.offset(), it->value } };
            } else if (insn.kind == Insn::Original) {
                switch (opcode) {
                case op_mov:
                    if (insn.uses.size() == 1 && insn.defs.size() == 1) {
                        if (auto scope = knownScope(insn.uses[0]))
                            newScope = { { insn.defs[0].offset(), *scope } };
                        if (auto it = regBinding.find(insn.uses[0].offset()); it != regBinding.end())
                            newBinding = { { insn.defs[0].offset(), it->value } };
                    }
                    break;
                case op_resolve_scope: {
                    auto bytecode = insn.instruction->as<OpResolveScope>();
                    ScopeValue base = scopeValueOf(mappedUse(insn, bytecode.m_scope));
                    if (base.via == UINT_MAX && insn.defs.size() == 1)
                        newScope = { { insn.defs[0].offset(), ScopeValue { base.base, bytecode.m_var } } };
                    break;
                }
                case op_get_from_scope: {
                    auto bytecode = insn.instruction->as<OpGetFromScope>();
                    ScopeValue scope = scopeValueOf(mappedUse(insn, bytecode.m_scope));
                    if (insn.defs.size() == 1)
                        newBinding = { { insn.defs[0].offset(), Key { scope.base, scope.via, bytecode.m_var } } };
                    break;
                }
                case op_put_to_scope: {
                    // Storing the empty value (how derived constructors park |this|) puts a binding *into* its TDZ, so
                    // only count stores of values known not to be empty.
                    auto bytecode = insn.instruction->as<OpPutToScope>();
                    VirtualRegister value = mappedUse(insn, bytecode.m_value);
                    bool valueNeverEmpty = value.isConstant() ? !!constantValue(value) || (m_codeBlock->constantSourceCodeRepresentation(value) == SourceCodeRepresentation::LinkTimeConstant) : neverEmpty.contains(value.offset());
                    if (bytecode.m_var != UINT_MAX && valueNeverEmpty) {
                        ScopeValue scope = scopeValueOf(mappedUse(insn, bytecode.m_scope));
                        stored = Key { scope.base, scope.via, bytecode.m_var };
                    }
                    break;
                }
                default:
                    break;
                }
            }

            bool producesNonEmpty = false;
            switch (opcode) {
            case op_mov:
                if (insn.uses.size() == 1) {
                    VirtualRegister src = insn.uses[0];
                    producesNonEmpty = src.isConstant() ? !!constantValue(src) : neverEmpty.contains(src.offset());
                }
                break;
            case op_call:
            case op_construct:
            case op_call_varargs:
            case op_construct_varargs:
            case op_get_by_id:
            case op_get_by_val:
            case op_get_by_id_direct:
            case op_get_length:
            case op_new_object:
            case op_new_array:
            case op_new_array_buffer:
            case op_new_array_with_size:
            case op_new_func:
            case op_new_func_exp:
            case op_new_async_func:
            case op_new_async_func_exp:
            case op_new_generator_func:
            case op_new_generator_func_exp:
            case op_new_async_generator_func:
            case op_new_async_generator_func_exp:
            case op_new_reg_exp:
            case op_to_string:
            case op_strcat:
            case op_typeof:
            case op_create_this:
            case op_resolve_scope:
                producesNonEmpty = insn.kind == Insn::Original;
                break;
            default:
                break;
            }
            for (auto r : insn.defs) {
                killRegister(r);
                neverEmpty.remove(r.offset());
            }
            if (producesNonEmpty && insn.defs.size() == 1)
                neverEmpty.add(insn.defs[0].offset());
            if (insn.clobberFrom != UINT_MAX) {
                neverEmpty.removeIf([&](int r) { return insn.clobbers(VirtualRegister { r }); });
                Vector<int, 4> clobbered;
                for (auto& entry : regScope) {
                    if (insn.clobbers(VirtualRegister { entry.key }))
                        clobbered.append(entry.key);
                }
                for (auto& entry : regBinding) {
                    if (insn.clobbers(VirtualRegister { entry.key }))
                        clobbered.append(entry.key);
                }
                for (auto& entry : checked) {
                    if (insn.clobbers(VirtualRegister { entry.value.base }))
                        clobbered.append(entry.value.base);
                }
                for (int r : clobbered)
                    killRegister(VirtualRegister { r });
            }
            if (newScope) {
                regScope.set(newScope->first, newScope->second);
                if (insn.copyTo.isValid())
                    regScope.set(insn.copyTo.offset(), newScope->second);
            }
            if (newBinding) {
                regBinding.set(newBinding->first, newBinding->second);
                if (insn.copyTo.isValid())
                    regBinding.set(insn.copyTo.offset(), newBinding->second);
            }
            if (stored)
                checked.add(stored->packed(), *stored); // A store that completes leaves the binding initialized.
        }
        return changed;
    };

    auto computeIn = [&](unsigned b, KeySet& result) {
        result.clear();
        auto& block = m_blocks[b];
        if (isHandlerEntry[b] || !b)
            return;
        bool first = true;
        for (unsigned p : block.predecessors) {
            if (!m_blocks[p].reachable || outStates[p].top)
                continue;
            if (first) {
                result = outStates[p].checked;
                first = false;
                continue;
            }
            if (result.isEmpty())
                break;
            result.removeIf([&](auto& entry) { return !outStates[p].checked.contains(entry.key); });
        }
    };

    bool changed;
    KeySet state;
    unsigned iterations = 0;
    do {
        changed = false;
        for (unsigned b = 0; b < m_blocks.size(); ++b) {
            auto& block = m_blocks[b];
            if (!block.reachable)
                continue;
            computeIn(b, state);
            transfer(block, state, false);
            bool differs = outStates[b].top || outStates[b].checked.size() != state.size();
            if (!differs) {
                for (auto& entry : state) {
                    if (!outStates[b].checked.contains(entry.key)) {
                        differs = true;
                        break;
                    }
                }
            }
            if (differs) {
                outStates[b].top = false;
                outStates[b].checked = state;
                changed = true;
            }
        }
        if (++iterations > 100)
            return false;
    } while (changed);

    bool result = false;
    for (unsigned b = 0; b < m_blocks.size(); ++b) {
        auto& block = m_blocks[b];
        if (!block.reachable)
            continue;
        computeIn(b, state);
        result |= transfer(block, state, true);
    }
    return result;
}

bool BytecodeOptimizerAccess::coalesceDestinations()
{
    // Within a block: `I: op ..., t <- ...` ... `J: mov d, t` where t dies at J, nothing between I and J touches d
    // or reads t  =>  make I write d directly and drop J.
    bool changed = false;
    Vector<FastBitVector> liveAfter; // live-out per instruction of the current block
    for (auto& block : m_blocks) {
        if (!block.reachable)
            continue;
        unsigned length = block.end - block.start;
        liveAfter.resize(length);
        FastBitVector live = block.liveOut;
        for (unsigned i = block.end; i-- > block.start;) {
            auto& insn = m_insns[i];
            liveAfter[i - block.start] = live;
            if (!insn.live)
                continue;
            stepLiveness(live, insn);
        }

        unsigned barrier = block.start; // Candidate ranges must not overlap ones already rewritten this round.
        for (unsigned j = block.start; j < block.end; ++j) {
            auto& mov = m_insns[j];
            if (!mov.live || !mov.isMov())
                continue;
            if (mov.uses.size() != 1 || mov.defs.size() != 1)
                continue;
            VirtualRegister t = mov.uses[0];
            VirtualRegister d = mov.defs[0];
            if (!isLocal(t) || !isLocal(d) || t == d)
                continue;
            if (liveAfter[j - block.start][t.toLocal()])
                continue;
            // Scan backwards for the defining instruction.
            unsigned defIndex = UINT_MAX;
            bool ok = true;
            for (unsigned k = j; k-- > std::max(barrier, block.start);) {
                auto& insn = m_insns[k];
                if (!insn.live)
                    continue;
                bool defsT = insn.defs.contains(t);
                bool usesT = insn.uses.contains(t);
                if (defsT) {
                    // The defining instruction may read d (reads happen before the write) but must not write it,
                    // and must write t through its dst operand so that we can rename it.
                    // (The defining instruction's own frame clobber happens before it writes its dst, so d may sit in
                    // that region: calls routinely return into what was their argument area.)
                    if (insn.defs.contains(d) || !allowsOperandSubstitution(insn) || insn.defs.size() != 1 || insn.hasSrcDst)
                        ok = false;
                    defIndex = k;
                    break;
                }
                if (usesT || insn.clobbers(t) || insn.defs.contains(d) || insn.uses.contains(d) || insn.clobbers(d)) {
                    ok = false;
                    break;
                }
                // d must not be observable by an exception handler while it would hold the new value early.
                if (liveAfter[k - block.start][d.toLocal()]) {
                    ok = false;
                    break;
                }
            }
            if (!ok || defIndex == UINT_MAX)
                continue;
            auto& def = m_insns[defIndex];
            if (liveAfter[defIndex - block.start][d.toLocal()])
                continue;
            if (def.kind == Insn::SynthMov)
                def.synthDst = d;
            else {
                bool found = false;
                for (auto& pair : def.defMap) {
                    if (pair.second == t) {
                        pair.second = d;
                        found = true;
                    }
                }
                if (!found) {
                    if (!def.explicitDefs.contains(t))
                        continue;
                    def.defMap.append({ t, d });
                }
            }
            computeUseDef(def);
            mov.live = false;
            changed = true;
            barrier = j + 1;
        }
    }
    return changed;
}

struct BytecodeOptimizerAccess::Mapper {
    BytecodeOptimizerAccess& optimizer;
    Insn* insn { nullptr };
    unsigned targetCursor { 0 };
    bool finalPass { false };

    VirtualRegister logical(BytecodeOperandName name, VirtualRegister r)
    {
        switch (name) {
        case BytecodeOperandName::firstFree:
        case BytecodeOperandName::srcDst:
            return r;
        case BytecodeOperandName::dst:
            for (auto& pair : insn->defMap) {
                if (pair.first == r)
                    return pair.second;
            }
            return r;
        default:
            for (auto& pair : insn->useMap) {
                if (pair.first == r)
                    return pair.second;
            }
            return r;
        }
    }

    VirtualRegister operator()(BytecodeOperandName name, VirtualRegister r)
    {
        return optimizer.physicalRegister(logical(name, r));
    }

    BoundLabel operator()(BytecodeOperandName, BoundLabel)
    {
        RELEASE_ASSERT(targetCursor < insn->targets.size());
        unsigned target = insn->targets[targetCursor++];
        RELEASE_ASSERT(target != noTarget);
        target = optimizer.nextLiveInsn(target);
        auto& targetInsn = optimizer.m_insns[target];
        int from = static_cast<int>(insn->newOffset);
        int to = static_cast<int>(targetInsn.newOffset == UINT_MAX ? targetInsn.oldOffset : targetInsn.newOffset);
        int delta = to - from;
        if (finalPass)
            RELEASE_ASSERT(delta);
        else if (!delta)
            delta = 1; // Placeholder; never zero, which would mean "out of line".
        return BoundLabel(delta);
    }

    unsigned operator()(BytecodeOperandName name, unsigned value)
    {
        if (isValueProfileOperand(name))
            return optimizer.m_generator.nextValueProfileIndex();
        // Frame offsets counted in registers from the callee frame (call argv, iterator stackOffset) move with
        // the temporaries when fresh registers are inserted below them.
        if (name == BytecodeOperandName::argv || name == BytecodeOperandName::stackOffset)
            return value + optimizer.m_registerShift;
        return value;
    }

    template<typename T>
    T operator()(BytecodeOperandName, T value) { return value; }
};

void BytecodeOptimizerAccess::emit()
{
    // Relaxation: instruction sizes depend on jump distances which depend on sizes. Sizes only grow across
    // iterations (minimumSize ratchets), so this terminates.
    JSInstructionStreamWriter newWriter;
    bool finalPass = false;
    for (unsigned iteration = 0; ; ++iteration) {
        RELEASE_ASSERT(iteration < 64);
        JSInstructionStreamWriter writer;
        m_codeBlock->metadata().restartForReemit();
        bool offsetsChanged = false;
        m_generator.withWriter(writer, [&] {
            for (unsigned i = 0; i < m_insns.size(); ++i) {
                auto& insn = m_insns[i];
                if (!insn.live)
                    continue;
                unsigned offset = m_generator.m_writer.position();
                if (insn.newOffset != offset)
                    offsetsChanged = true;
                insn.newOffset = offset;
                Mapper mapper { *this, &insn, 0, finalPass };
                switch (insn.kind) {
                case Insn::Original:
                    reemitInstruction(insn.instruction, &m_generator, insn.minimumSize, mapper);
                    break;
                case Insn::SynthMov:
                    OpMov::emit(&m_generator, physicalRegister(insn.synthDst), physicalRegister(insn.synthSrc));
                    break;
                case Insn::SynthRet:
                    OpRet::emit(&m_generator, physicalRegister(insn.synthSrc));
                    break;
                case Insn::SynthJmp: {
                    BoundLabel label = mapper(BytecodeOperandName::targetLabel, BoundLabel());
                    switch (insn.minimumSize) {
                    case OpcodeSize::Narrow:
                        OpJmp::emitWithSmallestSizeRequirement<OpcodeSize::Narrow>(&m_generator, label);
                        break;
                    case OpcodeSize::Wide16:
                        OpJmp::emitWithSmallestSizeRequirement<OpcodeSize::Wide16>(&m_generator, label);
                        break;
                    case OpcodeSize::Wide32:
                        OpJmp::emitWithSmallestSizeRequirement<OpcodeSize::Wide32>(&m_generator, label);
                        break;
                    }
                    break;
                }
                }
                auto emitted = m_generator.m_writer.ref(offset);
                OpcodeSize size = emitted->isWide32() ? OpcodeSize::Wide32 : emitted->isWide16() ? OpcodeSize::Wide16 : OpcodeSize::Narrow;
                if (static_cast<unsigned>(size) > static_cast<unsigned>(insn.minimumSize))
                    insn.minimumSize = size;
                if (insn.copyTo.isValid()) {
                    // Cache the result in its fresh register. The source is whatever the instruction's dst became.
                    VirtualRegister dst;
                    if (insn.kind == Insn::SynthMov)
                        dst = insn.synthDst;
                    else {
                        RELEASE_ASSERT(insn.explicitDefs.size() == 1);
                        dst = mapper.logical(BytecodeOperandName::dst, insn.explicitDefs[0]);
                    }
                    OpMov::emit(&m_generator, physicalRegister(insn.copyTo), physicalRegister(dst));
                }
            }
        });
        if (finalPass) {
            RELEASE_ASSERT(!offsetsChanged);
            newWriter.swap(writer);
            break;
        }
        if (!offsetsChanged)
            finalPass = true; // One more pass with exact deltas (and assertions) now that the layout is stable.
    }
    unsigned newSize = newWriter.size();

    auto newOffsetForOld = [&](unsigned oldOffset) -> unsigned {
        RELEASE_ASSERT(oldOffset < m_offsetToIndex.size());
        unsigned index = m_offsetToIndex[oldOffset];
        RELEASE_ASSERT(index != UINT_MAX);
        index = nextLiveInsn(index);
        if (index >= m_insns.size())
            return newSize;
        return m_insns[index].newOffset;
    };

    // Exception handlers.
    {
        Vector<UnlinkedHandlerInfo> handlers;
        for (auto handler : m_codeBlock->m_exceptionHandlers) {
            unsigned targetIndex = m_offsetToIndex[handler.target];
            RELEASE_ASSERT(targetIndex < m_insns.size() && m_insns[targetIndex].opcode == op_catch);
            if (!m_insns[targetIndex].live)
                continue; // Nothing reachable is covered by this handler anymore.
            unsigned start = newOffsetForOld(handler.start);
            unsigned end = newOffsetForOld(handler.end);
            if (start >= end)
                continue;
            handler.start = start;
            handler.end = end;
            handler.target = m_insns[targetIndex].newOffset;
            handlers.append(handler);
        }
        m_codeBlock->m_exceptionHandlers = WTF::move(handlers);
    }

    // Switch jump tables.
    for (auto& insn : m_insns) {
        if (!insn.live || insn.kind != Insn::Original)
            continue;
        auto rewrite = [&](int32_t& slot, unsigned targetIndex) {
            if (targetIndex == noTarget) {
                RELEASE_ASSERT(!slot);
                return;
            }
            targetIndex = nextLiveInsn(targetIndex);
            slot = static_cast<int32_t>(m_insns[targetIndex].newOffset) - static_cast<int32_t>(insn.newOffset);
            RELEASE_ASSERT(slot);
        };
        switch (insn.opcode) {
        case op_switch_imm:
        case op_switch_char: {
            unsigned tableIndex = insn.opcode == op_switch_imm ? insn.instruction->as<OpSwitchImm>().m_tableIndex : insn.instruction->as<OpSwitchChar>().m_tableIndex;
            auto& table = m_codeBlock->unlinkedSwitchJumpTable(tableIndex);
            unsigned cursor = 0;
            if (table.isList()) {
                for (unsigned i = 0; i < table.m_branchOffsets.size(); i += 2)
                    rewrite(table.m_branchOffsets[i + 1], insn.targets[cursor++]);
            } else {
                for (unsigned i = table.m_branchOffsets.size(); i--;)
                    rewrite(table.m_branchOffsets[i], insn.targets[cursor++]);
            }
            rewrite(table.m_defaultOffset, insn.targets[cursor++]);
            RELEASE_ASSERT(cursor == insn.targets.size());
            break;
        }
        case op_switch_string: {
            auto& table = m_codeBlock->unlinkedStringSwitchJumpTable(insn.instruction->as<OpSwitchString>().m_tableIndex);
            unsigned cursor = 0;
            for (auto& entry : table.m_offsetTable)
                rewrite(entry.value.m_branchOffset, insn.targets[cursor++]);
            rewrite(table.m_defaultOffset, insn.targets[cursor++]);
            RELEASE_ASSERT(cursor == insn.targets.size());
            break;
        }
        default:
            break;
        }
    }

    // Expression info: every surviving instruction keeps the entry that applied to it in the original stream.
    m_codeBlock->m_expressionInfoEncoder.rebuild([&](unsigned oldInstPC) -> unsigned {
        if (oldInstPC >= m_offsetToIndex.size())
            return newSize;
        // Expression info is recorded at instruction starts; be tolerant of anything else.
        unsigned probe = oldInstPC;
        while (probe < m_offsetToIndex.size() && m_offsetToIndex[probe] == UINT_MAX)
            ++probe;
        if (probe >= m_offsetToIndex.size())
            return newSize;
        return newOffsetForOld(probe);
    });

    // All jumps were sized to fit; nothing is out of line anymore.
    m_codeBlock->replaceOutOfLineJumpTargets();

    m_writer.swap(newWriter);
    m_generator.m_lastOpcodeID = JSGeneratorTraits::opcodeForDisablingOptimizations;
    m_generator.m_lastInstruction = m_writer.ref();

    if (m_registerShift) {
        // Fresh registers live right after the original vars (op_enter initializes them; they sit below every call
        // frame), and all temporaries moved up by the same (even) amount.
        m_codeBlock->setNumVars(m_originalNumVars + m_registerShift);
        m_codeBlock->setNumCalleeLocals(m_originalNumLocals + m_registerShift);
        for (unsigned i = 0; i < m_registerShift; ++i)
            m_generator.newRegister();
        RELEASE_ASSERT(m_codeBlock->numCalleeLocals() == m_originalNumLocals + m_registerShift);
    }
}

void BytecodeOptimizerAccess::run()
{
    m_handlers = m_codeBlock->m_exceptionHandlers;
    decode();
    computeHandlerMap();
    if (verbose || Options::dumpBytecodeOptimizer())
        dumpIR("after decode");

    unsigned originalCount = m_insns.size();
    if (Options::useBytecodeOptimizerScopeCache()) {
        buildBlocks();
        if (removeUnreachable())
            buildBlocks();
        if (cacheScopeResolutions()) {
            buildBlocks();
            computeLiveness();
            eliminateDeadStores();
        }
    }
    for (unsigned round = 0; round < 6; ++round) {
        bool changed = false;
        buildBlocks();
        changed |= removeUnreachable();
        if (changed)
            buildBlocks();
        if (simplifyJumps()) {
            changed = true;
            buildBlocks();
            if (removeUnreachable())
                buildBlocks();
        }
        computeLiveness();
        changed |= eliminateDeadStores();
        if (Options::useBytecodeOptimizerTDZ() && eliminateRedundantTDZChecks()) {
            changed = true;
            buildBlocks();
            computeLiveness();
            eliminateDeadStores();
        }
        if (Options::useBytecodeOptimizerCopyPropagation()) {
            // Coalesce `t <- op; mov d, t` into `d <- op` before forward propagation gets a chance to extend t's
            // live range past the mov.
            computeLiveness();
            if (coalesceDestinations()) {
                changed = true;
                computeLiveness();
                eliminateDeadStores();
            }
            if (propagateCopies()) {
                changed = true;
                computeLiveness();
                eliminateDeadStores();
            }
        }
        if (!changed)
            break;
    }
    // Blocks/liveness may be stale after the last DCE, but emission only needs per-instruction liveness flags
    // and reachability of handler targets, which DCE does not change.
    buildBlocks();

    if (verbose || Options::dumpBytecodeOptimizer())
        dumpIR("before emit");

    unsigned liveCount = 0;
    for (auto& insn : m_insns)
        liveCount += insn.live;
    unsigned oldSize = m_writer.size();
    emit();
    if (Options::reportBytecodeOptimizer()) {
        dataLogLn("BytecodeOptimizer: instructions ", originalCount, " -> ", liveCount, ", bytes ", oldSize, " -> ", m_writer.size());
        static std::array<std::atomic<unsigned>, numOpcodeIDs> s_before;
        static std::array<std::atomic<unsigned>, numOpcodeIDs> s_after;
        static std::once_flag once;
        std::call_once(once, [] {
            std::atexit([] {
                dataLogLn("BytecodeOptimizer opcode histogram (before after delta):");
                Vector<std::pair<int, unsigned>> rows;
                for (unsigned i = 0; i < numOpcodeIDs; ++i) {
                    if (s_before[i] || s_after[i])
                        rows.append({ static_cast<int>(s_after[i]) - static_cast<int>(s_before[i]), i });
                }
                std::sort(rows.begin(), rows.end());
                for (auto& row : rows)
                    dataLogLn("  ", opcodeNames[row.second], " ", s_before[row.second].load(), " ", s_after[row.second].load(), " ", row.first);
            });
        });
        static std::array<std::atomic<unsigned>, 8> s_movKinds;
        static std::once_flag once2;
        std::call_once(once2, [] {
            std::atexit([] {
                dataLogLn("BytecodeOptimizer surviving mov kinds: undefinedToCallSlot ", s_movKinds[0].load(), " constToCallSlot ", s_movKinds[1].load(), " localToCallSlot ", s_movKinds[2].load(), " argToCallSlot ", s_movKinds[3].load(), " constToLocal ", s_movKinds[4].load(), " argOrThisToLocal ", s_movKinds[5].load(), " localToLocal ", s_movKinds[6].load(), " other ", s_movKinds[7].load());
            });
        });
        // A mov's dst is a "call slot" if some later instruction in the block has it as an implicit (frame) use.
        for (unsigned i = 0; i < m_insns.size(); ++i) {
            auto& insn = m_insns[i];
            OpcodeID opcode = insn.effectiveOpcode();
            s_before[opcode]++;
            if (insn.live)
                s_after[opcode]++;
            if (!insn.live || opcode != op_mov || insn.uses.size() != 1 || insn.defs.size() != 1)
                continue;
            VirtualRegister src = insn.uses[0];
            VirtualRegister dst = insn.defs[0];
            bool toCallSlot = false;
            for (unsigned j = i + 1; j < m_insns.size() && j < i + 40; ++j) {
                if (m_insns[j].live && m_insns[j].implicitUses.contains(dst)) {
                    toCallSlot = true;
                    break;
                }
                if (m_insns[j].live && (m_insns[j].defs.contains(dst) || m_insns[j].isBranchOrSwitch()))
                    break;
            }
            unsigned kind;
            if (toCallSlot)
                kind = src.isConstant() ? (m_codeBlock->getConstant(src).isUndefined() ? 0 : 1) : src.isLocal() ? 2 : 3;
            else if (!dst.isLocal())
                kind = 7;
            else
                kind = src.isConstant() ? 4 : src.isLocal() ? 6 : 5;
            s_movKinds[kind]++;
        }
    }
}

void BytecodeOptimizerAccess::runIfAppropriate(BytecodeGenerator& generator)
{
    UnlinkedCodeBlockGenerator* codeBlock = generator.m_codeBlock.get();
    if (codeBlock->wasCompiledWithDebuggingOpcodes())
        return;
    if (!codeBlock->m_typeProfilerInfoMap.isEmpty() || !codeBlock->m_opProfileControlFlowBytecodeOffsets.isEmpty())
        return;
    if (!generator.m_writer.size() || generator.m_expressionTooDeep)
        return;
    // Generated monsters (deeply nested patterns etc.) are not worth quadratic-ish dataflow; leave them alone.
    if (codeBlock->numCalleeLocals() > 4096 || generator.m_writer.size() > 16 * 1024 * 1024)
        return;
    static std::atomic<unsigned> s_count;
    unsigned count = s_count++;
    if (count < Options::bytecodeOptimizerSkipFirst() || count - Options::bytecodeOptimizerSkipFirst() >= Options::bytecodeOptimizerMaxCount())
        return;
    if (Options::reportBytecodeOptimizer())
        dataLogLn("BytecodeOptimizer: #", count, " ", codeBlock->m_codeBlock->classInfo()->className, " line ", generator.m_scopeNode->firstLine());
    BytecodeOptimizerAccess optimizer(generator, codeBlock, generator.m_writer);
    optimizer.run();
}

void BytecodeOptimizer::run(BytecodeGenerator& generator)
{
    BytecodeOptimizerAccess::runIfAppropriate(generator);
}

} // namespace JSC
