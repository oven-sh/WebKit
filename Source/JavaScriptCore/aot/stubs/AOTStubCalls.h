/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTRecordTypes.h"
#include "AOTRelocation.h"
#include "AOTStubKey.h"
#include "AOTVector.h"
#include "CCallHelpers.h"
#include <expected>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;
class CodeUnitRecorder;
class StubCatalog;
struct CodeBody;
struct StubSummary;

using Typed::ValidationFailureKind;

// The payload of a word on the stack is its offset from the frame pointer, or from the stack pointer at the entry of the thunk.
enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant, Stack32OffStackPointer, Stack64OffStackPointer };

// StubKey::bits() of the callee, 1 << 32 | immediate if the thunk moves one (else 0), then two words a move.
using ArgumentMoves = Vector<uint64_t, 8>;

inline void appendArgumentMove(ArgumentMoves& moves, GPRReg destination, ArgumentSource source, uint64_t payload)
{
    moves.append(static_cast<uint64_t>(source) | static_cast<uint64_t>(destination) << 8);
    moves.append(payload);
}

// Small constants are flags that many sites share, so one thunk serves them. Anything larger differs from site to site, and the site moves it.
inline constexpr uint64_t maxConstantInArgumentThunk = 255;

struct ArgumentThunks {
    RelocatableCode code;
    Vector<uint32_t> offsets;
    Vector<StubKey> stubKeys;
};

std::expected<ArgumentThunks, LinkFailure> generateArgumentThunks(BuildContext&, const StubCatalog&, const Vector<const ArgumentMoves*>&);

// Under StubOptions::validates, for a call that may take words on the stack: the caller's frame holds the registers that the call keeps while it runs.
struct KeptRegisterCheck {
    RegisterSet clobbered;
    CCallHelpers::Address storage;
    FunctionID caller;
    uint32_t value;
    bool checksStackPointer { false };

    static constexpr size_t storageSize = (Reg::maxIndex() + 2) * sizeof(CPURegister);
};

struct RecordedCall {
    uint32_t relocation;
    OptionalIndex site;
    OptionalIndex firstReadSite;
    // A call through a register has no relocation: the label says where it returns to.
    std::optional<CCallHelpers::Label> returnAddress { };
};

class StubCalls {
public:
    StubCalls(const StubCatalog&, RelocationRecorder&, CodeUnitRecorder&);

    // What a failed check of registers reports.
    void setCaller(FunctionID caller) { m_caller = caller; }
    // Under validation each call of a stub is followed by a look at the registers that the stub's summary does not list, unless the build asks only about the program.
    void setChecksPreservedRegisters(bool checks) { m_checksPreservedRegisters = checks; }

    // Returns the index of the call in CodeBody::returnSites. Under validation no label beside the call is its address or its return address.
    uint32_t call(CCallHelpers&, StubKey, OptionalIndex site, const KeptRegisterCheck* = nullptr);
    uint32_t call(CCallHelpers&, StubKey, uint32_t immediate, OptionalIndex site, const KeptRegisterCheck* = nullptr, OptionalIndex firstReadSite = { });
    void call(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    void callForTwoReads(CCallHelpers&, StubKey, OptionalIndex firstRead, OptionalIndex secondRead);
    void tailCall(CCallHelpers&, StubKey);
    void tailCall(CCallHelpers&, StubKey, uint32_t immediate);
    void callFunction(CCallHelpers&, FunctionID callee, uint16_t entry, OptionalIndex site, const KeptRegisterCheck* = nullptr);
    void callRegister(CCallHelpers&, GPRReg address, OptionalIndex site, const KeptRegisterCheck* = nullptr);
    void jumpToFunction(CCallHelpers&, FunctionID callee, uint16_t entry);
    void moveAddressOfFunction(FunctionID callee, uint16_t entry, GPRReg destination);
    void moveAddressOfStub(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, GPRReg destination);

    // Under validation: calls the operation that reports a failure, with its arguments in place.
    void reportChangedRegister(CCallHelpers&);

    // The link buffer places the calls through registers; without one there may be none.
    void describeCalls(std::span<const Relocation>, CodeBody&, LinkBuffer* = nullptr);

private:
    struct ResolvedKey {
        StubKey key;
        std::optional<uint32_t> immediateToMove;
    };

    ResolvedKey resolveKey(StubKey, std::optional<uint32_t> immediate) const;
    StubKey prepare(CCallHelpers&, StubKey, std::optional<uint32_t> immediate);
    uint32_t indexOf(StubKey);
    uint32_t indexOf(FunctionID);
    uint32_t emitCall(CCallHelpers&, StubKey called, CodeSymbol, const RecordedCall&, const KeptRegisterCheck* = nullptr);
    void emitJump(CodeSymbol);

    const StubCatalog& m_catalog;
    RelocationRecorder& m_relocations;
    CodeUnitRecorder& m_units;
    FunctionID m_caller;
    bool m_checksPreservedRegisters { true };
    Vector<RecordedCall, 8> m_calls;
    Vector<StubKey> m_stubKeys;
    Vector<FunctionID> m_callees;
    Vector<ArgumentMoves> m_argumentMoves;
};

// Under StubOptions::validates. emitReport runs when the call changed a register that the summary does not list.
void emitCallCheckingPreservedRegisters(CCallHelpers&, const StubSummary&, const ScopedLambda<void()>& emitCall, const ScopedLambda<void()>& emitReport);
// The two halves of a check of the registers outside KeptRegisterCheck::clobbered. emitReport calls the operation with its arguments in place.
void emitStoresOfKeptRegisters(CCallHelpers&, const KeptRegisterCheck&);
void emitChecksOfKeptRegisters(CCallHelpers&, const KeptRegisterCheck&, ValidationFailureKind, std::optional<FunctionID> callee, const ScopedLambda<void()>& emitReport);

inline constexpr uint64_t hasImmediate = 1ull << 32;
inline constexpr size_t numberOfHeaderWords = 2;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
