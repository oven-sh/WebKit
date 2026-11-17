/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReturnAddressCheck.h"

#if ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)

#include "AOTBuildContext.h"
#include "AOTCallingConventions.h"
#include "AOTConvention.h"
#include "AOTFFIStubs.h"
#include "AOTOperationData.h"
#include "AOTPlacesInWebAssemblyCode.h"
#include "AOTRegExpCompiler.h"
#include "AOTSavedCodeAddresses.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsOperations.h"
#include "AOTStubsRegExp.h"
#include "AOTWasmInterface.h"
#include "FFIStubs.h"
#include "WasmHandlerInfo.h"
#include <wtf/HexNumber.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace A64 {

static constexpr unsigned ip1 = 17;
static constexpr unsigned framePointer = 29;
static constexpr unsigned lr = 30;
static constexpr unsigned spOrZero = 31;
static constexpr uint32_t everyGPR = ~(1u << spOrZero);
static constexpr uint32_t calleeSaves = (1u << framePointer) - (1u << 19);
static constexpr uint32_t mayOutliveCall = (1u << framePointer) - (1u << 2) - (3u << 16); // Many stubs keep what C++ would lose.

static constexpr uint32_t hint(unsigned number) { return 0xd503201f | number << 5; }
static constexpr uint32_t nop = hint(0);
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
static constexpr bool isTrap(uint32_t word) { return !(word >> 16) || matches(word, 0xffe0001f, 0xd4200000) || matches(word, 0xffe0001f, 0xd4400000); }
static constexpr bool isBranchOnFlagsOrZero(uint32_t word) { return matches(word, 0xff000010, 0x54000000) || matches(word, 0x7e000000, 0x34000000); }
static constexpr bool isBranchOnBit(uint32_t word) { return matches(word, 0x7e000000, 0x36000000); }
static constexpr bool isBranchOrSystem(uint32_t word) { return matches(word, 0x1c000000, 0x14000000); }
static constexpr bool isHintOrBarrier(uint32_t word) { return matches(word, 0xffffe01f, 0xd503201f); }
static constexpr bool isUnallocatedOrScalable(uint32_t word) { return matches(word, 0x18000000, 0); }
static constexpr bool isAuthenticationThatIsNoHint(uint32_t word) { return matches(word, 0xffff0000, 0xdac10000) || matches(word, 0xffe0fc00, 0x9ac03000); }
static constexpr bool isAddress(uint32_t word) { return matches(word, 0x9f000000, 0x10000000); }
static constexpr bool isAddressOfPage(uint32_t word) { return matches(word, 0x9f000000, 0x90000000); }
static constexpr bool isAddOrSubtractImmediate(uint32_t word) { return matches(word, 0xbf800000, 0x91000000); }
static constexpr bool isAddressOrAddressOfPage(uint32_t word) { return matches(word, 0x1f000000, 0x10000000); }
static constexpr bool isMove(uint32_t word) { return matches(word, 0xffe0ffe0, 0xaa0003e0); }
static constexpr bool isMoveOfZero(uint32_t word) { return matches(word, 0x7f9fffe0, 0x52800000) || (isMove(word) && rm(word) == spOrZero); }
static constexpr bool isMoveOfConstant(uint32_t word) { return matches(word, 0x3f800000, 0x12800000) || isMoveOfZero(word); }
static constexpr bool isComparisonOfRegisters(uint32_t word) { return matches(word, 0xffe0fc1f, 0xeb00001f); }
static constexpr bool isBranchIfEqual(uint32_t word) { return matches(word, 0xff00001f, 0x54000000); }
static constexpr bool isBranchUnlessEqual(uint32_t word) { return matches(word, 0xff00001f, 0x54000001); }
static constexpr bool isHintForOtherKeyOrNoModifier(uint32_t word) { return word == hint(8) || word == hint(12) || (matches(word, ~(7u << 5), hint(24)) && word != pacibsp && word != autibsp); }
static constexpr bool isLoadOrStore(uint32_t word) { return matches(word, 0x0a000000, 0x08000000); }
static constexpr bool isLoadOfByteIntoNothing(uint32_t word) { return matches(word, 0xfffffc1f, 0x3940001f); }
static constexpr bool isLoadOfLiteral(uint32_t word) { return matches(word, 0x3b000000, 0x18000000); }
static constexpr bool addsWholeIndexToBase(uint32_t word) { return matches(word, 0x3b207c00, 0x38206800); }
static constexpr bool isOfVectors(uint32_t word) { return matches(word, 0x0e000000, 0x0e000000); }
static constexpr bool namesStackPointerAsFirstSource(uint32_t word) { return rn(word) == spOrZero && (matches(word, 0x1f800000, 0x11000000) || matches(word, 0x1f200000, 0x0b200000)); }
static constexpr int64_t displacementOfAddress(uint32_t word) { return signedField(word, 5, 19) * 4 + field(word, 29, 2); }

static constexpr bool isSecondHalfOfAddress(uint32_t word, unsigned reg) { return isAddOrSubtractImmediate(word) && rn(word) == reg && rd(word) == reg; }

