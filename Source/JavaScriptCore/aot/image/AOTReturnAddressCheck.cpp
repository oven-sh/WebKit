/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReturnAddressCheck.h"

#if ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)

#include "AOTBuildContext.h"
#include "AOTFFIStubs.h"
#include "AOTRegExpCompiler.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "AOTWasmInterface.h"
#include "FFIStubs.h"
#include "WasmHandlerInfo.h"
#include <wtf/HexNumber.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace A64 {

static constexpr unsigned ip1 = 17;
static constexpr unsigned fp = 29;
static constexpr unsigned lr = 30;
static constexpr unsigned spOrZero = 31;

static constexpr uint32_t hint(unsigned number) { return 0xd503201f | number << 5; }
static constexpr uint32_t xpaclri = hint(7);
static constexpr uint32_t pacib1716 = hint(10);
static constexpr uint32_t autib1716 = hint(14);
static constexpr uint32_t pacibsp = hint(27);
static constexpr uint32_t autibsp = hint(31);

static constexpr bool matches(uint32_t word, uint32_t mask, uint32_t bits) { return (word & mask) == bits; }
static constexpr bool bit(uint32_t word, unsigned index) { return word >> index & 1; }
static constexpr unsigned field(uint32_t word, unsigned low, unsigned width) { return word >> low & ((1u << width) - 1); }
static constexpr int64_t signedField(uint32_t word, unsigned low, unsigned width)
{
    int64_t sign = 1ll << (width - 1);
    return (static_cast<int64_t>(field(word, low, width)) ^ sign) - sign;
}

static constexpr unsigned rd(uint32_t word) { return field(word, 0, 5); }
static constexpr unsigned rn(uint32_t word) { return field(word, 5, 5); }
static constexpr unsigned rt2(uint32_t word) { return field(word, 10, 5); }
static constexpr unsigned rm(uint32_t word) { return field(word, 16, 5); }

static constexpr bool isJump(uint32_t word) { return matches(word, 0xfc000000, 0x14000000); }
static constexpr bool isCall(uint32_t word) { return matches(word, 0xfc000000, 0x94000000); }
static constexpr bool isJumpToRegister(uint32_t word) { return matches(word, 0xfffffc1f, 0xd61f0000); }
static constexpr bool isCallOfRegister(uint32_t word) { return matches(word, 0xfffffc1f, 0xd63f0000); }
static constexpr bool isReturn(uint32_t word) { return matches(word, 0xfffffc1f, 0xd65f0000); }
static constexpr bool isTrap(uint32_t word) { return !(word >> 16) || matches(word, 0xff000000, 0xd4000000); }
static constexpr bool isBranchOnFlagsOrZero(uint32_t word) { return matches(word, 0xff000010, 0x54000000) || matches(word, 0x7e000000, 0x34000000); }
static constexpr bool isBranchOnBit(uint32_t word) { return matches(word, 0x7e000000, 0x36000000); }
static constexpr bool isBranchOrSystem(uint32_t word) { return matches(word, 0x1c000000, 0x14000000); }
static constexpr bool isAddress(uint32_t word) { return matches(word, 0x9f000000, 0x10000000); }
static constexpr bool isAddressOfPage(uint32_t word) { return matches(word, 0x9f000000, 0x90000000); }
static constexpr bool isAddOrSubtractImmediate(uint32_t word) { return matches(word, 0xbf800000, 0x91000000); }
static constexpr bool isAddOfRegisters(uint32_t word) { return matches(word, 0xff000000, 0x8b000000); }
static constexpr bool isMove(uint32_t word) { return matches(word, 0xffe0ffe0, 0xaa0003e0); }
static constexpr bool isMoveOfConstant(uint32_t word) { return matches(word, 0x3f800000, 0x12800000) || (isMove(word) && rm(word) == spOrZero); }
static constexpr bool isComparisonOfRegisters(uint32_t word) { return matches(word, 0xffe0fc1f, 0xeb00001f); }
static constexpr bool isBranchIfEqual(uint32_t word) { return matches(word, 0xff00001f, 0x54000000); }
static constexpr bool isBranchUnlessEqual(uint32_t word) { return matches(word, 0xff00001f, 0x54000001); }
static constexpr bool isHintForOtherKeyOrNoModifier(uint32_t word) { return word == hint(8) || word == hint(12) || (matches(word, ~(7u << 5), hint(24)) && word != pacibsp && word != autibsp); }
static constexpr bool isLoadOrStore(uint32_t word) { return matches(word, 0x0a000000, 0x08000000); }
static constexpr bool isLoadOfByteIntoNothing(uint32_t word) { return matches(word, 0xfffffc1f, 0x3940001f); }
static constexpr int64_t displacementOfAddress(uint32_t word) { return signedField(word, 5, 19) * 4 + field(word, 29, 2); }

