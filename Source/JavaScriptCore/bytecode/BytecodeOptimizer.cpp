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
#include <wtf/HashCountedSet.h>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

namespace {

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
    // Operands known to hold the value of a constant register (from copy propagation). Analysis-only: constants are
    // never substituted into operands other than a mov/ret source, because LLInt reads some operands as raw frame
    // slots (op_jeq_ptr/op_jneq_ptr's value, scope and iterator operands, ...).
    Vector<std::pair<VirtualRegister, VirtualRegister>, 1> knownConstants;
    // Statically resolved scope accesses: resolve_scope gets resolveType = firstStaticClosureVarResolveType + hops
    // beyond the function's own scope; get_from_scope gets ResolvedClosureVar with a known slot.
    std::optional<unsigned> staticOuterHops;
    std::optional<unsigned> staticScopeOffset;
    bool staticScopeOffsetIsLazyFunctionSlot { false };

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
    case op_new_reg_exp_shared:
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
    case op_resolve_scope: {
        // A Dynamic resolution can walk into a `with` object, whose lookup (Proxy has trap) is observable.
        ResolveType type = insn.instruction->as<OpResolveScope>().m_resolveType;
        return insn.staticOuterHops || type == GlobalProperty || type == GlobalPropertyWithVarInjectionChecks || type == ModuleVar;
    }
    case op_get_from_scope:
        return insn.staticScopeOffset || insn.instruction->as<OpGetFromScope>().m_getPutInfo.resolveType() == ResolvedClosureVar || insn.instruction->as<OpGetFromScope>().m_getPutInfo.resolveType() == ResolvedLazyClosureVar;
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
    // Reads and writes its iterator operand, and names the registers that op_iterator_open / op_iterator_next keep their state in.
    case op_iterator_close_check:
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
    // Turn |insn| into a synthesized mov/jmp/ret (jmp keeps insn.targets[0]); all per-operand rewrites are dropped.
    void replaceWith(Insn&, Insn::Kind, VirtualRegister dst = { }, VirtualRegister src = { });
    void computeImplicitUses(Insn&);
    void computeUseDef(Insn&);
    void applySubstitutions(Insn&);
    void buildBlocks();
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

    Vector<bool> handlerEntryBlocks() const;
    // Forward "must" dataflow over reachable blocks. In-state of a block = meet of the out-states of the
    // predecessors solved so far (unsolved ones act as top); the entry block and exception handler entries start
    // from State(). Iterates transfer(block, state, /* apply */ false) to a fixpoint, then calls
    // transfer(block, inState, /* apply */ true) once per block and returns whether any of those reported a change.
    // Gives up (returns false, nothing applied) if the fixpoint does not converge quickly.
    template<typename State, typename Meet, typename Transfer>
    bool forwardMustAnalysis(const Meet&, const Transfer&);

    struct Mapper;
    friend struct Mapper;

    BytecodeGenerator& m_generator;
    UnlinkedCodeBlockGenerator* m_codeBlock;
    JSInstructionStreamWriter& m_writer;
    Vector<UnlinkedHandlerInfo> m_handlers; // Copy in original offsets, used for coverage queries throughout.
    Vector<Insn> m_insns;
    Vector<unsigned> m_handlerForInsn; // innermost handler covering each instruction (by original offset), or UINT_MAX
    Vector<unsigned> m_enclosingHandler; // per handler: the next handler in the list whose range contains its start, or UINT_MAX
    Vector<unsigned> m_offsetToIndex;
    Vector<Block> m_blocks;
    Vector<unsigned> m_blockForInsn;
    unsigned m_numLocals; // in analysis numbering: original locals, then fresh registers
    unsigned m_originalNumLocals;
    unsigned m_originalNumVars;
    unsigned m_numFreshRegisters { 0 }; // allocated by the optimizer; physically placed right after the original vars

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
        unsigned via; // identifier index given to resolve_scope, or environmentVia(...) for a statically located record, or UINT_MAX for |base| itself
    };
    static unsigned environmentVia(unsigned localScopeDepth, unsigned hops) { return 0x80000000u | (localScopeDepth << 16) | hops; }
    UncheckedKeyHashMap<int, ScopeValue, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> m_scopeCacheContents; // cache register -> what it holds
    bool cacheScopeResolutions();
    bool resolveScopesStatically();
    unsigned m_staticResolves { 0 };
    unsigned m_staticGets { 0 };
    bool m_branchesHaveKnownConstants { false };
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

