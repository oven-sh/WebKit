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
#include "CCallHelpers.h"
#include <expected>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
enum class ValidationFailureKind : uint8_t;
class CodeUnitRecorder;
class StubCatalog;
struct CodeBody;
struct StubSummary;

enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant };

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
};

enum class ReturnSiteMark : uint8_t { None, ResultTest };

class StubCalls {
public:
    StubCalls(const StubCatalog&, RelocationRecorder&, CodeUnitRecorder&);

    // What a failed check of registers reports.
    void setCaller(FunctionID caller) { m_caller = caller; }

    // Returns the index of the call in CodeBody::returnSites. Under validation no label beside the call is its address or its return address.
    uint32_t call(CCallHelpers&, StubKey, OptionalIndex site, const KeptRegisterCheck* = nullptr);
    void call(CCallHelpers&, StubKey, uint32_t immediate, OptionalIndex site, const KeptRegisterCheck* = nullptr);
    void call(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&);
    void callForTwoReads(CCallHelpers&, StubKey, OptionalIndex firstRead, OptionalIndex secondRead);
    void tailCall(CCallHelpers&, StubKey);
    void tailCall(CCallHelpers&, StubKey, uint32_t immediate);
    void callFunction(CCallHelpers&, FunctionID callee, uint16_t entry, OptionalIndex site, const KeptRegisterCheck* = nullptr, ReturnSiteMark = ReturnSiteMark::None);
    void jumpToFunction(CCallHelpers&, FunctionID callee, uint16_t entry);
    void emitDistanceToFunction(CCallHelpers&, FunctionID callee, uint16_t entry, int32_t addend);

    // Under validation: calls the operation that reports a failure, with its arguments in place.
    void reportChangedRegister(CCallHelpers&);

    void describeCalls(std::span<const Relocation>, CodeBody&);

private:
    struct ResolvedKey {
        StubKey key;
        std::optional<uint32_t> immediateToMove;
    };

    ResolvedKey resolveKey(StubKey, std::optional<uint32_t> immediate) const;
    StubKey prepare(CCallHelpers&, StubKey, std::optional<uint32_t> immediate);
    uint32_t indexOf(StubKey);
    uint32_t indexOf(FunctionID);
    uint32_t emitCall(CCallHelpers&, StubKey called, CodeSymbol, const RecordedCall&, const KeptRegisterCheck* = nullptr, ReturnSiteMark = ReturnSiteMark::None);
    void emitJump(CodeSymbol);

    const StubCatalog& m_catalog;
    RelocationRecorder& m_relocations;
    CodeUnitRecorder& m_units;
    FunctionID m_caller;
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
