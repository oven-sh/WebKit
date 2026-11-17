/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)

#include "AOTCodeLayout.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/SegmentedVector.h>

namespace JSC { namespace AOT {

struct FFIStubs;
struct PlacesInWebAssemblyCode;

struct WebAssemblyCodeToCheck {
    std::span<const uint8_t> directory;
    const PlacesInWebAssemblyCode& places;
};

// Follows linked code from its entries along every jump and call whose target it can tell, then whatever that leaves out, and finds
// what breaks a rule of aot/README.md, "Signed return addresses".
class JS_EXPORT_PRIVATE ReturnAddressCheck {
    WTF_MAKE_NONCOPYABLE(ReturnAddressCheck);
public:
    enum class Rule : uint8_t {
        SavesOnlyWhatIsSigned,
        SignsOnlyWhatWasNeverInMemory,
        AuthenticatesOnlyWhatIsSigned,
        ReturnsOnlyToWhatIsAuthenticated,
        LeavesNothingUncheckedInLinkRegister,
        UsesOneKeyAndModifier,
        KeepsAddressOfLabelInSight,
        JumpsToNothingOffTheStack,
        HasOnlyKnownInstructions,
    };

    struct Violation {
        uint32_t offset;
        Rule rule;
    };

    explicit ReturnAddressCheck(std::span<const uint8_t> code);

    void addCalledEntry(uint32_t offset);
    void addEntryInsideFrame(uint32_t offset);
    void addFunction(const CodeBody&, uint32_t offsetOfBody, uint32_t sizeOfDetoursBehindBody = 0);
    void addStubs(const StubBlob&, uint32_t offsetOfBlob);
    // Veneers cut into a copy. What nothing leads to here is followed in the original.
    void addCopyOfOtherCode(uint32_t offset, uint32_t size);
#if ENABLE(WEBASSEMBLY)
    void addWebAssembly(const WebAssemblyCodeToCheck&, uint32_t offsetOfCode);
#endif
    void addWordBehindCall(uint32_t offset) { m_wordsBehindCalls.set(offset / sizeof(uint32_t)); }
    void addData(uint32_t offset, uint32_t size);
    void addCodeWhoseFaultsSignLinkRegister(uint32_t offset, uint32_t size);
    // The first jump through a register from that offset on is the one that uses the table.
    void addJumpTable(uint32_t offset, Vector<uint32_t>&& targets);

    enum class Piece : uint8_t { Function, ArgumentThunks, Stubs, RegExp, WebAssembly };
    void addPiece(uint32_t offset, uint32_t size, Piece);

    std::optional<Violation> run();
    bool hasReachedFromEntry(uint32_t offset) const { return m_reachedFromEntries.get(offset / sizeof(uint32_t)); }
    bool hasReachedFromEntrySomethingIn(uint32_t offset, uint32_t size) const;

private:
    // A bit for each register. What came out of memory has none of the first three.
    struct State {
        friend bool operator==(const State&, const State&) = default;

        bool isPlain(unsigned reg) const { return (isTrusted & mayBeReturnedTo) >> reg & 1; }
        bool mayBeLeftToWhoeverSignsIt(unsigned reg) const { return (isTrusted & ~pointsIntoStackOnSomePath) >> reg & 1; }
        void set(unsigned reg, bool trusted, bool returnable, bool hasSignature);
        void forget(unsigned reg) { set(reg, false, false, false); }
        void setPlain(unsigned reg) { set(reg, true, true, false); }
        void copy(unsigned from, unsigned to);
        State keeping(uint32_t registers) const;
        State joinedWith(const State&) const;

        uint32_t isTrusted { 0 };
        uint32_t mayBeReturnedTo { 0 };
        uint32_t isSigned { 0 };
        uint32_t isUnsignedOnSomePath { 0 };
        uint32_t wasStrippedOnSomePath { 0 };
        uint32_t isMadeOfLabelOnSomePath { 0 };
        uint32_t pointsIntoStackOnSomePath { 0 };
        uint32_t cameOffStackOnSomePath { 0 };
    };

    struct PlacedPiece {
        uint32_t begin;
        uint32_t end;
        Piece piece;
    };

    uint32_t wordAt(uint32_t index) const;
    bool merge(uint32_t index, State&);
    void goTo(int64_t index, State);
    void enterWithPlainLinkRegister(uint32_t offset);
    bool isInPieceFollowed(int64_t index) const { return index >= m_beginOfPieceFollowed && index < m_endOfPieceFollowed; }
    std::optional<Rule> leaveFor(int64_t target, const State&);
    const PlacedPiece* pieceAt(uint32_t index) const;
    std::optional<int64_t> targetOfVeneerAt(uint32_t index) const;
    std::optional<uint32_t> stubThatNeverReturnsBehind(int64_t index) const;
    bool noteAddressTaken(uint32_t index, int64_t offset, const State&);
    bool mayJumpToWhatCameOffStack(uint32_t index, unsigned reg) const;
    std::optional<Rule> stepOverHint(uint32_t word, State&) const;
    std::optional<Rule> step(uint32_t word, State&) const;
    std::optional<Violation> follow(uint32_t index);
    uint32_t takeNextPlaceToLookAt();
    std::optional<Violation> followWhatWaits();
    void releaseStatesInside(const PlacedPiece&);

    std::span<const uint8_t> m_code;
    uint32_t m_numberOfWords;
    Vector<uint32_t> m_numbersOfStates;
    SegmentedVector<State, 1024> m_states;
    Vector<uint32_t> m_numbersReleased;
    HashMap<uint32_t, Vector<uint32_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_jumpTables;
    uint32_t m_numberThatWait { 0 };
    uint32_t m_behindAllThatWait { 0 };
    const PlacedPiece* m_pieceFollowed { nullptr };
    uint32_t m_beginOfPieceFollowed { 0 };
    uint32_t m_endOfPieceFollowed { 0 };
    uint32_t m_placeFollowed { UINT32_MAX };
    uint32_t m_firstLeftBehindInPiece { UINT32_MAX };
    uint32_t m_firstLeftBehindElsewhere { UINT32_MAX };
    Vector<uint32_t> m_entriesUnlessReached;
    Vector<PlacedPiece> m_pieces;
    BitVector m_waits;
    BitVector m_enteredFromOtherPieces;
    BitVector m_addressesThatNeedNoSignature;
    BitVector m_entriesOfFunctions;
    BitVector m_stubsThatNeverReturn;
    BitVector m_stubsThatReadThrowSiteBehindCall;
    BitVector m_wordsBehindCalls;
    BitVector m_data;
    BitVector m_codeWhoseFaultsSign;
    BitVector m_reached;
    BitVector m_reachedFromEntries;
};

JS_EXPORT_PRIVATE ASCIILiteral nameOf(ReturnAddressCheck::Rule);
JS_EXPORT_PRIVATE String nameOfStubAt(const StubBlob&, uint32_t offset);

JS_EXPORT_PRIVATE void checkReturnAddresses(const CodeLinkerInputs&, const WebAssemblyCodeToCheck&, const FFIStubs&, std::span<const uint8_t> code, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)
