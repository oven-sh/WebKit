/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCalls.h"

#if ENABLE(AOT)

#include "AOTCodeUnits.h"
#include "AOTCompiledCode.h"
#include "AOTImageFormat.h"
#include "AOTRuntimeValidation.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsCalls.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPolls.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

static GPRReg immediateGPR(const StubCatalog& catalog, StubKey key)
{
    std::optional<GPRReg> immediate = catalog.summaryOf(key).signature.immediate;
    RELEASE_ASSERT(immediate);
    return *immediate;
}

static RegisterSet destinationRegistersOf(const ArgumentMoves& words)
{
    RegisterSet destinations;
    for (size_t i = numberOfHeaderWords; i < words.size(); i += 2)
        destinations.add(static_cast<GPRReg>(words[i] >> 8));
    return destinations;
}

static bool returnsToItsCallSite(const StubCatalog& catalog, StubKey called)
{
    if (neverReturns(called))
        return false;
    // It replaces its return address, so no code after the call runs, and the PrologueCheck names the call by the label before it.
    if (called == pollAtPrologue())
        return false;
    // A stub that suspends copies the frame from sp up, and control comes back with the registers of the code that resumes it.
    return !catalog.effectsOf(called).maySuspend;
}

static bool canCheckPreservedRegisters(const StubCatalog& catalog, StubKey called)
{
    return !catalog.summaryOf(called).signature.usesCallConvention && returnsToItsCallSite(catalog, called);
}

template<typename T>
static uint32_t findOrAppend(Vector<T>& list, T value)
{
    size_t index = list.find(value);
    if (index == notFound) {
        index = list.size();
        list.append(value);
    }
    return safeCast<uint32_t>(index);
}

StubCalls::StubCalls(const StubCatalog& catalog, RelocationRecorder& relocations, CodeUnitRecorder& units)
    : m_catalog(catalog)
    , m_relocations(relocations)
    , m_units(units)
{
}

StubCalls::ResolvedKey StubCalls::resolveKey(StubKey key, std::optional<uint32_t> immediate) const
{
    RELEASE_ASSERT(exists(key) && !key.immediate() && key.callerFrame() != CallerFrame::Frameless);
    if (immediate && m_catalog.bindsImmediate(key, *immediate))
        return { key.withImmediate(*immediate), std::nullopt };
    RELEASE_ASSERT(descriptorOf(key.family()).binding != ImmediateBinding::OperationOffset);
    return { key, immediate };
}

StubKey StubCalls::prepare(CCallHelpers& jit, StubKey key, std::optional<uint32_t> immediate)
{
    auto [finalKey, immediateToMove] = resolveKey(key, immediate);
    if (immediateToMove) {
        GPRReg destination = immediateGPR(m_catalog, finalKey);
        CCallHelpers::Label begin = jit.label();
        jit.move(CCallHelpers::TrustedImm32(*immediateToMove), destination);
        m_units.addPlain(begin, jit.label(), RegisterSet { destination });
    }
    return finalKey;
}

uint32_t StubCalls::indexOf(StubKey key)
{
    return findOrAppend(m_stubKeys, key);
}

uint32_t StubCalls::indexOf(FunctionID callee)
{
    return findOrAppend(m_callees, callee);
}

void StubCalls::reportChangedRegister(CCallHelpers& jit)
{
    StubKey report = tailCallOperation(OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(Entry::operationAOTReportValidationFailure));
    uint32_t relocation = m_relocations.call({ SymbolSpace::Stub, indexOf(report) });
    m_calls.append({ relocation, { }, { } });
    jit.breakpoint();
}

uint32_t StubCalls::emitCall(CCallHelpers& jit, StubKey called, CodeSymbol symbol, const RecordedCall& record, const KeptRegisterCheck* keptRegisters)
{
    // A check may emit a call of its own behind this one, and the records are in the order of the code.
    uint32_t call = 0;
    auto emit = [&] {
        call = safeCast<uint32_t>(m_calls.size());
        m_calls.append({ m_relocations.call(symbol), record.site, record.firstReadSite });
    };
    bool isFunction = symbol.space == SymbolSpace::Function;
    if (m_catalog.options().validates && m_checksPreservedRegisters && !isFunction && canCheckPreservedRegisters(m_catalog, called)) {
        StubSummary summary = m_catalog.summaryOf(called);
        if (symbol.space == SymbolSpace::ArgumentThunk)
            summary.clobbered.merge(destinationRegistersOf(m_argumentMoves[symbol.index]));
        emitCallCheckingPreservedRegisters(jit, summary, emit, [&] {
            jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(ChangedRegister::encodeForStub(called.family())))), operationArgumentGPR(1));
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(ValidationFailureKind::CallChangedKeptRegister)), operationArgumentGPR(2));
            jit.move(CCallHelpers::TrustedImm32(m_caller.index()), operationArgumentGPR(3));
            jit.move(CCallHelpers::TrustedImm32(Typed::ValueID().index()), operationArgumentGPR(4));
            reportChangedRegister(jit);
        });
    } else if (keptRegisters && (isFunction || returnsToItsCallSite(m_catalog, called))) {
        RELEASE_ASSERT(m_catalog.options().validates);
        std::optional<FunctionID> callee;
        if (isFunction)
            callee = m_callees[symbol.index];
        emitStoresOfKeptRegisters(jit, *keptRegisters);
        emit();
        emitChecksOfKeptRegisters(jit, *keptRegisters, ValidationFailureKind::CallChangedKeptRegister, callee, [&] {
            reportChangedRegister(jit);
        });
    } else
        emit();
    return call;
}