// MacroAssembler::movePCRelativeAddress(), as setRelocationDistance() or applyRelocation() leaves it.
static constexpr std::optional<int64_t> offsetWhoseAddressIsTaken(uint32_t offset, uint32_t word, uint32_t next)
{
    constexpr int64_t page = 4096;
    bool goesOn = isAddOrSubtractImmediate(next) && rn(next) == rd(word) && rd(next) == rd(word);
    int64_t added = goesOn ? (bit(next, 30) ? -1 : 1) * static_cast<int64_t>(field(next, 10, 12)) * (bit(next, 22) ? page : 1) : 0;
    if (isAddress(word))
        return offset + displacementOfAddress(word) + added;
    if (isAddressOfPage(word) && goesOn)
        return (offset & -page) + displacementOfAddress(word) * page + added;
    return std::nullopt;
}

static constexpr uint32_t storedAsReturnAddressOfFrameRecord(uint32_t word)
{
    bool isStoreOfPair = matches(word, 0xfe400000, 0xa8000000);
    if (isStoreOfPair && (rd(word) == fp || matches(word, 0xffff83e0, 0xa9000000 | fp << 5))) // stp fp, x, [..] and stp .., x, [fp]
        return 1u << rt2(word);
    if (matches(word, 0xffffffe0, 0xf9000400 | fp << 5) || matches(word, 0xffffffe0, 0xf8008000 | fp << 5)) // str and stur x, [fp, #8]
        return 1u << rd(word);
    return 0;
}

struct RegistersOfInstruction {
    void write(unsigned reg) { written |= 1u << reg; }
    void store(unsigned reg) { stored |= 1u << reg; }
    void loadOrStore(bool isLoad, unsigned reg) { (isLoad ? written : stored) |= 1u << reg; }

    uint32_t written { 0 };
    uint32_t stored { 0 };
};

static RegistersOfInstruction registersOfLoadOrStore(uint32_t word)
{
    RegistersOfInstruction result;
    bool isOfGPRs = !bit(word, 26);
    bool isLoad = bit(word, 22);
    if (matches(word, 0x3b000000, 0x18000000)) { // From a place relative to the instruction.
        if (isOfGPRs && field(word, 30, 2) != 3)
            result.write(rd(word));
    } else if (matches(word, 0x3f000000, 0x08000000)) { // Exclusive, ordered, compare and swap.
        bool isOrdered = bit(word, 23);
        bool hasSecond = bit(word, 21);
        if (hasSecond && (isOrdered || !bit(word, 31))) {
            result.write(rm(word));
            result.store(rd(word));
            if (!isOrdered) {
                result.write((rm(word) + 1) % 32);
                result.store((rd(word) + 1) % 32);
            }
            return result;
        }
        result.loadOrStore(isLoad, rd(word));
        if (hasSecond)
            result.loadOrStore(isLoad, rt2(word));
        if (!isLoad && !isOrdered)
            result.write(rm(word));
    } else if (matches(word, 0x3a000000, 0x28000000)) { // Pairs.
        if (isOfGPRs) {
            result.loadOrStore(isLoad, rd(word));
            result.loadOrStore(isLoad, rt2(word));
        }
        if (bit(word, 23))
            result.write(rn(word));
    } else if (matches(word, 0x3f200c00, 0x38200000)) { // Atomic operations.
        result.write(rd(word));
        result.store(rm(word));
    } else if (matches(word, 0x3a000000, 0x38000000) || matches(word, 0x3f200c00, 0x19000000)) { // One register.
        bool isPrefetch = field(word, 30, 2) == 3 && field(word, 22, 2) == 2;
        if (isOfGPRs && !isPrefetch)
            result.loadOrStore(field(word, 22, 2), rd(word));
        if (matches(word, 0x3b200400, 0x38000400))
            result.write(rn(word));
    } else if (matches(word, 0xbe000000, 0x0c000000)) { // Structures of vectors.
        if (bit(word, 23))
            result.write(rn(word));
    } else
        result.written = ~0u;
    return result;
}