// MacroAssembler::movePCRelativeAddress(), as setRelocationDistance() or applyRelocation() leaves it.
static constexpr std::optional<int64_t> offsetWhoseAddressIsTaken(uint32_t offset, uint32_t word, uint32_t next)
{
    constexpr int64_t page = 4096;
    bool goesOn = isSecondHalfOfAddress(next, rd(word));
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
    if (isStoreOfPair && (rd(word) == framePointer || matches(word, 0xffff83e0, 0xa9000000 | framePointer << 5))) // stp fp, x, [..] and stp .., x, [fp]
        return 1u << rt2(word);
    if (matches(word, 0xffffffe0, 0xf9000400 | framePointer << 5) || matches(word, 0xffffffe0, 0xf8008000 | framePointer << 5)) // str and stur x, [fp, #8]
        return 1u << rd(word);
    return 0;
}

struct RegistersOfLoadOrStore {
    void write(unsigned reg) { written |= 1u << reg; }
    void store(unsigned reg) { stored |= 1u << reg; }
    void loadOrStore(bool isLoad, unsigned reg) { (isLoad ? written : stored) |= 1u << reg; }
    void writeBack(unsigned reg) { writtenBack |= 1u << reg; }

    uint32_t written { 0 };
    uint32_t stored { 0 };
    uint32_t writtenBack { 0 };
};

static std::optional<RegistersOfLoadOrStore> registersOfLoadOrStore(uint32_t word)
{
    if (matches(word, 0xff200400, 0xf8200400)) // ldraa, ldrab
        return std::nullopt;
    RegistersOfLoadOrStore result;
    bool isOfGPRs = !bit(word, 26);
    bool isLoad = bit(word, 22);
    if (isLoadOfLiteral(word)) {
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
            result.writeBack(rn(word));
    } else if (matches(word, 0x3f200c00, 0x38200000)) { // Atomic operations.
        result.write(rd(word));
        result.store(rm(word));
    } else if (matches(word, 0x3a000000, 0x38000000) || matches(word, 0x3f200c00, 0x19000000)) { // One register.
        bool isPrefetch = field(word, 30, 2) == 3 && field(word, 22, 2) == 2;
        if (isOfGPRs && !isPrefetch)
            result.loadOrStore(field(word, 22, 2), rd(word));
        if (matches(word, 0x3b200400, 0x38000400))
            result.writeBack(rn(word));
    } else if (matches(word, 0xbe000000, 0x0c000000)) { // Structures of vectors.
        if (bit(word, 23))
            result.writeBack(rn(word));
    } else
        return std::nullopt;
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

static bool movesFromGPRToFPR(uint32_t word)
{
    if (matches(word, 0x5f20fc00, 0x1e200000) || matches(word, 0x5f200000, 0x1e000000))
        return !movesFromFPRToGPR(word);
    return matches(word, 0xbfe0ec00, 0x0e000c00);
}

// Of an instruction that calculates with GPRs.
static uint32_t registersThatResultIsMadeOf(uint32_t word)
{
    uint32_t sources = 1u << rn(word);
    if (matches(word, 0x7f800000, 0x72800000)) // movk
        sources = 1u << rd(word);
    else if (matches(word, 0x7f800000, 0x33000000)) // bfm
        sources |= 1u << rd(word);
    else if (matches(word, 0x5fe00000, 0x5ac00000)) // clz and the like
        return sources & everyGPR;
    else if (matches(word, 0x1f800000, 0x13800000) || matches(word, 0x0e000000, 0x0a000000)) // extr, and all with two registers
        sources |= 1u << rm(word);
    if (matches(word, 0x1f000000, 0x1b000000)) // madd and the like
        sources |= 1u << rt2(word);
    return sources & everyGPR;
}

static constexpr bool isConditionalComparison(uint32_t word) { return matches(word, 0x1fe00000, 0x1a400000); }
static constexpr bool isConditionalComparisonOfRegisters(uint32_t word) { return matches(word, 0xffe00800, 0xfa400000); }

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
    for (uint32_t* bits : { &isTrusted, &mayBeReturnedTo, &isSigned, &isUnsignedOnSomePath, &wasStrippedOnSomePath, &isMadeOfLabelOnSomePath, &pointsIntoStackOnSomePath, &cameOffStackOnSomePath })
        *bits = (*bits & ~(1u << to)) | static_cast<uint32_t>(A64::bit(*bits, from)) << to;
    isUnsignedOnSomePath |= mayBeReturnedTo & ~isSigned & 1u << A64::lr;
}

auto ReturnAddressCheck::State::keeping(uint32_t registers) const -> State
{
    return {
        isTrusted & registers,
        mayBeReturnedTo & registers,
        isSigned & registers,
        isUnsignedOnSomePath & registers,
        wasStrippedOnSomePath & registers,
        isMadeOfLabelOnSomePath & registers,
        pointsIntoStackOnSomePath & registers,
        cameOffStackOnSomePath & registers,
    };
}

auto ReturnAddressCheck::State::joinedWith(const State& other) const -> State
{
    return {
        isTrusted & other.isTrusted,
        mayBeReturnedTo & other.mayBeReturnedTo,
        isSigned & other.isSigned,
        isUnsignedOnSomePath | other.isUnsignedOnSomePath,
        wasStrippedOnSomePath | other.wasStrippedOnSomePath,
        isMadeOfLabelOnSomePath | other.isMadeOfLabelOnSomePath,
        pointsIntoStackOnSomePath | other.pointsIntoStackOnSomePath,
        cameOffStackOnSomePath | other.cameOffStackOnSomePath,
    };
}

