/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
#include "AirCode.h"

#if ENABLE(B3_JIT)

#include "AirAllocateRegistersAndStackAndGenerateCode.h"
#include "AirCCallSpecial.h"
#include "AirCFG.h"
#include "AirDominators.h"
#include "AirInstInlines.h"
#include "AirNaturalLoops.h"
#include "AllowMacroScratchRegisterUsageIf.h"
#include "B3BasicBlockUtils.h"
#include "B3Procedure.h"
#include "CCallHelpers.h"
#include <wtf/ListDump.h>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace B3 { namespace Air {

WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(CFG);
WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(Code);
WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(Dominators);
WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(NaturalLoops);

static void defaultPrologueGenerator(CCallHelpers& jit, Code& code)
{
    jit.emitFunctionPrologue();

    // NOTE: on ARM64, if the callee saves have bigger offsets due to a potential tail call,
    // the macro assembler might assert scratch register usage on store operations emitted by emitSave.
    AllowMacroScratchRegisterUsageIf allowScratch(jit, isARM64());

    if (code.frameSize()) {
        jit.subPtr(MacroAssembler::TrustedImm32(code.frameSize()), MacroAssembler::stackPointerRegister);
    }
    
    jit.emitSave(code.calleeSaveRegisterAtOffsetList());
}

Code::Code(Procedure& proc)
    : m_proc(proc)
    , m_cfg(new CFG(*this))
    , m_preserveB3Origins(Options::dumpAirGraphAtEachPhase() || Options::dumpFTLDisassembly())
    , m_lastPhaseName("initial")
    , m_defaultPrologueGenerator(createSharedTask<PrologueGeneratorFunction>(&defaultPrologueGenerator))
{
    // Come up with initial orderings of registers. The user may replace this with something else.
    std::optional<WeakRandom> weakRandom;
    if (Options::airRandomizeRegs())
        weakRandom.emplace();
    forEachBank(
        [&](Bank bank) {
            Vector<Reg> volatileRegs;
            Vector<Reg> fullCalleeSaveRegs;
            Vector<Reg> calleeSaveRegs;
            RegisterSet all = bank == GP ? RegisterSet::allGPRs() : RegisterSet::allFPRs();
            all.exclude(RegisterSet::stackRegisters());
            all.exclude(RegisterSet::reservedHardwareRegisters());
            auto calleeSave = RegisterSet::calleeSaveRegisters();
            all.forEach(
                [&] (Reg reg) {
                    if (!calleeSave.contains(reg, IgnoreVectors))
                        volatileRegs.append(reg);
                    if (calleeSave.contains(reg, conservativeWidth(reg)))
                        fullCalleeSaveRegs.append(reg);
                    else if (calleeSave.contains(reg, conservativeWidthWithoutVectors(reg)))
                        calleeSaveRegs.append(reg);
                });
            Vector<Reg> result;
            result.appendVector(volatileRegs);
            result.appendVector(fullCalleeSaveRegs);
            result.appendVector(calleeSaveRegs);
            if (Options::airRandomizeRegs()) {
                WeakRandom random(Options::airRandomizeRegsSeed() ? Options::airRandomizeRegsSeed() : weakRandom->getUint32());
                shuffleVector(result, [&] (unsigned limit) { return random.getUint32(limit); });
            }
            setRegsInPriorityOrder(bank, result);
        });

    m_pinnedRegs.add(MacroAssembler::framePointerRegister, IgnoreVectors);
}

Code::~Code() = default;

void Code::emitDefaultPrologue(CCallHelpers& jit)
{
    defaultPrologueGenerator(jit, *this);
}

void Code::emitEpilogue(CCallHelpers& jit)
{
    if (m_epilogueGenerator) {
        m_epilogueGenerator->run(jit, *this);
        return;
    }
    if (frameSize()) {
        // NOTE: on ARM64, if the callee saves have bigger offsets due to a potential tail call,
        // the macro assembler might assert scratch register usage on load operations emitted by emitRestore.
        AllowMacroScratchRegisterUsageIf allowScratch(jit, isARM64());
        jit.emitRestore(calleeSaveRegisterAtOffsetList());
        jit.emitFunctionEpilogue();
    } else
        jit.emitFunctionEpilogueWithEmptyFrame();
    jit.ret();
}

