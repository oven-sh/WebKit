/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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
#include "AirLowerStackArgs.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirFrameLayout.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"

namespace JSC { namespace B3 { namespace Air {

void lowerStackArgs(Code& code)
{
    PhaseScope phaseScope(code, "lowerStackArgs"_s);
    
    // Now we need to deduce how much argument area we need. We always reserve the conservative
    // register bytes for Bank::FP because CallArgs do not record which bank they are.
    unsigned conservativeCallArgBytes = code.usesSIMD() ? conservativeRegisterBytes(Bank::FP) : conservativeRegisterBytesWithoutVectors(Bank::FP);
    struct Site {
        unsigned blockIndex;
        unsigned instIndex;
    };
    Vector<Site> sites;
    for (BasicBlock* block : code) {
        unsigned blockSize = block->size();
        for (unsigned instIndex = 0; instIndex < blockSize; ++instIndex) {
            Inst& inst = block->at(instIndex);
            bool isSite = isARM64() && (inst.kind.opcode == Lea32 || inst.kind.opcode == Lea64);
            for (Arg& arg : inst.args()) {
                if (arg.isCallArg()) {
                    ASSERT(arg.offset() >= 0);
                    code.requestCallArgAreaSizeInBytes(arg.offset() + conservativeCallArgBytes);
                    isSite = true;
                } else if (arg.isStack())
                    isSite = true;
            }
            if (isSite)
                sites.append(Site { block->index(), instIndex });
        }
    }

    if (!code.frameLayout().isPlanned)
        code.setFrameSize(code.frameSize() + code.callArgAreaSizeInBytes());
    const unsigned bytesBelowFrameRecord = code.bytesBelowFrameRecord();

    // Finally, transform the code to use Addr's instead of StackSlot's. This is a lossless
    // transformation since we can search the StackSlots array to figure out which StackSlot any
    // offset-from-FP refers to.

    InsertionSet insertionSet(code);
    for (size_t site = 0; site < sites.size();) {
        BasicBlock* block = code[sites[site].blockIndex];
        // FIXME We can keep track of the last large offset which was materialized in this block, and reuse the register
        // if it hasn't been clobbered instead of renetating imm+add+addr every time. https://bugs.webkit.org/show_bug.cgi?id=171387

        for (; site < sites.size() && sites[site].blockIndex == block->index(); ++site) {
            unsigned instIndex = sites[site].instIndex;
            Inst& inst = block->at(instIndex);
            bool extendedOffsetAddrRegInUse = false;

            auto stackAddr = [&] (unsigned insertionIndex, Arg arg, Width width, Value::OffsetType offsetFromFP) -> Arg {
                int32_t offsetFromSP = offsetFromFP + bytesBelowFrameRecord;

                if (inst.admitsExtendedOffsetAddr(arg)) {
                    // Stackmaps and patchpoints expect addr inputs relative to SP or FP only. We might as well
                    // not even bother generating an addr with valid form for these opcodes since extended offset
                    // addr is always valid.
                    return Arg::extendedOffsetAddr(offsetFromFP);
                }

#if CPU(X86_64)
                if (code.policy().frame.prefersStackPointerAddresses && offsetFromFP < std::numeric_limits<int8_t>::min() && offsetFromSP >= 0 && offsetFromSP <= std::numeric_limits<int8_t>::max())
                    return Arg::addr(Air::Tmp(MacroAssembler::stackPointerRegister), offsetFromSP);
#endif
                Arg result = Arg::addr(Air::Tmp(GPRInfo::callFrameRegister), offsetFromFP);
                if (result.isValidForm(Move, width))
                    return result;

#if CPU(ARM64)
                if (code.frameLayout().slotArea == SlotArea::AboveFrameRecord) {
                    if (inst.kind.opcode == Patch)
                        return Arg::extendedOffsetAddr(offsetFromFP);
                    RELEASE_ASSERT(!extendedOffsetAddrRegInUse && Arg::isValidImmForm(offsetFromFP));
                    Air::Tmp tmp = Air::Tmp(extendedOffsetAddrRegister());
                    extendedOffsetAddrRegInUse = true;
                    insertionSet.insert(insertionIndex, Add64, inst.origin, Arg::imm(offsetFromFP), Air::Tmp(GPRInfo::callFrameRegister), tmp);
                    return Arg::addr(tmp, 0);
                }
#else
                RELEASE_ASSERT(code.frameLayout().slotArea != SlotArea::AboveFrameRecord);
#endif
                result = Arg::addr(Air::Tmp(MacroAssembler::stackPointerRegister), offsetFromSP);
                if (result.isValidForm(Move, width))
                    return result;

                if (inst.kind.opcode == Patch)
                    return Arg::extendedOffsetAddr(offsetFromFP);

#if CPU(ARM64) || CPU(RISCV64)
                RELEASE_ASSERT(!extendedOffsetAddrRegInUse);
                Air::Tmp tmp = Air::Tmp(extendedOffsetAddrRegister());
                extendedOffsetAddrRegInUse = true;

                Arg largeOffset = Arg::isValidImmForm(offsetFromSP) ? Arg::imm(offsetFromSP) : Arg::bigImm(offsetFromSP);
                insertionSet.insert(insertionIndex, Move, inst.origin, largeOffset, tmp);
                insertionSet.insert(insertionIndex, Add64, inst.origin, Air::Tmp(MacroAssembler::stackPointerRegister), tmp);
                result = Arg::addr(tmp, 0);
                return result;
#elif CPU(X86_64)
                UNUSED_PARAM(insertionIndex);
                // Can't happen on x86: immediates are always big enough for frame size.
                RELEASE_ASSERT_NOT_REACHED();
#else
#error Unhandled architecture.
#endif
            };

            auto isMove = [] (Inst& inst) -> std::optional<Width> {
                switch (inst.kind.opcode) {
                case Move:
                    return pointerWidth();
                case Move32:
                case MoveFloat:
                    return Width32;
                case MoveDouble:
                    return Width64;
                case MoveVector:
                    return Width128;
                default:
                    return std::nullopt;
                }
            };

            if (isARM64() && (inst.kind.opcode == Lea32 || inst.kind.opcode == Lea64)) {
                // On ARM64, Lea is just an add. We can't handle this below because
                // taking into account the Width to see if we can compute the immediate
                // is wrong.
                auto lowerArmLea = [&] (Value::OffsetType offset, Tmp base) {
                    ASSERT(inst.args()[1].isTmp());

                    if (Arg::isValidImmForm(offset))
                        inst = Inst(inst.kind.opcode == Lea32 ? Add32 : Add64, inst.origin, Arg::imm(offset), base, inst.args()[1]);
                    else {
                        Air::Tmp tmp = Air::Tmp(extendedOffsetAddrRegister());
                        Arg offsetArg = Arg::bigImm(offset);
                        insertionSet.insert(instIndex, Move, inst.origin, offsetArg, tmp);
                        inst = Inst(inst.kind.opcode == Lea32 ? Add32 : Add64, inst.origin, tmp, base, inst.args()[1]);
                    }
                };

                switch (inst.args()[0].kind()) {
                case Arg::Stack: {
                    StackSlot* slot = inst.args()[0].stackSlot();
                    lowerArmLea(inst.args()[0].offset() + slot->offsetFromFP(), Tmp(GPRInfo::callFrameRegister));
                    break;
                }
                case Arg::CallArg:
                    lowerArmLea(inst.args()[0].offset() - bytesBelowFrameRecord, Tmp(GPRInfo::callFrameRegister));
                    break;
                case Arg::Addr:
                    lowerArmLea(inst.args()[0].offset(), inst.args()[0].base());
                    break;
                case Arg::ExtendedOffsetAddr:
                    ASSERT_NOT_REACHED();
                    break;
                default:
                    break;
                }

                continue;
            }

            // Moves between stack spills may require using the extendedOffsetReg for both the src and dst addresses.
            // In that case, split the move into separate load and store instructions so that the extendedOffsetReg can
            // be used for each address, one at a time.
            std::optional<Width> moveWidth = isMove(inst);
            if (isARM64() && moveWidth && inst.args().size() == 3 && inst.args()[0].isStack()) {
                Arg& src = inst.args()[0];
                src = stackAddr(instIndex, src, *moveWidth, src.offset() + src.stackSlot()->offsetFromFP());
                if (extendedOffsetAddrRegInUse) {
                    Arg scratch = inst.args()[2];
                    ASSERT(scratch.isReg());
                    // Insert Mov src, scratchReg
                    insertionSet.insert(instIndex, static_cast<Opcode>(inst.kind.opcode), inst.origin, src, scratch);
                    extendedOffsetAddrRegInUse = false; // Used by the inserted instruction; no longer needed.
                    // Modify inst to be 'Move scratch, dest'.
                    inst.setArgs(scratch, inst.args()[1]);
                }
                // Fall through to handle remainder of the original or modified inst, including potential ZDef handling.
            }

            inst.forEachArg(
                [&] (Arg& arg, Arg::Role role, Bank, Width width) {
                    switch (arg.kind()) {
                    case Arg::Stack: {
                        StackSlot* slot = arg.stackSlot();
                        if (inst.kind.opcode == Move && slot->kind() == StackSlotKind::Spill)
                            inst.kind.spill = true;
                        if (Arg::isZDef(role)
                            && slot->kind() == StackSlotKind::Spill
                            && slot->byteSize() > bytesForWidth(width)) {
                            // Currently we only handle this simple case because it's the only one
                            // that arises: ZDef's are only 32-bit right now. So, when we hit these
                            // assertions it means that we need to implement those other kinds of
                            // zero fills.
                            RELEASE_ASSERT(slot->byteSize() == 8);
                            RELEASE_ASSERT(width == Width32);

#if CPU(ARM64) || CPU(RISCV64)
                            Air::Opcode storeOpcode = Move32;
                            Air::Arg::Kind operandKind = Arg::ZeroReg;
                            Air::Arg operand = Arg::zeroReg();
#elif CPU(X86_64)
                            Air::Opcode storeOpcode = Move32;
                            Air::Arg::Kind operandKind = Arg::Imm;
                            Air::Arg operand = Arg::imm(0);
#else
#error Unhandled architecture.
#endif
                            RELEASE_ASSERT(isValidForm(code.targetFeatures(), storeOpcode, operandKind, Arg::Stack));
                            insertionSet.insert(
                                instIndex + 1, storeOpcode, inst.origin, operand,
                                stackAddr(instIndex + 1, arg, width, arg.offset() + 4 + slot->offsetFromFP()));
                            extendedOffsetAddrRegInUse = false;
                        }
                        arg = stackAddr(instIndex, arg, width, arg.offset() + slot->offsetFromFP());
                        break;
                    }
                    case Arg::CallArg:
                        arg = stackAddr(instIndex, arg, width, arg.offset() - bytesBelowFrameRecord);
                        break;
                    default:
                        break;
                    }
                }
            );
        }
        insertionSet.execute(block);
    }
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