void StubCalls::emitJump(CodeSymbol symbol)
{
    m_relocations.jump(symbol);
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, OptionalIndex site, const KeptRegisterCheck* keptRegisters)
{
    StubKey finalKey = prepare(jit, key, std::nullopt);
    return emitCall(jit, finalKey, { SymbolSpace::Stub, indexOf(finalKey) }, { 0, site, { } }, keptRegisters);
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, uint32_t immediate, OptionalIndex site, const KeptRegisterCheck* keptRegisters)
{
    StubKey finalKey = prepare(jit, key, immediate);
    return emitCall(jit, finalKey, { SymbolSpace::Stub, indexOf(finalKey) }, { 0, site, { } }, keptRegisters);
}

void StubCalls::call(CCallHelpers& jit, StubKey key, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves& moves)
{
    RELEASE_ASSERT(!moves.isEmpty() && !(moves.size() % 2));
    auto [finalKey, immediateToMove] = resolveKey(key, immediate);
    RELEASE_ASSERT(!immediateToMove || m_catalog.summaryOf(finalKey).signature.immediate);
    ArgumentMoves words;
    words.append(finalKey.bits());
    words.append(immediateToMove ? hasImmediate | *immediateToMove : 0);
    words.appendVector(moves);
    uint32_t index = safeCast<uint32_t>(m_argumentMoves.size());
    m_argumentMoves.append(WTF::move(words));
    emitCall(jit, finalKey, { SymbolSpace::ArgumentThunk, index }, { 0, site, { } });
}

void StubCalls::callForTwoReads(CCallHelpers& jit, StubKey key, OptionalIndex firstRead, OptionalIndex secondRead)
{
    StubKey finalKey = prepare(jit, key, std::nullopt);
    emitCall(jit, finalKey, { SymbolSpace::Stub, indexOf(finalKey) }, { 0, secondRead, firstRead });
}

void StubCalls::tailCall(CCallHelpers& jit, StubKey key)
{
    emitJump({ SymbolSpace::Stub, indexOf(prepare(jit, key, std::nullopt)) });
}

void StubCalls::tailCall(CCallHelpers& jit, StubKey key, uint32_t immediate)
{
    emitJump({ SymbolSpace::Stub, indexOf(prepare(jit, key, immediate)) });
}

void StubCalls::callFunction(CCallHelpers& jit, FunctionID callee, uint16_t entry, OptionalIndex site, const KeptRegisterCheck* keptRegisters)
{
    emitCall(jit, StubKey(), { SymbolSpace::Function, indexOf(callee), entry }, { 0, site, { } }, keptRegisters);
}

void StubCalls::callRegister(CCallHelpers& jit, GPRReg address, OptionalIndex site, const KeptRegisterCheck* keptRegisters)
{
    auto emit = [&] {
        CCallHelpers::Label begin = jit.label();
        jit.call(address, JSEntryPtrTag);
        CCallHelpers::Label end = jit.label();
        m_units.pin(begin, end);
        m_calls.append({ 0, site, { }, end });
    };
    // As for a call of the stub that jumps to the address.
    if (!keptRegisters || !returnsToItsCallSite(m_catalog, callResolvedValue())) {
        emit();
        return;
    }
    RELEASE_ASSERT(m_catalog.options().validates);
    emitStoresOfKeptRegisters(jit, *keptRegisters);
    emit();
    emitChecksOfKeptRegisters(jit, *keptRegisters, ValidationFailureKind::CallChangedKeptRegister, std::nullopt, [&] {
        reportChangedRegister(jit);
    });
}

void StubCalls::jumpToFunction(CCallHelpers&, FunctionID callee, uint16_t entry)
{
    emitJump({ SymbolSpace::Function, indexOf(callee), entry });
}

void StubCalls::describeCalls(std::span<const Relocation> relocations, CodeBody& body, LinkBuffer* linkBuffer)
{
    body.stubKeys = WTF::move(m_stubKeys);
    body.callees = WTF::move(m_callees);
    body.argumentMoves = WTF::move(m_argumentMoves);
    body.returnSites.reserveCapacity(m_calls.size());
    for (const RecordedCall& call : m_calls) {
        uint32_t returnOffset;
        if (call.returnAddress) {
            RELEASE_ASSERT(linkBuffer);
            returnOffset = linkBuffer->offsetOf(*call.returnAddress);
        } else {
            const Relocation& relocation = relocations[call.relocation];
            RELEASE_ASSERT(relocation.kind == RelocationKind::Call);
            returnOffset = relocation.offset + static_cast<uint32_t>(sizeOfNearCall);
        }
        body.returnSites.append({ returnOffset, call.site });
        if (call.firstReadSite)
            body.firstReadSites.append({ returnOffset, *call.firstReadSite });
    }
    // Calls are recorded as they are emitted and relocations ascend, so both lists ascend and call() can promise an index.
    ASSERT(std::ranges::is_sorted(body.returnSites, { }, &ReturnSite::offset));
    ASSERT(std::ranges::is_sorted(body.firstReadSites, { }, &FirstReadSite::returnOffset));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
