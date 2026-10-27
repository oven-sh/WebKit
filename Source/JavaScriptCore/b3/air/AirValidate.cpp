/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "AirValidate.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirCode.h"
#include "AirFrameStates.h"
#include "AirInstInlines.h"
#include "AirReuseConditionFlags.h"
#include "AirTmpBoundaryRows.h"
#include "B3Procedure.h"
#include <wtf/IndexSet.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

class Validater {
public:
    Validater(Code& code, UTF8CStringView dumpBefore)
        : m_code(code)
        , m_dumpBefore(dumpBefore)
    {
    }

#define VALIDATE(condition, message) do {                               \
        if (condition)                                                  \
            break;                                                      \
        fail(__FILE__, __LINE__, WTF_PRETTY_FUNCTION, #condition, toUTF8CString message); \
    } while (false)
    
    void run()
    {
        UncheckedKeyHashSet<StackSlot*> validSlots;
        UncheckedKeyHashSet<BasicBlock*> validBlocks;
        UncheckedKeyHashSet<Special*> validSpecials;
        
        for (BasicBlock* block : m_code)
            validBlocks.add(block);
        for (StackSlot* slot : m_code.stackSlots())
            validSlots.add(slot);
        for (Special* special : m_code.specials())
            validSpecials.add(special);

        for (BasicBlock* block : m_code) {
            // Blocks that are entrypoints must not have predecessors.
            if (m_code.isEntrypoint(block))
                VALIDATE(!block->numPredecessors(), ("At entrypoint ", *block));
            
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                for (Arg& arg : inst.args()) {
                    switch (arg.kind()) {
                    case Arg::Stack:
                        VALIDATE(validSlots.contains(arg.stackSlot()), ("At ", inst, " in ", *block));
                        break;
                    case Arg::Special:
                        VALIDATE(validSpecials.contains(arg.special()), ("At ", inst, " in ", *block));
                        break;
                    default:
                        break;
                    }
                }
                VALIDATE(inst.isValidForm(m_code.targetFeatures()), ("At ", inst, " in ", *block));
                if (inst.kind.opcode == Patch)
                    VALIDATE(inst.hasCurrentForms(), ("At ", inst, " in ", *block));
                if (instIndex == block->size() - 1)
                    VALIDATE(inst.isTerminal(), ("At ", inst, " in ", *block));
                else
                    VALIDATE(!inst.isTerminal(), ("At ", inst, " in ", *block));

                // forEachArg must return Arg&'s that point into the args array.
                inst.forEachArg(
                    [&] (Arg& arg, Arg::Role role, Bank, Width width) {
                        VALIDATE(&arg >= &inst.args()[0], ("At ", arg, " in ", inst, " in ", *block));
                        VALIDATE(&arg <= &inst.args().back(), ("At ", arg, " in ", inst, " in ", *block));

                        // FIXME: replace with a check for wasm simd instructions.
                        // Bun: a pair of 64-bit registers is loaded from 128 bits of memory, with or without SIMD.
                        VALIDATE(Options::useWasmSIMD()
                            || !Arg::isAnyUse(role)
                            || width <= Width64
                            || inst.kind.opcode == LoadPair64, ("At ", inst, " in ", *block, " arg ", arg));
                    });
                
                switch (inst.kind.opcode) {
                case EntrySwitch:
                    VALIDATE(block->numSuccessors() == m_code.proc().numEntrypoints(), ("At ", inst, " in ", *block));
                    break;
                case Shuffle:
                    // We can't handle trapping shuffles because of how we lower them. That could
                    // be fixed though. Ditto for shuffles that would do fences, which is the other
                    // use of this bit.
                    VALIDATE(!inst.kind.effects, ("At ", inst, " in ", *block));
                    break;
                case VectorExtendLow:
                case VectorExtendHigh:
                    VALIDATE(elementByteSize(inst.args()[0].simdInfo().lane) <= 8, ("At ", inst, " in ", *block));
                    VALIDATE(elementByteSize(inst.args()[0].simdInfo().lane) >= 2, ("At ", inst, " in ", *block));
                    break;
                case ExtractRegister64:
                    VALIDATE(inst.args()[2].isImm(), ("At ", inst, " in ", *block));
                    VALIDATE(inst.args()[2].asTrustedImm32().m_value < 64, ("At ", inst, " in ", *block));
                    break;
                case ExtractRegister32:
                    VALIDATE(inst.args()[2].isImm(), ("At ", inst, " in ", *block));
                    VALIDATE(inst.args()[2].asTrustedImm32().m_value < 32, ("At ", inst, " in ", *block));
                    break;
                case EnterFrame:
                case LeaveFrame:
                    VALIDATE(m_code.policy().frame.emitsFrame, ("At ", inst, " in ", *block));
                    break;
                default:
                    break;
                }
            }
            for (BasicBlock* successor : block->successorBlocks())
                VALIDATE(validBlocks.contains(successor), ("In ", *block));
        }