void Code::setRegsInPriorityOrder(Bank bank, const Vector<Reg>& regs)
{
    regsInPriorityOrderImpl(bank) = regs;
    m_mutableRegs = { };
    forEachBank(
        [&] (Bank bank) {
            for (Reg reg : regsInPriorityOrder(bank))
                m_mutableRegs.add(reg, IgnoreVectors);
        });
}

void Code::pinRegister(Reg reg)
{
    Vector<Reg>& regs = regsInPriorityOrderImpl(Arg(Tmp(reg)).bank());
    ASSERT(regs.contains(reg));
    regs.removeFirst(reg);
    m_mutableRegs.remove(reg);
    ASSERT(!regs.contains(reg));
    m_pinnedRegs.add(reg, IgnoreVectors);
}

RegisterSet Code::mutableGPRs()
{
    RegisterSet result = m_mutableRegs.toRegisterSet();
    result.filter(RegisterSet::allGPRs());
    return result;
}

bool Code::needsUsedRegisters() const
{
    return m_proc.needsUsedRegisters();
}

BasicBlock* Code::addBlock(double frequency)
{
    std::unique_ptr<BasicBlock> block(new BasicBlock(m_blocks.size(), frequency));
    BasicBlock* result = block.get();
    m_blocks.append(WTF::move(block));
    return result;
}

StackSlot* Code::addStackSlot(uint64_t byteSize, StackSlotKind kind)
{
    StackSlot* result = m_stackSlots.addNew(byteSize, kind);
    if (m_stackIsAllocated) {
        // FIXME: This is unnecessarily awful. Fortunately, it doesn't run often.
        unsigned extent = WTF::roundUpToMultipleOf(result->alignment(), frameSize() - stackAdjustmentForAlignment() + byteSize);
        result->setOffsetFromFP(-static_cast<ptrdiff_t>(extent));
        setFrameSize(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(extent) + stackAdjustmentForAlignment());
    }
    return result;
}

Special* Code::addSpecial(std::unique_ptr<Special> special)
{
    special->m_code = this;
    return m_specials.add(WTF::move(special));
}

CCallSpecial* Code::cCallSpecial()
{
    if (!m_cCallSpecial) {
        m_cCallSpecial = static_cast<CCallSpecial*>(
            addSpecial(makeUnique<CCallSpecial>(usesSIMD())));
    }

    return m_cCallSpecial;
}

bool Code::isEntrypoint(BasicBlock* block) const
{
    // Note: This function must work both before and after LowerEntrySwitch.

    if (m_entrypoints.isEmpty())
        return !block->index();
    
    for (const FrequentedBlock& entrypoint : m_entrypoints) {
        if (entrypoint.block() == block)
            return true;
    }
    return false;
}

std::optional<unsigned> Code::entrypointIndex(BasicBlock* block) const
{
    RELEASE_ASSERT(m_entrypoints.size());
    for (unsigned i = 0; i < m_entrypoints.size(); ++i) {
        if (m_entrypoints[i].block() == block)
            return i;
    }
    return std::nullopt;
}

void Code::setCalleeSaveRegisterAtOffsetList(RegisterAtOffsetList&& registerAtOffsetList, StackSlot* slot)
{
    m_uncorrectedCalleeSaveRegisterAtOffsetList = WTF::move(registerAtOffsetList);
    for (const RegisterAtOffset& registerAtOffset : m_uncorrectedCalleeSaveRegisterAtOffsetList) {
        ASSERT(registerAtOffset.width() <= Width64);
        m_calleeSaveRegisters.add(registerAtOffset.reg(), registerAtOffset.width());
    }
    m_calleeSaveStackSlot = slot;
}