void BytecodeOptimizerAccess::replaceWith(Insn& insn, Insn::Kind kind, VirtualRegister dst, VirtualRegister src)
{
    ASSERT(kind != Insn::Original);
    insn.kind = kind;
    insn.synthDst = dst;
    insn.synthSrc = src;
    if (kind == Insn::SynthJmp)
        insn.targets.shrink(1);
    else
        insn.targets.shrink(0);
    insn.useMap.shrink(0);
    insn.defMap.shrink(0);
    insn.knownConstants.shrink(0);
    insn.staticOuterHops = std::nullopt;
    insn.staticScopeOffset = std::nullopt;
    insn.staticScopeOffsetIsLazyFunctionSlot = false;
    computeUseDef(insn);
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
    // An exception escaping handler h's range next reaches the first later (outer) handler covering the same code.
    m_enclosingHandler.fill(UINT_MAX, m_handlers.size());
    for (unsigned h = 0; h < m_handlers.size(); ++h) {
        for (unsigned outer = h + 1; outer < m_handlers.size(); ++outer) {
            if (m_handlers[outer].start <= m_handlers[h].start && m_handlers[outer].end > m_handlers[h].start) {
                m_enclosingHandler[h] = outer;
                break;
            }
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
        for (unsigned h = m_handlerForInsn[block.start]; h != UINT_MAX; h = m_enclosingHandler[h])
            visit(blockStartingAt(m_offsetToIndex[m_handlers[h].target]));
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

std::optional<bool> BytecodeOptimizerAccess::evaluateConstantBranch(const Insn& insn) const
{
    auto operand = [&](VirtualRegister original) -> std::optional<JSValue> {
        VirtualRegister reg = original;
        for (auto& pair : insn.useMap) {
            if (pair.first == original)
                reg = pair.second;
        }
        for (auto& pair : insn.knownConstants) {
            if (pair.first == reg)
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
    auto triState = [](TriState state) -> std::optional<bool> {
        if (state == TriState::Indeterminate)
            return std::nullopt;
        return state == TriState::True;
    };
    auto strictEqual = [&](VirtualRegister lhsReg, VirtualRegister rhsReg) -> std::optional<bool> {
        auto lhs = operand(lhsReg);
        auto rhs = operand(rhsReg);
        if (!lhs || !rhs)
            return std::nullopt;
        return triState(JSValue::pureStrictEqual(*lhs, *rhs));
    };
    switch (insn.opcode) {
    case op_jtrue:
    case op_jfalse: {
        auto value = operand(insn.opcode == op_jtrue ? insn.instruction->as<OpJtrue>().m_condition : insn.instruction->as<OpJfalse>().m_condition);
        if (!value)
            return std::nullopt;
        auto truth = triState(value->pureToBoolean());
        if (!truth)
            return std::nullopt;
        return insn.opcode == op_jtrue ? *truth : !*truth;
    }
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null: {
        VirtualRegister reg;
        switch (insn.opcode) {
        case op_jeq_null:
            reg = insn.instruction->as<OpJeqNull>().m_value;
            break;
        case op_jneq_null:
            reg = insn.instruction->as<OpJneqNull>().m_value;
            break;
        case op_jundefined_or_null:
            reg = insn.instruction->as<OpJundefinedOrNull>().m_value;
            break;
        default:
            reg = insn.instruction->as<OpJnundefinedOrNull>().m_value;
            break;
        }
        auto value = operand(reg);
        if (!value)
            return std::nullopt;
        bool nullish = value->isUndefinedOrNull();
        return (insn.opcode == op_jeq_null || insn.opcode == op_jundefined_or_null) ? nullish : !nullish;
    }
    case op_jstricteq: {
        auto bytecode = insn.instruction->as<OpJstricteq>();
        return strictEqual(bytecode.m_lhs, bytecode.m_rhs);
    }
    case op_jnstricteq: {
        auto bytecode = insn.instruction->as<OpJnstricteq>();
        auto equal = strictEqual(bytecode.m_lhs, bytecode.m_rhs);
        if (!equal)
            return std::nullopt;
        return !*equal;
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
            }
        }
        if (insn.isJmp()) {
            // jmp to the next live instruction is a no-op.
            if (nextLiveInsn(insn.targets[0]) == nextLiveInsn(i + 1)) {
                insn.live = false;
                changed = true;
                continue;
            }
            // jmp to a ret: return directly (same size, one dispatch less, and the ret block may become dead).
            unsigned target = nextLiveInsn(insn.targets[0]);
            if (target < m_insns.size() && m_insns[target].effectiveOpcode() == op_ret && m_insns[target].uses.size() == 1) {
                replaceWith(insn, Insn::SynthRet, { }, m_insns[target].uses[0]);
                changed = true;
            }
            continue;
        }
        if (insn.kind == Insn::Original && insn.targets.size() == 1) {
            if (auto known = evaluateConstantBranch(insn)) {
                if (*known)
                    replaceWith(insn, Insn::SynthJmp);
                else
                    insn.live = false;
                changed = true;
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
                    }
                break;
            default:
                break;
            }
        }
    }
    return changed;
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
        if (target < m_insns.size() && m_blockForInsn[target] != UINT_MAX)
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

Vector<bool> BytecodeOptimizerAccess::handlerEntryBlocks() const
{
    Vector<bool> result;
    result.fill(false, m_blocks.size());
    for (auto& handler : m_handlers) {
        unsigned target = nextLiveInsn(m_offsetToIndex[handler.target]);
        if (target < m_insns.size() && m_blockForInsn[target] != UINT_MAX)
            result[m_blockForInsn[target]] = true;
    }
    return result;
}

template<typename State, typename Meet, typename Transfer>
bool BytecodeOptimizerAccess::forwardMustAnalysis(const Meet& meet, const Transfer& transfer)
{
    Vector<std::optional<State>> outStates(m_blocks.size());
    Vector<bool> isHandlerEntry = handlerEntryBlocks();
    auto computeIn = [&](unsigned b) -> State {
        State result { };
        if (isHandlerEntry[b] || !b)
            return result;
        bool first = true;
        for (unsigned p : m_blocks[b].predecessors) {
            if (!m_blocks[p].reachable || !outStates[p])
                continue;
            if (first) {
                result = *outStates[p];
                first = false;
            } else
                meet(result, *outStates[p]);
        }
        return result;
    };
    for (unsigned iteration = 0; ; ++iteration) {
        if (iteration > 100)
            return false;
        bool changed = false;
        for (unsigned b = 0; b < m_blocks.size(); ++b) {
            if (!m_blocks[b].reachable)
                continue;
            State state = computeIn(b);
            transfer(m_blocks[b], state, false);
            if (!outStates[b] || !(*outStates[b] == state)) {
                outStates[b] = WTF::move(state);
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    bool result = false;
    for (unsigned b = 0; b < m_blocks.size(); ++b) {
        if (!m_blocks[b].reachable)
            continue;
        State state = computeIn(b);
        result |= transfer(m_blocks[b], state, true);
    }
    return result;
}

bool BytecodeOptimizerAccess::propagateCopies()
{
    // Forward "available copies" dataflow: after `mov d, s`, uses of d can read s instead until either is
    // redefined. Meet is intersection; exception handler entry blocks start empty.
    using CopyMap = UncheckedKeyHashMap<int, int, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>>; // d.offset() -> s.offset()

    auto killRegister = [](CopyMap& copies, VirtualRegister r) {
        if (copies.isEmpty())
            return;
        copies.remove(r.offset());
        copies.removeIf([&](auto& entry) { return entry.value == r.offset(); });
    };
    for (auto& insn : m_insns)
        insn.knownConstants.shrink(0);
    m_branchesHaveKnownConstants = false;

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
                    if (source.isConstant() && insn.kind == Insn::Original && insn.opcode != op_mov) {
                        if (insn.isBranchOrSwitch() && !insn.knownConstants.contains(std::pair { r, source })) {
                            insn.knownConstants.append({ r, source });
                            m_branchesHaveKnownConstants = true;
                        }
                        continue;
                    }
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
            if (insn.kind == Insn::Original && insn.opcode == op_yield) {
                // A generator body is re-entered with fresh arguments after each yield: copies of argument
                // registers do not survive it.
                copies.removeIf([&](auto& entry) { return VirtualRegister { entry.key }.isArgument() || VirtualRegister { entry.value }.isArgument(); });
            }
            if (insn.clobberFrom != UINT_MAX)
                copies.removeIf([&](auto& entry) { return insn.clobbers(VirtualRegister { entry.key }) || insn.clobbers(VirtualRegister { entry.value }); });
            if (insn.isMov() && insn.defs.size() == 1 && insn.uses.size() == 1) {
                VirtualRegister d = insn.defs[0];
                VirtualRegister s = insn.uses[0];
                bool propagatable = s.isLocal() || s.isArgument() || (s.isConstant() && (constantValue(s) || m_codeBlock->constantSourceCodeRepresentation(s) == SourceCodeRepresentation::LinkTimeConstant));
                if (isLocal(d) && d != s && propagatable)
                    copies.set(d.offset(), s.offset());
            }
        }
        return changed;
    };

    auto meet = [](CopyMap& into, const CopyMap& other) {
        into.removeIf([&](auto& entry) {
            auto it = other.find(entry.key);
            return it == other.end() || it->value != entry.value;
        });
    };
    return forwardMustAnalysis<CopyMap>(meet, transfer);
}

bool BytecodeOptimizerAccess::resolveScopesStatically()
{
    // For free variables that the enclosing-scope chain proves live in an environment record at a fixed distance:
    //   resolve_scope dst, scope, X         (X is |hops| records beyond this function's own scope)
    //     -> mov dst, scope                 if hops + localScopeDepth == 0 (the record *is* the current scope)
    //     -> resolve_scope with a static resolve type otherwise (link walks pointers, no name lookups)
    //   get_from_scope dst, s, X (s known to be X's record) -> ResolvedClosureVar with X's slot.
    // put_to_scope is left dynamic: linking it also invalidates the variable's watchpoint.
    auto* link = m_generator.m_parentDeclaredNames.get();
    if (!link)
        return false;
    bool changed = false;
    // Which environment (identified by hop count from [[Scope]]) a register is known to hold, block-locally.
    struct Known {
        unsigned hops;
    };
    for (auto& block : m_blocks) {
        if (!block.reachable)
            continue;
        UncheckedKeyHashMap<int, Known, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> regEnv;
        for (unsigned i = block.start; i < block.end; ++i) {
            auto& insn = m_insns[i];
            if (!insn.live)
                continue;
            std::optional<std::pair<int, Known>> established;
            if (insn.kind == Insn::Original && insn.opcode == op_resolve_scope && insn.useMap.isEmpty() && insn.defMap.isEmpty()) {
                auto bytecode = insn.instruction->as<OpResolveScope>();
                if (bytecode.m_resolveType == GlobalProperty && bytecode.m_scope == m_codeBlock->scopeRegister()) {
                    auto resolution = link->resolve(m_codeBlock->identifier(bytecode.m_var).impl());
                    if (resolution.kind == DeclaredNamesLink::Resolution::Slot) {
                        if (!resolution.hops && !bytecode.m_localScopeDepth)
                            replaceWith(insn, Insn::SynthMov, bytecode.m_dst, bytecode.m_scope);
                        else
                            insn.staticOuterHops = resolution.hops;
                        established = { { bytecode.m_dst.offset(), Known { resolution.hops } } };
                        changed = true;
                        m_staticResolves++;
                    }
                }
            } else if (insn.kind == Insn::Original && insn.opcode == op_get_from_scope && insn.useMap.isEmpty() && !insn.staticScopeOffset) {
                auto bytecode = insn.instruction->as<OpGetFromScope>();
                auto it = regEnv.find(bytecode.m_scope.offset());
                if (it != regEnv.end() && bytecode.m_getPutInfo.resolveType() == GlobalProperty) {
                    auto resolution = link->resolve(m_codeBlock->identifier(bytecode.m_var).impl());
                    // A slot past 255 would force the instruction wide (+8 bytes) just to save one link-time lookup;
                    // not worth the bytes unless the instruction is wide already.
                    bool fits = resolution.offset <= UINT8_MAX || insn.instruction->isWide16() || insn.instruction->isWide32();
                    if (resolution.kind == DeclaredNamesLink::Resolution::Slot && resolution.hops == it->value.hops && fits) {
                        insn.staticScopeOffset = resolution.offset;
                        insn.staticScopeOffsetIsLazyFunctionSlot = resolution.isLazyFunctionSlot;
                        changed = true;
                        m_staticGets++;
                    }
                }
            }
            for (auto r : insn.defs)
                regEnv.remove(r.offset());
            if (insn.clobberFrom != UINT_MAX)
                regEnv.removeIf([&](auto& entry) { return insn.clobbers(VirtualRegister { entry.key }); });
            if (insn.isMov() && insn.uses.size() == 1 && insn.defs.size() == 1) {
                if (auto it = regEnv.find(insn.uses[0].offset()); it != regEnv.end())
                    established = { { insn.defs[0].offset(), it->value } };
            }
            if (established) {
                regEnv.set(established->first, established->second);
                if (insn.copyTo.isValid())
                    regEnv.set(insn.copyTo.offset(), established->second);
            }
        }
    }
    return changed;
}

bool BytecodeOptimizerAccess::cacheScopeResolutions()
{
    // resolve_scope of a name declared by an enclosing function/module scope yields the same environment record
    // every time it runs against the same starting scope. Cache such resolutions in fresh registers (placed below
    // all call frames so calls do not clobber them) and turn repeats into movs. Names that would fall through to
    // the global object are left alone: a later global lexical binding can change what they resolve to.
    // A candidate is either a statically located environment record (every name living |hops| records out
    // resolves to the same object, so they share one cache) or a single stable name without a static slot
    // (an import: resolves to the exporting module's environment).
    struct Candidate {
        int scope;
        unsigned localScopeDepth;
        unsigned identifier; // UINT_MAX for environment-record candidates
        unsigned hops; // UINT_MAX for name candidates
        unsigned count { 0 };
        VirtualRegister cache;
        bool operator==(const Candidate& other) const { return scope == other.scope && localScopeDepth == other.localScopeDepth && identifier == other.identifier && hops == other.hops; }
    };
    Vector<Candidate> candidates;
    auto* link = m_generator.m_parentDeclaredNames.get();
    auto candidateIndex = [&](const Insn& insn) -> std::optional<unsigned> {
        if (!link || !insn.live || insn.kind != Insn::Original || insn.opcode != op_resolve_scope)
            return std::nullopt;
        auto bytecode = insn.instruction->as<OpResolveScope>();
        if (!insn.useMap.isEmpty() || !insn.defMap.isEmpty())
            return std::nullopt;
        if (bytecode.m_resolveType != GlobalProperty && !insn.staticOuterHops)
            return std::nullopt;
        Candidate key { bytecode.m_scope.offset(), bytecode.m_localScopeDepth, bytecode.m_var, UINT_MAX, 0, { } };
        if (insn.staticOuterHops) {
            key.identifier = UINT_MAX;
            key.hops = *insn.staticOuterHops;
        } else if (link->resolve(m_codeBlock->identifier(bytecode.m_var).impl()).kind != DeclaredNamesLink::Resolution::Stable)
            return std::nullopt;
        for (unsigned i = 0; i < candidates.size(); ++i) {
            if (candidates[i] == key)
                return i;
        }
        candidates.append(key);
        return candidates.size() - 1;
    };
    Vector<int> candidateOf; // per instruction, -1 if none
    candidateOf.fill(-1, m_insns.size());
    for (unsigned i = 0; i < m_insns.size(); ++i) {
        if (auto index = candidateIndex(m_insns[i])) {
            candidateOf[i] = *index;
            candidates[*index].count++;
        }
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
        m_scopeCacheContents.add(candidates[selected[i]].cache.offset(), ScopeValue { candidates[selected[i]].scope, candidates[selected[i]].identifier != UINT_MAX ? candidates[selected[i]].identifier : environmentVia(candidates[selected[i]].localScopeDepth, candidates[selected[i]].hops) });
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
            if (int index = candidateOf[i]; index >= 0 && slotOf[index] >= 0 && insn.kind == Insn::Original)
                slot = slotOf[index];
            if (slot && (available & (1u << *slot))) {
                if (apply) {
                    replaceWith(insn, Insn::SynthMov, insn.defs[0], candidates[selected[*slot]].cache);
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
    struct Available {
        Bits bits { 0 };
        bool operator==(const Available&) const = default;
    };
    auto meet = [](Available& into, const Available& other) { into.bits &= other.bits; };
    return forwardMustAnalysis<Available>(meet, [&](Block& block, Available& state, bool apply) { return transfer(block, state.bits, apply); });
}

bool BytecodeOptimizerAccess::eliminateRedundantTDZChecks()
{
    // A binding leaves its temporal dead zone exactly once and never re-enters it. So a check_tdz on a value loaded
    // from a binding is redundant if every path to it already checked, or stored to, that same binding.
    //
    // Bindings are named by how they were reached, not by the register holding the scope. When the enclosing scopes
    // prove that X lives in an environment record (no `with` object, sloppy eval or global object can answer the
    // lookup), resolve_scope(base, X) is a pure function of |base|'s value, so "get_from_scope(resolve_scope(base, X), Y)"
    // denotes the same binding every time it is evaluated while |base| holds the same value. Keys are
    // (base register, resolved name X or none, name Y) and are dropped when the base register is redefined.
    // Without that proof (dynamic scopes can answer differently each time, e.g. a `with` object whose property is
    // deleted between two reads, exposing an outer binding still in its TDZ) the loaded value is keyed on the register
    // holding the scope object itself, which is exact. Forward "must" dataflow, meet is intersection.
    struct Key {
        int base { 0 };
        unsigned via { UINT_MAX }; // identifier index given to resolve_scope, or UINT_MAX if the scope is |base| itself
        unsigned name { UINT_MAX };
        bool operator==(const Key&) const = default;
        uint64_t packed() const { return (static_cast<uint64_t>(via) << 32) | name; } // unique within one base
    };
    // The dataflow state: initialized-or-checked bindings, grouped by base register so that redefining a register
    // drops its keys in O(1) (module top levels define temporaries tens of thousands of times). The number of
    // bindings tracked at once is capped to keep the per-block state copies of the dataflow cheap; TDZ checks worth
    // removing follow their binding's first use closely, so forgetting old ones costs little.
    struct KeySet {
        enum { capacity = 256 };
        using Keys = UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
        UncheckedKeyHashMap<int, Keys, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> byBase;
        unsigned size { 0 };

        bool contains(const Key& key) const
        {
            auto it = byBase.find(key.base);
            return it != byBase.end() && it->value.contains(key.packed());
        }
        void add(const Key& key)
        {
            if (size >= capacity)
                return;
            size += byBase.add(key.base, Keys { }).iterator->value.add(key.packed()).isNewEntry;
        }
        void recount()
        {
            size = 0;
            for (auto& entry : byBase)
                size += entry.value.size();
        }
        void removeBase(int base)
        {
            if (byBase.remove(base))
                recount();
        }
        void remove(const Key& key)
        {
            auto it = byBase.find(key.base);
            if (it != byBase.end() && it->value.remove(key.packed()))
                --size;
        }
        void removeBasesClobberedBy(const Insn& insn)
        {
            if (byBase.removeIf([&](auto& entry) { return insn.clobbers(VirtualRegister { entry.key }); }))
                recount();
        }
        void intersect(const KeySet& other)
        {
            byBase.removeIf([&](auto& entry) {
                auto it = other.byBase.find(entry.key);
                if (it == other.byBase.end())
                    return true;
                entry.value.removeIf([&](uint64_t key) { return !it->value.contains(key); });
                return entry.value.isEmpty();
            });
            recount();
        }
        bool operator==(const KeySet& other) const
        {
            if (size != other.size || byBase.size() != other.byBase.size())
                return false;
            for (auto& entry : byBase) {
                auto it = other.byBase.find(entry.key);
                if (it == other.byBase.end() || it->value.size() != entry.value.size())
                    return false;
                for (uint64_t key : entry.value) {
                    if (!it->value.contains(key))
                        return false;
                }
            }
            return true;
        }
    };

    auto mappedUse = [](const Insn& insn, VirtualRegister original) {
        for (auto& pair : insn.useMap) {
            if (pair.first == original)
                return pair.second;
        }
        return original;
    };
    auto* link = m_generator.m_parentDeclaredNames.get();

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
        // Registers currently serving as the base of some regScope/regBinding entry, so that redefining any other
        // register costs O(1).
        HashCountedSet<int, IntHash<int>, WTF::SignedWithZeroKeyHashTraits<int>> localBases;
        auto setRegScope = [&](int r, ScopeValue value) {
            if (auto it = regScope.find(r); it != regScope.end()) {
                localBases.remove(it->value.base);
                it->value = value;
            } else
                regScope.add(r, value);
            localBases.add(value.base);
        };
        auto setRegBinding = [&](int r, Key key) {
            if (auto it = regBinding.find(r); it != regBinding.end()) {
                localBases.remove(it->value.base);
                it->value = key;
            } else
                regBinding.add(r, key);
            localBases.add(key.base);
        };
        auto removeLocalIf = [&](const auto& predicate) {
            regScope.removeIf([&](auto& entry) {
                if (!predicate(entry.key, entry.value.base))
                    return false;
                localBases.remove(entry.value.base);
                return true;
            });
            regBinding.removeIf([&](auto& entry) {
                if (!predicate(entry.key, entry.value.base))
                    return false;
                localBases.remove(entry.value.base);
                return true;
            });
        };
        auto killRegister = [&](VirtualRegister r) {
            checked.removeBase(r.offset());
            bool isBase = localBases.contains(r.offset());
            if (!isBase && !regScope.contains(r.offset()) && !regBinding.contains(r.offset()))
                return;
            removeLocalIf([&](int key, int base) { return key == r.offset() || (isBase && base == r.offset()); });
        };

        for (unsigned i = block.start; i < block.end; ++i) {
            auto& insn = m_insns[i];
            if (!insn.live)
                continue;
            OpcodeID opcode = insn.effectiveOpcode();
            if (opcode == op_check_tdz && insn.kind == Insn::Original && insn.uses.size() == 1) {
                auto it = regBinding.find(insn.uses[0].offset());
                if (it != regBinding.end()) {
                    if (checked.contains(it->value)) {
                        if (apply) {
                            insn.live = false;
                            changed = true;
                        }
                    } else
                        checked.add(it->value);
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
                    bool singleDst = insn.defs.size() == 1 || (insn.defs.size() == 2 && insn.copyTo.isValid());
                    bool resolvesToEnvironmentRecord = insn.staticOuterHops || (bytecode.m_resolveType == GlobalProperty && link && link->resolve(m_codeBlock->identifier(bytecode.m_var).impl()).kind != DeclaredNamesLink::Resolution::Dynamic);
                    if (base.via == UINT_MAX && singleDst && resolvesToEnvironmentRecord) {
                        unsigned via = insn.staticOuterHops ? environmentVia(bytecode.m_localScopeDepth, *insn.staticOuterHops) : bytecode.m_var;
                        newScope = { { insn.defs[0].offset(), ScopeValue { base.base, via } } };
                    }
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
                    if (bytecode.m_var != UINT_MAX) {
                        ScopeValue scope = scopeValueOf(mappedUse(insn, bytecode.m_scope));
                        Key key { scope.base, scope.via, bytecode.m_var };
                        if (valueNeverEmpty)
                            stored = key;
                        else
                            checked.remove(key);
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
            case op_new_reg_exp_shared:
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
                removeLocalIf([&](int key, int base) { return insn.clobbers(VirtualRegister { key }) || insn.clobbers(VirtualRegister { base }); });
                checked.removeBasesClobberedBy(insn);
            }
            if (newScope) {
                setRegScope(newScope->first, newScope->second);
                if (insn.copyTo.isValid())
                    setRegScope(insn.copyTo.offset(), newScope->second);
            }
            if (newBinding) {
                setRegBinding(newBinding->first, newBinding->second);
                if (insn.copyTo.isValid())
                    setRegBinding(insn.copyTo.offset(), newBinding->second);
            }
            if (stored) {
                removeLocalIf([&](int key, int) {
                    auto it = regBinding.find(key);
                    return it != regBinding.end() && it->value == *stored;
                });
                checked.add(*stored); // A store that completes leaves the binding initialized.
            }
        }
        return changed;
    };

    auto meet = [](KeySet& into, const KeySet& other) { into.intersect(other); };
    return forwardMustAnalysis<KeySet>(meet, transfer);
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
        if (static_cast<size_t>(length) * m_numLocals > 64 * 1024 * 1024)
            continue; // per-instruction liveness for this block would be too large; skip it
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
            if (auto* handler = handlerForInsn(mov)) {
                // Renaming the def to d would leave t stale in the handler if anything between the def and the mov throws.
                unsigned target = nextLiveInsn(m_offsetToIndex[handler->target]);
                if (target < m_insns.size() && m_blockForInsn[target] != UINT_MAX && m_blocks[m_blockForInsn[target]].liveIn[t.toLocal()])
                    continue;
            }
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

    ResolveType operator()(BytecodeOperandName, ResolveType type)
    {
        if (insn->staticOuterHops)
            return static_cast<ResolveType>(firstStaticClosureVarResolveType + *insn->staticOuterHops);
        return type;
    }

    GetPutInfo operator()(BytecodeOperandName, GetPutInfo info)
    {
        if (insn->staticScopeOffset && insn->effectiveOpcode() == op_get_from_scope)
            return GetPutInfo(info.resolveMode(), insn->staticScopeOffsetIsLazyFunctionSlot ? ResolvedLazyClosureVar : ResolvedClosureVar, info.initializationMode(), info.ecmaMode());
        return info;
    }

    unsigned operator()(BytecodeOperandName name, unsigned value)
    {
        if (name == BytecodeOperandName::offset && insn->staticScopeOffset && insn->effectiveOpcode() == op_get_from_scope)
            return *insn->staticScopeOffset;
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
        if (iteration == 16) {
            // Each pass can widen as little as one branch; rather than iterate once per branch on a pathological
            // chain, force every branch wide, after which offsets settle within a pass or two.
            for (auto& insn : m_insns) {
                if (insn.isBranchOrSwitch() || insn.isJmp())
                    insn.minimumSize = OpcodeSize::Wide32;
            }
        }
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

    // Switch jump tables: same walk (and order) as extractStoredJumpTargetsForInstruction() used in decode().
    for (auto& insn : m_insns) {
        if (!insn.live || insn.kind != Insn::Original)
            continue;
        if (insn.opcode != op_switch_imm && insn.opcode != op_switch_char && insn.opcode != op_switch_string)
            continue;
        unsigned cursor = 0;
        updateStoredJumpTargetsForInstruction(m_codeBlock, 0, newWriter.ref(insn.newOffset), [&](int32_t slot) -> int32_t {
            unsigned target = insn.targets[cursor++];
            if (target == noTarget) {
                RELEASE_ASSERT(!slot);
                return 0;
            }
            int32_t delta = static_cast<int32_t>(m_insns[nextLiveInsn(target)].newOffset) - static_cast<int32_t>(insn.newOffset);
            RELEASE_ASSERT(delta);
            return delta;
        });
        RELEASE_ASSERT(cursor == insn.targets.size());
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
    if (Options::dumpBytecodeOptimizer())
        dumpIR("after decode");

    unsigned originalCount = m_insns.size();
    buildBlocks();
    if (removeUnreachable())
        buildBlocks();
    if (Options::useBytecodeOptimizerStaticScopes())
        resolveScopesStatically();
    if (Options::useBytecodeOptimizerScopeCache())
        cacheScopeResolutions();
    if (Options::useBytecodeOptimizerTDZ())
        eliminateRedundantTDZChecks();
    for (unsigned round = 0; round < 6; ++round) {
        bool changed = false;
        buildBlocks();
        if (removeUnreachable())
            buildBlocks();
        if (simplifyJumps()) {
            changed = true;
            buildBlocks();
            if (removeUnreachable())
                buildBlocks();
        }
        computeLiveness();
        changed |= eliminateDeadStores();
        if (Options::useBytecodeOptimizerCopyPropagation()) {
            // Coalesce `t <- op; mov d, t` into `d <- op` before forward propagation gets a chance to extend t's
            // live range past the mov. Neither changes any block's live-out set, and propagation does not consult
            // liveness, so one liveness computation per round suffices; the movs they orphan die in the next round.
            changed |= coalesceDestinations();
            changed |= propagateCopies();
            // Constants only reach branch operands as analysis facts; give simplifyJumps() a round to fold them.
            changed |= m_branchesHaveKnownConstants && round < 2;
        }
        if (!changed)
            break;
    }
    // Blocks/liveness may be stale after the last DCE, but emission only needs per-instruction liveness flags
    // and reachability of handler targets, which DCE does not change.
    buildBlocks();

    if (Options::dumpBytecodeOptimizer())
        dumpIR("before emit");

    unsigned liveCount = 0;
    for (auto& insn : m_insns)
        liveCount += insn.live + (insn.live && insn.copyTo.isValid());
    unsigned oldSize = m_writer.size();
    emit();
    if (Options::reportBytecodeOptimizer())
        dataLogLn("BytecodeOptimizer: instructions ", originalCount, " -> ", liveCount, ", bytes ", oldSize, " -> ", m_writer.size(), ", static scope resolutions ", m_staticResolves, ", static loads ", m_staticGets);
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
    BytecodeOptimizerAccess optimizer(generator, codeBlock, generator.m_writer);
    optimizer.run();
}

void BytecodeOptimizer::run(BytecodeGenerator& generator)
{
    BytecodeOptimizerAccess::runIfAppropriate(generator);
}


} // namespace JSC