static bool movesFromFPRToGPR(uint32_t word)
{
    unsigned opcode = field(word, 16, 3);
    if (matches(word, 0x5f20fc00, 0x1e200000))
        return opcode != 2 && opcode != 3 && opcode != 7;
    if (matches(word, 0x5f200000, 0x1e000000))
        return opcode < 2;
    return matches(word, 0xbfe0ec00, 0x0e002c00);
}

static RegistersOfInstruction registersOf(uint32_t word)
{
    if (isLoadOrStore(word))
        return registersOfLoadOrStore(word);
    RegistersOfInstruction result;
    if (isBranchOrSystem(word)) {
        if (matches(word, 0xffe00000, 0xd5200000))
            result.write(rd(word));
    } else if (!matches(word, 0x0e000000, 0x0e000000) || movesFromFPRToGPR(word))
        result.write(rd(word));
    return result;
}

} // namespace A64

void ReturnAddressCheck::State::set(unsigned reg, bool trusted, bool returnable, bool hasSignature)
{
    if (reg == A64::spOrZero)
        return;
    *this = keeping(~(1u << reg));
    isTrusted |= static_cast<uint32_t>(trusted) << reg;
    mayBeReturnedTo |= static_cast<uint32_t>(returnable) << reg;
    isSigned |= static_cast<uint32_t>(hasSignature) << reg;
    isUnsignedOnSomePath |= static_cast<uint32_t>(returnable) << reg;
}

void ReturnAddressCheck::State::copy(unsigned from, unsigned to)
{
    if (to == A64::spOrZero)
        return;
    for (uint32_t* bits : { &isTrusted, &mayBeReturnedTo, &isSigned, &isUnsignedOnSomePath, &wasStrippedOnSomePath })
        *bits = (*bits & ~(1u << to)) | static_cast<uint32_t>(A64::bit(*bits, from)) << to;
}

auto ReturnAddressCheck::State::keeping(uint32_t registers) const -> State
{
    return { isTrusted & registers, mayBeReturnedTo & registers, isSigned & registers, isUnsignedOnSomePath & registers, wasStrippedOnSomePath & registers };
}

auto ReturnAddressCheck::State::joinedWith(const State& other) const -> State
{
    return { isTrusted & other.isTrusted, mayBeReturnedTo & other.mayBeReturnedTo, isSigned & other.isSigned, isUnsignedOnSomePath | other.isUnsignedOnSomePath, wasStrippedOnSomePath | other.wasStrippedOnSomePath };
}

ReturnAddressCheck::ReturnAddressCheck(std::span<const uint8_t> code)
    : m_code(code)
    , m_numberOfWords(safeCast<uint32_t>(code.size() / sizeof(uint32_t)))
    , m_stubsThatNeverReturn(m_numberOfWords)
    , m_wordsBehindCalls(m_numberOfWords)
    , m_data(m_numberOfWords)
    , m_codeWhoseFaultsSign(m_numberOfWords)
    , m_reached(m_numberOfWords)
{
}

void ReturnAddressCheck::addCalledEntry(uint32_t offset)
{
    State state;
    state.setPlain(A64::lr);
    goTo(offset / sizeof(uint32_t), state);
}

void ReturnAddressCheck::addEntryInsideFrame(uint32_t offset)
{
    State state;
    state.set(A64::lr, true, false, false);
    goTo(offset / sizeof(uint32_t), state);
}