        for (BasicBlock* block : m_code) {
            // We expect the predecessor list to be de-duplicated.
            UncheckedKeyHashSet<BasicBlock*> predecessors;
            for (BasicBlock* predecessor : block->predecessors())
                predecessors.add(predecessor);
            VALIDATE(block->numPredecessors() == predecessors.size(), ("At ", *block));
        }

        if (Inst* inst = findUseOfUndefinedFlags(m_code))
            VALIDATE(false, ("Flags are not defined at ", *inst));

        if (std::optional<Tmp> tmp = findUndefinedTmp(m_code))
            VALIDATE(false, ("A path from an entrypoint uses ", *tmp, " before any definition"));

        if (Inst* inst = findMisplacedBlockEntryClobber(m_code))
            VALIDATE(false, ("A clobber at block entry has to be the first instruction of a block that is not an entrypoint: ", *inst));

        if (m_code.policy().frame.emitsFrame && m_code.frameLayout().isPlanned) {
            std::optional<FrameStateViolation> violation = findFrameStateViolation(m_code);
            VALIDATE(!violation, ("Frame state violation of kind ", static_cast<unsigned>(violation->kind), " at ", *violation->block, " index ", violation->instIndex));
        }
    }

private:
    NO_RETURN_DUE_TO_CRASH void fail(
        const char* filename, int lineNumber, const char* function, const char* condition,
        UTF8CString message)
    {
        UTF8CString failureMessage;
        {
            StringPrintStream out;
            out.print("AIR VALIDATION FAILURE\n");
            out.print("    ", condition, " (", filename, ":", lineNumber, ")\n");
            out.print("    ", message, "\n");
            out.print("    After ", m_code.lastPhaseName(), "\n");
            failureMessage = out.toUTF8CString();
        }

        dataLog(failureMessage);
        if (m_dumpBefore) {
            dataLog("Before ", m_code.lastPhaseName(), ":\n");
            dataLog(m_dumpBefore);
        }
        dataLog("At time of failure:\n");
        dataLog(m_code);

        dataLog(failureMessage);
        WTFReportAssertionFailure(filename, lineNumber, function, condition);
        CRASH();
    }
    
    Code& m_code;
    UTF8CStringView m_dumpBefore;
};

bool anyInstUsesFlags(Code& code)
{
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            if (usesFlags(inst.kind.opcode))
                return true;
        }
    }
    return false;
}

bool flagsAfter(bool areDefined, const Inst& inst)
{
    Opcode opcode = inst.kind.opcode;
    return definesFlags(opcode) || setsFlagsToResult(inst) || (areDefined && preservesFlags(opcode));
}

bool flagsAtTail(bool areDefinedAtHead, BasicBlock* block)
{
    bool areDefined = areDefinedAtHead;
    for (Inst& inst : *block)
        areDefined = flagsAfter(areDefined, inst);
    return areDefined;
}