#if USE(BUN_JSC_ADDITIONS)
void Code::tryToPlaceSpillsAboveFrameRecord()
{
    constexpr unsigned maxBytesAbove = 480;
    constexpr unsigned maxBytesBelow = 240;
    if (!isARM64() || !m_mayPlaceSpillsAboveFrameRecord || m_callArgAreaSize || !m_frameSize)
        return;
    unsigned saved = m_calleeSaveStackSlot ? m_calleeSaveStackSlot->byteSize() : 0;
    bool savesAreOnTop = saved && m_calleeSaveStackSlot->offsetFromFP() == -static_cast<intptr_t>(saved);
    unsigned above = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(m_frameSize - (savesAreOnTop ? saved : 0));
    unsigned below = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(saved);
    if (above > maxBytesAbove || below > maxBytesBelow)
        return;
    for (StackSlot* slot : m_stackSlots) {
        if (slot != m_calleeSaveStackSlot)
            slot->setOffsetFromFP(slot->offsetFromFP() + m_frameSize + sizeof(CallerFrameAndPC));
    }
    if (saved)
        m_calleeSaveStackSlot->setOffsetFromFP(-static_cast<intptr_t>(below));
    m_placesSpillsAboveFrameRecord = true;
    m_bytesAboveFrameRecord = above;
    m_frameSize = above + below;
}

void Code::tryToPlaceCalleeSavesAtFootOfFrame()
{
    constexpr unsigned maxFrameSize = 496;
    if (!isX86_64() || !m_mayPlaceCalleeSavesAtFootOfFrame || m_callArgAreaSize || !m_calleeSaveStackSlot || m_frameSize > maxFrameSize)
        return;
    unsigned saved = m_calleeSaveStackSlot->byteSize();
    if (m_calleeSaveStackSlot->offsetFromFP() != -static_cast<intptr_t>(saved) || saved != m_uncorrectedCalleeSaveRegisterAtOffsetList.registerCount() * sizeof(CPURegister))
        return;
    for (const RegisterAtOffset& entry : m_uncorrectedCalleeSaveRegisterAtOffsetList) {
        if (!entry.reg().isGPR())
            return;
    }
    for (StackSlot* slot : m_stackSlots) {
        if (slot != m_calleeSaveStackSlot && saved % slot->alignment())
            return;
    }
    for (StackSlot* slot : m_stackSlots) {
        if (slot != m_calleeSaveStackSlot)
            slot->setOffsetFromFP(slot->offsetFromFP() + saved);
    }
    m_calleeSaveStackSlot->setOffsetFromFP(-static_cast<intptr_t>(m_frameSize));
    m_placesCalleeSavesAtFootOfFrame = true;
}

void Code::tryToPushRegistersInPrologue()
{
    constexpr unsigned maxFrameSize = 496;
    constexpr unsigned maxInstructions = 16;
    constexpr unsigned maxPushes = 6;
    constexpr intptr_t wordSize = sizeof(CPURegister);
    if (!isX86_64() || !m_mayPlaceCalleeSavesAtFootOfFrame || m_callArgAreaSize || m_frameSize > maxFrameSize || (m_calleeSaveStackSlot && !m_placesCalleeSavesAtFootOfFrame) || m_entrypoints.size() > 1)
        return;
    BasicBlock* entry = at(0);
    if (!entry || entry->numPredecessors())
        return;
    std::array<Reg, MacroAssembler::numberOfRegisters()> valueAtEntryIn;
    for (unsigned i = 0; i < valueAtEntryIn.size(); ++i)
        valueAtEntryIn[i] = Reg::fromIndex(i);
    valueAtEntryIn[Reg(MacroAssembler::stackPointerRegister).index()] = Reg();
    valueAtEntryIn[Reg(MacroAssembler::framePointerRegister).index()] = Reg();
    auto isGPR = [](const Arg& arg) {
        return arg.isReg() && arg.reg().isGPR();
    };
    for (unsigned index = 0; index < entry->size() && index < maxInstructions && m_registersPushedInPrologue.size() < maxPushes; ++index) {
        Inst& inst = entry->at(index);
        bool isMove = inst.kind.opcode == Move && inst.args().size() == 2 && isGPR(inst.args()[0]);
        if (isMove && isGPR(inst.args()[1])) {
            valueAtEntryIn[inst.args()[1].reg().index()] = valueAtEntryIn[inst.args()[0].reg().index()];
            continue;
        }
        if (isMove && inst.args()[1].isStack()) {
            StackSlot* slot = inst.args()[1].stackSlot();
            Reg source = valueAtEntryIn[inst.args()[0].reg().index()];
            intptr_t from = slot->offsetFromFP();
            intptr_t to = -wordSize * static_cast<intptr_t>(m_registersPushedInPrologue.size() + 1);
            if (!source || !slot->isSpill() || slot->byteSize() != wordSize || inst.args()[1].offset() || from > to)
                return;
            auto liesIn = [](StackSlot* other, intptr_t word) {
                return other->offsetFromFP() >= word && other->offsetFromFP() + static_cast<intptr_t>(other->byteSize()) <= word + wordSize;
            };
            auto touches = [](StackSlot* other, intptr_t word) {
                return other->offsetFromFP() < word + wordSize && other->offsetFromFP() + static_cast<intptr_t>(other->byteSize()) > word;
            };
            for (StackSlot* other : m_stackSlots) {
                if ((touches(other, from) && !liesIn(other, from)) || (touches(other, to) && !liesIn(other, to)))
                    return;
            }
            if (from != to) {
                for (StackSlot* other : m_stackSlots) {
                    if (liesIn(other, from))
                        other->setOffsetFromFP(other->offsetFromFP() - from + to);
                    else if (liesIn(other, to))
                        other->setOffsetFromFP(other->offsetFromFP() - to + from);
                }
            }
            m_registersPushedInPrologue.append(source);
            inst = Inst(Nop, inst.origin);
            continue;
        }
        if (inst.kind.opcode == Patch || inst.isTerminal())
            return;
        for (Arg& arg : inst.args()) {
            if (arg.isStack() || arg.isCallArg())
                return;
        }
        inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
            if (Arg::isAnyDef(role) && tmp.isGPR())
                valueAtEntryIn[tmp.reg().index()] = Reg();
        });
    }
}
#endif