static bool isEnteredByUnwinder(StubKey key)
{
    return key.family() == StubFamily::Catch || key.family() == StubFamily::RejectInFrame || key.family() == StubFamily::RejectAwaitedCall;
}

static bool takesSecondReturnAddress(StubKey key)
{
    return key.family() == StubFamily::PollBeforeEntering;
}

void ReturnAddressCheck::addStubs(const StubBlob& blob, uint32_t offsetOfBlob)
{
    for (const StubPlacement& placement : blob.placements) {
        uint32_t offset = offsetOfBlob + placement.offset;
        if (neverReturns(placement.key))
            m_stubsThatNeverReturn.set(offset / sizeof(uint32_t));
        if (isEnteredByUnwinder(placement.key))
            addEntryInsideFrame(offset);
        else if (descriptorOf(placement.key.family()).linkage != StubLinkage::InternalEntry)
            addCalledEntry(offset);
        else if (!takesSecondReturnAddress(placement.key))
            m_entriesUnlessReached.append(offset);
    }
    for (const StubReturnSite& site : blob.returnSites)
        m_entriesUnlessReached.append(offsetOfBlob + site.offset);
    addCodeThatTakesAddressesOfItsLabels(offsetOfBlob, blob.code.bytes.size(), Labels::AreCalledOrReturnedTo);
}

static void markWordsOfCheckedCode(BitVector& words, uint32_t offset, uint32_t size)
{
    for (uint32_t index = offset / sizeof(uint32_t); index < (offset + size) / sizeof(uint32_t); ++index)
        words.set(index);
}

void ReturnAddressCheck::addData(uint32_t offset, uint32_t size)
{
    markWordsOfCheckedCode(m_data, offset, size);
}

void ReturnAddressCheck::addCodeWhoseFaultsSignLinkRegister(uint32_t offset, uint32_t size)
{
    markWordsOfCheckedCode(m_codeWhoseFaultsSign, offset, size);
}

void ReturnAddressCheck::addJumpTable(uint32_t offsetOfAddress, Vector<uint32_t>&& targets)
{
    uint32_t index = offsetOfAddress / sizeof(uint32_t);
    while (!A64::isJumpToRegister(wordAt(index)))
        ++index;
    m_jumpTables.add(index, WTF::move(targets));
}

uint32_t ReturnAddressCheck::wordAt(uint32_t index) const
{
    return WTF::unalignedLoad<uint32_t>(m_code.subspan(static_cast<size_t>(index) * sizeof(uint32_t), sizeof(uint32_t)).data());
}

bool ReturnAddressCheck::merge(uint32_t index, State& state)
{
    auto added = m_states.add(index, state);
    if (added.isNewEntry)
        return true;
    State& known = added.iterator->value;
    state = state.joinedWith(known);
    return std::exchange(known, state) != state;
}

void ReturnAddressCheck::goTo(int64_t index, State state)
{
    if (index >= 0 && index < m_numberOfWords && merge(static_cast<uint32_t>(index), state))
        m_worklist.append(static_cast<uint32_t>(index));
}

bool ReturnAddressCheck::leadsToStubThatNeverReturns(int64_t index) const
{
    constexpr unsigned longestThunk = 64;
    for (unsigned steps = 0; steps < longestThunk && index >= 0 && index < m_numberOfWords; ++steps) {
        if (m_stubsThatNeverReturn.get(index))
            return true;
        uint32_t word = wordAt(static_cast<uint32_t>(index));
        if (A64::isJump(word))
            index += A64::signedField(word, 0, 26);
        else if (A64::isBranchOrSystem(word) || A64::isTrap(word))
            return false;
        else
            ++index;
    }
    return false;
}

void ReturnAddressCheck::noteAddressOfLabel(uint32_t index, int64_t offsetOfLabel, const State& state)
{
    uint32_t offset = index * sizeof(uint32_t);
    auto next = std::ranges::upper_bound(m_codeWithLabels, offset, { }, &CodeWithLabels::begin);
    if (next == m_codeWithLabels.begin())
        return;
    const CodeWithLabels& code = next[-1];
    if (offset >= code.end || offsetOfLabel < code.begin || offsetOfLabel >= code.end)
        return;
    if (code.labels == Labels::AreCalledOrReturnedTo) {
        addCalledEntry(offsetOfLabel);
        return;
    }
    goTo(offsetOfLabel / sizeof(uint32_t), state.keeping(1u << A64::lr));
}

