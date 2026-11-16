/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)

#include "AOTCodeLayout.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

struct FFIStubs;

struct WebAssemblyCodeToCheck {
    std::span<const uint8_t> directory;
    std::span<const uint32_t> targetsOfJumpTables;
};

// Follows linked code from its entries along every jump and call whose target it can tell, and finds what breaks a rule of
// aot/README.md, "Signed return addresses".
class JS_EXPORT_PRIVATE ReturnAddressCheck {
    WTF_MAKE_NONCOPYABLE(ReturnAddressCheck);
public:
    enum class Rule : uint8_t {
        SavesOnlyWhatIsSigned,
        SignsOnlyWhatWasNeverInMemory,
        AuthenticatesOnlyWhatIsSigned,
        ReturnsOnlyToWhatIsAuthenticated,
        LeavesNothingUncheckedInLinkRegister,
    };

    struct Violation {
        uint32_t offset;
        Rule rule;
    };

    explicit ReturnAddressCheck(std::span<const uint8_t> code);

    void addCalledEntry(uint32_t offset);
    void addEntryInsideFrame(uint32_t offset);
    void addStubs(const StubBlob&, uint32_t offsetOfBlob);
#if ENABLE(WEBASSEMBLY)
    void addWebAssembly(const WebAssemblyCodeToCheck&, uint32_t offsetOfCode);
#endif
    void addWordBehindCall(uint32_t offset) { m_wordsBehindCalls.set(offset / sizeof(uint32_t)); }
    void addData(uint32_t offset, uint32_t size);
    void addCodeWhoseFaultsSignLinkRegister(uint32_t offset, uint32_t size);
    // The first jump through a register behind the place that takes the address of the table is the one that uses it.
    void addJumpTable(uint32_t offsetOfAddress, Vector<uint32_t>&& targets);

    enum class Labels : bool { AreJumpedTo, AreCalledOrReturnedTo };
    void addCodeThatTakesAddressesOfItsLabels(uint32_t offset, uint32_t size, Labels labels) { m_codeWithLabels.append({ offset, offset + size, labels }); }

    std::optional<Violation> run();
    bool hasReached(uint32_t offset) const { return m_reached.get(offset / sizeof(uint32_t)); }
    bool hasReachedSomethingIn(uint32_t offset, uint32_t size) const;

private:
    // A bit for each register. What came out of memory has none of them.
    struct State {
        friend bool operator==(const State&, const State&) = default;

        bool isPlain(unsigned reg) const { return (isTrusted & mayBeReturnedTo) >> reg & 1; }
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
    };

    struct CodeWithLabels {
        uint32_t begin;
        uint32_t end;
        Labels labels;
    };

    uint32_t wordAt(uint32_t index) const;
    bool merge(uint32_t index, State&);
    void goTo(int64_t index, State);
    bool leadsToStubThatNeverReturns(int64_t index) const;
    void noteAddressOfLabel(uint32_t index, int64_t offsetOfLabel, const State&);
    std::optional<Rule> step(uint32_t word, State&) const;
    std::optional<Violation> follow(uint32_t index);

    std::span<const uint8_t> m_code;
    uint32_t m_numberOfWords;
    HashMap<uint32_t, State, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_states;
    HashMap<uint32_t, Vector<uint32_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_jumpTables;
    Vector<uint32_t> m_worklist;
    Vector<uint32_t> m_entriesUnlessReached;
    Vector<CodeWithLabels> m_codeWithLabels;
    BitVector m_stubsThatNeverReturn;
    BitVector m_wordsBehindCalls;
    BitVector m_data;
    BitVector m_codeWhoseFaultsSign;
    BitVector m_reached;
};

JS_EXPORT_PRIVATE ASCIILiteral nameOf(ReturnAddressCheck::Rule);

void checkReturnAddresses(const CodeLinkerInputs&, const WebAssemblyCodeToCheck&, const FFIStubs&, std::span<const uint8_t> code, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(RETURN_ADDRESS_SIGNING)
