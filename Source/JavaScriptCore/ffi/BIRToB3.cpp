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
#include "BIRToB3.h"

#if USE(BUN_JSC_ADDITIONS) && ENABLE(B3_JIT)

#include "B3ArgumentRegValue.h"
#include "B3AtomicValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CCallValue.h"
#include "B3Const32Value.h"
#include "B3Const128Value.h"
#include "B3Const64Value.h"
#include "B3ExtractValue.h"
#include "B3FenceValue.h"
#include "B3ConstDoubleValue.h"
#include "B3ConstFloatValue.h"
#include "B3ConstPtrValue.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3SIMDValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3SwitchValue.h"
#include "AirCode.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "CCallHelpers.h"
#include "FFICallingConvention.h"
#include "Options.h"
#include "SIMDShuffle.h"
#include <bit>
#include <cstring>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace FFI {

using namespace B3;
using BIR::Op;

namespace {

// What B3's CCall calls. It passes arguments the way JIT operations take them, which on Windows is
// not the way C functions do (SYSV_ABI there), so nothing else may be its target: calls into the
// program and into C libraries go through emitPatchpointCall.
uint64_t SYSV_ABI convertDoubleToUInt64(double value) { return static_cast<uint64_t>(value); }
double SYSV_ABI convertUInt64ToDouble(uint64_t value) { return static_cast<double>(value); }
float SYSV_ABI convertUInt64ToFloat(uint64_t value) { return static_cast<float>(value); }
uint64_t SYSV_ABI countPopulation(uint64_t value) { return std::popcount(value); }
void* SYSV_ABI copyMemory(void* destination, const void* source, size_t size) { return memcpy(destination, source, size); }
void* SYSV_ABI moveMemory(void* destination, const void* source, size_t size) { return memmove(destination, source, size); }
void* SYSV_ABI fillMemory(void* destination, int byte, size_t size) { return memset(destination, byte, size); }

// Whether B3's CCall and a C function agree on where arguments go.
#if OS(WINDOWS) && CPU(X86_64)
constexpr bool b3CallsAreNativeCalls = false;
#else
constexpr bool b3CallsAreNativeCalls = true;
#endif

template<typename Function>
void* cFunctionPointer(Function* function)
{
    return tagCFunction<void*, OperationPtrTag>(function);
}

// Where the host C ABI puts each argument of a call. Used from both sides: a caller stores to
// [sp + stackOffset], a callee finds the same bytes at [fp + 16 + stackOffset].
struct ArgumentLocation {
    enum class Kind : uint8_t { GPR, FPR, Stack, StackBytes } kind { Kind::GPR };
    Reg reg;
    unsigned stackOffset { 0 };
};

struct ArgumentLayout {
    Vector<ArgumentLocation> locations;
    unsigned stackBytes { 0 };
    unsigned namedGPRs { 0 };
    unsigned namedFPRs { 0 };
    unsigned namedStackBytes { 0 };
};

#if CPU(ARM64)
constexpr GPRReg indirectResultGPR = ARM64Registers::x8;
#endif

std::span<const FPRReg> floatResultRegisters()
{
#if CPU(X86_64)
    static constexpr std::array<FPRReg, 2> registers { X86Registers::xmm0, X86Registers::xmm1 };
#else
    static constexpr std::array<FPRReg, 4> registers { ARM64Registers::q0, ARM64Registers::q1, ARM64Registers::q2, ARM64Registers::q3 };
#endif
    return registers;
}

std::span<const GPRReg> integerResultRegisters()
{
    static constexpr std::array<GPRReg, 2> registers { GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2 };
    return registers;
}

// `anonymous` are the types of the arguments a variadic call passes after the named ones.
ArgumentLayout layoutArguments(const BIR::Signature& signature, std::span<const B3::Type> anonymous = { })
{
    constexpr NativeCC cc = hostNativeCC();
    auto gprs = integerArgumentRegisters(cc);
    auto fprs = floatArgumentRegisters(cc);
    unsigned gprIndex = 0;
    unsigned fprIndex = 0;
    unsigned nextStackOffset = shadowStackBytes(cc);
    ArgumentLayout layout;

    auto placeScalar = [&](unsigned position, bool isFloatingPoint, unsigned size, bool isNamed) {
        ArgumentLocation location;
        if (cc == NativeCC::Win64) {
            if (position < 4) {
                location.kind = isFloatingPoint ? ArgumentLocation::Kind::FPR : ArgumentLocation::Kind::GPR;
                location.reg = isFloatingPoint ? Reg(fprs[position]) : Reg(gprs[position]);
            } else {
                location.kind = ArgumentLocation::Kind::Stack;
                location.stackOffset = position * 8;
                nextStackOffset = location.stackOffset + 8;
            }
            return location;
        }
#if OS(DARWIN) && CPU(ARM64)
        // Apple's arm64 ABI passes every anonymous argument on the stack in 8-byte slots.
        if (!isNamed) {
            location.kind = ArgumentLocation::Kind::Stack;
            nextStackOffset = roundUpToMultipleOf<8>(nextStackOffset);
            location.stackOffset = nextStackOffset;
            nextStackOffset += 8;
            return location;
        }
#else
        UNUSED_PARAM(isNamed);
#endif
        if (isFloatingPoint && fprIndex < fprs.size()) {
            location.kind = ArgumentLocation::Kind::FPR;
            location.reg = fprs[fprIndex++];
        } else if (!isFloatingPoint && gprIndex < gprs.size()) {
            location.kind = ArgumentLocation::Kind::GPR;
            location.reg = gprs[gprIndex++];
        } else {
            location.kind = ArgumentLocation::Kind::Stack;
            unsigned slot = size == 16 ? 16 : stackPackingForNativeCC(cc) == StackPacking::Natural ? size : 8;
            nextStackOffset = roundUpToMultipleOf(slot, nextStackOffset);
            location.stackOffset = nextStackOffset;
            nextStackOffset += slot;
        }
        return location;
    };

    unsigned position = 0;
    for (const BIR::Parameter& parameter : signature.parameters) {
        switch (parameter.kind) {
        case BIR::ParamKind::Value: {
            bool isFloatingPoint = parameter.type == BIR::Type::F32 || parameter.type == BIR::Type::F64 || parameter.type == BIR::Type::V128;
            unsigned size = parameter.type == BIR::Type::V128 ? 16 : (parameter.type == BIR::Type::I32 || parameter.type == BIR::Type::F32) ? 4 : 8;
            layout.locations.append(placeScalar(position, isFloatingPoint, size, true));
            break;
        }
        case BIR::ParamKind::IndirectResult: {
#if CPU(ARM64)
            ArgumentLocation location;
            location.kind = ArgumentLocation::Kind::GPR;
            location.reg = indirectResultGPR;
            layout.locations.append(location);
            --position; // x8 is not one of the argument positions.
#else
            layout.locations.append(placeScalar(position, false, 8, true));
#endif
            break;
        }
        case BIR::ParamKind::ByValStack: {
            ArgumentLocation location;
            location.kind = ArgumentLocation::Kind::StackBytes;
            nextStackOffset = roundUpToMultipleOf(static_cast<unsigned>(parameter.alignment), nextStackOffset);
            location.stackOffset = nextStackOffset;
            nextStackOffset += static_cast<unsigned>(parameter.size);
            if (parameter.exhausts == BIR::Exhausts::IntegerRegisters)
                gprIndex = gprs.size();
            else if (parameter.exhausts == BIR::Exhausts::FloatRegisters)
                fprIndex = fprs.size();
            layout.locations.append(location);
            break;
        }
        }
        ++position;
    }
    layout.namedGPRs = gprIndex;
    layout.namedFPRs = fprIndex;
    layout.namedStackBytes = nextStackOffset - shadowStackBytes(cc);
    if (cc == NativeCC::Win64)
        layout.namedGPRs = std::min<unsigned>(position, 4);

    for (B3::Type type : anonymous) {
        layout.locations.append(placeScalar(position, type.isFloat() || type.isVector(), type.isVector() ? 16 : 8, false));
        ++position;
    }
    layout.stackBytes = roundUpToMultipleOf<16>(nextStackOffset);
    return layout;
}

constexpr unsigned savedFrameAndReturnAddressBytes = 2 * sizeof(void*);

} // anonymous namespace


BIRToB3::BIRToB3(const BIR::Module& module, const BIRLinkEnvironment& environment, Procedure& proc)
    : m_module(module)
    , m_environment(environment)
    , m_proc(proc)
    , m_inlinedInstructionBudget(Options::maximumBIRInlinedInstructionsPerFunction())
{
}

bool BIRToB3::shouldInlineCallee(unsigned functionIndex, bool hasConstantArgument) const
{
    // A variadic body reads its own frame (VaStart), so it must keep one. One that moves the stack
    // pointer relies on its own return to release what it allocated.
    // One that calls setjmp can be re-entered by longjmp, which restores the registers of the frame it
    // runs in: that must be a frame whose variables the frontend kept in memory, not a caller's.
    const BIR::Function& callee = m_module.functions[functionIndex];
    if (m_module.signatures[callee.signature].isVariadic || callee.movesStackPointer || callee.callsReturnsTwice || callee.usesFrameAddress)
        return false;
    if (m_inlineStack.contains(functionIndex) || !callee.hasBody)
        return false;
    if (callee.isNeverInline)
        return false;
    size_t size = callee.insts.size();
    // The author says its callers' constant arguments are what make it fast, so it has a budget of its
    // own, there only to stop a pathological nest.
    if (callee.isAlwaysInline)
        return size <= m_alwaysInlineInstructionBudget;
    if (size > m_inlinedInstructionBudget)
        return false;
    // About what passing arguments, calling, and a prologue and epilogue come to: inlining something
    // this small shrinks the caller no matter how many callers there are.
    if (size <= Options::maximumBIRInlineTrivialCalleeInstructionCount())
        return true;
    // A constant argument usually decides branches and folds arithmetic in the callee, so its copy
    // here ends up smaller than the callee is.
    if (hasConstantArgument && size <= Options::maximumBIRInlineConstantArgumentCalleeInstructionCount())
        return true;
    // What inlining this callee everywhere adds to the module: a copy per call site, less the
    // standalone copy a private function no longer needs once every call to it is inlined. A helper
    // with one caller is free; a big function called from many places is not worth it.
    bool needsOwnCopy = callee.isExported || callee.isAddressTaken;
    size_t copies = callee.callSiteCount - (needsOwnCopy ? 0 : 1);
    // The author asking for it is worth a larger share of the module, as in other compilers.
    size_t allowedGrowth = Options::maximumBIRInlineGrowthPerCallee() * (callee.hasInlineHint ? Options::birInlineHintGrowthMultiplier() : 1);
    return size <= Options::maximumBIRInlineCalleeInstructionCount() && size * copies <= allowedGrowth;
}