auto ReturnAddressCheck::step(uint32_t word, State& state) const -> std::optional<Rule>
{
    using namespace A64;
    if (isHintForOtherKeyOrNoModifier(word))
        return Rule::UsesOneKeyAndModifier;
    uint32_t mayBeUnsigned = state.isUnsignedOnSomePath | state.wasStrippedOnSomePath;
    if (word == pacibsp || word == pacib1716) {
        unsigned reg = word == pacibsp ? lr : ip1;
        if (!state.isPlain(reg))
            return Rule::SignsOnlyWhatWasNeverInMemory;
        state.set(reg, true, false, true);
    } else if (word == autibsp || word == autib1716) {
        unsigned reg = word == autibsp ? lr : ip1;
        if (bit(mayBeUnsigned, reg))
            return Rule::AuthenticatesOnlyWhatIsSigned;
        state.set(reg, false, true, false);
    } else if (word == xpaclri) {
        if (bit(state.isSigned, lr))
            state.setPlain(lr);
        else if (!state.isPlain(lr)) {
            state.forget(lr);
            state.wasStrippedOnSomePath |= 1u << lr;
        }
    } else if (isLoadOfByteIntoNothing(word)) {
        if (bit(state.mayBeReturnedTo, rn(word)))
            state.setPlain(rn(word));
    } else if (isAddress(word) || isAddressOfPage(word) || isMoveOfConstant(word))
        state.setPlain(rd(word));
    else if (isMove(word))
        state.copy(rm(word), rd(word));
    else if (isAddOrSubtractImmediate(word) && bit(mayBeUnsigned, rn(word)))
        state.copy(rn(word), rd(word));
    else {
        RegistersOfInstruction registers = registersOf(word);
        if (registers.stored & (1u << lr | storedAsReturnAddressOfFrameRecord(word)) & mayBeUnsigned)
            return Rule::SavesOnlyWhatIsSigned;
        state = state.keeping(~registers.written);
    }
    return std::nullopt;
}

