/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS)

#include "DFGSpeculativeJIT.h"

#if ENABLE(DFG_JIT) && USE(JSVALUE64)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "DFGSlowPathGenerator.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "JSArrayBufferView.h"
#include "JSCast.h"
#include "JSFFIFunction.h"
#include "JSGlobalObject.h"

namespace JSC { namespace DFG {

static bool ffiUntypedConversionMayUseStringArena(FFI::Type type)
{
    switch (type) {
    case FFI::Type::CString:
        return true;
    default:
        return false;
    }
}

void SpeculativeJIT::compileCallFFI(Node* node)
{
    JSFFIFunction* function = node->ffiFunction();
    FFI::Signature& signature = node->ffiSignature();
    void* target = function->target();
    JSGlobalObject* globalObject = function->globalObject();

    FFI::FFIContext* ffiContext = &globalObject->ffiContext();

    CodePtr<JITThunkPtrTag> invokeThunk = signature.invokeThunk();
    if (!invokeThunk) [[unlikely]] {
        flushRegisters();
        callOperationWithoutExceptionCheck(operationThrowOutOfMemoryError, TrustedImmPtr(&vm()));
        exceptionCheck();
        GPRTemporary result(this);
        move(TrustedImm64(JSValue::encode(jsUndefined())), result.gpr());
        jsValueResult(result.gpr(), node);
        return;
    }

    const unsigned nativeArgumentCount = signature.argumentCount();
    const unsigned returnSlotIndex = nativeArgumentCount;

    DFG_ASSERT(m_graph, node, node->numChildren() == 2 + nativeArgumentCount, node->numChildren(), nativeArgumentCount);
    DFG_ASSERT(m_graph, node, m_graph.m_parameterSlots * sizeof(Register) >= signature.slotBufferBytes(), m_graph.m_parameterSlots, signature.slotCount());

    FrozenValue* frozenGlobalObject = m_graph.freeze(globalObject);

    auto slotAddressFor = [&](unsigned slotIndex) -> Address {
        return Address(stackPointerRegister, static_cast<int32_t>(slotIndex * FFI::slotSize));
    };

    bool needsArenaBracket = false;
    for (unsigned i = 0; i < nativeArgumentCount; ++i) {
        FFI::Type type = signature.argumentType(i);
        Edge edge = m_graph.varArgChild(node, 2 + i);
        if (edge.useKind() == UntypedUse && ffiUntypedConversionMayUseStringArena(type))
            needsArenaBracket = true;
    }

    if (needsArenaBracket) {
        flushRegisters();
        setupArguments<decltype(operationFFIArenaEnter)>(TrustedImmPtr(frozenGlobalObject));
        appendCall(operationFFIArenaEnter);
    }

    auto emitArenaExitIfExceptionPending = [this, frozenGlobalObject] {
        Jump noException = emitExceptionCheck(vm(), AssemblyHelpers::InvertedExceptionCheck);
        setupArguments<decltype(operationFFIArenaExit)>(TrustedImmPtr(frozenGlobalObject));
        appendCall(operationFFIArenaExit);
        loadPtr(vm().addressOfException(), GPRInfo::returnValueGPR);
        noException.link(this);
    };

    auto storeInt32AsIntegerType = [&](FFI::Type type, GPRReg valueGPR, GPRReg scratchGPR, Address slot) {
        switch (type) {
        case FFI::Type::Char:
        case FFI::Type::Int8:
            signExtend8To32(valueGPR, scratchGPR);
            signExtend32ToPtr(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Uint8:
            zeroExtend8To32(valueGPR, scratchGPR);
            zeroExtend32ToWord(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Int16:
            signExtend16To32(valueGPR, scratchGPR);
            signExtend32ToPtr(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Uint16:
            zeroExtend16To32(valueGPR, scratchGPR);
            zeroExtend32ToWord(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Int32:
            signExtend32ToPtr(valueGPR, scratchGPR);
            break;
        case FFI::Type::Uint32:
            zeroExtend32ToWord(valueGPR, scratchGPR);
            break;
        case FFI::Type::Bool:
            compare32(NotEqual, valueGPR, TrustedImm32(0), scratchGPR);
            zeroExtend32ToWord(scratchGPR, scratchGPR);
            break;
        default:
            DFG_CRASH(m_graph, node, "CallFFI: not an integer-class FFI type");
            return;
        }
        store64(scratchGPR, slot);
    };

    for (unsigned i = 0; i < nativeArgumentCount; ++i) {
        FFI::Type type = signature.argumentType(i);
        Address slotAddress = slotAddressFor(i);

        Edge& edge = m_graph.varArgChild(node, 2 + i);

        switch (edge.useKind()) {
        case KnownInt32Use: {
            SpeculateInt32Operand value(this, edge);
            GPRTemporary scratch(this);
            storeInt32AsIntegerType(type, value.gpr(), scratch.gpr(), slotAddress);
            break;
        }

        case KnownBooleanUse: {
            DFG_ASSERT(m_graph, node, type == FFI::Type::Bool, static_cast<unsigned>(type));
            SpeculateBooleanOperand value(this, edge);
            GPRTemporary scratch(this);
            GPRReg scratchGPR = scratch.gpr();
            move(value.gpr(), scratchGPR);
            and64(TrustedImm32(1), scratchGPR);
            store64(scratchGPR, slotAddress);
            break;
        }

        case DoubleRepUse: {
            SpeculateDoubleOperand value(this, edge);
            FPRReg valueFPR = value.fpr();
            switch (type) {
            case FFI::Type::Double:
                storeDouble(valueFPR, slotAddress);
                break;
            case FFI::Type::Float: {
                FPRTemporary floatScratch(this);
                FPRReg floatFPR = floatScratch.fpr();
                convertDoubleToFloat(valueFPR, floatFPR);
                store32(TrustedImm32(0), slotAddress.withOffset(4));
                storeFloat(floatFPR, slotAddress);
                break;
            }
            default:
                DFG_CRASH(m_graph, node, "CallFFI: DoubleRepUse edge for a non-floating-point FFI type");
                break;
            }
            break;
        }

        case UntypedUse: {
            JSValueOperand value(this, edge);
            GPRTemporary scratch(this);
            GPRTemporary slotAddressTemp(this);
            FPRTemporary fpScratchTemp(this);
            GPRReg valueGPR = value.gpr();
            GPRReg scratchGPR = scratch.gpr();
            GPRReg slotAddressGPR = slotAddressTemp.gpr();
            FPRReg scratchFPR = fpScratchTemp.fpr();

            addPtr(TrustedImm32(static_cast<int32_t>(i * FFI::slotSize)), stackPointerRegister, slotAddressGPR);

            JumpList slowCases;
            JumpList stored;

            auto storeTypedArrayViewVector = [&] {
                slowCases.append(branchIfNotType(valueGPR, JSTypeRange { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayType) }));
                slowCases.append(branchTest8(NonZero, Address(valueGPR, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
                loadPtr(Address(valueGPR, JSArrayBufferView::offsetOfVector()), scratchGPR);
                slowCases.append(branchTestPtr(Zero, scratchGPR));
                cageTypedArrayStorage(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
            };

            switch (type) {
            case FFI::Type::Char:
            case FFI::Type::Int8:
            case FFI::Type::Uint8:
            case FFI::Type::Int16:
            case FFI::Type::Uint16:
            case FFI::Type::Int32:
            case FFI::Type::Uint32:
                slowCases.append(branchIfNotInt32(valueGPR));
                storeInt32AsIntegerType(type, valueGPR, scratchGPR, slotAddress);
                break;

            case FFI::Type::Bool: {
                Jump notInt32 = branchIfNotInt32(valueGPR);
                compare32(NotEqual, valueGPR, TrustedImm32(0), scratchGPR);
                zeroExtend32ToWord(scratchGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notInt32.link(this);
                slowCases.append(branchIfNotBoolean(valueGPR, scratchGPR));
                move(valueGPR, scratchGPR);
                and64(TrustedImm32(1), scratchGPR);
                store64(scratchGPR, slotAddress);
                break;
            }

            case FFI::Type::Int64:
            case FFI::Type::Uint64:
            case FFI::Type::Int64Fast:
            case FFI::Type::Uint64Fast: {
                Jump notInt32 = branchIfNotInt32(valueGPR);
                signExtend32ToPtr(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notInt32.link(this);
                slowCases.append(branchIfNotCell(valueGPR));
                slowCases.append(branchIfNotHeapBigInt(valueGPR));
                toBigInt64(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                break;
            }

            case FFI::Type::Double:
            case FFI::Type::Float: {
                slowCases.append(branchIfNotNumber(valueGPR));
                Jump notInt32 = branchIfNotInt32(valueGPR);
                convertInt32ToDouble(valueGPR, scratchFPR);
                Jump converted = jump();
                notInt32.link(this);
                unboxDouble(valueGPR, scratchGPR, scratchFPR);
                converted.link(this);
                if (type == FFI::Type::Double)
                    storeDouble(scratchFPR, slotAddress);
                else {
                    convertDoubleToFloat(scratchFPR, scratchFPR);
                    store32(TrustedImm32(0), slotAddress.withOffset(4));
                    storeFloat(scratchFPR, slotAddress);
                }
                break;
            }

            case FFI::Type::Pointer:
            case FFI::Type::CString:
            case FFI::Type::Function: {
                Jump notInt32 = branchIfNotInt32(valueGPR);
                signExtend32ToPtr(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notInt32.link(this);
                Jump notNumber = branchIfNotNumber(valueGPR);
                unboxDouble(valueGPR, scratchGPR, scratchFPR);
                truncateDoubleToInt64(scratchFPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notNumber.link(this);
                slowCases.append(branchIfNotCell(valueGPR));
                storeTypedArrayViewVector();
                break;
            }

            case FFI::Type::Buffer:
                slowCases.append(branchIfNotCell(valueGPR));
                storeTypedArrayViewVector();
                break;

            case FFI::Type::BufferLength:
                slowCases.append(jump());
                break;

            case FFI::Type::JSValue:
                store64(valueGPR, slotAddress);
                break;

            case FFI::Type::Void:
            case FFI::Type::RESERVED_WasNapiEnv:
                DFG_CRASH(m_graph, node, "CallFFI: unexpected JS argument type");
                break;
            }

            stored.link(this);

            if (!slowCases.empty()) {
                Label doneLabel = label();
                Vector<SilentRegisterSavePlan> savePlans;
                silentSpillAllRegistersImpl(false, savePlans, NoResult);
                uint32_t typeTag = static_cast<uint32_t>(type);
                bool exitArenaOnException = needsArenaBracket;
                addSlowPathGeneratorLambda([=, this, savePlans = WTF::move(savePlans), slowCases = WTF::move(slowCases)]() mutable {
                    slowCases.link(this);
                    silentSpill(savePlans);
                    setupArguments<decltype(operationFFIWriteSlot)>(TrustedImmPtr(frozenGlobalObject), TrustedImmPtr(ffiContext), TrustedImm32(static_cast<int32_t>(typeTag)), JSValueRegs(valueGPR), slotAddressGPR);
                    appendCall(operationFFIWriteSlot);
                    if (exitArenaOnException)
                        emitArenaExitIfExceptionPending();
                    std::optional<GPRReg> exceptionReg = tryHandleOrGetExceptionUnderSilentSpill<decltype(operationFFIWriteSlot)>(savePlans, NoResult);
                    silentFill(savePlans);
                    if (exceptionReg)
                        exceptionCheck(*exceptionReg);
                    jump().linkTo(doneLabel, this);
                });
            }
            break;
        }

        default:
            DFG_CRASH(m_graph, node, "CallFFI: unexpected use kind on an argument edge");
            break;
        }
    }

    flushRegisters();

    emitStoreCodeOrigin(node->origin.semantic);
    storePtr(GPRInfo::callFrameRegister, &vm().topCallFrame);
    move(TrustedImmPtr(target), GPRInfo::argumentGPR0);
    move(stackPointerRegister, GPRInfo::argumentGPR1);
    move(TrustedImmPtr(invokeThunk.taggedPtr()), GPRInfo::nonArgGPR0);
    call(GPRInfo::nonArgGPR0, OperationPtrTag);

    if (needsArenaBracket && signature.returnType() != FFI::Type::CString) {
        setupArguments<decltype(operationFFIArenaExit)>(TrustedImmPtr(frozenGlobalObject));
        appendCall(operationFFIArenaExit);
    } else if (needsArenaBracket)
        emitArenaExitIfExceptionPending();

    exceptionCheck();

    Address returnSlot = slotAddressFor(returnSlotIndex);
    switch (signature.returnType()) {
    case FFI::Type::Void: {
        GPRTemporary result(this);
        move(TrustedImm64(JSValue::encode(jsUndefined())), result.gpr());
        jsValueResult(result.gpr(), node);
        break;
    }

    case FFI::Type::Char:
    case FFI::Type::Int8:
    case FFI::Type::Uint8:
    case FFI::Type::Int16:
    case FFI::Type::Uint16:
    case FFI::Type::Int32: {
        GPRTemporary result(this);
        load32(returnSlot, result.gpr());
        strictInt32Result(result.gpr(), node);
        break;
    }

    case FFI::Type::Uint32: {
        GPRTemporary result(this);
        GPRTemporary value(this);
        FPRTemporary fpValue(this);
        GPRReg resultGPR = result.gpr();
        GPRReg valueGPR = value.gpr();
        FPRReg valueFPR = fpValue.fpr();
        load64(returnSlot, valueGPR);
        Jump doesNotFitInt32 = branch32(LessThan, valueGPR, TrustedImm32(0));
        boxInt32(valueGPR, JSValueRegs(resultGPR));
        Jump done = jump();
        doesNotFitInt32.link(this);
        convertUInt32ToDouble(valueGPR, valueFPR);
        boxDouble(valueFPR, resultGPR);
        done.link(this);
        jsValueResult(resultGPR, node);
        break;
    }

    case FFI::Type::Bool: {
        GPRTemporary result(this);
        GPRReg resultGPR = result.gpr();
        load32(returnSlot, resultGPR);
#if ASSERT_ENABLED
        Jump canonical = branch32(BelowOrEqual, resultGPR, TrustedImm32(1));
        breakpoint();
        canonical.link(this);
#endif
        or32(TrustedImm32(JSValue::ValueFalse), resultGPR);
        jsValueResult(resultGPR, node, DataFormatJSBoolean);
        break;
    }

    case FFI::Type::Double:
    case FFI::Type::Float: {
        FPRTemporary fpValue(this);
        GPRTemporary result(this);
        FPRReg valueFPR = fpValue.fpr();
        GPRReg resultGPR = result.gpr();
        if (signature.returnType() == FFI::Type::Double)
            loadDouble(returnSlot, valueFPR);
        else {
            loadFloat(returnSlot, valueFPR);
            convertFloatToDouble(valueFPR, valueFPR);
        }
        purifyNaN(valueFPR, valueFPR);
        boxDouble(valueFPR, resultGPR);
        jsValueResult(resultGPR, node);
        break;
    }

    case FFI::Type::JSValue: {
        GPRTemporary result(this);
        load64(returnSlot, result.gpr());
        jsValueResult(result.gpr(), node);
        break;
    }

    case FFI::Type::Int64:
    case FFI::Type::Uint64:
    case FFI::Type::Int64Fast:
    case FFI::Type::Uint64Fast:
    case FFI::Type::Pointer:
    case FFI::Type::CString:
    case FFI::Type::Function:
    case FFI::Type::Buffer: {
        GPRTemporary slotValue(this);
        GPRTemporary result(this);
        GPRReg slotValueGPR = slotValue.gpr();
        GPRReg resultGPR = result.gpr();
        load64(returnSlot, slotValueGPR);
        callOperation(operationFFIBoxSlot, resultGPR, TrustedImmPtr(frozenGlobalObject), TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(signature.returnType()))), slotValueGPR, TrustedImm32(needsArenaBracket && signature.returnType() == FFI::Type::CString ? 1 : 0));
        jsValueResult(resultGPR, node);
        break;
    }

    case FFI::Type::RESERVED_WasNapiEnv:
    case FFI::Type::BufferLength:
        DFG_CRASH(m_graph, node, "CallFFI: the reserved tag / buffer_length is never a return type");
        break;
    }

    FFI::g_ffiCompileCounts.dfgCallFFI++;
}

} } // namespace JSC::DFG

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(DFG_JIT) && USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