ReturnAddressCheck::ReturnAddressCheck(std::span<const uint8_t> code)
    : m_code(code)
    , m_numberOfWords(safeCast<uint32_t>(code.size() / sizeof(uint32_t)))
    , m_numbersOfStates(FillWith { }, m_numberOfWords, 0)
    , m_waits(m_numberOfWords + 1)
    , m_enteredFromOtherPieces(m_numberOfWords)
    , m_addressesThatNeedNoSignature(m_numberOfWords)
    , m_entriesOfFunctions(m_numberOfWords)
    , m_stubsThatNeverReturn(m_numberOfWords)
    , m_stubsThatReadThrowSiteBehindCall(m_numberOfWords)
    , m_wordsBehindCalls(m_numberOfWords)
    , m_data(m_numberOfWords)
    , m_codeWhoseFaultsSign(m_numberOfWords)
    , m_reached(m_numberOfWords)
{
}

void ReturnAddressCheck::enterWithPlainLinkRegister(uint32_t offset)
{
    State state;
    state.setPlain(A64::lr);
    goTo(offset / sizeof(uint32_t), state);
}

void ReturnAddressCheck::addCalledEntry(uint32_t offset)
{
    m_addressesThatNeedNoSignature.set(offset / sizeof(uint32_t));
    enterWithPlainLinkRegister(offset);
}

void ReturnAddressCheck::addEntryInsideFrame(uint32_t offset)
{
    State state;
    state.set(A64::lr, true, false, false);
    goTo(offset / sizeof(uint32_t), state);
}

void ReturnAddressCheck::addPiece(uint32_t offset, uint32_t size, Piece piece)
{
    if (size)
        m_pieces.append({ offset, offset + size, piece });
}

void ReturnAddressCheck::addFunction(const CodeBody& body, uint32_t offsetOfBody, uint32_t sizeOfDetoursBehindBody)
{
    addPiece(offsetOfBody, body.bytes.size() + sizeOfDetoursBehindBody, Piece::Function);
    for (const CodeEntry& entry : body.entries) {
        uint32_t offset = offsetOfBody + entry.offset;
        bool isWhereFunctionResumes = entry.id != primaryEntry && entry.id != Typed::fusedCallEntry;
        if (isWhereFunctionResumes) {
            enterWithPlainLinkRegister(offset);
            continue;
        }
        addCalledEntry(offset);
        m_entriesOfFunctions.set(offset / sizeof(uint32_t));
    }
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
        m_addressesThatNeedNoSignature.set(offset / sizeof(uint32_t));
        if (neverReturns(placement.key))
            m_stubsThatNeverReturn.set(offset / sizeof(uint32_t));
        if (placement.key == throwAtSite(ThrowSiteSource::WordsAfterCall, CallerFrame::HasFrame))
            m_stubsThatReadThrowSiteBehindCall.set(offset / sizeof(uint32_t));
        if (isEnteredByUnwinder(placement.key))
            addEntryInsideFrame(offset);
        else if (descriptorOf(placement.key.family()).linkage != StubLinkage::InternalEntry)
            addCalledEntry(offset);
        else if (!takesSecondReturnAddress(placement.key))
            m_entriesUnlessReached.append(offset);
    }
    for (const StubReturnSite& site : blob.returnSites) {
        uint32_t offset = offsetOfBlob + site.offset;
        bool isMarkThatNothingReturnsTo = site.kind == StubFrameKind::HostCall;
        m_addressesThatNeedNoSignature.set(offset / sizeof(uint32_t), isMarkThatNothingReturnsTo);
        m_entriesUnlessReached.append(offset);
    }
    addPiece(offsetOfBlob, blob.code.bytes.size(), Piece::Stubs);
}

static void markWordsOfCheckedCode(BitVector& words, uint32_t offset, uint32_t size)
{
    for (uint32_t index = offset / sizeof(uint32_t); index < (offset + size) / sizeof(uint32_t); ++index)
        words.set(index);
}

void ReturnAddressCheck::addCopyOfOtherCode(uint32_t offset, uint32_t size)
{
    markWordsOfCheckedCode(m_reached, offset, size);
}

void ReturnAddressCheck::addData(uint32_t offset, uint32_t size)
{
    markWordsOfCheckedCode(m_data, offset, size);
}

void ReturnAddressCheck::addCodeWhoseFaultsSignLinkRegister(uint32_t offset, uint32_t size)
{
    markWordsOfCheckedCode(m_codeWhoseFaultsSign, offset, size);
}

void ReturnAddressCheck::addJumpTable(uint32_t offset, Vector<uint32_t>&& targets)
{
    uint32_t index = offset / sizeof(uint32_t);
    while (index < m_numberOfWords && !A64::isJumpToRegister(wordAt(index)))
        ++index;
    m_jumpTables.add(index, WTF::move(targets));
}

uint32_t ReturnAddressCheck::wordAt(uint32_t index) const
{
    return WTF::unalignedLoad<uint32_t>(m_code.subspan(static_cast<size_t>(index) * sizeof(uint32_t), sizeof(uint32_t)).data());
}

bool ReturnAddressCheck::merge(uint32_t index, State& state)
{
    uint32_t& number = m_numbersOfStates[index];
    if (!number) {
        if (m_numbersReleased.isEmpty()) {
            m_states.append(state);
            number = m_states.size();
        } else {
            number = m_numbersReleased.takeLast();
            m_states[number - 1] = state;
        }
        return true;
    }
    State& known = m_states[number - 1];
    state = state.joinedWith(known);
    return std::exchange(known, state) != state;
}