auto ReturnAddressCheck::follow(uint32_t index) -> std::optional<Violation>
{
    using namespace A64;
    State state = m_states.get(index);
    std::optional<unsigned> placeInTable;
    std::optional<uint32_t> previousWord;
    for (; index < m_numberOfWords && !m_data.get(index); previousWord = wordAt(index++)) {
        if (previousWord && m_states.contains(index) && !merge(index, state))
            return std::nullopt;
        m_reached.set(index);
        uint32_t word = wordAt(index);
        std::optional<uint32_t> comparison = previousWord && isComparisonOfRegisters(*previousWord) ? previousWord : std::nullopt;
        uint32_t offset = index * sizeof(uint32_t);
        if (isTrap(word))
            return std::nullopt;
        if (isJump(word)) {
            goTo(index + signedField(word, 0, 26), state);
            return std::nullopt;
        }
        if (isReturn(word)) {
            if (!bit(state.mayBeReturnedTo, rn(word)))
                return Violation { offset, Rule::ReturnsOnlyToWhatIsAuthenticated };
            if (rn(word) != lr && !bit(state.isTrusted, lr))
                return Violation { offset, Rule::LeavesNothingUncheckedInLinkRegister };
            return std::nullopt;
        }
        if ((isJumpToRegister(word) || isCallOfRegister(word)) && bit(state.wasStrippedOnSomePath, rn(word)))
            return Violation { offset, Rule::ReturnsOnlyToWhatIsAuthenticated };
        if (isJumpToRegister(word)) {
            if (auto table = m_jumpTables.find(index); table != m_jumpTables.end()) {
                for (uint32_t target : table->value)
                    goTo(target / sizeof(uint32_t), state);
            } else if (placeInTable != rn(word) && !bit(state.isTrusted, lr))
                return Violation { offset, Rule::LeavesNothingUncheckedInLinkRegister };
            return std::nullopt;
        }
        if (isCall(word) || isCallOfRegister(word)) {
            state.setPlain(lr);
            if (isCall(word)) {
                int64_t callee = index + signedField(word, 0, 26);
                goTo(callee, state);
                if (leadsToStubThatNeverReturns(callee))
                    return std::nullopt;
            }
            // A callee may have kept any other register in memory.
            state = { };
            state.setPlain(lr);
            while (m_wordsBehindCalls.get(index + 1))
                ++index;
            continue;
        }
        if (isBranchOnFlagsOrZero(word) || isBranchOnBit(word)) {
            State ifEqual = state;
            if (comparison && (state.isPlain(rn(*comparison)) || state.isPlain(rm(*comparison)))) {
                ifEqual.setPlain(rn(*comparison));
                ifEqual.setPlain(rm(*comparison));
            }
            goTo(index + signedField(word, 5, isBranchOnBit(word) ? 14 : 19), isBranchIfEqual(word) ? ifEqual : state);
            if (isBranchUnlessEqual(word))
                state = ifEqual;
            continue;
        }

        if (m_codeWhoseFaultsSign.get(index) && isLoadOrStore(word) && !isLoadOfByteIntoNothing(word) && !bit(state.isTrusted, lr))
            return Violation { offset, Rule::SignsOnlyWhatWasNeverInMemory };
        if (std::optional<int64_t> label = offsetWhoseAddressIsTaken(offset, word, index + 1 < m_numberOfWords ? wordAt(index + 1) : 0))
            noteAddressOfLabel(index, *label, state);
        bool addsToAddress = isAddOfRegisters(word) && (state.isPlain(rn(word)) || state.isPlain(rm(word)));
        placeInTable = addsToAddress ? std::optional(rd(word)) : std::nullopt;
        if (std::optional<Rule> broken = step(word, state))
            return Violation { offset, *broken };
    }
    return std::nullopt;
}

auto ReturnAddressCheck::run() -> std::optional<Violation>
{
    AOT::sort(m_codeWithLabels, { }, &CodeWithLabels::begin);
    for (size_t next = 0;; ++next) {
        while (!m_worklist.isEmpty()) {
            if (std::optional<Violation> violation = follow(m_worklist.takeLast()))
                return violation;
        }
        if (next == m_entriesUnlessReached.size())
            return std::nullopt;
        if (!hasReached(m_entriesUnlessReached[next]))
            addCalledEntry(m_entriesUnlessReached[next]);
    }
}

bool ReturnAddressCheck::hasReachedSomethingIn(uint32_t offset, uint32_t size) const
{
    return m_reached.findBit(offset / sizeof(uint32_t), true) < (offset + size) / sizeof(uint32_t);
}