RegisterAtOffsetList Code::calleeSaveRegisterAtOffsetList() const
{
    RegisterAtOffsetList result = m_uncorrectedCalleeSaveRegisterAtOffsetList;
    if (StackSlot* slot = m_calleeSaveStackSlot)
        result.adjustOffsets(slot->byteSize() + slot->offsetFromFP());
    return result;
}

void Code::resetReachability()
{
#if USE(BUN_JSC_ADDITIONS)
    recomputePredecessorsFromRoots(m_blocks, [&](auto&& visit) {
        if (m_entrypoints.isEmpty()) {
            visit(m_blocks[0].get());
            return;
        }
        for (const FrequentedBlock& entrypoint : m_entrypoints)
            visit(entrypoint.block());
    });
#else
    clearPredecessors(m_blocks);
    if (m_entrypoints.isEmpty())
        updatePredecessorsAfter(m_blocks[0].get());
    else {
        for (const FrequentedBlock& entrypoint : m_entrypoints)
            updatePredecessorsAfter(entrypoint.block());
    }
#endif
    
    for (auto& block : m_blocks) {
        if (isBlockDead(block.get()) && !isEntrypoint(block.get()))
            block = nullptr;
    }
}

void Code::dump(PrintStream& out) const
{
    if (!m_entrypoints.isEmpty())
        out.print(tierName, "Entrypoints: ", listDump(m_entrypoints), "\n");
    for (BasicBlock* block : *this)
        out.print(deepDump(block));
    if (stackSlots().size()) {
        out.print(tierName, "Stack slots:\n");
        for (StackSlot* slot : stackSlots())
            out.print(tierName, "    ", pointerDump(slot), ": ", deepDump(slot), "\n");
    }
    if (specials().size()) {
        out.print(tierName, "Specials:\n");
        for (Special* special : specials())
            out.print(tierName, "    ", deepDump(special), "\n");
    }
    if (m_frameSize || m_stackIsAllocated)
        out.print(tierName, "Frame size: ", m_frameSize, m_stackIsAllocated ? " (Allocated)" : "", "\n");
    if (m_callArgAreaSize)
        out.print(tierName, "Call arg area size: ", m_callArgAreaSize, "\n");
    RegisterAtOffsetList calleeSaveRegisters = this->calleeSaveRegisterAtOffsetList();
    if (calleeSaveRegisters.registerCount())
        out.print(tierName, "Callee saves: ", calleeSaveRegisters, "\n");
}

unsigned Code::findNextBlockIndex(unsigned index) const
{
    return findFirstBlockIndex(index + 1);
}

