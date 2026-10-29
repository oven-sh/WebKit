/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTScratchRegisterChecker.h"

#if ENABLE(AOT_SCRATCH_REGISTER_CHECKER)

#include "MacroAssembler.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <inttypes.h>
#include <optional>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string_view>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

// The library holds the decoder only where it can print code. This copy is private to this file: some of its functions are extern "C", which a namespace does not hide.
IGNORE_CLANG_WARNINGS_BEGIN("everything")
namespace {
namespace Binja {
#pragma clang attribute push(__attribute__((internal_linkage)), apply_to = function)
#include "../../disassembler/ARM64/binja_impl.c.h"
#pragma clang attribute pop
} // namespace Binja
} // anonymous namespace
IGNORE_CLANG_WARNINGS_END

namespace JSC { namespace AOT {

namespace {

// x0 to x30. Neither sp nor zr is counted.
constexpr unsigned numberOfGeneralRegisters = 31;

enum class ControlFlow : uint8_t { GoesOn, Jumps, JumpsOrGoesOn, Calls, Leaves };

struct InstructionEffects {
    bool reads(GPRReg reg) const { return numberOfReads[reg]; }
    bool writes(GPRReg reg) const { return writtenRegisters & (1u << reg); }

    // One per operand that names the register: `add x0, x1, x1` reads x1 twice.
    std::array<uint8_t, numberOfGeneralRegisters> numberOfReads { };
    uint32_t writtenRegisters { 0 };
    // In bytes from the instruction.
    std::optional<int64_t> branchOffset;
    // A branch, a call, a return, a trap, or a word that is no instruction.
    bool mayLeaveStraightLine { false };
    bool isTest { false };
    // A pair loaded off the stack pointer.
    bool isRestore { false };
    // It reads no memory, and no general register unless it is a move.
    bool isConstantOrCopy { false };
    ControlFlow controlFlow { ControlFlow::GoesOn };
};

enum class RegisterAccess : uint8_t { Read, Write, ReadAndWrite };

} // anonymous namespace

static std::optional<unsigned> generalRegisterNumber(Binja::Register reg)
{
    if (reg >= Binja::REG_W0 && reg <= Binja::REG_W30)
        return reg - Binja::REG_W0;
    if (reg >= Binja::REG_X0 && reg <= Binja::REG_X30)
        return reg - Binja::REG_X0;
    return std::nullopt;
}

static bool equalsAnyOf(std::string_view mnemonic, std::initializer_list<std::string_view> names)
{
    for (std::string_view name : names) {
        if (mnemonic == name)
            return true;
    }
    return false;
}

static bool startsWithAnyOf(std::string_view mnemonic, std::initializer_list<std::string_view> prefixes)
{
    for (std::string_view prefix : prefixes) {
        if (mnemonic.starts_with(prefix))
            return true;
    }
    return false;
}

static bool mayLeaveStraightLine(std::string_view mnemonic)
{
    return equalsAnyOf(mnemonic, { "b", "cbz", "cbnz", "tbz", "tbnz", "udf", "hlt", "svc", "eret" })
        || startsWithAnyOf(mnemonic, { "b.", "bl", "br", "ret" });
}

static ControlFlow controlFlowOf(std::string_view mnemonic)
{
    if (!mayLeaveStraightLine(mnemonic))
        return ControlFlow::GoesOn;
    if (mnemonic == "b")
        return ControlFlow::Jumps;
    if (mnemonic.starts_with("b.") || equalsAnyOf(mnemonic, { "cbz", "cbnz", "tbz", "tbnz" }))
        return ControlFlow::JumpsOrGoesOn;
    if (mnemonic.starts_with("bl"))
        return ControlFlow::Calls;
    return ControlFlow::Leaves;
}

static RegisterAccess accessOfRegisterOperand(std::string_view mnemonic, unsigned index)
{
    if (startsWithAnyOf(mnemonic, { "stxr", "stlxr", "stxp", "stlxp" }))
        return index ? RegisterAccess::Read : RegisterAccess::Write;
    if (mnemonic.starts_with("casp"))
        return index < 2 ? RegisterAccess::ReadAndWrite : RegisterAccess::Read;
    if (mnemonic.starts_with("cas"))
        return index ? RegisterAccess::Read : RegisterAccess::ReadAndWrite;
    if (startsWithAnyOf(mnemonic, { "swp", "ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin" }))
        return index == 1 ? RegisterAccess::Write : RegisterAccess::Read;
    if (mayLeaveStraightLine(mnemonic)
        || mnemonic.starts_with("st")
        || equalsAnyOf(mnemonic, { "cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "fccmpe", "msr", "prfm", "prfum" }))
        return RegisterAccess::Read;
    if (equalsAnyOf(mnemonic, { "ldp", "ldnp", "ldpsw", "ldxp", "ldaxp" }))
        return index < 2 ? RegisterAccess::Write : RegisterAccess::Read;
    if (index)
        return RegisterAccess::Read;
    if (equalsAnyOf(mnemonic, { "movk", "bfi", "bfxil", "bfm", "bfc" }))
        return RegisterAccess::ReadAndWrite;
    return RegisterAccess::Write;
}

static bool decode(uint32_t word, Binja::Instruction& instruction)
{
    memset(&instruction, 0, sizeof(instruction));
    return Binja::aarch64_decompose(word, &instruction, 0) == DECODE_STATUS_OK;
}

static InstructionEffects decodeEffects(uint32_t word)
{
    InstructionEffects effects;
    Binja::Instruction instruction;
    if (!decode(word, instruction)) {
        effects.mayLeaveStraightLine = true;
        effects.controlFlow = ControlFlow::Leaves;
        return effects;
    }

    auto record = [&](Binja::Register name, RegisterAccess access) {
        std::optional<unsigned> reg = generalRegisterNumber(name);
        if (!reg)
            return;
        if (access != RegisterAccess::Write)
            ++effects.numberOfReads[*reg];
        if (access != RegisterAccess::Read)
            effects.writtenRegisters |= 1u << *reg;
    };

    std::string_view mnemonic = Binja::get_operation(&instruction);
    effects.mayLeaveStraightLine = mayLeaveStraightLine(mnemonic);
    effects.isTest = mnemonic == "tst";
    effects.controlFlow = controlFlowOf(mnemonic);
    bool isPairLoad = mnemonic == "ldp";
    auto finish = [&] {
        bool readsGeneralRegister = std::ranges::any_of(effects.numberOfReads, [](uint8_t count) { return !!count; });
        effects.isConstantOrCopy = effects.writtenRegisters && !mnemonic.starts_with("ld") && (mnemonic == "mov" || !readsGeneralRegister);
    };
    for (unsigned index = 0; index < MAX_OPERANDS; ++index) {
        const Binja::InstructionOperand& operand = instruction.operands[index];
        switch (operand.operandClass) {
        case Binja::NONE:
            finish();
            return effects;
        case Binja::REG:
            record(operand.reg[0], accessOfRegisterOperand(mnemonic, index));
            break;
        case Binja::MEM_PRE_IDX:
        case Binja::MEM_POST_IDX:
            effects.isRestore |= isPairLoad && operand.reg[0] == Binja::REG_SP;
            record(operand.reg[0], RegisterAccess::ReadAndWrite);
            record(operand.reg[1], RegisterAccess::Read);
            break;
        case Binja::LABEL:
            if (effects.mayLeaveStraightLine)
                effects.branchOffset = static_cast<int64_t>(operand.immediate);
            break;
        default:
            effects.isRestore |= isPairLoad && operand.reg[0] == Binja::REG_SP;
            for (Binja::Register name : operand.reg)
                record(name, RegisterAccess::Read);
            break;
        }
    }
    finish();
    return effects;
}

Vector<ScratchRegisterConflict> findScratchRegisterConflicts(std::span<const uint8_t> code, std::span<const uint32_t> entryOffsets)
{
    constexpr std::array scratchRegisters { MacroAssembler::dataTempRegister, MacroAssembler::memoryTempRegister };
    using Kind = ScratchRegisterConflict::Kind;

    auto words = spanReinterpretCast<const uint32_t>(code);
    BitVector startsOver(words.size());
    for (uint32_t offset : entryOffsets)
        startsOver.set(offset / sizeof(uint32_t));
    Vector<InstructionEffects> effectsOfWords(words.size(), [&](size_t index) {
        InstructionEffects effects = decodeEffects(words[index]);
        if (effects.branchOffset) {
            int64_t target = static_cast<int64_t>(index) + *effects.branchOffset / static_cast<int64_t>(sizeof(uint32_t));
            if (target >= 0 && target < static_cast<int64_t>(words.size()))
                startsOver.set(target);
        }
        return effects;
    });

    Vector<ScratchRegisterConflict> conflicts;
    std::array<std::optional<uint32_t>, scratchRegisters.size()> offsetsOfUnreadWrites;
    for (size_t index = 0; index < words.size(); ++index) {
        const InstructionEffects& effects = effectsOfWords[index];
        uint32_t offset = index * sizeof(uint32_t);
        if (startsOver.get(index))
            offsetsOfUnreadWrites.fill(std::nullopt);
        for (size_t i = 0; i < scratchRegisters.size(); ++i) {
            GPRReg reg = scratchRegisters[i];
            std::optional<uint32_t>& offsetOfUnreadWrite = offsetsOfUnreadWrites[i];
            if (effects.numberOfReads[reg] > 1 && !effects.isTest)
                conflicts.append({ Kind::ReadAsTwoOperands, reg, offset });
            if (effects.reads(reg))
                offsetOfUnreadWrite = std::nullopt;
            if (!effects.writes(reg))
                continue;
            if (offsetOfUnreadWrite)
                conflicts.append({ Kind::ReplacedUnread, reg, offset, *offsetOfUnreadWrite });
            if (effects.isRestore)
                offsetOfUnreadWrite = std::nullopt;
            else
                offsetOfUnreadWrite = offset;
        }
        if (effects.mayLeaveStraightLine)
            offsetsOfUnreadWrites.fill(std::nullopt);
    }
    return conflicts;
}

Vector<UnreadValue> findUnreadConstantsAndCopies(std::span<const uint8_t> code)
{
    constexpr uint32_t everyRegister = (1u << numberOfGeneralRegisters) - 1;
    auto words = spanReinterpretCast<const uint32_t>(code);
    Vector<InstructionEffects> effectsOfWords(words.size(), [&](size_t index) {
        return decodeEffects(words[index]);
    });
    Vector<uint32_t> readBeforeWrittenAt(words.size(), [](size_t) {
        return 0u;
    });

    auto at = [&](int64_t index) {
        return index >= 0 && index < static_cast<int64_t>(words.size()) ? readBeforeWrittenAt[index] : everyRegister;
    };
    auto behind = [&](size_t index) -> uint32_t {
        const InstructionEffects& effects = effectsOfWords[index];
        int64_t next = static_cast<int64_t>(index) + 1;
        int64_t target = effects.branchOffset ? static_cast<int64_t>(index) + *effects.branchOffset / static_cast<int64_t>(sizeof(uint32_t)) : -1;
        switch (effects.controlFlow) {
        case ControlFlow::GoesOn:
        case ControlFlow::Calls:
            return at(next);
        case ControlFlow::Jumps:
            return at(target);
        case ControlFlow::JumpsOrGoesOn:
            return at(target) | at(next);
        case ControlFlow::Leaves:
            return everyRegister;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    for (bool changed = true; changed;) {
        changed = false;
        for (size_t index = words.size(); index--;) {
            const InstructionEffects& effects = effectsOfWords[index];
            uint32_t read = effects.controlFlow == ControlFlow::Calls ? everyRegister : 0;
            for (unsigned reg = 0; reg < numberOfGeneralRegisters; ++reg)
                read |= effects.numberOfReads[reg] ? 1u << reg : 0;
            uint32_t result = read | (behind(index) & ~effects.writtenRegisters);
            changed |= result != readBeforeWrittenAt[index];
            readBeforeWrittenAt[index] = result;
        }
    }

    Vector<UnreadValue> unreadValues;
    for (size_t index = 0; index < words.size(); ++index) {
        const InstructionEffects& effects = effectsOfWords[index];
        if (!effects.isConstantOrCopy)
            continue;
        uint32_t unread = effects.writtenRegisters & ~behind(index);
        for (unsigned reg = 0; reg < numberOfGeneralRegisters; ++reg) {
            if (unread & (1u << reg))
                unreadValues.append({ static_cast<GPRReg>(reg), static_cast<uint32_t>(index * sizeof(uint32_t)) });
        }
    }
    return unreadValues;
}

void printInstructions(PrintStream& out, std::span<const uint8_t> code, uint32_t firstOffset, uint32_t lastOffset)
{
    auto words = spanReinterpretCast<const uint32_t>(code);
    for (uint32_t offset = firstOffset; offset <= lastOffset && offset / sizeof(uint32_t) < words.size(); offset += sizeof(uint32_t)) {
        Binja::Instruction instruction;
        std::array<char, 256> text { };
        if (decode(words[offset / sizeof(uint32_t)], instruction))
            Binja::aarch64_disassemble(&instruction, text.data(), text.size());
        out.println("    ", offset, ": ", text[0] ? text.data() : "not an instruction");
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT_SCRATCH_REGISTER_CHECKER)