ASCIILiteral nameOf(ReturnAddressCheck::Rule rule)
{
    using Rule = ReturnAddressCheck::Rule;
    switch (rule) {
    case Rule::SavesOnlyWhatIsSigned:
        return "a return address is saved without a signature"_s;
    case Rule::SignsOnlyWhatWasNeverInMemory:
        return "what is signed may have come out of memory"_s;
    case Rule::AuthenticatesOnlyWhatIsSigned:
        return "what is authenticated has no signature"_s;
    case Rule::ReturnsOnlyToWhatIsAuthenticated:
        return "what is returned to may have come out of memory, or still has its signature"_s;
    case Rule::LeavesNothingUncheckedInLinkRegister:
        return "other code gets a link register that may have come out of memory"_s;
    case Rule::UsesOneKeyAndModifier:
        return "an address is signed or authenticated with the other key, or without a modifier"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addFunctionsToCheck(ReturnAddressCheck& check, const CodeLinkerInputs& inputs)
{
    const LinkedFunctions& functions = inputs.layoutInputs.functions;
    for (uint32_t position = 0; position < functions.size(); ++position) {
        const CodeBody& body = functions[position].body;
        if (body.bytes.isEmpty())
            continue;
        uint32_t start = inputs.layout.bodyStarts[position];
        check.addCodeThatTakesAddressesOfItsLabels(start, body.bytes.size(), ReturnAddressCheck::Labels::AreCalledOrReturnedTo);
        for (const CodeEntry& entry : body.entries)
            check.addCalledEntry(start + entry.offset);
        for (const SiteHandler& handler : functions[position].sites.rareLists->handlers)
            check.addEntryInsideFrame(start + handler.codeOffset);
        for (const FaultingAccess& access : functions[position].sites.faultingAccesses) {
            if (!functions[position].frame.hasFrameAt(access.codeOffset))
                check.addCodeWhoseFaultsSignLinkRegister(start + access.codeOffset, sizeof(uint32_t));
        }
        for (const PollSite& poll : functions[position].sites.rareLists->pollSites) {
            if (poll.callerIsFrameless)
                check.addCodeWhoseFaultsSignLinkRegister(start + poll.offset, sizeOfLoopPoll);
        }
        for (const Relocation& relocation : body.relocations) {
            if (relocation.kind == RelocationKind::NumberWord)
                check.addWordBehindCall(start + relocation.offset);
            else if (relocation.space == SymbolSpace::JumpTable) {
                std::span<const uint32_t> targets = body.rareLists->jumpTables[relocation.symbol].targets.span();
                check.addJumpTable(start + relocation.offset, Vector<uint32_t>(targets.size(), [&](size_t index) {
                    return start + targets[index];
                }));
            }
        }
        if (inputs.layoutInputs.sizeOfFarInstanceWords(position))
            check.addData(start + FarInstanceWords::offsetBehind(body.bytes.size()), inputs.layoutInputs.farInstanceWords[position].sizeOfDistances());
    }
}

static void addRegExpsToCheck(ReturnAddressCheck& check, const CodeLinkerInputs& inputs)
{
    const RegExpSection& section = inputs.layoutInputs.regExpSection;
    uint32_t start = inputs.layout.regExpCodeOffset;
    for (const std::array<uint32_t, 2>& entries : section.entries) {
        for (uint32_t entry : entries)
            check.addCalledEntry(start + entry);
    }
    for (std::optional<uint32_t> matcher : section.matchers) {
        if (matcher)
            check.addCalledEntry(start + *matcher);
    }
    for (const RegExpSectionItem& item : section.items) {
        if (item.kind == RegExpSectionItem::Kind::Body)
            check.addCodeThatTakesAddressesOfItsLabels(start + item.offset, item.size, ReturnAddressCheck::Labels::AreJumpedTo);
    }
}

static void addFFIStubsToCheck(ReturnAddressCheck& check, const FFIStubs& stubs, uint32_t start)
{
    if (stubs.directory.isEmpty())
        return;
    const FFI::StubDirectory& directory = spanReinterpretCast<const FFI::StubDirectory>(stubs.directory.span().first(sizeof(FFI::StubDirectory)))[0];
    check.addCalledEntry(start + directory.invoke);
    for (uint32_t entry : directory.callbackEntries())
        check.addCalledEntry(start + entry);
}

#if ENABLE(WEBASSEMBLY)
template<typename T>
static std::span<const T> recordsInWasmDirectory(const WebAssemblyCodeToCheck& webAssembly, size_t offset, size_t count)
{
    return spanReinterpretCast<const T>(webAssembly.directory.subspan(offset, count * sizeof(T)));
}

void ReturnAddressCheck::addWebAssembly(const WebAssemblyCodeToCheck& webAssembly, uint32_t start)
{
    if (webAssembly.directory.empty())
        return;
    const Wasm::DirectoryInImage& directory = recordsInWasmDirectory<Wasm::DirectoryInImage>(webAssembly, 0, 1)[0];
    if (!directory.numberOfModules)
        return;
    addEntryInsideFrame(start + directory.catchRunway);
    addCalledEntry(start + directory.faultWithoutFrame);
    for (const Wasm::ModuleInImage& module : recordsInWasmDirectory<Wasm::ModuleInImage>(webAssembly, sizeof(directory), directory.numberOfModules)) {
        for (uint32_t functions : module.functions) {
            for (const Wasm::FunctionInImage& function : recordsInWasmDirectory<Wasm::FunctionInImage>(webAssembly, functions, module.numberOfFunctions)) {
                addCalledEntry(start + function.entry);
                for (const Wasm::HandlerInImage& handler : recordsInWasmDirectory<Wasm::HandlerInImage>(webAssembly, module.handlers + function.firstHandler * sizeof(Wasm::HandlerInImage), function.numberOfHandlers)) {
                    if (static_cast<Wasm::HandlerType>(handler.type) != Wasm::HandlerType::Delegate)
                        addEntryInsideFrame(start + handler.code);
                }
            }
        }
        for (uint32_t stubs : { module.importStubs, module.javaScriptImportStubs }) {
            for (uint32_t stub : recordsInWasmDirectory<uint32_t>(webAssembly, stubs, module.numberOfImportedFunctions))
                addCalledEntry(start + stub);
        }
        for (const Wasm::JSEntrypointInImage& entry : recordsInWasmDirectory<Wasm::JSEntrypointInImage>(webAssembly, module.jsEntrypoints, module.numberOfJSEntrypoints))
            addCalledEntry(start + entry.code);
        for (const Wasm::FramelessRangeInImage& range : recordsInWasmDirectory<Wasm::FramelessRangeInImage>(webAssembly, module.framelessRanges, module.numberOfFramelessRanges))
            addCodeWhoseFaultsSignLinkRegister(start + range.begin, range.end - range.begin);
    }
    for (uint32_t target : webAssembly.targetsOfJumpTables) {
        bool hasNoFrame = m_codeWhoseFaultsSign.get((start + target) / sizeof(uint32_t));
        if (hasNoFrame)
            addCalledEntry(start + target);
        else
            addEntryInsideFrame(start + target);
    }
}
#endif

void checkReturnAddresses(const CodeLinkerInputs& inputs, [[maybe_unused]] const WebAssemblyCodeToCheck& webAssembly, const FFIStubs& ffiStubs, std::span<const uint8_t> code, BuildContext& context)
{
    const CodeLayout& layout = inputs.layout;
    ReturnAddressCheck check(code.first(layout.executableSize));
    addFunctionsToCheck(check, inputs);
    for (uint32_t copy : layout.sharedCodeCopies)
        check.addStubs(inputs.layoutInputs.sharedCode.blob, copy);
    addRegExpsToCheck(check, inputs);
    addFFIStubsToCheck(check, ffiStubs, layout.ffiCodeOffset);
#if ENABLE(WEBASSEMBLY)
    check.addWebAssembly(webAssembly, layout.webAssemblyCodeOffset);
#endif

    std::optional<ReturnAddressCheck::Violation> violation = check.run();
    if (!violation) {
        auto wasChecked = [&](uint32_t offset, size_t size) {
            return !size || check.hasReachedSomethingIn(offset, safeCast<uint32_t>(size));
        };
        RELEASE_ASSERT(wasChecked(layout.webAssemblyCodeOffset, inputs.layoutInputs.webAssemblyCodeSize));
        RELEASE_ASSERT(wasChecked(layout.ffiCodeOffset, layout.ffiCodeSize));
        RELEASE_ASSERT(wasChecked(layout.regExpCodeOffset, inputs.layoutInputs.regExpSection.dataOffset));
        return;
    }
    FunctionID function;
    auto nextBody = std::ranges::upper_bound(layout.bodyStarts, violation->offset);
    if (size_t position = nextBody - layout.bodyStarts.begin(); position && violation->offset - nextBody[-1] < inputs.layoutInputs.functions[position - 1].body.bytes.size())
        function = inputs.layoutInputs.functions[position - 1].unit.function;
    context.diagnostics().add(InternalError {
        .kind = InternalErrorKind::WrongValueInRegister,
        .function = function,
        .detail = makeString(nameOf(violation->rule), ", at 0x"_s, hex(violation->offset, Lowercase), " in the code of the image"_s),
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)