BasicBlock* Code::findNextBlock(BasicBlock* block) const
{
    unsigned index = findNextBlockIndex(block->index());
    if (index < size())
        return at(index);
    return nullptr;
}

void Code::addFastTmp(Tmp tmp)
{
    m_fastTmps.add(tmp);
}

void* Code::addDataSection(size_t size)
{
    return m_proc.addDataSection(size);
}

unsigned Code::jsHash() const
{
    unsigned result = 0;
    
    for (BasicBlock* block : *this) {
        result *= 1000001;
        for (Inst& inst : *block) {
            result *= 97;
            result += inst.jsHash();
        }
        for (BasicBlock* successor : block->successorBlocks()) {
            result *= 7;
            result += successor->index();
        }
    }
    for (StackSlot* slot : stackSlots()) {
        result *= 101;
        result += slot->jsHash();
    }
    
    return result;
}

void Code::setNumEntrypoints(unsigned numEntrypoints)
{
    m_prologueGenerators = { FillWith { }, numEntrypoints, m_defaultPrologueGenerator.copyRef() };
}

bool Code::usesSIMD() const
{
    return m_proc.usesSIMD();
}

void Code::setIonGraphPasses(Ref<JSON::Array>&& array)
{
    m_ionGraphPasses = WTF::move(array);
}

void Code::appendIonGraphPass(ASCIILiteral passName)
{
    // Right now, IonGraph cannot handle LIR visualization well: it is assuming MIR <-> LIR one on one, but B3/Air has ability to optimize and change Air after lowering from B3.
    // For now, we render Air as MIR too.
    auto pass = JSON::Object::create();
    pass->setString("name"_s, makeString("Air: "_s, passName));
    {
        auto ionGraph = JSON::Object::create();
        auto ionBlocks = JSON::Array::create();
        ionGraph->setArray("blocks"_s, ionBlocks);
        unsigned globalIndex = 0;

        for (auto* block : *this) {
            if (!block)
                continue;

            auto ionBlock = JSON::Object::create();
            auto attributes = JSON::Array::create();
            auto predecessors = JSON::Array::create();
            auto successors = JSON::Array::create();
            auto instructions = JSON::Array::create();

            for (const auto& inst : *block) {
                unsigned index = globalIndex++;
                auto instruction = JSON::Object::create();

                StringPrintStream stream;
                inst.dump(stream);
                instruction->setInteger("ptr"_s, index + 1);
                instruction->setInteger("id"_s, index);
                instruction->setArray("attributes"_s, JSON::Array::create());
                instruction->setArray("inputs"_s, JSON::Array::create());
                instruction->setString("opcode"_s, stream.toString());
                instruction->setArray("uses"_s, JSON::Array::create());
                instruction->setArray("memInputs"_s, JSON::Array::create());
                instruction->setString("type"_s, ""_s);

                instructions->pushObject(WTF::move(instruction));
            }

            for (auto* predecessor : block->predecessors())
                predecessors->pushInteger(predecessor->index());

            for (auto successor : block->successors())
                successors->pushInteger(successor.block()->index());

            ionBlock->setInteger("ptr"_s, block->index() + 1);
            ionBlock->setInteger("id"_s, block->index());
            ionBlock->setInteger("loopDepth"_s, 0);
            ionBlock->setArray("attributes"_s, JSON::Array::create());
            ionBlock->setArray("predecessors"_s, WTF::move(predecessors));
            ionBlock->setArray("successors"_s, WTF::move(successors));
            ionBlock->setArray("instructions"_s, WTF::move(instructions));
            ionBlocks->pushObject(ionBlock);
        }

        pass->setObject("mir"_s, WTF::move(ionGraph)); // MIR stands for SpiderMonkey's middle-level IR.
    }
    {
        auto ionGraph = JSON::Object::create();
        ionGraph->setArray("blocks"_s, JSON::Array::create());
        pass->setObject("lir"_s, WTF::move(ionGraph)); // LIR stands for SpiderMonkey's low-level IR.
    }
    RefPtr { m_ionGraphPasses }->pushObject(pass);
}


#if USE(BUN_JSC_ADDITIONS)
void Code::noteTableInCode(MacroAssembler::Label table, unsigned numberOfEntries)
{
    m_tablesInCode.append({ table, numberOfEntries });
}

#endif
} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
