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

enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant, Stack32OffStackPointer, Stack64OffStackPointer, ThrowWordWithMention };

using ArgumentMoves = ListOfUnit<uint64_t, 8>;

inline constexpr uint64_t hasImmediate = 1ull << 32;
inline constexpr uint64_t immediateIsMention = 1ull << 33;
inline constexpr size_t numberOfHeaderWords = 2;

inline void appendArgumentMove(ArgumentMoves& moves, GPRReg destination, ArgumentSource source, uint64_t payload)
{
    moves.append(static_cast<uint64_t>(source) | static_cast<uint64_t>(destination) << 8);
    moves.append(payload);
}

inline ArgumentSource sourceOfArgumentMove(const ArgumentMoves& moves, size_t index)
{
    return static_cast<ArgumentSource>(moves[index] & 0xff);
}

inline void setSourceOfArgumentMove(ArgumentMoves& moves, size_t index, ArgumentSource source, uint64_t payload)
{
    moves[index] = (moves[index] & ~static_cast<uint64_t>(0xff)) | static_cast<uint64_t>(source);
    moves[index + 1] = payload;
}

inline constexpr uint64_t maxConstantInArgumentThunk = 255;

struct ArgumentThunks {
    RelocatableCode code;
    Vector<uint32_t> offsets;
    Vector<StubKey> stubKeys;
};

std::expected<ArgumentThunks, LinkFailure> generateArgumentThunks(BuildContext&, const StubCatalog&, const Vector<const ArgumentMoves*>&);

struct NumberedImmediate {
    uint32_t mention;
};

struct RecordedCall {
    uint32_t relocation;
    OptionalIndex site;
    OptionalIndex firstReadSite;
    std::optional<CCallHelpers::Label> returnAddress { };
};

struct RememberedStubKey {
    uint64_t bits;
    bool bindsImmediate;
    bool bindsOperationOffset;
    bool knowsImmediateGPR;
    GPRReg immediateGPR;
};

struct RememberedStubKeys {
    static constexpr unsigned logOfNumberOfKeys = 8;

    std::array<RememberedStubKey, 1u << logOfNumberOfKeys> keys;
};

class StubCalls {
public:
    StubCalls(const StubCatalog&, RelocationRecorder&, RememberedStubKeys* = nullptr);

    uint32_t call(CCallHelpers&, StubKey, OptionalIndex site);
    uint32_t call(CCallHelpers&, StubKey, uint32_t immediate, OptionalIndex site, OptionalIndex firstReadSite = { });
    uint32_t call(CCallHelpers&, StubKey, NumberedImmediate, OptionalIndex site, OptionalIndex firstReadSite = { });
    uint32_t call(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    uint32_t callWithImmediateOfSite(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&);
    uint32_t call(CCallHelpers&, StubKey, NumberedImmediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    void callForTwoReads(CCallHelpers&, StubKey, OptionalIndex firstRead, OptionalIndex secondRead);
    void tailCall(CCallHelpers&, StubKey);
    void tailCall(CCallHelpers&, StubKey, uint32_t immediate);
    void tailCall(CCallHelpers&, StubKey, NumberedImmediate);
    void callFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry, OptionalIndex site);
    void callRegister(CCallHelpers&, GPRReg address, OptionalIndex site);
    void jumpToFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry);
    void moveAddressOfFunction(Typed::CodeUnit callee, uint16_t entry, GPRReg destination);
    void moveAddressOfStub(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, GPRReg destination);

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
    Vector<std::optional<uint32_t>, 16> m_mentionsOfImmediates;
    Vector<uint32_t> m_placesOfStubKeys;
    Vector<Typed::CodeUnit> m_callees;
    ListOfUnit<ArgumentMoves> m_argumentMoves;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