B3::Type BIRToB3::toB3(BIR::Type type)
{
    switch (type) {
    case BIR::Type::Void:
        return Void;
    case BIR::Type::I32:
        return Int32;
    case BIR::Type::I64:
        return Int64;
    case BIR::Type::F32:
        return Float;
    case BIR::Type::F64:
        return Double;
    case BIR::Type::V128:
        return V128;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value* BIRToB3::constant(B3::Type type, int64_t value)
{
    switch (type.kind()) {
    case Int32:
        return m_block->appendNew<Const32Value>(m_proc, m_origin, static_cast<int32_t>(value));
    case Int64:
        return m_block->appendNew<Const64Value>(m_proc, m_origin, value);
    case Float:
        return m_block->appendNew<ConstFloatValue>(m_proc, m_origin, std::bit_cast<float>(static_cast<uint32_t>(value)));
    case Double:
        return m_block->appendNew<ConstDoubleValue>(m_proc, m_origin, std::bit_cast<double>(value));
    case V128:
        return m_block->appendNew<Const128Value>(m_proc, m_origin, v128_t { static_cast<uint64_t>(value), static_cast<uint64_t>(value) });
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

BasicBlock* BIRToB3::newBlock()
{
    return m_blockFactory ? m_blockFactory() : m_proc.addBlock();
}

Value* BIRToB3::pointer(const void* address)
{
    return m_block->appendNew<ConstPtrValue>(m_proc, m_origin, address);
}

void BIRToB3::lowerFunction(unsigned functionIndex)
{
    m_inlineStack.append(functionIndex);
    const BIR::Function& function = m_module.functions[functionIndex];
    const BIR::Signature& signature = m_module.signatures[function.signature];

    BasicBlock* entry = m_proc.addBlock();
    m_block = entry;
    Vector<Value*> arguments;
    Value* framePointer = nullptr;
    auto stackArgumentAddress = [&](unsigned stackOffset) -> int32_t {
        if (!framePointer)
            framePointer = entry->appendNew<Value>(m_proc, FramePointer, m_origin);
        return static_cast<int32_t>(savedFrameAndReturnAddressBytes + stackOffset);
    };
    ArgumentLayout layout = layoutArguments(signature);
    for (unsigned i = 0; i < layout.locations.size(); ++i) {
        B3::Type type = toB3(signature.parameters[i].type);
        const ArgumentLocation& location = layout.locations[i];
        Value* argument = nullptr;
        switch (location.kind) {
        case ArgumentLocation::Kind::GPR:
            argument = entry->appendNew<ArgumentRegValue>(m_proc, m_origin, location.reg.gpr());
            if (type == Int32)
                argument = entry->appendNew<Value>(m_proc, B3::Trunc, m_origin, argument);
            break;
        case ArgumentLocation::Kind::FPR:
            if (type == V128) {
                argument = entry->appendNew<ArgumentRegValue>(m_proc, m_origin, location.reg.fpr(), ArgumentRegValue::UsesVectorArgs);
                break;
            }
            argument = entry->appendNew<ArgumentRegValue>(m_proc, m_origin, location.reg.fpr());
            if (type == Float)
                argument = entry->appendNew<Value>(m_proc, B3::Trunc, m_origin, argument);
            break;
        case ArgumentLocation::Kind::Stack: {
            int32_t offset = stackArgumentAddress(location.stackOffset);
            argument = entry->appendNew<MemoryValue>(m_proc, Load, type, m_origin, framePointer, offset);
            break;
        }
        case ArgumentLocation::Kind::StackBytes: {
            // The caller's copy in its outgoing arguments is this function's object.
            int32_t offset = stackArgumentAddress(location.stackOffset);
            argument = entry->appendNew<Value>(m_proc, B3::Add, m_origin, framePointer, entry->appendNew<Const64Value>(m_proc, m_origin, offset));
            break;
        }
        }
        arguments.append(argument);
    }
    if (signature.isVariadic)
        emitVariadicEntry(signature, entry);
    emitBody(function, entry, arguments.span(), nullptr);
}

BIRToB3::Inlined BIRToB3::lowerInline(unsigned functionIndex, BasicBlock* block, std::span<Value* const> arguments, Origin origin)
{
    const BIR::Function& function = m_module.functions[functionIndex];
    const BIR::Signature& signature = m_module.signatures[function.signature];
    RELEASE_ASSERT(arguments.size() == signature.parameters.size());
    m_origin = origin;
    if (m_inlineStack.isEmpty())
        m_inlineStack.append(functionIndex);

    // A by-value aggregate is the callee's own object: give the inlined body a private copy.
    Vector<Value*> privateArguments;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        const BIR::Parameter& parameter = signature.parameters[i];
        if (parameter.kind != BIR::ParamKind::ByValStack) {
            privateArguments.append(arguments[i]);
            continue;
        }
        m_block = block;
        Value* copy = block->appendNew<SlotBaseValue>(m_proc, m_origin, m_proc.addStackSlot(parameter.size));
        block->appendNew<CCallValue>(m_proc, Int64, m_origin, pointer(cFunctionPointer(copyMemory)), copy, arguments[i], constant(Int64, static_cast<int64_t>(parameter.size)));
        privateArguments.append(copy);
    }

    ReturnTarget target;
    target.continuation = newBlock();
    for (BIR::Type result : signature.results)
        target.results.append(m_proc.addVariable(toB3(result)));
    emitBody(function, block, privateArguments.span(), &target);

    Inlined inlined;
    inlined.continuation = target.continuation;
    for (Variable* variable : target.results)
        inlined.results.append(target.continuation->appendNew<VariableValue>(m_proc, B3::Get, m_origin, variable));
    if (!inlined.results.isEmpty())
        inlined.result = inlined.results[0];
    return inlined;
}

void BIRToB3::emitBody(const BIR::Function& function, BasicBlock* entry, std::span<Value* const> arguments, const ReturnTarget* returnTarget)
{
    m_body.returnTarget = returnTarget;
    m_body.values.fill(nullptr, function.valueCount);
    for (unsigned i = 0; i < arguments.size(); ++i)
        m_body.values[i] = arguments[i];

    m_block = entry;
    m_body.locals.shrink(0);
    for (BIR::Type type : function.locals) {
        Variable* variable = m_proc.addVariable(toB3(type));
        entry->appendNew<VariableValue>(m_proc, B3::Set, m_origin, variable, constant(toB3(type), 0));
        m_body.locals.append(variable);
    }

    m_body.slots.shrink(0);
    for (const BIR::Slot& slot : function.slots) {
        // A B3 stack slot is aligned to its size, up to 16. One that asks for more than its size gets
        // padded to it; one that asks for more than 16 gets room to round its address up (SlotAddr).
        uint64_t size = std::max<uint64_t>(slot.size, 1);
        if (slot.alignment <= 16)
            size = std::max(size, slot.alignment);
        else
            size = roundUpToMultipleOf<16>(size) + slot.alignment;
        m_body.slots.append(m_proc.addStackSlot(size));
    }

    m_body.blocks.shrink(0);
    for (unsigned i = 0; i < function.blocks.size(); ++i)
        m_body.blocks.append(newBlock());
    entry->appendNewControlValue(m_proc, Jump, m_origin, FrequentedBlock(m_body.blocks[0]));

    for (unsigned i = 0; i < function.blocks.size(); ++i) {
        m_block = m_body.blocks[i];
        const BIR::Block& block = function.blocks[i];
        for (unsigned j = 0; j < block.instCount; ++j)
            emitInst(function, function.insts[block.firstInst + j]);
    }
}

Value* BIRToB3::emitFloatToInt(BIR::Type resultType, Value* value, bool isSigned)
{
    bool fromDouble = value->type() == Double;
    if (!isSigned && resultType == BIR::Type::I64) {
        if (!fromDouble)
            value = m_block->appendNew<Value>(m_proc, FloatToDouble, m_origin, value);
        return m_block->appendNew<CCallValue>(m_proc, Int64, m_origin, Effects::none(), pointer(cFunctionPointer(convertDoubleToUInt64)), value);
    }
    // An unsigned 32-bit result is the low half of the signed 64-bit truncation.
    bool wide = resultType == BIR::Type::I64 || !isSigned;
    PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, wide ? Int64 : Int32, m_origin);
    patchpoint->append(value, ValueRep::SomeRegister);
    patchpoint->effects = Effects::none();
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        if (fromDouble) {
            if (wide)
                jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
            else
                jit.truncateDoubleToInt32(params[1].fpr(), params[0].gpr());
        } else {
            if (wide)
                jit.truncateFloatToInt64(params[1].fpr(), params[0].gpr());
            else
                jit.truncateFloatToInt32(params[1].fpr(), params[0].gpr());
        }
    });
    if (wide && resultType == BIR::Type::I32)
        return m_block->appendNew<Value>(m_proc, B3::Trunc, m_origin, patchpoint);
    return patchpoint;
}

Value* BIRToB3::emitIntToFloat(BIR::Type resultType, Value* value, bool isSigned)
{
    bool toDouble = resultType == BIR::Type::F64;
    if (!isSigned) {
        if (value->type() == Int64) {
            if (toDouble)
                return m_block->appendNew<CCallValue>(m_proc, Double, m_origin, Effects::none(), pointer(cFunctionPointer(convertUInt64ToDouble)), value);
            return m_block->appendNew<CCallValue>(m_proc, Float, m_origin, Effects::none(), pointer(cFunctionPointer(convertUInt64ToFloat)), value);
        }
        value = m_block->appendNew<Value>(m_proc, B3::ZExt32, m_origin, value);
    }
    return m_block->appendNew<Value>(m_proc, toDouble ? IToD : IToF, m_origin, value);
}