IndexSet<BasicBlock*> computeDefinedAtHead(Code& code)
{
    IndexSet<BasicBlock*> definedAtHead;
    Vector<BasicBlock*> worklist;
    for (BasicBlock* block : code) {
        worklist.append(block);
        for (BasicBlock* successor : block->successorBlocks())
            definedAtHead.add(successor);
    }
    for (BasicBlock* block : code) {
        if (code.isEntrypoint(block))
            definedAtHead.remove(block);
    }

    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        if (flagsAtTail(definedAtHead.contains(block), block))
            continue;
        for (BasicBlock* successor : block->successorBlocks()) {
            if (definedAtHead.remove(successor))
                worklist.append(successor);
        }
    }
    return definedAtHead;
}

bool anyInstHasNonRegisterTmp(Code& code)
{
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            bool hasNonRegisterTmp = false;
            inst.forEachTmpFast([&](Tmp& tmp) {
                hasNonRegisterTmp |= !tmp.isReg();
            });
            if (hasNonRegisterTmp)
                return true;
        }
    }
    return false;
}

} // anonymous namespace

void validate(Code& code, UTF8CStringView dumpBefore)
{
    Validater validater(code, dumpBefore);
    validater.run();
}

Inst* findUseOfUndefinedFlags(Code& code)
{
    if (!anyInstUsesFlags(code))
        return nullptr;

    IndexSet<BasicBlock*> definedAtHead = computeDefinedAtHead(code);
    for (BasicBlock* block : code) {
        bool areDefined = definedAtHead.contains(block);
        for (Inst& inst : *block) {
            if (usesFlags(inst.kind.opcode) && !areDefined)
                return &inst;
            areDefined = flagsAfter(areDefined, inst);
        }
    }
    return nullptr;
}

Inst* findMisplacedBlockEntryClobber(Code& code)
{
    for (BasicBlock* block : code) {
        bool isEntrypoint = block == code[0] || code.isEntrypoint(block);
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);
            if (!inst.clobbersAtBlockEntry())
                continue;
            if (instIndex || isEntrypoint)
                return &inst;
        }
    }
    return nullptr;
}

class TmpRowsAtEntrypoints : public TmpBoundaryRows {
public:
    static constexpr bool recordsLayoutBoundaries = false;

    TmpRowsAtEntrypoints(const BlockFacts& facts, Code& code)
        : TmpBoundaryRows(facts)
        , m_code(code)
    {
    }

    bool needsLiveAtTail(BasicBlock* block) const { return m_code.isEntrypoint(block); }

private:
    Code& m_code;
};

std::optional<Tmp> findUndefinedTmp(Code& code)
{
    if (!anyInstHasNonRegisterTmp(code))
        return std::nullopt;

    UsesAndDefsByBlock usesAndDefs(code, [](Tmp tmp) { return !tmp.isReg(); });
    BitMatrixLiveness<TmpRowsAtEntrypoints> liveness(code, usesAndDefs.facts(), code);
    liveness.compute();
    RELEASE_ASSERT(liveness.status() == BitMatrixLivenessStatus::Solved);

    auto liveColumns = liveness.makeWorkset();
    for (BasicBlock* block : code) {
        if (!code.isEntrypoint(block))
            continue;
        liveness.copyLiveAtTailInto(liveColumns, block);
        const BlockEnds begin = block->index() ? usesAndDefs.blockEnds[block->index() - 1] : BlockEnds { };
        const BlockEnds end = usesAndDefs.blockEnds[block->index()];
        for (unsigned i = begin.defs; i < end.defs; ++i) {
            if (usesAndDefs.crossesBoundary[usesAndDefs.defs[i]])
                liveColumns.remove(usesAndDefs.indexToColumn[usesAndDefs.defs[i]]);
        }
        for (unsigned i = begin.exposedUses; i < end.exposedUses; ++i)
            liveColumns.add(usesAndDefs.indexToColumn[usesAndDefs.exposedUses[i]]);
        std::optional<Tmp> undefinedTmp;
        liveColumns.forEachSetBit([&](unsigned column) {
            if (!undefinedTmp)
                undefinedTmp = Tmp::tmpForLinearIndex(code, usesAndDefs.columnToIndex[column]);
        });
        if (undefinedTmp)
            return undefinedTmp;
    }
    return std::nullopt;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

