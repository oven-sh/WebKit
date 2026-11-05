/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTCodeSites.h"
#include "AOTListOfUnit.h"
#include "AOTRecordTypes.h"
#include "AOTRelocation.h"
#include "AOTStubKey.h"
#include "AOTVector.h"
#include "CCallHelpers.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class BuildContext;
class StubCatalog;
struct CodeBody;

// The payload of a word on the stack is its offset from the frame pointer, or from the stack pointer at the entry of the thunk.
// The payload of ThrowWordWithMention is ThrowSiteWord::inRegister() of the first word of a throw site and, in place of its wide `a`, an index into the mentions of the function.
// applyNumbersToFunctions() makes a Constant of it, so lists are compared and thunks are generated with numbers alone.
enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant, Stack32OffStackPointer, Stack64OffStackPointer, ThrowWordWithMention };

// StubKey::bits() of the callee, hasImmediate | immediate if the thunk moves one (else 0), then two words a move.
// With immediateIsMention the immediate is an index into the mentions of the function, and applyNumbersToFunctions() puts the number in its place.
// Two lists with the same words share one thunk, whichever functions they come from.
using ArgumentMoves = ListOfUnit<uint64_t, 8>;

inline constexpr uint64_t hasImmediate = 1ull << 32;
inline constexpr uint64_t immediateIsMention = 1ull << 33;
inline constexpr size_t numberOfHeaderWords = 2;

// The caller of StubCalls::call passes the moves alone. StubCalls puts the two header words in front of them.
inline void appendArgumentMove(ArgumentMoves& moves, GPRReg destination, ArgumentSource source, uint64_t payload)
{
    moves.append(static_cast<uint64_t>(source) | static_cast<uint64_t>(destination) << 8);
    moves.append(payload);
}

// `index` is that of the first word of a move.
inline ArgumentSource sourceOfArgumentMove(const ArgumentMoves& moves, size_t index)
{
    return static_cast<ArgumentSource>(moves[index] & 0xff);
}

inline void setSourceOfArgumentMove(ArgumentMoves& moves, size_t index, ArgumentSource source, uint64_t payload)
{
    moves[index] = (moves[index] & ~static_cast<uint64_t>(0xff)) | static_cast<uint64_t>(source);
    moves[index + 1] = payload;
}

// Small constants are flags that many sites share, so one thunk serves them. Anything larger differs from site to site, and the site moves it.
inline constexpr uint64_t maxConstantInArgumentThunk = 255;

struct ArgumentThunks {
    RelocatableCode code;
    // [index in the list that generateArgumentThunks was given]
    Vector<uint32_t> offsets;
    Vector<StubKey> stubKeys;
};

// Image passes each distinct list once, sorted by its words.
std::expected<ArgumentThunks, LinkFailure> generateArgumentThunks(BuildContext&, const StubCatalog&, const Vector<const ArgumentMoves*>&);

// An immediate that only Image can count: an index into the MentionList of the function, of a mention that has a Number.
// Where StubCatalog::bindsImmediate() the key of the call waits for the number: CodeBody::numberedStubKeys. Under ImmediateBinding::InThunk a thunk moves it: immediateIsMention.
// Elsewhere the site moves it into the immediate register of the stub with RelocationRecorder::moveNumber.
struct NumberedImmediate {
    uint32_t mention;
};

struct RecordedCall {
    uint32_t relocation;
    OptionalIndex site;
    OptionalIndex firstReadSite;
    // Of a call through a register, which has no relocation: where it returns to.
    std::optional<CCallHelpers::Label> returnAddress { };
};

// The calls, jumps and addresses of stubs and functions in the code of one function. One per function being encoded.
struct RememberedStubKey {
    uint64_t bits;
    bool bindsImmediate;
    bool bindsOperationOffset;
    bool knowsImmediateGPR;
    GPRReg immediateGPR;
};

// What follows from a key alone. Whoever compiles one function after another keeps one and hands it to each StubCalls.
struct RememberedStubKeys {
    static constexpr unsigned logOfNumberOfKeys = 8;

    std::array<RememberedStubKey, 1u << logOfNumberOfKeys> keys;
};

class StubCalls {
public:
    StubCalls(const StubCatalog&, RelocationRecorder&, RememberedStubKeys* = nullptr);

    // Returns the index of the call in CodeBody::returnSites.
    uint32_t call(CCallHelpers&, StubKey, OptionalIndex site);
    uint32_t call(CCallHelpers&, StubKey, uint32_t immediate, OptionalIndex site, OptionalIndex firstReadSite = { });
    uint32_t call(CCallHelpers&, StubKey, NumberedImmediate, OptionalIndex site, OptionalIndex firstReadSite = { });
    // One near call to the thunk that makes the moves and jumps to the stub. For rare calls.
    uint32_t call(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    // The same, but for an immediate that the key does not bind: it differs from site to site, so the site moves it in front of the near call, and the thunk leaves the immediate register alone.
    uint32_t callWithImmediateOfSite(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&);
    // The site moves the number in front of the near call, and the thunk leaves the immediate register alone.
    uint32_t call(CCallHelpers&, StubKey, NumberedImmediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    void callForTwoReads(CCallHelpers&, StubKey, OptionalIndex firstRead, OptionalIndex secondRead);
    void tailCall(CCallHelpers&, StubKey);
    void tailCall(CCallHelpers&, StubKey, uint32_t immediate);
    void tailCall(CCallHelpers&, StubKey, NumberedImmediate);
    // The entry is one of the unit's own.
    void callFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry, OptionalIndex site);
    // The register holds the address of an entry of compiled code: what the method table holds.
    void callRegister(CCallHelpers&, GPRReg address, OptionalIndex site);
    void jumpToFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry);
    void moveAddressOfFunction(Typed::CodeUnit callee, uint16_t entry, GPRReg destination);
    void moveAddressOfStub(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, GPRReg destination);

    // Fills CodeBody::stubKeys, numberedStubKeys, callees, argumentMoves, returnSites and firstReadSites.
    // The buffer says where a call through a register returns to. Whoever makes no such call needs none.
    void describeCalls(std::span<const Relocation>, CodeBody&, LinkBuffer* = nullptr);

private:
    struct ResolvedKey {
        StubKey key;
        std::optional<uint32_t> immediateToMove;
    };

    ResolvedKey resolveKey(StubKey, std::optional<uint32_t> immediate) const;
    StubKey prepare(CCallHelpers&, StubKey, std::optional<uint32_t> immediate);
    CodeSymbol prepare(StubKey, NumberedImmediate);
    uint32_t indexOf(StubKey, std::optional<uint32_t> mentionOfImmediate = std::nullopt);
    uint32_t indexOf(Typed::CodeUnit);
    uint32_t emitCall(CCallHelpers&, StubKey called, CodeSymbol, const RecordedCall&);
    void emitJump(CodeSymbol);

    const StubCatalog& m_catalog;
    RelocationRecorder& m_relocations;
    std::unique_ptr<RememberedStubKeys> m_ownRememberedKeys;
    RememberedStubKeys& m_rememberedKeys;
    Vector<RecordedCall, 32> m_calls;
    Vector<StubKey> m_stubKeys;
    // [index in m_stubKeys]: the mention whose number the key waits for. Until Image puts it in, such a key has the immediate 0, and is none of the keys that have that immediate.
    Vector<std::optional<uint32_t>, 16> m_mentionsOfImmediates;
    Vector<uint32_t> m_placesOfStubKeys;
    Vector<Typed::CodeUnit> m_callees;
    ListOfUnit<ArgumentMoves> m_argumentMoves;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