void BIRToB3::emitVariadicEntry(const BIR::Signature& signature, BasicBlock* entry)
{
    // The anonymous arguments are wherever the caller's ABI put them: the argument registers
    // after the named ones, then the stack. Spill every argument register now, before anything
    // clobbers it, so va_arg can walk them as memory.
    constexpr NativeCC cc = hostNativeCC();
    auto gprs = integerArgumentRegisters(cc);
    auto fprs = floatArgumentRegisters(cc);

    ArgumentLayout layout = layoutArguments(signature);
    VariadicFrame frame;
    frame.namedArguments = signature.parameters.size();
    frame.namedGPRs = layout.namedGPRs;
    frame.namedFPRs = layout.namedFPRs;
    frame.namedStackBytes = layout.namedStackBytes;

    if (cc == NativeCC::Win64) {
        // Win64 callers leave 32 bytes of home space above the return address for exactly this.
        Value* framePointer = entry->appendNew<Value>(m_proc, FramePointer, m_origin);
        for (unsigned i = 0; i < gprs.size(); ++i) {
            Value* argument = entry->appendNew<ArgumentRegValue>(m_proc, m_origin, gprs[i]);
            entry->appendNew<MemoryValue>(m_proc, B3::Store, m_origin, argument, framePointer, static_cast<int32_t>(savedFrameAndReturnAddressBytes + i * 8));
        }
    } else if (!(cc == NativeCC::AAPCS64 && stackPackingForNativeCC(cc) == StackPacking::Natural)) {
        // SysV: six GPRs then eight 16-byte vector slots. AAPCS64: eight GPRs then eight 16-byte slots.
        unsigned gprBytes = gprs.size() * 8;
        frame.registerSaveArea = m_proc.addStackSlot(gprBytes + fprs.size() * 16);
        Value* base = entry->appendNew<SlotBaseValue>(m_proc, m_origin, frame.registerSaveArea);
        for (unsigned i = 0; i < gprs.size(); ++i) {
            Value* argument = entry->appendNew<ArgumentRegValue>(m_proc, m_origin, gprs[i]);
            entry->appendNew<MemoryValue>(m_proc, B3::Store, m_origin, argument, base, static_cast<int32_t>(i * 8));
        }
        for (unsigned i = 0; i < fprs.size(); ++i) {
            Value* argument = entry->appendNew<ArgumentRegValue>(m_proc, m_origin, fprs[i]);
            entry->appendNew<MemoryValue>(m_proc, B3::Store, m_origin, argument, base, static_cast<int32_t>(gprBytes + i * 16));
        }
    }
    m_variadicFrame = frame;
}

void BIRToB3::emitVaStart(Value* vaList)
{
    constexpr NativeCC cc = hostNativeCC();
    const VariadicFrame& frame = m_variadicFrame;
    auto store32 = [&](int32_t value, int32_t offset) {
        m_block->appendNew<MemoryValue>(m_proc, B3::Store, m_origin, constant(Int32, value), vaList, offset);
    };
    auto store64 = [&](Value* value, int32_t offset) {
        m_block->appendNew<MemoryValue>(m_proc, B3::Store, m_origin, value, vaList, offset);
    };
    auto frameAddress = [&](unsigned offset) -> Value* {
        Value* framePointer = m_block->appendNew<Value>(m_proc, FramePointer, m_origin);
        return m_block->appendNew<Value>(m_proc, B3::Add, m_origin, framePointer, constant(Int64, offset));
    };
    if (cc == NativeCC::Win64) {
        store64(frameAddress(savedFrameAndReturnAddressBytes + frame.namedArguments * 8), 0);
        return;
    }
    if (!frame.registerSaveArea) {
        // Apple arm64: every anonymous argument is on the stack.
        store64(frameAddress(savedFrameAndReturnAddressBytes + roundUpToMultipleOf<8>(frame.namedStackBytes)), 0);
        return;
    }
    Value* saveArea = m_block->appendNew<SlotBaseValue>(m_proc, m_origin, frame.registerSaveArea);
    Value* stackArguments = frameAddress(savedFrameAndReturnAddressBytes + frame.namedStackBytes);
    if (cc == NativeCC::SysV64) {
        store32(frame.namedGPRs * 8, 0);
        store32(48 + frame.namedFPRs * 16, 4);
        store64(stackArguments, 8);
        store64(saveArea, 16);
        return;
    }
    // AAPCS64: the offsets are negative distances from the end of each save area.
    store64(stackArguments, 0);
    store64(m_block->appendNew<Value>(m_proc, B3::Add, m_origin, saveArea, constant(Int64, 64)), 8);
    store64(m_block->appendNew<Value>(m_proc, B3::Add, m_origin, saveArea, constant(Int64, 64 + 128)), 16);
    store32(-static_cast<int32_t>((8 - frame.namedGPRs) * 8), 24);
    store32(-static_cast<int32_t>((8 - frame.namedFPRs) * 16), 28);
}