void ReturnAddressCheck::goTo(int64_t index, State state)
{
    if (index < 0 || index >= m_numberOfWords)
        return;
    if (!isInPieceFollowed(index))
        m_enteredFromOtherPieces.quickSet(index);
    if (!merge(static_cast<uint32_t>(index), state) || m_waits.quickSet(index))
        return;
    ++m_numberThatWait;
    m_behindAllThatWait = std::max(m_behindAllThatWait, static_cast<uint32_t>(index + 1));
    if (index >= m_placeFollowed)
        return;
    uint32_t& first = index >= m_beginOfPieceFollowed ? m_firstLeftBehindInPiece : m_firstLeftBehindElsewhere;
    first = std::min(first, static_cast<uint32_t>(index));
}

auto ReturnAddressCheck::leaveFor(int64_t target, const State& state) -> std::optional<Rule>
{
    std::optional<Piece> pieceLeft = m_pieceFollowed && !isInPieceFollowed(target) ? std::optional(m_pieceFollowed->piece) : std::nullopt;
    if ((target >= 0 && m_entriesOfFunctions.get(target)) || pieceLeft == Piece::WebAssembly) {
        if (state.isMadeOfLabelOnSomePath)
            return Rule::KeepsAddressOfLabelInSight;
        goTo(target, state.keeping(1u << A64::lr));
        return std::nullopt;
    }
    // A stub is handed the target of a call, or a cell that holds it. Whether that has been on the stack depends on the program.
    State handedOn = state;
    if (pieceLeft && pieceLeft != Piece::Stubs) {
        handedOn.cameOffStackOnSomePath &= 1u << A64::lr;
        handedOn.pointsIntoStackOnSomePath &= 1u << A64::lr;
    }
    goTo(target, handedOn);
    return std::nullopt;
}

auto ReturnAddressCheck::pieceAt(uint32_t index) const -> const PlacedPiece*
{
    uint32_t offset = index * sizeof(uint32_t);
    auto next = std::ranges::upper_bound(m_pieces, offset, { }, &PlacedPiece::begin);
    return next != m_pieces.begin() && offset < next[-1].end ? &next[-1] : nullptr;
}

std::optional<int64_t> ReturnAddressCheck::targetOfVeneerAt(uint32_t index) const
{
    using namespace A64;
    uint32_t word = wordAt(index);
    if (index + 2 >= m_numberOfWords || !isSecondHalfOfAddress(wordAt(index + 1), rd(word)) || !isJumpToRegister(wordAt(index + 2)) || rn(wordAt(index + 2)) != rd(word))
        return std::nullopt;
    return *offsetWhoseAddressIsTaken(index * sizeof(uint32_t), word, wordAt(index + 1)) / static_cast<int64_t>(sizeof(uint32_t));
}

std::optional<uint32_t> ReturnAddressCheck::stubThatNeverReturnsBehind(int64_t index) const
{
    constexpr unsigned longestThunk = 64;
    for (unsigned steps = 0; steps < longestThunk && index >= 0 && index < m_numberOfWords; ++steps) {
        if (m_stubsThatNeverReturn.get(index))
            return static_cast<uint32_t>(index);
        uint32_t word = wordAt(static_cast<uint32_t>(index));
        if (A64::isJump(word))
            index += A64::signedField(word, 0, 26);
        else if (A64::isBranchOrSystem(word) || A64::isTrap(word))
            return std::nullopt;
        else
            ++index;
    }
    return std::nullopt;
}

// Returns whether the address has to be signed to go to memory.
bool ReturnAddressCheck::noteAddressTaken(uint32_t index, int64_t offset, const State& state)
{
    int64_t label = offset / static_cast<int64_t>(sizeof(uint32_t));
    if (offset < 0 || label >= m_numberOfWords || m_data.quickGet(label))
        return false;
    const PlacedPiece* piece = pieceAt(index);
    bool isPlaceToBacktrackTo = piece && piece->piece == Piece::RegExp && offset >= piece->begin && offset < piece->end;
    if (isPlaceToBacktrackTo) {
        goTo(label, state.keeping(1u << A64::lr));
        return false;
    }
    enterWithPlainLinkRegister(offset);
    return !m_addressesThatNeedNoSignature.quickGet(label) && wordAt(label) != A64::hint(hintAtTargetOfUnsignedJump);
}