namespace {

SIMDLane simdLane(BIR::Lane lane) { return static_cast<SIMDLane>(static_cast<uint8_t>(lane) + 1); }
bool isIntegerLane(BIR::Lane lane) { return lane <= BIR::Lane::I64x2; }
unsigned laneCount(BIR::Lane lane)
{
    switch (lane) {
    case BIR::Lane::I8x16:
        return 16;
    case BIR::Lane::I16x8:
        return 8;
    case BIR::Lane::I32x4:
    case BIR::Lane::F32x4:
        return 4;
    case BIR::Lane::I64x2:
    case BIR::Lane::F64x2:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
B3::Type laneScalarType(BIR::Lane lane)
{
    switch (lane) {
    case BIR::Lane::I8x16:
    case BIR::Lane::I16x8:
    case BIR::Lane::I32x4:
        return Int32;
    case BIR::Lane::I64x2:
        return Int64;
    case BIR::Lane::F32x4:
        return Float;
    case BIR::Lane::F64x2:
        return Double;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // anonymous namespace

// For the lane shapes no instruction set operates on (8-bit multiply, integer divide, 64-bit
// min/max and unsigned compares): take the vectors apart, do it on scalars, put it back together.
Value* BIRToB3::emitLaneWise(BIR::Lane lane, bool isSigned, Value* a, Value* b, const Function<Value*(Value*, Value*)>& operation)
{
    SIMDLane b3Lane = simdLane(lane);
    B3::Type scalarType = laneScalarType(lane);
    bool isNarrow = lane == BIR::Lane::I8x16 || lane == BIR::Lane::I16x8;
    SIMDSignMode extractSign = isNarrow ? (isSigned ? SIMDSignMode::Signed : SIMDSignMode::Unsigned) : SIMDSignMode::None;
    Value* result = a;
    for (unsigned i = 0; i < laneCount(lane); ++i) {
        uint8_t index = static_cast<uint8_t>(i);
        Value* left = m_block->appendNew<SIMDValue>(m_proc, m_origin, VectorExtractLane, scalarType, b3Lane, extractSign, index, a);
        Value* right = b ? m_block->appendNew<SIMDValue>(m_proc, m_origin, VectorExtractLane, scalarType, b3Lane, extractSign, index, b) : nullptr;
        result = m_block->appendNew<SIMDValue>(m_proc, m_origin, VectorReplaceLane, V128, b3Lane, SIMDSignMode::None, index, result, operation(left, right));
    }
    return result;
}

Value* BIRToB3::emitVectorConversion(BIR::VConvertKind kind, Value* value)
{
    auto convert = [&](B3::Opcode opcode, SIMDLane lane, SIMDSignMode signMode) -> Value* {
        return m_block->appendNew<SIMDValue>(m_proc, m_origin, opcode, V128, lane, signMode, value);
    };
    using K = BIR::VConvertKind;
    switch (kind) {
    case K::I32x4ToF32x4S:
        return convert(VectorConvert, SIMDLane::i32x4, SIMDSignMode::Signed);
    case K::I32x4ToF32x4U:
        return convert(VectorConvert, SIMDLane::i32x4, SIMDSignMode::Unsigned);
    case K::F32x4ToI32x4S:
        return convert(VectorTruncSat, SIMDLane::f32x4, SIMDSignMode::Signed);
    case K::F32x4ToI32x4U:
        return convert(VectorTruncSat, SIMDLane::f32x4, SIMDSignMode::Unsigned);
    case K::I32x4LowToF64x2S:
        return convert(VectorConvertLow, SIMDLane::i32x4, SIMDSignMode::Signed);
    case K::I32x4LowToF64x2U:
        return convert(VectorConvertLow, SIMDLane::i32x4, SIMDSignMode::Unsigned);
    case K::F64x2ToI32x4ZeroS:
        return convert(VectorTruncSat, SIMDLane::f64x2, SIMDSignMode::Signed);
    case K::F64x2ToI32x4ZeroU:
        return convert(VectorTruncSat, SIMDLane::f64x2, SIMDSignMode::Unsigned);
    case K::F32x4LowToF64x2:
        return convert(VectorPromote, SIMDLane::f32x4, SIMDSignMode::None);
    case K::F64x2ToF32x4Zero:
        return convert(VectorDemote, SIMDLane::f64x2, SIMDSignMode::None);
    case K::I8x16LowToI16x8S:
        return convert(VectorExtendLow, SIMDLane::i16x8, SIMDSignMode::Signed);
    case K::I8x16LowToI16x8U:
        return convert(VectorExtendLow, SIMDLane::i16x8, SIMDSignMode::Unsigned);
    case K::I8x16HighToI16x8S:
        return convert(VectorExtendHigh, SIMDLane::i16x8, SIMDSignMode::Signed);
    case K::I8x16HighToI16x8U:
        return convert(VectorExtendHigh, SIMDLane::i16x8, SIMDSignMode::Unsigned);
    case K::I16x8LowToI32x4S:
        return convert(VectorExtendLow, SIMDLane::i32x4, SIMDSignMode::Signed);
    case K::I16x8LowToI32x4U:
        return convert(VectorExtendLow, SIMDLane::i32x4, SIMDSignMode::Unsigned);
    case K::I16x8HighToI32x4S:
        return convert(VectorExtendHigh, SIMDLane::i32x4, SIMDSignMode::Signed);
    case K::I16x8HighToI32x4U:
        return convert(VectorExtendHigh, SIMDLane::i32x4, SIMDSignMode::Unsigned);
    case K::I32x4LowToI64x2S:
        return convert(VectorExtendLow, SIMDLane::i64x2, SIMDSignMode::Signed);
    case K::I32x4LowToI64x2U:
        return convert(VectorExtendLow, SIMDLane::i64x2, SIMDSignMode::Unsigned);
    case K::I32x4HighToI64x2S:
        return convert(VectorExtendHigh, SIMDLane::i64x2, SIMDSignMode::Signed);
    case K::I32x4HighToI64x2U:
        return convert(VectorExtendHigh, SIMDLane::i64x2, SIMDSignMode::Unsigned);
    case K::I64x2ToF64x2S:
    case K::I64x2ToF64x2U:
    case K::F64x2ToI64x2S:
    case K::F64x2ToI64x2U: {
        // No instruction set below AVX-512 converts 64-bit integer lanes; do the two lanes as scalars.
        bool toFloat = kind == K::I64x2ToF64x2S || kind == K::I64x2ToF64x2U;
        bool isSigned = kind == K::I64x2ToF64x2S || kind == K::F64x2ToI64x2S;
        SIMDLane from = toFloat ? SIMDLane::i64x2 : SIMDLane::f64x2;
        SIMDLane to = toFloat ? SIMDLane::f64x2 : SIMDLane::i64x2;
        Value* result = m_block->appendNew<Const128Value>(m_proc, m_origin, v128_t { 0, 0 });
        for (uint8_t i = 0; i < 2; ++i) {
            Value* scalar = m_block->appendNew<SIMDValue>(m_proc, m_origin, VectorExtractLane, toFloat ? Int64 : Double, from, SIMDSignMode::None, i, value);
            Value* converted = toFloat ? emitIntToFloat(BIR::Type::F64, scalar, isSigned) : emitFloatToInt(BIR::Type::I64, scalar, isSigned);
            result = m_block->appendNew<SIMDValue>(m_proc, m_origin, VectorReplaceLane, V128, to, SIMDSignMode::None, i, result, converted);
        }
        return result;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value* BIRToB3::emitVectorOperation(const BIR::Function& function, const BIR::Inst& inst)
{
    BIR::Lane lane = static_cast<BIR::Lane>(inst.aux);
    SIMDLane b3Lane = inst.aux <= static_cast<uint8_t>(BIR::Lane::F64x2) ? simdLane(lane) : SIMDLane::v128;
    bool isSigned = inst.imm & 1;
    uint8_t index = static_cast<uint8_t>(inst.imm >> 8);
    SIMDSignMode signMode = isSigned ? SIMDSignMode::Signed : SIMDSignMode::Unsigned;
    bool isNarrow = lane == BIR::Lane::I8x16 || lane == BIR::Lane::I16x8;
    Value* a = m_body.values[inst.a];
    auto b = [&] { return m_body.values[inst.b]; };
    auto simd = [&](B3::Opcode opcode, B3::Type type, SIMDLane simdLane, SIMDSignMode mode, auto... children) -> Value* {
        return m_block->appendNew<SIMDValue>(m_proc, m_origin, opcode, type, simdLane, mode, children...);
    };
    auto bytes = [&] {
        return v128_t { static_cast<uint64_t>(function.extra[inst.extraOffset]), static_cast<uint64_t>(function.extra[inst.extraOffset + 1]) };
    };
    auto scalar = [&](B3::Opcode opcode) {
        return emitLaneWise(lane, isSigned, a, b(), [&](Value* left, Value* right) -> Value* {
            Value* result = m_block->appendNew<Value>(m_proc, opcode, m_origin, left, right);
            return result;
        });
    };
    auto scalarCompare = [&](B3::Opcode opcode) {
        return emitLaneWise(lane, isSigned, a, b(), [&](Value* left, Value* right) -> Value* {
            Value* condition = m_block->appendNew<Value>(m_proc, opcode, m_origin, left, right);
            B3::Type type = left->type();
            return m_block->appendNew<Value>(m_proc, B3::Select, m_origin, condition, constant(type, -1), constant(type, 0));
        });
    };

    switch (inst.op) {
    case Op::ConstV128:
        return m_block->appendNew<Const128Value>(m_proc, m_origin, bytes());
    case Op::VSplat:
        return simd(VectorSplat, V128, b3Lane, SIMDSignMode::None, a);
    case Op::VExtract:
        return simd(VectorExtractLane, laneScalarType(lane), b3Lane, isNarrow ? signMode : SIMDSignMode::None, index, a);
    case Op::VReplace:
        return simd(VectorReplaceLane, V128, b3Lane, SIMDSignMode::None, index, a, b());
    case Op::VAdd:
        return simd(VectorAdd, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VSub:
        return simd(VectorSub, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VMul:
        if (lane == BIR::Lane::I8x16)
            return scalar(B3::Mul);
        return simd(VectorMul, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VDiv:
        if (isIntegerLane(lane))
            return scalar(isSigned ? B3::Div : B3::UDiv);
        return simd(VectorDiv, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VRem:
        return scalar(isSigned ? B3::Mod : B3::UMod);
    case Op::VMin:
    case Op::VMax: {
        bool isMin = inst.op == Op::VMin;
        if (!isIntegerLane(lane))
            return simd(isMin ? VectorPmin : VectorPmax, V128, b3Lane, SIMDSignMode::None, a, b());
        if (lane == BIR::Lane::I64x2) {
            B3::Opcode compare = isSigned ? (isMin ? B3::LessThan : B3::GreaterThan) : (isMin ? B3::Below : B3::Above);
            return emitLaneWise(lane, isSigned, a, b(), [&](Value* left, Value* right) -> Value* {
                return m_block->appendNew<Value>(m_proc, B3::Select, m_origin, m_block->appendNew<Value>(m_proc, compare, m_origin, left, right), left, right);
            });
        }
        return simd(isMin ? VectorMin : VectorMax, V128, b3Lane, signMode, a, b());
    }
    case Op::VNeg:
        return simd(VectorNeg, V128, b3Lane, SIMDSignMode::None, a);
    case Op::VAbs:
        return simd(VectorAbs, V128, b3Lane, SIMDSignMode::None, a);
    case Op::VSqrt:
        return simd(VectorSqrt, V128, b3Lane, SIMDSignMode::None, a);
    case Op::VAnd:
        return simd(VectorAnd, V128, SIMDLane::v128, SIMDSignMode::None, a, b());
    case Op::VOr:
        return simd(VectorOr, V128, SIMDLane::v128, SIMDSignMode::None, a, b());
    case Op::VXor:
        return simd(VectorXor, V128, SIMDLane::v128, SIMDSignMode::None, a, b());
    case Op::VNot:
        return simd(VectorNot, V128, SIMDLane::v128, SIMDSignMode::None, a);
    case Op::VShl:
        return simd(VectorShl, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VShrS:
        return simd(VectorShr, V128, b3Lane, SIMDSignMode::Signed, a, b());
    case Op::VShrU:
        return simd(VectorShr, V128, b3Lane, SIMDSignMode::Unsigned, a, b());
    case Op::VEq:
        return simd(VectorEqual, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VNe:
        return simd(VectorNotEqual, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VLt:
    case Op::VLe:
    case Op::VGt:
    case Op::VGe: {
        bool isUnsignedInteger = isIntegerLane(lane) && !isSigned;
        if (isUnsignedInteger && lane == BIR::Lane::I64x2) {
            B3::Opcode opcode = inst.op == Op::VLt ? B3::Below : inst.op == Op::VLe ? B3::BelowEqual : inst.op == Op::VGt ? B3::Above : B3::AboveEqual;
            return scalarCompare(opcode);
        }
        B3::Opcode opcode;
        if (isUnsignedInteger)
            opcode = inst.op == Op::VLt ? VectorBelow : inst.op == Op::VLe ? VectorBelowOrEqual : inst.op == Op::VGt ? VectorAbove : VectorAboveOrEqual;
        else
            opcode = inst.op == Op::VLt ? VectorLessThan : inst.op == Op::VLe ? VectorLessThanOrEqual : inst.op == Op::VGt ? VectorGreaterThan : VectorGreaterThanOrEqual;
        return simd(opcode, V128, b3Lane, isIntegerLane(lane) ? signMode : SIMDSignMode::None, a, b());
    }
    case Op::VSelect:
        return simd(VectorBitwiseSelect, V128, SIMDLane::v128, SIMDSignMode::None, b(), m_body.values[inst.c], a);
    case Op::VShuffle: {
        v128_t pattern = bytes();
        if constexpr (isX86()) {
            // pshufb reads one table and zeroes the byte for an index with its top bit set.
            v128_t left = pattern;
            v128_t right = pattern;
            for (unsigned i = 0; i < 16; ++i) {
                if (left.u8x16[i] > 15)
                    left.u8x16[i] = 0xff;
                if (right.u8x16[i] < 16)
                    right.u8x16[i] = 0xff;
                else
                    right.u8x16[i] -= 16;
            }
            Value* fromA = simd(VectorSwizzle, V128, SIMDLane::i8x16, SIMDSignMode::None, a, m_block->appendNew<Const128Value>(m_proc, m_origin, left));
            Value* fromB = simd(VectorSwizzle, V128, SIMDLane::i8x16, SIMDSignMode::None, b(), m_block->appendNew<Const128Value>(m_proc, m_origin, right));
            return simd(VectorOr, V128, SIMDLane::v128, SIMDSignMode::None, fromA, fromB);
        }
        return simd(VectorSwizzle, V128, SIMDLane::i8x16, SIMDSignMode::None, a, b(), m_block->appendNew<Const128Value>(m_proc, m_origin, pattern));
    }
    case Op::VAddSat:
        return simd(VectorAddSat, V128, b3Lane, signMode, a, b());
    case Op::VSubSat:
        return simd(VectorSubSat, V128, b3Lane, signMode, a, b());
    case Op::VAvgU:
        return simd(VectorAvgRound, V128, b3Lane, SIMDSignMode::None, a, b());
    case Op::VExtMul:
        return simd(index ? VectorMulHigh : VectorMulLow, V128, b3Lane, signMode, a, b());
    case Op::VNarrow:
        return simd(VectorNarrow, V128, b3Lane, signMode, a, b());
    case Op::VDot:
        return simd(VectorDotProduct, V128, SIMDLane::i32x4, SIMDSignMode::None, a, b());
    case Op::VSwizzle: {
        Value* indices = b();
        if constexpr (isX86()) {
            // pshufb only zeroes a byte whose index has its top bit set; adding 0x70 with unsigned
            // saturation sets that bit for every index above 15 and leaves 0..15 selecting the same byte.
            Value* bias = m_block->appendNew<Const128Value>(m_proc, m_origin, v128_t { 0x7070707070707070ull, 0x7070707070707070ull });
            indices = simd(VectorAddSat, V128, SIMDLane::i8x16, SIMDSignMode::Unsigned, bias, indices);
        }
        return simd(VectorSwizzle, V128, SIMDLane::i8x16, SIMDSignMode::None, a, indices);
    }
    case Op::VConvert:
        return emitVectorConversion(static_cast<BIR::VConvertKind>(inst.aux), a);
    case Op::VBitmask:
        return simd(VectorBitmask, Int32, b3Lane, SIMDSignMode::None, a);
    case Op::VAnyTrue:
        return simd(VectorAnyTrue, Int32, SIMDLane::v128, SIMDSignMode::None, a);
    case Op::VAllTrue:
        return simd(VectorAllTrue, Int32, b3Lane, SIMDSignMode::None, a);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Value* BIRToB3::emitAtomic(const BIR::Inst& inst)
{
    BIR::MemKind kind = static_cast<BIR::MemKind>(inst.aux);
    BIR::MemOrder order = static_cast<BIR::MemOrder>(inst.imm & 0xff);
    Width width = kind == BIR::MemKind::I64 ? Width64 : kind == BIR::MemKind::I32 ? Width32 : (kind == BIR::MemKind::I16S || kind == BIR::MemKind::I16U) ? Width16 : Width8;
    B3::Type valueType = width == Width64 ? Int64 : Int32;
    // B3's atomic read-modify-write operations are all sequentially consistent, which is at least
    // as strong as any order C can ask for.
    switch (inst.op) {
    case Op::AtomicLoad: {
        B3::Opcode opcode = B3::Load;
        switch (kind) {
        case BIR::MemKind::I8S:
            opcode = Load8S;
            break;
        case BIR::MemKind::I8U:
            opcode = Load8Z;
            break;
        case BIR::MemKind::I16S:
            opcode = Load16S;
            break;
        case BIR::MemKind::I16U:
            opcode = Load16Z;
            break;
        default:
            break;
        }
        HeapRange fence = order == BIR::MemOrder::Relaxed ? HeapRange() : HeapRange::top();
        if (opcode == B3::Load)
            return m_block->appendNew<MemoryValue>(m_proc, opcode, valueType, m_origin, m_body.values[inst.a], 0, HeapRange::top(), fence);
        return m_block->appendNew<MemoryValue>(m_proc, opcode, m_origin, m_body.values[inst.a], 0, HeapRange::top(), fence);
    }
    case Op::AtomicStore: {
        Value* stored = m_body.values[inst.a];
        Value* address = m_body.values[inst.b];
        if (order == BIR::MemOrder::SequentiallyConsistent) {
            m_block->appendNew<AtomicValue>(m_proc, AtomicXchg, m_origin, width, stored, address);
            return nullptr;
        }
        B3::Opcode opcode = width == Width8 ? Store8 : width == Width16 ? Store16 : B3::Store;
        HeapRange fence = order == BIR::MemOrder::Relaxed ? HeapRange() : HeapRange::top();
        m_block->appendNew<MemoryValue>(m_proc, opcode, m_origin, stored, address, 0, HeapRange::top(), fence);
        return nullptr;
    }
    case Op::AtomicRmw: {
        B3::Opcode opcode;
        switch (static_cast<BIR::AtomicOp>((inst.imm >> 16) & 0xff)) {
        case BIR::AtomicOp::Add:
            opcode = AtomicXchgAdd;
            break;
        case BIR::AtomicOp::Sub:
            opcode = AtomicXchgSub;
            break;
        case BIR::AtomicOp::And:
            opcode = AtomicXchgAnd;
            break;
        case BIR::AtomicOp::Or:
            opcode = AtomicXchgOr;
            break;
        case BIR::AtomicOp::Xor:
            opcode = AtomicXchgXor;
            break;
        case BIR::AtomicOp::Exchange:
            opcode = AtomicXchg;
            break;
        }
        Value* old = m_block->appendNew<AtomicValue>(m_proc, opcode, m_origin, width, m_body.values[inst.a], m_body.values[inst.b]);
        if (width == Width8)
            return m_block->appendNew<Value>(m_proc, BitAnd, m_origin, old, constant(Int32, 0xff));
        if (width == Width16)
            return m_block->appendNew<Value>(m_proc, BitAnd, m_origin, old, constant(Int32, 0xffff));
        return old;
    }
    case Op::AtomicCas: {
        Value* old = m_block->appendNew<AtomicValue>(m_proc, AtomicStrongCAS, m_origin, width, m_body.values[inst.a], m_body.values[inst.b], m_body.values[inst.c]);
        if (width == Width8)
            return m_block->appendNew<Value>(m_proc, BitAnd, m_origin, old, constant(Int32, 0xff));
        if (width == Width16)
            return m_block->appendNew<Value>(m_proc, BitAnd, m_origin, old, constant(Int32, 0xffff));
        return old;
    }
    case Op::Fence:
        if (order != BIR::MemOrder::Relaxed)
            m_block->appendNew<FenceValue>(m_proc, m_origin);
        return nullptr;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// A struct assignment, or the `memcpy(&value, pointer, sizeof(value))` that portable C reads unaligned
// memory with, is a handful of bytes: a call into libc costs more than the copy. Both targets allow
// unaligned accesses.
static constexpr uint64_t maximumInlineMemoryOperationSize = 64;

template<typename Functor>
static void forEachChunk(uint64_t size, const Functor& functor)
{
    uint64_t offset = 0;
    for (uint64_t width : { 8, 4, 2, 1 }) {
        while (size - offset >= width) {
            functor(offset, width);
            offset += width;
        }
    }
}

bool BIRToB3::emitSmallMemoryCopy(Value* destination, Value* source, Value* size)
{
    if (!size->hasInt64() || static_cast<uint64_t>(size->asInt64()) > maximumInlineMemoryOperationSize)
        return false;
    // Every load before any store: the ranges may overlap (this is memmove).
    Vector<Value*, 8> loaded;
    forEachChunk(size->asInt64(), [&](uint64_t offset, uint64_t width) {
        B3::Opcode opcode = width == 1 ? Load8Z : width == 2 ? Load16Z : Load;
        loaded.append(m_block->appendNew<MemoryValue>(m_proc, opcode, width == 8 ? Int64 : Int32, m_origin, source, static_cast<int32_t>(offset)));
    });
    unsigned index = 0;
    forEachChunk(size->asInt64(), [&](uint64_t offset, uint64_t width) {
        B3::Opcode opcode = width == 1 ? Store8 : width == 2 ? Store16 : Store;
        m_block->appendNew<MemoryValue>(m_proc, opcode, m_origin, loaded[index++], destination, static_cast<int32_t>(offset));
    });
    return true;
}

bool BIRToB3::emitSmallMemoryFill(Value* destination, Value* byte, Value* size)
{
    if (!size->hasInt64() || static_cast<uint64_t>(size->asInt64()) > maximumInlineMemoryOperationSize)
        return false;
    if (!size->asInt64())
        return true;
    // The byte in every position of a 64-bit value; narrower stores use its low bits.
    Value* low = m_block->appendNew<Value>(m_proc, B3::BitAnd, m_origin, byte, constant(Int32, 0xff));
    Value* pattern = m_block->appendNew<Value>(m_proc, B3::Mul, m_origin, m_block->appendNew<Value>(m_proc, B3::ZExt32, m_origin, low), constant(Int64, 0x0101010101010101ll));
    Value* pattern32 = m_block->appendNew<Value>(m_proc, B3::Trunc, m_origin, pattern);
    forEachChunk(size->asInt64(), [&](uint64_t offset, uint64_t width) {
        B3::Opcode opcode = width == 1 ? Store8 : width == 2 ? Store16 : Store;
        m_block->appendNew<MemoryValue>(m_proc, opcode, m_origin, width == 8 ? pattern : pattern32, destination, static_cast<int32_t>(offset));
    });
    return true;
}

void BIRToB3::emitInlineAssembly(const BIR::Function& function, const BIR::Inst& inst)
{
    // The frontend chose a register for every operand and assembled the code for that choice, so all
    // that is left is to have the values there and to say what the code overwrites.
    auto extra = function.extra.span().subspan(inst.extraOffset, inst.extraCount);
    size_t cursor = 0;
    bool hasEffects = extra[cursor++] & 1;
    size_t byteCount = extra[cursor++];
    Vector<uint8_t> code;
    for (size_t i = 0; i < byteCount; ++i)
        code.append(static_cast<uint8_t>(extra[cursor++]));

    auto toReg = [](uint8_t reg) -> Reg {
#if CPU(X86_64)
        if (reg < 16)
            return Reg(static_cast<GPRReg>(reg));
        return Reg(static_cast<FPRReg>(reg - 16));
#else
        if (reg < 32)
            return Reg(static_cast<GPRReg>(reg));
        return Reg(static_cast<FPRReg>(reg - 32));
#endif
    };

    size_t inputCount = extra[cursor++];
    Vector<std::pair<Value*, Reg>> inputs;
    for (size_t i = 0; i < inputCount; ++i) {
        Value* input = m_body.values[static_cast<uint32_t>(extra[cursor])];
        inputs.append({ input, toReg(static_cast<uint8_t>(extra[cursor + 1])) });
        cursor += 2;
    }
    size_t outputCount = extra[cursor++];
    Vector<B3::Type> outputTypes;
    Vector<ValueRep, 1> outputConstraints;
    for (size_t i = 0; i < outputCount; ++i) {
        outputTypes.append(toB3(static_cast<BIR::Type>(extra[cursor])));
        outputConstraints.append(ValueRep::reg(toReg(static_cast<uint8_t>(extra[cursor + 1]))));
        cursor += 2;
    }
    size_t clobberCount = extra[cursor++];
    RegisterSet clobbered;
    for (size_t i = 0; i < clobberCount; ++i)
    {
        Reg reg = toReg(static_cast<uint8_t>(extra[cursor++]));
        // A vector register is clobbered in full.
        if (reg.isFPR())
            clobbered.add(reg, conservativeWidth(reg));
        else
            clobbered.add(reg, IgnoreVectors);
    }

    B3::Type type = outputTypes.isEmpty() ? B3::Type(Void) : outputTypes.size() == 1 ? outputTypes[0] : m_proc.addTuple(Vector<B3::Type>(outputTypes));
    PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, type, m_origin);
    patchpoint->effects = hasEffects ? Effects::forCall() : Effects::none();
    patchpoint->effects.controlDependent = true;
    for (auto& [input, reg] : inputs)
        patchpoint->append(input, ValueRep::reg(reg));
    if (!outputConstraints.isEmpty())
        patchpoint->resultConstraints = WTF::move(outputConstraints);
    patchpoint->clobberLate(clobbered);
    patchpoint->setGenerator([code = WTF::move(code)](CCallHelpers& jit, const StackmapGenerationParams&) {
#if CPU(ARM64)
        // Whole instructions: the decoder refused anything else.
        for (size_t i = 0; i + 4 <= code.size(); i += 4) {
            uint32_t instruction;
            memcpy(&instruction, code.span().data() + i, 4);
            jit.m_assembler.buffer().putInt(static_cast<int32_t>(instruction));
        }
#else
        for (uint8_t byte : code)
            jit.m_assembler.buffer().putByte(static_cast<int8_t>(byte));
#endif
    });
    if (outputTypes.size() == 1)
        m_body.values[inst.result] = patchpoint;
    else {
        for (unsigned i = 0; i < outputTypes.size(); ++i)
            m_body.values[inst.result + i] = m_block->appendNew<ExtractValue>(m_proc, m_origin, outputTypes[i], patchpoint, i);
    }
}

Value* BIRToB3::emitVolatileAccess(const BIR::Inst& inst)
{
    // B3 has no volatile memory operation (a fenced store is an xchg on x86). A patchpoint that
    // claims to read and write everything is opaque to it: B3 will not merge it with another access,
    // forward a value through it, move it past another memory operation, or drop it.
    BIR::MemKind kind = static_cast<BIR::MemKind>(inst.aux);
    int32_t offset = static_cast<int32_t>(inst.imm);
    bool isLoad = inst.op == Op::Load;
    PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, isLoad ? toB3(inst.resultType) : B3::Type(Void), m_origin);
    patchpoint->effects = Effects::forCall();
    if (isLoad)
        patchpoint->resultConstraints = { ValueRep::SomeEarlyRegister };
    else
        patchpoint->append(m_body.values[inst.a], ValueRep::SomeRegister);
    patchpoint->append(m_body.values[isLoad ? inst.a : inst.b], ValueRep::SomeRegister);
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Address address(params[1].gpr(), offset);
        if (isLoad) {
            switch (kind) {
            case BIR::MemKind::I8S:
                jit.load8SignedExtendTo32(address, params[0].gpr());
                break;
            case BIR::MemKind::I8U:
                jit.load8(address, params[0].gpr());
                break;
            case BIR::MemKind::I16S:
                jit.load16SignedExtendTo32(address, params[0].gpr());
                break;
            case BIR::MemKind::I16U:
                jit.load16(address, params[0].gpr());
                break;
            case BIR::MemKind::I32:
                jit.load32(address, params[0].gpr());
                break;
            case BIR::MemKind::I64:
                jit.load64(address, params[0].gpr());
                break;
            case BIR::MemKind::F32:
                jit.loadFloat(address, params[0].fpr());
                break;
            case BIR::MemKind::F64:
                jit.loadDouble(address, params[0].fpr());
                break;
            case BIR::MemKind::V128:
                jit.loadVector(address, params[0].fpr());
                break;
            }
            return;
        }
        switch (kind) {
        case BIR::MemKind::I8S:
        case BIR::MemKind::I8U:
            jit.store8(params[0].gpr(), address);
            break;
        case BIR::MemKind::I16S:
        case BIR::MemKind::I16U:
            jit.store16(params[0].gpr(), address);
            break;
        case BIR::MemKind::I32:
            jit.store32(params[0].gpr(), address);
            break;
        case BIR::MemKind::I64:
            jit.store64(params[0].gpr(), address);
            break;
        case BIR::MemKind::F32:
            jit.storeFloat(params[0].fpr(), address);
            break;
        case BIR::MemKind::F64:
            jit.storeDouble(params[0].fpr(), address);
            break;
        case BIR::MemKind::V128:
            jit.storeVector(params[0].fpr(), address);
            break;
        }
    });
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    return patchpoint;
}

Value* BIRToB3::emitStackOperation(const BIR::Inst& inst)
{
    // B3 frames have a fixed size; C's alloca moves the stack pointer at run time. That is sound
    // because Air addresses stack slots from the frame pointer (see Code::hasDynamicStackAllocation)
    // and restores the stack pointer from it on the way out. The outgoing call arguments stay at the
    // bottom of the stack, so an allocation starts above them.
    m_proc.code().setHasDynamicStackAllocation();
    GPRReg stackPointer = CCallHelpers::stackPointerRegister;
    switch (inst.op) {
    case Op::StackAlloc: {
        int64_t alignment = std::max<int64_t>(inst.imm, 16);
        Value* bytes = m_block->appendNew<Value>(m_proc, BitAnd, m_origin,
            m_block->appendNew<Value>(m_proc, B3::Add, m_origin, m_body.values[inst.a], constant(Int64, 15)), constant(Int64, -16));
        // Rounding the pointer up (below) moves the block by less than the alignment: reserve that too.
        if (alignment > 16)
            bytes = m_block->appendNew<Value>(m_proc, B3::Add, m_origin, bytes, constant(Int64, alignment));
        PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, Int64, m_origin);
        patchpoint->effects = Effects::forCall();
        patchpoint->resultConstraints = { ValueRep::SomeEarlyRegister };
        patchpoint->append(bytes, ValueRep::SomeRegister);
        patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
            GPRReg result = params[0].gpr();
            jit.move(stackPointer, result);
            jit.subPtr(params[1].gpr(), result);
            jit.andPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(-alignment)), result);
            jit.move(result, stackPointer);
            jit.addPtr(CCallHelpers::TrustedImm32(params.code().callArgAreaSizeInBytes()), result);
        });
        if (alignment <= 16)
            return patchpoint;
        // The outgoing argument area is only 16-byte aligned, so align the pointer itself too.
        return m_block->appendNew<Value>(m_proc, BitAnd, m_origin,
            m_block->appendNew<Value>(m_proc, B3::Add, m_origin, patchpoint, constant(Int64, alignment - 1)), constant(Int64, -alignment));
    }
    case Op::StackSave: {
        PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, Int64, m_origin);
        patchpoint->effects = Effects::forCall();
        patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.move(stackPointer, params[0].gpr());
        });
        return patchpoint;
    }
    case Op::StackRestore: {
        PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, Void, m_origin);
        patchpoint->effects = Effects::forCall();
        patchpoint->append(m_body.values[inst.a], ValueRep::SomeRegister);
        patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.move(params[0].gpr(), stackPointer);
        });
        return nullptr;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Value* BIRToB3::emitBitOperation(BIR::Op op, Value* value)
{
    bool is64 = value->type() == Int64;
    if (op == Op::Clz)
        return m_block->appendNew<Value>(m_proc, B3::Clz, m_origin, value);
#if CPU(X86_64)
    bool hasInstruction = op != Op::Popcnt || MacroAssembler::supportsCountPopulation();
#else
    bool hasInstruction = op != Op::Popcnt;
#endif
    if (!hasInstruction) {
        Value* wide = is64 ? value : m_block->appendNew<Value>(m_proc, B3::ZExt32, m_origin, value);
        Value* result = m_block->appendNew<CCallValue>(m_proc, Int64, m_origin, Effects::none(), pointer(cFunctionPointer(countPopulation)), wide);
        return is64 ? result : m_block->appendNew<Value>(m_proc, B3::Trunc, m_origin, result);
    }
    PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, value->type(), m_origin);
    patchpoint->append(value, ValueRep::SomeRegister);
    patchpoint->effects = Effects::none();
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        GPRReg result = params[0].gpr();
        GPRReg input = params[1].gpr();
        switch (op) {
        case Op::Ctz:
            if (is64)
                jit.countTrailingZeros64(input, result);
            else
                jit.countTrailingZeros32(input, result);
            break;
        case Op::Popcnt:
#if CPU(X86_64)
            if (is64)
                jit.countPopulation64(input, result);
            else
                jit.countPopulation32(input, result);
#endif
            break;
        case Op::Bswap:
            if (is64) {
                jit.move(input, result);
                jit.byteSwap64(result);
            } else {
                jit.move(input, result);
                jit.byteSwap32(result);
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    });
    return patchpoint;
}

Vector<Value*, 1> BIRToB3::emitPatchpointCall(const BIR::Signature& signature, Value* target, const Vector<Value*>& arguments)
{
    // Everything B3's CCall cannot say: a variadic callee (SysV wants the vector register count
    // in al, Win64 wants floating-point arguments duplicated in the integer registers), aggregates
    // copied into the outgoing arguments, x8, and results in more than one register.
    constexpr NativeCC cc = hostNativeCC();
    size_t fixedCount = signature.parameters.size();
    Vector<B3::Type> anonymousTypes;
    for (size_t i = fixedCount; i < arguments.size(); ++i)
        anonymousTypes.append(arguments[i]->type());
    ArgumentLayout layout = layoutArguments(signature, anonymousTypes.span());

    Vector<B3::Type> resultTypes;
    for (BIR::Type result : signature.results)
        resultTypes.append(toB3(result));
#if CPU(X86_64)
    constexpr GPRReg calleeGPR = X86Registers::r10;
    constexpr GPRReg copyGPR = X86Registers::r11;
#else
    constexpr GPRReg calleeGPR = ARM64Registers::x9;
    constexpr GPRReg copyGPR = ARM64Registers::x10;
#endif
    // Everything the call reads is computed first; the patchpoint that uses it comes after.
    Vector<std::pair<Value*, ValueRep>> children;
    children.append({ target, ValueRep::reg(calleeGPR) });

    struct ByValCopy {
        unsigned stackOffset;
        unsigned size;
        unsigned childIndex; // which child holds the source address
    };
    Vector<ByValCopy> copies;
    // An aggregate this small goes to the outgoing arguments eight bytes at a time, as ordinary
    // stack arguments: nothing about it has to be in a register while the call is set up. (Every
    // register-pinned operand of the patchpoint is live at once, and there are only so many.)
    constexpr unsigned maximumByValueSizeInPieces = 64;
    auto appendInPieces = [&](Value* source, unsigned size, unsigned stackOffset) {
        unsigned offset = 0;
        for (; size - offset >= 8; offset += 8) {
            Value* piece = m_block->appendNew<MemoryValue>(m_proc, B3::Load, Int64, m_origin, source, static_cast<int32_t>(offset));
            children.append({ piece, ValueRep::stackArgument(static_cast<int32_t>(stackOffset + offset)) });
        }
        if (offset == size)
            return;
        // The last few bytes, read exactly, fill the low end of one more eight-byte argument slot.
        Value* tail = nullptr;
        unsigned tailBase = offset;
        auto add = [&](B3::Opcode opcode, unsigned width) {
            Value* loaded = m_block->appendNew<MemoryValue>(m_proc, opcode, Int32, m_origin, source, static_cast<int32_t>(offset));
            Value* wide = m_block->appendNew<Value>(m_proc, B3::ZExt32, m_origin, loaded);
            if (unsigned shift = (offset - tailBase) * 8)
                wide = m_block->appendNew<Value>(m_proc, B3::Shl, m_origin, wide, constant(Int32, shift));
            tail = tail ? m_block->appendNew<Value>(m_proc, B3::BitOr, m_origin, tail, wide) : wide;
            offset += width;
        };
        if (size - offset >= 4)
            add(B3::Load, 4);
        if (size - offset >= 2)
            add(Load16Z, 2);
        if (size - offset >= 1)
            add(Load8Z, 1);
        children.append({ tail, ValueRep::stackArgument(static_cast<int32_t>(stackOffset + tailBase)) });
    };
    unsigned usedVectorRegisters = 0;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        Value* argument = arguments[i];
        const ArgumentLocation& location = layout.locations[i];
        switch (location.kind) {
        case ArgumentLocation::Kind::GPR:
            children.append({ argument, ValueRep::reg(location.reg) });
            break;
        case ArgumentLocation::Kind::FPR:
            children.append({ argument, ValueRep::reg(location.reg) });
            ++usedVectorRegisters;
            if (cc == NativeCC::Win64 && signature.isVariadic) {
                Value* bits = argument->type() == Float
                    ? m_block->appendNew<Value>(m_proc, B3::ZExt32, m_origin, m_block->appendNew<Value>(m_proc, BitwiseCast, m_origin, argument))
                    : m_block->appendNew<Value>(m_proc, BitwiseCast, m_origin, argument);
                children.append({ bits, ValueRep::reg(integerArgumentRegisters(cc)[i]) });
            }
            break;
        case ArgumentLocation::Kind::Stack:
            children.append({ argument, ValueRep::stackArgument(static_cast<int32_t>(location.stackOffset)) });
            break;
        case ArgumentLocation::Kind::StackBytes: {
            unsigned size = static_cast<unsigned>(signature.parameters[i].size);
            if (size <= maximumByValueSizeInPieces) {
                appendInPieces(argument, size, location.stackOffset);
                break;
            }
            copies.append({ location.stackOffset, size, static_cast<unsigned>(children.size()) });
            children.append({ argument, ValueRep::SomeRegister });
            break;
        }
        }
    }

    B3::Type patchpointType = resultTypes.isEmpty() ? B3::Type(Void) : resultTypes.size() == 1 ? resultTypes[0] : m_proc.addTuple(Vector<B3::Type>(resultTypes));
    PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, patchpointType, m_origin);
    patchpoint->effects = Effects::forCall();
    patchpoint->clobberEarly(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(m_module.usesVectors ? RegisterSet::allRegisters() : RegisterSet::allScalarRegisters()));
    {
        unsigned nextGPR = 0;
        unsigned nextFPR = 0;
        Vector<ValueRep, 1> constraints;
        for (B3::Type type : resultTypes)
            constraints.append(type.isFloat() || type.isVector() ? ValueRep::reg(floatResultRegisters()[nextFPR++]) : ValueRep::reg(integerResultRegisters()[nextGPR++]));
        // A patchpoint with no result keeps the placeholder constraint B3 gave it.
        if (!constraints.isEmpty())
            patchpoint->resultConstraints = WTF::move(constraints);
    }
    for (auto& [child, constraint] : children)
        patchpoint->append(child, constraint);
    m_proc.requestCallArgAreaSizeInBytes(layout.stackBytes);

    unsigned resultReps = resultTypes.size();
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        for (const ByValCopy& copy : copies) {
            // params[] lists one rep per result, then one per child.
            GPRReg source = params[resultReps + copy.childIndex].gpr();
            for (unsigned offset = 0; offset < copy.size; offset += 8) {
                jit.load64(CCallHelpers::Address(source, offset), copyGPR);
                jit.store64(copyGPR, CCallHelpers::Address(CCallHelpers::stackPointerRegister, copy.stackOffset + offset));
            }
        }
#if CPU(X86_64)
        if (cc == NativeCC::SysV64)
            jit.move(CCallHelpers::TrustedImm32(usedVectorRegisters), X86Registers::eax);
#else
        UNUSED_VARIABLE(usedVectorRegisters);
#endif
        jit.call(calleeGPR, OperationPtrTag);
    });

    Vector<Value*, 1> results;
    if (resultTypes.size() == 1)
        results.append(patchpoint);
    else {
        for (unsigned i = 0; i < resultTypes.size(); ++i)
            results.append(m_block->appendNew<ExtractValue>(m_proc, m_origin, resultTypes[i], patchpoint, i));
    }
    return results;
}

Vector<Value*, 1> BIRToB3::emitCall(const BIR::Function& function, const BIR::Inst& inst, const BIR::Signature& signature, Value* target)
{
    Vector<Value*> arguments;
    for (unsigned i = 0; i < inst.extraCount; ++i)
        arguments.append(m_body.values[static_cast<uint32_t>(function.extra[inst.extraOffset + i])]);
    if (!signature.isScalar() || !b3CallsAreNativeCalls)
        return emitPatchpointCall(signature, target, arguments);
    CCallValue* call = m_block->appendNew<CCallValue>(m_proc, toB3(signature.soleResultOrVoid()), m_origin, target);
    call->appendArgs(arguments);
    return { call };
}

void BIRToB3::emitReturn(const BIR::Function& function, const BIR::Inst& inst)
{
    Vector<Value*, 1> returned;
    for (unsigned i = 0; i < inst.extraCount; ++i)
        returned.append(m_body.values[static_cast<uint32_t>(function.extra[inst.extraOffset + i])]);

    if (m_body.returnTarget) {
        for (unsigned i = 0; i < returned.size(); ++i)
            m_block->appendNew<VariableValue>(m_proc, B3::Set, m_origin, m_body.returnTarget->results[i], returned[i]);
        m_block->appendNewControlValue(m_proc, B3::Jump, m_origin, FrequentedBlock(m_body.returnTarget->continuation));
        return;
    }
    if (returned.size() == 1 && !returned[0]->type().isVector()) {
        m_block->appendNewControlValue(m_proc, Return, m_origin, returned[0]);
        return;
    }
    // Several result registers: pin each value to its register and leave through our own epilogue.
    PatchpointValue* epilogue = m_block->appendNew<PatchpointValue>(m_proc, Void, m_origin);
    epilogue->effects.terminal = true;
    epilogue->clobber(RegisterSet::macroClobberedGPRs());
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (Value* value : returned)
        epilogue->append(value, value->type().isFloat() || value->type().isVector() ? ValueRep::reg(floatResultRegisters()[nextFPR++]) : ValueRep::reg(integerResultRegisters()[nextGPR++]));
    epilogue->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        params.code().emitEpilogue(jit);
    });
}

void BIRToB3::emitInst(const BIR::Function& function, const BIR::Inst& inst)
{
    auto value = [&](uint32_t id) { return m_body.values[id]; };
    auto define = [&](Value* result) { m_body.values[inst.result] = result; };
    auto defineAll = [&](const auto& results) {
        for (unsigned i = 0; i < inst.resultCount; ++i)
            m_body.values[inst.result + i] = results[i];
    };
    auto binary = [&](B3::Opcode opcode) { define(m_block->appendNew<Value>(m_proc, opcode, m_origin, value(inst.a), value(inst.b))); };
    auto unary = [&](B3::Opcode opcode) { define(m_block->appendNew<Value>(m_proc, opcode, m_origin, value(inst.a))); };

    switch (inst.op) {
    case Op::ConstI32:
        define(constant(Int32, inst.imm));
        return;
    case Op::ConstI64:
        define(constant(Int64, inst.imm));
        return;
    case Op::ConstF32:
        define(constant(Float, inst.imm));
        return;
    case Op::ConstF64:
        define(constant(Double, inst.imm));
        return;
    case Op::ConstV128:
    case Op::VSplat:
    case Op::VExtract:
    case Op::VReplace:
    case Op::VAdd:
    case Op::VSub:
    case Op::VMul:
    case Op::VDiv:
    case Op::VRem:
    case Op::VMin:
    case Op::VMax:
    case Op::VNeg:
    case Op::VAbs:
    case Op::VSqrt:
    case Op::VAnd:
    case Op::VOr:
    case Op::VXor:
    case Op::VNot:
    case Op::VShl:
    case Op::VShrS:
    case Op::VShrU:
    case Op::VEq:
    case Op::VNe:
    case Op::VLt:
    case Op::VLe:
    case Op::VGt:
    case Op::VGe:
    case Op::VSelect:
    case Op::VShuffle:
    case Op::VConvert:
    case Op::VBitmask:
    case Op::VAnyTrue:
    case Op::VAllTrue:
    case Op::VAddSat:
    case Op::VSubSat:
    case Op::VAvgU:
    case Op::VExtMul:
    case Op::VNarrow:
    case Op::VDot:
    case Op::VSwizzle:
        define(emitVectorOperation(function, inst));
        return;
    case Op::AtomicLoad:
    case Op::AtomicRmw:
    case Op::AtomicCas:
        define(emitAtomic(inst));
        return;
    case Op::AtomicStore:
    case Op::Fence:
        emitAtomic(inst);
        return;
    case Op::Add:
        return binary(B3::Add);
    case Op::Sub:
        return binary(B3::Sub);
    case Op::Mul:
        return binary(B3::Mul);
    case Op::Div:
        return binary(B3::Div);
    case Op::UDiv:
        return binary(B3::UDiv);
    case Op::Rem:
        return binary(B3::Mod);
    case Op::URem:
        return binary(B3::UMod);
    case Op::And:
        return binary(B3::BitAnd);
    case Op::Or:
        return binary(B3::BitOr);
    case Op::Xor:
        return binary(B3::BitXor);
    case Op::Shl:
        return binary(B3::Shl);
    case Op::ShrS:
        return binary(B3::SShr);
    case Op::ShrU:
        return binary(B3::ZShr);
    case Op::Neg:
        return unary(B3::Neg);
    case Op::RotL:
        return binary(B3::RotL);
    case Op::RotR:
        return binary(B3::RotR);
    case Op::MulHigh:
        return binary(B3::MulHigh);
    case Op::UMulHigh:
        return binary(B3::UMulHigh);
    case Op::Clz:
    case Op::Ctz:
    case Op::Popcnt:
    case Op::Bswap:
        define(emitBitOperation(inst.op, value(inst.a)));
        return;
    case Op::Eq:
        return binary(B3::Equal);
    case Op::Ne:
        return binary(B3::NotEqual);
    case Op::Lt:
        return binary(B3::LessThan);
    case Op::Le:
        return binary(B3::LessEqual);
    case Op::Gt:
        return binary(B3::GreaterThan);
    case Op::Ge:
        return binary(B3::GreaterEqual);
    case Op::ULt:
        return binary(B3::Below);
    case Op::ULe:
        return binary(B3::BelowEqual);
    case Op::UGt:
        return binary(B3::Above);
    case Op::UGe:
        return binary(B3::AboveEqual);
    case Op::SExt8:
        return unary(B3::SExt8);
    case Op::SExt16:
        return unary(B3::SExt16);
    case Op::SExt32:
        return unary(B3::SExt32);
    case Op::ZExt32:
        return unary(B3::ZExt32);
    case Op::Trunc:
        return unary(B3::Trunc);
    case Op::SToF:
        define(emitIntToFloat(inst.resultType, value(inst.a), true));
        return;
    case Op::UToF:
        define(emitIntToFloat(inst.resultType, value(inst.a), false));
        return;
    case Op::FToS:
        define(emitFloatToInt(inst.resultType, value(inst.a), true));
        return;
    case Op::FToU:
        define(emitFloatToInt(inst.resultType, value(inst.a), false));
        return;
    case Op::FPromote:
        return unary(B3::FloatToDouble);
    case Op::FDemote:
        return unary(B3::DoubleToFloat);
    case Op::Bitcast:
        return unary(B3::BitwiseCast);
    case Op::Load: {
        if (inst.isVolatile) {
            define(emitVolatileAccess(inst));
            return;
        }
        Value* address = value(inst.a);
        int32_t offset = static_cast<int32_t>(inst.imm);
        switch (static_cast<BIR::MemKind>(inst.aux)) {
        case BIR::MemKind::I8S:
            define(m_block->appendNew<MemoryValue>(m_proc, Load8S, m_origin, address, offset));
            return;
        case BIR::MemKind::I8U:
            define(m_block->appendNew<MemoryValue>(m_proc, Load8Z, m_origin, address, offset));
            return;
        case BIR::MemKind::I16S:
            define(m_block->appendNew<MemoryValue>(m_proc, Load16S, m_origin, address, offset));
            return;
        case BIR::MemKind::I16U:
            define(m_block->appendNew<MemoryValue>(m_proc, Load16Z, m_origin, address, offset));
            return;
        case BIR::MemKind::I32:
        case BIR::MemKind::I64:
        case BIR::MemKind::F32:
        case BIR::MemKind::F64:
        case BIR::MemKind::V128:
            define(m_block->appendNew<MemoryValue>(m_proc, B3::Load, toB3(inst.resultType), m_origin, address, offset));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case Op::Store: {
        if (inst.isVolatile) {
            emitVolatileAccess(inst);
            return;
        }
        Value* stored = value(inst.a);
        Value* address = value(inst.b);
        int32_t offset = static_cast<int32_t>(inst.imm);
        B3::Opcode opcode = B3::Store;
        switch (static_cast<BIR::MemKind>(inst.aux)) {
        case BIR::MemKind::I8U:
            opcode = Store8;
            break;
        case BIR::MemKind::I16U:
            opcode = Store16;
            break;
        default:
            break;
        }
        m_block->appendNew<MemoryValue>(m_proc, opcode, m_origin, stored, address, offset);
        return;
    }
    case Op::SlotAddr: {
        Value* address = m_block->appendNew<SlotBaseValue>(m_proc, m_origin, m_body.slots[inst.a]);
        if (uint64_t alignment = function.slots[inst.a].alignment; alignment > 16) {
            Value* bumped = m_block->appendNew<Value>(m_proc, B3::Add, m_origin, address, constant(Int64, static_cast<int64_t>(alignment - 1)));
            address = m_block->appendNew<Value>(m_proc, B3::BitAnd, m_origin, bumped, constant(Int64, -static_cast<int64_t>(alignment)));
        }
        define(address);
        return;
    }
    case Op::DataAddr:
        define(pointer(m_environment.dataBase + inst.imm));
        return;
    case Op::InlineAsm:
        emitInlineAssembly(function, inst);
        return;
    case Op::CpuId: {
#if CPU(X86_64)
        PatchpointValue* patchpoint = m_block->appendNew<PatchpointValue>(m_proc, m_proc.addTuple({ Int32, Int32, Int32, Int32 }), m_origin);
        patchpoint->effects = Effects::none();
        patchpoint->effects.controlDependent = true; // A leaf may only exist after an earlier one said so.
        patchpoint->append(value(inst.a), ValueRep::reg(X86Registers::eax));
        patchpoint->append(value(inst.b), ValueRep::reg(X86Registers::ecx));
        patchpoint->resultConstraints = { ValueRep::reg(X86Registers::eax), ValueRep::reg(X86Registers::ebx), ValueRep::reg(X86Registers::ecx), ValueRep::reg(X86Registers::edx) };
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams&) {
            jit.cpuid();
        });
        for (unsigned i = 0; i < 4; ++i)
            m_body.values[inst.result + i] = m_block->appendNew<ExtractValue>(m_proc, m_origin, Int32, patchpoint, i);
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
        return;
    }
    case Op::FrameAddress:
        define(m_block->appendNew<Value>(m_proc, B3::FramePointer, m_origin));
        return;
    case Op::TlsAddr: {
        // The same thread always gets the same block, so B3 may hoist and merge these calls.
        Value* base = m_block->appendNew<CCallValue>(m_proc, Int64, m_origin, Effects::none(),
            pointer(tagCFunction<void*, OperationPtrTag>(m_environment.threadLocalBase)), pointer(m_environment.threadLocalContext));
        define(m_block->appendNew<Value>(m_proc, B3::Add, m_origin, base, constant(Int64, inst.imm)));
        return;
    }
    case Op::FuncAddr:
        m_referencedFunctions.append(inst.a);
        define(m_block->appendNew<MemoryValue>(m_proc, B3::Load, Int64, m_origin, pointer(&m_environment.functionTable[inst.a]), 0));
        return;
    case Op::ExternAddr:
        define(pointer(m_environment.externAddresses[inst.a]));
        return;
    case Op::LocalGet:
        define(m_block->appendNew<VariableValue>(m_proc, B3::Get, m_origin, m_body.locals[inst.a]));
        return;
    case Op::LocalSet:
        m_block->appendNew<VariableValue>(m_proc, B3::Set, m_origin, m_body.locals[inst.a], value(inst.b));
        return;
    case Op::Call: {
        const BIR::Signature& signature = m_module.signatures[m_module.functions[inst.a].signature];
        Vector<Value*> arguments;
        bool hasConstantArgument = false;
        for (unsigned i = 0; i < inst.extraCount; ++i) {
            arguments.append(value(static_cast<uint32_t>(function.extra[inst.extraOffset + i])));
            hasConstantArgument |= arguments.last()->isConstant();
        }
        if (shouldInlineCallee(inst.a, hasConstantArgument)) {
            (m_module.functions[inst.a].isAlwaysInline ? m_alwaysInlineInstructionBudget : m_inlinedInstructionBudget) -= m_module.functions[inst.a].insts.size();
            Body caller = std::exchange(m_body, { });
            m_inlineStack.append(inst.a);
            Inlined inlined = lowerInline(inst.a, m_block, arguments.span(), m_origin);
            m_inlineStack.removeLast();
            m_body = WTF::move(caller);
            m_block = inlined.continuation;
            defineAll(inlined.results);
            return;
        }
        m_referencedFunctions.append(inst.a);
        Value* target = m_block->appendNew<MemoryValue>(m_proc, B3::Load, Int64, m_origin, pointer(&m_environment.functionTable[inst.a]), 0);
        defineAll(emitCall(function, inst, signature, target));
        return;
    }
    case Op::CallExtern: {
        const BIR::Signature& signature = m_module.signatures[m_module.externs[inst.a].signature];
        // The C library's own copy and fill, by their reserved names, with a size known here.
        if (inst.extraCount == 3 && signature.isScalar() && signature.results.size() == 1 && signature.results[0] == BIR::Type::I64) {
            auto name = m_module.externs[inst.a].name.span();
            auto is = [&](ASCIILiteral literal) { return equalSpans(name, literal.span()); };
            auto argument = [&](unsigned i) { return value(static_cast<uint32_t>(function.extra[inst.extraOffset + i])); };
            bool isCopy = is("memcpy"_s) || is("memmove"_s);
            if ((isCopy && argument(1)->type() == Int64 && argument(2)->type() == Int64 && emitSmallMemoryCopy(argument(0), argument(1), argument(2)))
                || (is("memset"_s) && argument(1)->type() == Int32 && argument(2)->type() == Int64 && emitSmallMemoryFill(argument(0), argument(1), argument(2)))) {
                define(argument(0));
                return;
            }
        }
        defineAll(emitCall(function, inst, signature, pointer(m_environment.externAddresses[inst.a])));
        return;
    }
    case Op::CallIndirect: {
        defineAll(emitCall(function, inst, m_module.signatures[inst.a], value(inst.b)));
        return;
    }
    case Op::Select:
        define(m_block->appendNew<Value>(m_proc, B3::Select, m_origin, value(inst.a), value(inst.b), value(inst.c)));
        return;
    case Op::MemCopy:
        if (!emitSmallMemoryCopy(value(inst.a), value(inst.b), value(inst.c)))
            m_block->appendNew<CCallValue>(m_proc, Int64, m_origin, pointer(cFunctionPointer(moveMemory)), value(inst.a), value(inst.b), value(inst.c));
        return;
    case Op::MemSet:
        if (!emitSmallMemoryFill(value(inst.a), value(inst.b), value(inst.c)))
            m_block->appendNew<CCallValue>(m_proc, Int64, m_origin, pointer(cFunctionPointer(fillMemory)), value(inst.a), value(inst.b), value(inst.c));
        return;
    case Op::StackAlloc:
    case Op::StackSave:
        define(emitStackOperation(inst));
        return;
    case Op::StackRestore:
        emitStackOperation(inst);
        return;
    case Op::VaStart:
        emitVaStart(value(inst.a));
        return;
    case Op::Jump:
        m_block->appendNewControlValue(m_proc, B3::Jump, m_origin, FrequentedBlock(m_body.blocks[inst.a]));
        return;
    case Op::Br:
        m_block->appendNewControlValue(m_proc, Branch, m_origin, value(inst.a), FrequentedBlock(m_body.blocks[inst.b]), FrequentedBlock(m_body.blocks[inst.c]));
        return;
    case Op::Switch: {
        SwitchValue* switchValue = m_block->appendNew<SwitchValue>(m_proc, m_origin, value(inst.a));
        switchValue->setFallThrough(FrequentedBlock(m_body.blocks[inst.b]));
        for (unsigned i = 0; i < inst.extraCount; i += 2)
            switchValue->appendCase(SwitchCase(function.extra[inst.extraOffset + i], FrequentedBlock(m_body.blocks[static_cast<uint32_t>(function.extra[inst.extraOffset + i + 1])])));
        return;
    }
    case Op::Ret:
        emitReturn(function, inst);
        return;
    case Op::RetVoid:
        if (m_body.returnTarget)
            m_block->appendNewControlValue(m_proc, B3::Jump, m_origin, FrequentedBlock(m_body.returnTarget->continuation));
        else
            m_block->appendNewControlValue(m_proc, Return, m_origin);
        return;
    case Op::Unreachable:
        m_block->appendNewControlValue(m_proc, Oops, m_origin);
        return;
    case Op::Trap: {
        PatchpointValue* trap = m_block->appendNew<PatchpointValue>(m_proc, Void, m_origin);
        trap->effects = Effects::forCall();
        trap->effects.terminal = true;
        trap->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams&) {
            jit.breakpoint();
        });
        m_block->appendNewControlValue(m_proc, Oops, m_origin);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::FFI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(B3_JIT)