bool ReturnAddressCheck::mayJumpToWhatCameOffStack(uint32_t index, unsigned reg) const
{
    const PlacedPiece* piece = pieceAt(index);
    if (!piece)
        return false;
    switch (piece->piece) {
    case Piece::Function:
        return reg == valueEntryGPR; // The target of a call, which is spilled like any other value.
    case Piece::ArgumentThunks:
    case Piece::Stubs:
        return false;
    case Piece::RegExp:
    case Piece::WebAssembly:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto ReturnAddressCheck::stepOverHint(uint32_t word, State& state) const -> std::optional<Rule>
{
    using namespace A64;
    if (isHintForOtherKeyOrNoModifier(word))
        return Rule::UsesOneKeyAndModifier;
    if (word == pacibsp || word == pacib1716) {
        unsigned reg = word == pacibsp ? lr : ip1;
        if (!state.isPlain(reg))
            return Rule::SignsOnlyWhatWasNeverInMemory;
        if (bit(state.isMadeOfLabelOnSomePath, reg))
            state.isMadeOfLabelOnSomePath = 0;
        state.set(reg, true, false, true);
    } else if (word == autibsp || word == autib1716) {
        unsigned reg = word == autibsp ? lr : ip1;
        if (bit(state.isUnsignedOnSomePath | state.wasStrippedOnSomePath, reg))
            return Rule::AuthenticatesOnlyWhatIsSigned;
        state.set(reg, false, true, false);
    } else if (word == xpaclri) {
        if (bit(state.isSigned, lr))
            state.setPlain(lr);
        else if (!state.isPlain(lr)) {
            state.forget(lr);
            state.wasStrippedOnSomePath |= 1u << lr;
        }
    }
    return std::nullopt;
}

// A label whose address was signed, compared or put in the link register has been used: what is left of it in other registers is dead.
// That needs one label at a time, which follow() sees to.
auto ReturnAddressCheck::step(uint32_t word, State& state) const -> std::optional<Rule>
{
    using namespace A64;
    uint32_t mayBeUnsigned = state.isUnsignedOnSomePath | state.wasStrippedOnSomePath | (state.mayBeReturnedTo & ~state.isSigned);
    uint32_t pointersIntoStack = state.pointsIntoStackOnSomePath | 1u << framePointer;
    if (isLoadOfByteIntoNothing(word)) {
        if (bit(state.mayBeReturnedTo, rn(word)))
            state.setPlain(rn(word));
        return std::nullopt;
    }
    if (isLoadOrStore(word)) {
        std::optional<RegistersOfLoadOrStore> registers = registersOfLoadOrStore(word);
        if (!registers)
            return Rule::HasOnlyKnownInstructions;
        if (registers->stored & (1u << lr | storedAsReturnAddressOfFrameRecord(word)) & mayBeUnsigned)
            return Rule::SavesOnlyWhatIsSigned;
        if (registers->stored & state.isMadeOfLabelOnSomePath)
            return Rule::KeepsAddressOfLabelInSight;
        bool isInStack = (!isLoadOfLiteral(word) && bit(pointersIntoStack | 1u << spOrZero, rn(word))) || (addsWholeIndexToBase(word) && bit(pointersIntoStack, rm(word)));
        uint32_t labelsMovedOn = state.isMadeOfLabelOnSomePath & registers->writtenBack & ~registers->written;
        state = state.keeping(~(registers->written | registers->writtenBack));
        state.isMadeOfLabelOnSomePath |= labelsMovedOn;
        if (isInStack) {
            state.cameOffStackOnSomePath |= registers->written & everyGPR;
            state.pointsIntoStackOnSomePath |= registers->writtenBack & everyGPR;
        }
        return std::nullopt;
    }
    if (isOfVectors(word)) {
        // Nothing follows what an FPR holds, so it counts as memory.
        if (movesFromGPRToFPR(word) && bit(state.isMadeOfLabelOnSomePath, rn(word)))
            return Rule::KeepsAddressOfLabelInSight;
        if (movesFromFPRToGPR(word)) {
            state.forget(rd(word));
            state.cameOffStackOnSomePath |= 1u << rd(word) & everyGPR;
        }
        return std::nullopt;
    }
    if (isUnallocatedOrScalable(word) || isAuthenticationThatIsNoHint(word))
        return Rule::HasOnlyKnownInstructions;
    if (isAddressOrAddressOfPage(word) || isMoveOfConstant(word)) {
        bool isPlain = rd(word) == lr || !isMoveOfConstant(word) || isMoveOfZero(word);
        state.set(rd(word), isPlain, isPlain, false);
        state.isUnsignedOnSomePath &= ~(1u << rd(word)) | 1u << lr; // The target of a call may outlive a call. follow() knows the labels.
        return std::nullopt;
    }

    uint32_t sources = registersThatResultIsMadeOf(word);
    bool usesLabel = sources & state.isMadeOfLabelOnSomePath;
    if (rd(word) == spOrZero || isConditionalComparison(word)) {
        if (usesLabel && (isComparisonOfRegisters(word) || isConditionalComparisonOfRegisters(word)))
            state.isMadeOfLabelOnSomePath = 0;
        return std::nullopt;
    }
    bool isMadeOfWhatCameOffStack = sources & (state.cameOffStackOnSomePath | state.wasStrippedOnSomePath);
    uint32_t mayBeInAddressOfSlot = state.isTrusted | (1u << framePointer & ~state.cameOffStackOnSomePath);
    if (isMove(word))
        state.copy(rm(word), rd(word));
    else if (isAddOrSubtractImmediate(word) && rn(word) != spOrZero && bit(mayBeUnsigned | state.mayBeReturnedTo, rn(word)))
        state.copy(rn(word), rd(word));
    else if (rd(word) == lr && !(sources & ~mayBeInAddressOfSlot))
        state.set(lr, true, false, false); // B3::extendedOffsetAddrRegister()
    else
        state.forget(rd(word));
    if (isMadeOfWhatCameOffStack)
        state.cameOffStackOnSomePath |= 1u << rd(word);
    if ((sources & pointersIntoStack) || namesStackPointerAsFirstSource(word))
        state.pointsIntoStackOnSomePath |= 1u << rd(word);
    if (usesLabel)
        state.isMadeOfLabelOnSomePath = rd(word) == lr ? 0 : state.isMadeOfLabelOnSomePath | 1u << rd(word);
    return std::nullopt;
}

auto ReturnAddressCheck::follow(uint32_t index) -> std::optional<Violation>
{
    using namespace A64;
    State state = m_states[m_numbersOfStates[index] - 1];
    uint32_t word = nop;
    for (const uint32_t first = index; index < m_numberOfWords; ++index) {
        uint32_t offset = index * sizeof(uint32_t);
        auto leave = [&](int64_t target) -> std::optional<Violation> {
            if (std::optional<Rule> broken = leaveFor(target, state))
                return Violation { offset, *broken };
            return std::nullopt;
        };
        uint32_t previousWord = std::exchange(word, wordAt(index));
        if (m_numbersOfStates[index]) {
            if (index != first && m_entriesOfFunctions.quickGet(index))
                return leave(index);
            bool hasChanged = merge(index, state);
            bool hasWaited = m_waits.quickClear(index);
            m_numberThatWait -= hasWaited;
            if (!hasWaited && !hasChanged)
                return std::nullopt;
            previousWord = nop;
        }
        if (m_data.quickGet(index))
            return std::nullopt;
        m_reached.quickSet(index);
        if (isTrap(word))
            return std::nullopt;

        if (!isBranchOrSystem(word)) {
            if (isLoadOrStore(word) && m_codeWhoseFaultsSign.quickGet(index) && !isLoadOfByteIntoNothing(word)) {
                bool isPartOfTeardown = rn(word) == spOrZero || rn(word) == framePointer;
                if (isPartOfTeardown ? !bit(state.isTrusted, lr) : !state.mayBeLeftToWhoeverSignsIt(lr))
                    return Violation { offset, Rule::SignsOnlyWhatWasNeverInMemory };
            }
            bool takesAddressOfLabel = false;
            if (isAddressOrAddressOfPage(word)) {
                if (std::optional<int64_t> target = targetOfVeneerAt(index)) {
                    m_reached.quickSet(index + 1);
                    m_reached.quickSet(index + 2);
                    state.forget(rd(word));
                    return leave(*target);
                }
                if (std::optional<int64_t> taken = offsetWhoseAddressIsTaken(offset, word, index + 1 < m_numberOfWords ? wordAt(index + 1) : 0))
                    takesAddressOfLabel = noteAddressTaken(index, *taken, state);
            }
            if (std::optional<Rule> broken = step(word, state))
                return Violation { offset, *broken };
            if (takesAddressOfLabel && rd(word) != lr) {
                if (state.isMadeOfLabelOnSomePath)
                    return Violation { offset, Rule::KeepsAddressOfLabelInSight };
                state.setPlain(rd(word));
                state.isMadeOfLabelOnSomePath = 1u << rd(word);
            }
            continue;
        }
        if (isHintOrBarrier(word)) {
            if (std::optional<Rule> broken = stepOverHint(word, state))
                return Violation { offset, *broken };
            continue;
        }
        if (isBranchOnFlagsOrZero(word) || isBranchOnBit(word)) {
            State ifEqual = state;
            if (isComparisonOfRegisters(previousWord) && state.isPlain(rn(previousWord)))
                ifEqual.copy(rn(previousWord), rm(previousWord));
            else if (isComparisonOfRegisters(previousWord) && state.isPlain(rm(previousWord)))
                ifEqual.copy(rm(previousWord), rn(previousWord));
            if (std::optional<Rule> broken = leaveFor(index + signedField(word, 5, isBranchOnBit(word) ? 14 : 19), isBranchIfEqual(word) ? ifEqual : state))
                return Violation { offset, *broken };
            if (isBranchUnlessEqual(word))
                state = ifEqual;
            continue;
        }
        if (isJump(word))
            return leave(index + signedField(word, 0, 26));
        if (isJumpToRegister(word)) {
            if (auto table = m_jumpTables.find(index); table != m_jumpTables.end()) {
                for (uint32_t target : table->value)
                    goTo(target / sizeof(uint32_t), state);
                return std::nullopt;
            }
        }
        bool leavesForUnknownCode = isReturn(word) || isJumpToRegister(word) || isCallOfRegister(word);
        if (leavesForUnknownCode && (state.isMadeOfLabelOnSomePath & ~(1u << rn(word))))
            return Violation { offset, Rule::KeepsAddressOfLabelInSight };
        if (isReturn(word)) {
            if (!bit(state.mayBeReturnedTo, rn(word)))
                return Violation { offset, Rule::ReturnsOnlyToWhatIsAuthenticated };
            if (rn(word) != lr && !state.mayBeLeftToWhoeverSignsIt(lr))
                return Violation { offset, Rule::LeavesNothingUncheckedInLinkRegister };
            return std::nullopt;
        }
        if ((isJumpToRegister(word) || isCallOfRegister(word)) && bit(state.wasStrippedOnSomePath, rn(word)))
            return Violation { offset, Rule::ReturnsOnlyToWhatIsAuthenticated };
        if (isJumpToRegister(word)) {
            if (bit(state.cameOffStackOnSomePath, rn(word)) && !mayJumpToWhatCameOffStack(index, rn(word)))
                return Violation { offset, Rule::JumpsToNothingOffTheStack };
            if (!state.mayBeLeftToWhoeverSignsIt(lr))
                return Violation { offset, Rule::LeavesNothingUncheckedInLinkRegister };
            return std::nullopt;
        }
        if (!isCall(word) && !isCallOfRegister(word))
            return Violation { offset, Rule::HasOnlyKnownInstructions };

        if (state.isMadeOfLabelOnSomePath & calleeSaves)
            return Violation { offset, Rule::KeepsAddressOfLabelInSight };
        state.setPlain(lr);
        if (isCall(word)) {
            int64_t callee = index + signedField(word, 0, 26);
            if (std::optional<Violation> violation = leave(callee))
                return violation;
            if (std::optional<uint32_t> stub = stubThatNeverReturnsBehind(callee)) {
                if (m_stubsThatReadThrowSiteBehindCall.quickGet(*stub) && index + 1 < m_numberOfWords)
                    addData(offset + sizeof(uint32_t), std::bit_cast<ThrowSiteWord>(wordAt(index + 1)).numberOfWordsOfSite() * sizeof(uint32_t));
                return std::nullopt;
            }
        }
        // A callee may have kept any other register in memory.
        State behindCall;
        behindCall.setPlain(lr);
        behindCall.pointsIntoStackOnSomePath = state.pointsIntoStackOnSomePath & mayOutliveCall;
        behindCall.cameOffStackOnSomePath = (state.cameOffStackOnSomePath | state.isUnsignedOnSomePath) & (mayOutliveCall | 1u << framePointer);
        state = behindCall;
        while (m_wordsBehindCalls.get(index + 1))
            ++index;
    }
    return std::nullopt;
}

// By address, so that a place has heard of most that lead to it when it is followed. A jump back waits for the end of its piece, where
// the rare paths lie that all jump back, and a stub for the last of its callers.
uint32_t ReturnAddressCheck::takeNextPlaceToLookAt()
{
    bool goesBackInPiece = m_firstLeftBehindInPiece != UINT32_MAX;
    uint32_t end = goesBackInPiece ? std::min(m_endOfPieceFollowed, m_behindAllThatWait) : m_behindAllThatWait;
    bool endWaits = m_waits.quickSet(end); // Stops the search there.
    uint32_t index = m_waits.findBit(std::min(m_placeFollowed, end), true);
    m_waits.quickSet(end, endWaits);
    if (index < end)
        return index;
    return std::exchange(goesBackInPiece ? m_firstLeftBehindInPiece : m_firstLeftBehindElsewhere, UINT32_MAX);
}

auto ReturnAddressCheck::followWhatWaits() -> std::optional<Violation>
{
    while (m_numberThatWait) {
        uint32_t index = takeNextPlaceToLookAt();
        if (!isInPieceFollowed(index)) {
            if (m_pieceFollowed && (m_pieceFollowed->piece == Piece::Function || m_pieceFollowed->piece == Piece::WebAssembly))
                releaseStatesInside(*m_pieceFollowed);
            m_pieceFollowed = pieceAt(index);
            m_beginOfPieceFollowed = m_pieceFollowed ? m_pieceFollowed->begin / sizeof(uint32_t) : index;
            m_endOfPieceFollowed = m_pieceFollowed ? m_pieceFollowed->end / sizeof(uint32_t) : index + 1;
        }
        m_placeFollowed = index;
        if (!m_waits.quickGet(index))
            continue;
        if (std::optional<Violation> violation = follow(index))
            return violation;
    }
    m_behindAllThatWait = 0;
    m_placeFollowed = UINT32_MAX;
    m_firstLeftBehindInPiece = UINT32_MAX;
    m_firstLeftBehindElsewhere = UINT32_MAX;
    return std::nullopt;
}

// A piece is followed again when a place where it is entered hears something new. That comes to an end, because those places remember.
void ReturnAddressCheck::releaseStatesInside(const PlacedPiece& piece)
{
    for (uint32_t index = piece.begin / sizeof(uint32_t); index < piece.end / sizeof(uint32_t); ++index) {
        if (m_numbersOfStates[index] && !m_enteredFromOtherPieces.quickGet(index))
            m_numbersReleased.append(std::exchange(m_numbersOfStates[index], 0));
    }
}

auto ReturnAddressCheck::run() -> std::optional<Violation>
{
    AOT::sort(m_pieces, { }, &PlacedPiece::begin);
    if (std::optional<Violation> violation = followWhatWaits())
        return violation;
    for (uint32_t entry : m_entriesUnlessReached) {
        if (m_reached.get(entry / sizeof(uint32_t)))
            continue;
        enterWithPlainLinkRegister(entry);
        if (std::optional<Violation> violation = followWhatWaits())
            return violation;
    }
    m_reachedFromEntries = m_reached;

    // Dead code, or code that is entered in a way that nobody has told of. It breaks a rule if it does so however it is entered.
    constexpr State stateThatAllowsEverything { ~0u, ~0u, ~0u };
    for (uint32_t index = m_reached.findBit(0, false); index < m_numberOfWords; index = m_reached.findBit(index + 1, false)) {
        if (m_data.quickGet(index) || m_wordsBehindCalls.quickGet(index) || A64::isTrap(wordAt(index)))
            continue;
        goTo(index, stateThatAllowsEverything);
        if (std::optional<Violation> violation = followWhatWaits())
            return violation;
    }
    return std::nullopt;
}

bool ReturnAddressCheck::hasReachedFromEntrySomethingIn(uint32_t offset, uint32_t size) const
{
    return m_reachedFromEntries.findBit(offset / sizeof(uint32_t), true) < (offset + size) / sizeof(uint32_t);
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
    case Rule::KeepsAddressOfLabelInSight:
        return "the address of a label goes to memory, or to code that may put it there, without a signature"_s;
    case Rule::JumpsToNothingOffTheStack:
        return "what is jumped to may have come off the stack"_s;
    case Rule::HasOnlyKnownInstructions:
        return "the check does not know this instruction, or data lies here that it was not told of"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String nameOfStubAt(const StubBlob& blob, uint32_t offset)
{
    const StubPlacement* found = nullptr;
    for (const StubPlacement& placement : blob.placements) {
        if (placement.offset <= offset && (!found || placement.offset > found->offset))
            found = &placement;
    }
    return found ? makeString(descriptorOf(found->key.family()).name, '+', offset - found->offset) : String::number(offset);
}

static void addFunctionsToCheck(ReturnAddressCheck& check, const CodeLinkerInputs& inputs)
{
    const LinkedFunctions& functions = inputs.layoutInputs.functions;
    for (uint32_t position = 0; position < functions.size(); ++position) {
        const CodeBody& body = functions[position].body;
        if (body.bytes.isEmpty())
            continue;
        uint32_t start = inputs.layout.bodyStarts[position];
        check.addFunction(body, start, inputs.layoutInputs.sizeOfFarInstanceWords(position));
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
        if (!matcher)
            continue;
        check.addData(start + *matcher - sizeOfMatcherHeader, sizeOfMatcherHeader);
        check.addCalledEntry(start + *matcher);
    }
    for (const RegExpSectionItem& item : section.items) {
        if (item.kind == RegExpSectionItem::Kind::Body)
            check.addPiece(start + item.offset, item.size, ReturnAddressCheck::Piece::RegExp);
    }
}

static void addFFIStubsToCheck(ReturnAddressCheck& check, const FFIStubs& stubs, uint32_t start)
{
    if (stubs.directory.isEmpty())
        return;
    const FFI::StubDirectory& directory = spanReinterpretCast<const FFI::StubDirectory>(stubs.directory.span().first(sizeof(FFI::StubDirectory)))[0];
    check.addData(start + stubs.callbackContextOffset, stubs.callbackContextSize);
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
                addPiece(start + function.entry, function.size, Piece::WebAssembly);
                addCalledEntry(start + function.entry);
                m_entriesOfFunctions.set((start + function.entry) / sizeof(uint32_t));
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
    for (PlacesInWebAssemblyCode::Data data : webAssembly.places.data)
        addData(start + data.offset, data.size);
    for (PlacesInWebAssemblyCode::JumpThroughTable jump : webAssembly.places.jumpsThroughTables) {
        uint32_t table = start + jump.offsetOfTable;
        addJumpTable(start + jump.offsetBehindJump - sizeof(uint32_t), Vector<uint32_t>(jump.numberOfEntries, [&](size_t entry) {
            return table + wordAt(table / sizeof(uint32_t) + entry);
        }));
    }
}
#endif

void checkReturnAddresses(const CodeLinkerInputs& inputs, [[maybe_unused]] const WebAssemblyCodeToCheck& webAssembly, const FFIStubs& ffiStubs, std::span<const uint8_t> code, BuildContext& context)
{
    const CodeLayout& layout = inputs.layout;
    ReturnAddressCheck check(code.first(layout.executableSize));
    if (layout.constantsOffset < layout.executableSize)
        check.addData(layout.constantsOffset, layout.executableSize - layout.constantsOffset);
    addFunctionsToCheck(check, inputs);
    const SharedCode& sharedCode = inputs.layoutInputs.sharedCode;
    for (uint32_t copy : layout.sharedCodeCopies) {
        check.addStubs(sharedCode.blob, copy);
        check.addPiece(copy + sharedCode.argumentThunksOffset, sharedCode.argumentThunks.code.bytes.size(), ReturnAddressCheck::Piece::ArgumentThunks);
        if (copy != layout.sharedCodeCopies[0])
            check.addCopyOfOtherCode(copy, sharedCode.veneersOffset);
    }
    addRegExpsToCheck(check, inputs);
    addFFIStubsToCheck(check, ffiStubs, layout.ffiCodeOffset);
#if ENABLE(WEBASSEMBLY)
    check.addWebAssembly(webAssembly, layout.webAssemblyCodeOffset);
#endif

    std::optional<ReturnAddressCheck::Violation> violation = check.run();
    if (!violation) {
        auto wasChecked = [&](uint32_t offset, size_t size) {
            return !size || check.hasReachedFromEntrySomethingIn(offset, safeCast<uint32_t>(size));
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
    String stub;
    for (uint32_t copy : layout.sharedCodeCopies) {
        if (violation->offset - copy < sharedCode.blob.code.bytes.size())
            stub = makeString(" ("_s, nameOfStubAt(sharedCode.blob, violation->offset - copy), ')');
    }
    context.diagnostics().add(InternalError {
        .kind = InternalErrorKind::WrongValueInRegister,
        .function = function,
        .detail = makeString(nameOf(violation->rule), ", at 0x"_s, hex(violation->offset, Lowercase), " in the code of the image"_s, stub, ". See \"Signed return addresses\" in aot/README.md"_s),
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)
