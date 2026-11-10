/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCalls.h"

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTImageFormat.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

static_assert(numberOfHeaderWords == 2);

static constexpr size_t fewestStubKeysWithTable = 16;
static constexpr size_t fewestCalleesWithIndexes = 16;
static constexpr size_t smallestTableOfStubKeys = 64;
static_assert(2 * (fewestStubKeysWithTable + 1) <= smallestTableOfStubKeys && !(smallestTableOfStubKeys & (smallestTableOfStubKeys - 1)));

static ALWAYS_INLINE RememberedStubKey& rememberStubKey(RememberedStubKeys& rememberedKeys, const StubCatalog& catalog, StubKey key)
{
    constexpr uint32_t multiplier = 0x9e3779b1u;
    RememberedStubKey& remembered = rememberedKeys.keys[(static_cast<uint32_t>(key.bits() >> 32) * multiplier) >> (32 - RememberedStubKeys::logOfNumberOfKeys)];
    if (remembered.bits != key.bits() || !key.bits()) [[unlikely]] {
        RELEASE_ASSERT(exists(key) && !key.immediate() && key.callerFrame() != CallerFrame::Frameless);
        remembered = { key.bits(), catalog.bindsImmediate(key), descriptorOf(key.family()).binding == ImmediateBinding::OperationOffset, false, InvalidGPRReg };
    }
    return remembered;
}

static ALWAYS_INLINE GPRReg immediateGPROfStub(RememberedStubKeys& rememberedKeys, const StubCatalog& catalog, StubKey key)
{
    RememberedStubKey& remembered = rememberStubKey(rememberedKeys, catalog, key);
    if (remembered.knowsImmediateGPR) [[likely]]
        return remembered.immediateGPR;
    RememberedStubKey known = remembered;
    std::optional<GPRReg> immediate = catalog.summaryOf(key).signature.immediate;
    RELEASE_ASSERT(immediate);
    known.knowsImmediateGPR = true;
    known.immediateGPR = *immediate;
    remembered = known;
    return *immediate;
}

static uint32_t hashOfStubKey(StubKey key, std::optional<uint32_t> mentionOfImmediate)
{
    constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    uint64_t bits = key.bits() ^ (mentionOfImmediate ? (static_cast<uint64_t>(*mentionOfImmediate) + 1) * multiplier : 0);
    return static_cast<uint32_t>((bits * multiplier) >> 32);
}

static void moveNumberedImmediate(RelocationRecorder& relocations, NumberedImmediate immediate, GPRReg destination)
{
    relocations.moveNumber({ SymbolSpace::Number, immediate.mention }, destination);
}

static uint32_t appendWordsOfThunk(auto& lists, StubKey callee, uint64_t wordOfImmediate, const ArgumentMoves& moves)
{
    RELEASE_ASSERT((wordOfImmediate || !moves.isEmpty()) && !(moves.size() % 2));
    uint32_t index = safeCast<uint32_t>(lists.size());
    lists.grow(lists.size() + 1);
    ArgumentMoves& list = lists.last();
    list.reserveInitialCapacity(numberOfHeaderWords + moves.size());
    list.grow(numberOfHeaderWords + moves.size());
    std::span<uint64_t> words = list.mutableSpan();
    words[0] = callee.bits();
    words[1] = wordOfImmediate;
    memcpySpan(words.subspan(numberOfHeaderWords), moves.span());
    return index;
}

static uint32_t appendArgumentMovesOfThunk(auto& lists, StubKey callee, std::optional<uint32_t> immediateToMove, const ArgumentMoves& moves)
{
    RELEASE_ASSERT(!moves.isEmpty());
    return appendWordsOfThunk(lists, callee, immediateToMove ? hasImmediate | *immediateToMove : 0, moves);
}

static uint32_t appendThunkThatMovesNumber(auto& lists, StubKey callee, NumberedImmediate immediate, const ArgumentMoves& moves)
{
    return appendWordsOfThunk(lists, callee, hasImmediate | immediateIsMention | immediate.mention, moves);
}

static bool movesNumbersInThunks(StubKey key)
{
    return descriptorOf(key.family()).binding == ImmediateBinding::InThunk;
}

static bool argumentMovesUseRegister(const ArgumentMoves& moves, GPRReg reg)
{
    for (size_t i = 0; i < moves.size(); i += 2) {
        if (static_cast<GPRReg>(moves[i] >> 8) == reg)
            return true;
        if (static_cast<ArgumentSource>(moves[i] & 0xff) == ArgumentSource::Register && static_cast<GPRReg>(moves[i + 1]) == reg)
            return true;
    }
    return false;
}

StubCalls::StubCalls(const StubCatalog& catalog, RelocationRecorder& relocations, RememberedStubKeys* rememberedKeys)
    : m_catalog(catalog)
    , m_relocations(relocations)
    , m_ownRememberedKeys(rememberedKeys ? nullptr : std::make_unique<RememberedStubKeys>())
    , m_rememberedKeys(rememberedKeys ? *rememberedKeys : *m_ownRememberedKeys)
{
}

ALWAYS_INLINE_INTO_OTHER_FILES StubCalls::ResolvedKey StubCalls::resolveKey(StubKey key, std::optional<uint32_t> immediate) const
{
    const RememberedStubKey& remembered = rememberStubKey(m_rememberedKeys, m_catalog, key);
    if (immediate && remembered.bindsImmediate)
        return { key.withImmediate(*immediate), std::nullopt };
    RELEASE_ASSERT(!remembered.bindsOperationOffset);
    return { key, immediate };
}

ALWAYS_INLINE_INTO_OTHER_FILES StubKey StubCalls::prepare(CCallHelpers& jit, StubKey key, std::optional<uint32_t> immediate)
{
    auto [finalKey, immediateToMove] = resolveKey(key, immediate);
    if (immediateToMove) {
        GPRReg destination = immediateGPROfStub(m_rememberedKeys, m_catalog, finalKey);
        RELEASE_ASSERT(!m_registerFilledInFrontOfNextCall || (destination != GPRInfo::returnValueGPR && Reg(destination) != m_registerFilledInFrontOfNextCall));
        jit.move(CCallHelpers::TrustedImm32(*immediateToMove), destination);
    }
    return finalKey;
}

CodeSymbol StubCalls::prepare(StubKey key, NumberedImmediate immediate)
{
    StubKey finalKey = resolveKey(key, std::nullopt).key;
    if (rememberStubKey(m_rememberedKeys, m_catalog, finalKey).bindsImmediate)
        return { SymbolSpace::Stub, indexOf(finalKey.withImmediate(0), immediate.mention) };
    if (movesNumbersInThunks(finalKey))
        return { SymbolSpace::ArgumentThunk, appendThunkThatMovesNumber(m_argumentMoves, finalKey, immediate, { }) };
    GPRReg destination = immediateGPROfStub(m_rememberedKeys, m_catalog, finalKey);
    RELEASE_ASSERT(!m_registerFilledInFrontOfNextCall || (destination != GPRInfo::returnValueGPR && Reg(destination) != m_registerFilledInFrontOfNextCall));
    moveNumberedImmediate(m_relocations, immediate, destination);
    return { SymbolSpace::Stub, indexOf(finalKey, siblingUnderNameWithColumn(finalKey) ? std::optional<uint32_t>(immediate.mention) : std::nullopt) };
}

uint32_t StubCalls::indexOf(StubKey key, std::optional<uint32_t> mentionOfImmediate)
{
    std::span<const StubKey> keys = m_stubKeys.span();
    std::span<const std::optional<uint32_t>> mentions = m_mentionsOfImmediates.span();
    bool usesTable = keys.size() >= fewestStubKeysWithTable;
    size_t place = 0;
    if (usesTable) {
        if (2 * (keys.size() + 1) > m_placesOfStubKeys.size()) {
            m_placesOfStubKeys.fill(0, std::max(smallestTableOfStubKeys, 2 * m_placesOfStubKeys.size()));
            size_t mask = m_placesOfStubKeys.size() - 1;
            for (size_t index = 0; index < keys.size(); ++index) {
                size_t emptyPlace = hashOfStubKey(keys[index], mentions[index]) & mask;
                while (m_placesOfStubKeys[emptyPlace])
                    emptyPlace = (emptyPlace + 1) & mask;
                m_placesOfStubKeys[emptyPlace] = static_cast<uint32_t>(index + 1);
            }
        }
        size_t mask = m_placesOfStubKeys.size() - 1;
        for (place = hashOfStubKey(key, mentionOfImmediate) & mask; m_placesOfStubKeys[place]; place = (place + 1) & mask) {
            uint32_t index = m_placesOfStubKeys[place] - 1;
            if (keys[index] == key && mentions[index] == mentionOfImmediate)
                return index;
        }
    } else {
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys[index] == key && mentions[index] == mentionOfImmediate)
                return static_cast<uint32_t>(index);
        }
    }
    m_stubKeys.append(key);
    m_mentionsOfImmediates.append(mentionOfImmediate);
    uint32_t index = safeCast<uint32_t>(keys.size());
    if (usesTable)
        m_placesOfStubKeys[place] = index + 1;
    return index;
}

uint32_t StubCalls::indexOf(Typed::CodeUnit callee)
{
    RELEASE_ASSERT(callee);
    if (m_indexesOfCallees.isEmpty() && m_callees.size() < fewestCalleesWithIndexes) {
        size_t index = m_callees.find(callee);
        if (index == notFound) {
            index = m_callees.size();
            m_callees.append(callee);
        }
        return safeCast<uint32_t>(index);
    }
    auto keyOf = [](Typed::CodeUnit unit) {
        return static_cast<uint64_t>(unit.function.index()) << 32 | static_cast<uint64_t>(unit.role) << 24 | unit.placeOfSelector();
    };
    if (m_indexesOfCallees.isEmpty()) {
        for (uint32_t index = 0; index < m_callees.size(); ++index)
            m_indexesOfCallees.add(keyOf(m_callees[index]), index);
    }
    auto result = m_indexesOfCallees.add(keyOf(callee), safeCast<uint32_t>(m_callees.size()));
    if (result.isNewEntry)
        m_callees.append(callee);
    return result.iterator->value;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t StubCalls::emitCall(CCallHelpers&, StubKey called, CodeSymbol symbol, const RecordedCall& record)
{
    if (m_registerFilledInFrontOfNextCall) [[unlikely]] {
        const StubSignature signature = signatureOf(called);
        RELEASE_ASSERT(symbol.space == SymbolSpace::Stub && signature.filledOutOfResultInFront == m_registerFilledInFrontOfNextCall);
        RELEASE_ASSERT(signature.movedIntoResultInFront == m_registerMovedIntoResultInFrontOfNextCall);
        m_registerFilledInFrontOfNextCall = Reg();
        m_registerMovedIntoResultInFrontOfNextCall = Reg();
        symbol.entry = signature.movedIntoResultInFront ? entryInFrontOfStubThatDoesTwoMoves : entryInFrontOfStub;
    }
    uint32_t call = safeCast<uint32_t>(m_calls.size());
    if (call == m_calls.capacity()) [[unlikely]]
        m_calls.reserveCapacity(2 * static_cast<size_t>(call));
    m_calls.unsafeAppendWithoutCapacityCheck({ m_relocations.call(symbol), record.site, record.firstReadSite });
    return call;
}

void StubCalls::emitJump(CodeSymbol symbol)
{
    m_relocations.jump(symbol);
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, OptionalIndex site)
{
    StubKey finalKey = prepare(jit, key, std::nullopt);
    return emitCall(jit, finalKey, { SymbolSpace::Stub, indexOf(finalKey) }, { 0, site, { } });
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, uint32_t immediate, OptionalIndex site, OptionalIndex firstReadSite)
{
    StubKey finalKey = prepare(jit, key, immediate);
    return emitCall(jit, finalKey, { SymbolSpace::Stub, indexOf(finalKey) }, { 0, site, firstReadSite });
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, NumberedImmediate immediate, OptionalIndex site, OptionalIndex firstReadSite)
{
    return emitCall(jit, key, prepare(key, immediate), { 0, site, firstReadSite });
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves& moves, OptionalIndex firstReadSite)
{
    auto [finalKey, immediateToMove] = resolveKey(key, immediate);
    RELEASE_ASSERT(!immediateToMove || immediateGPROfStub(m_rememberedKeys, m_catalog, finalKey) != InvalidGPRReg);
    uint32_t thunk = appendArgumentMovesOfThunk(m_argumentMoves, finalKey, immediateToMove, moves);
    return emitCall(jit, finalKey, { SymbolSpace::ArgumentThunk, thunk }, { 0, site, firstReadSite });
}

uint32_t StubCalls::callWithImmediateOfSite(CCallHelpers& jit, StubKey key, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves& moves)
{
    auto [finalKey, immediateToMove] = resolveKey(key, immediate);
    if (immediateToMove) {
        GPRReg destination = immediateGPROfStub(m_rememberedKeys, m_catalog, finalKey);
        RELEASE_ASSERT(!argumentMovesUseRegister(moves, destination));
        jit.move(CCallHelpers::TrustedImm32(*immediateToMove), destination);
    }
    uint32_t thunk = appendArgumentMovesOfThunk(m_argumentMoves, finalKey, std::nullopt, moves);
    return emitCall(jit, finalKey, { SymbolSpace::ArgumentThunk, thunk }, { 0, site, { } });
}

uint32_t StubCalls::call(CCallHelpers& jit, StubKey key, NumberedImmediate immediate, OptionalIndex site, const ArgumentMoves& moves, OptionalIndex firstReadSite)
{
    StubKey finalKey = resolveKey(key, std::nullopt).key;
    GPRReg destination = immediateGPROfStub(m_rememberedKeys, m_catalog, finalKey);
    RELEASE_ASSERT(!argumentMovesUseRegister(moves, destination));
    if (movesNumbersInThunks(finalKey))
        return emitCall(jit, finalKey, { SymbolSpace::ArgumentThunk, appendThunkThatMovesNumber(m_argumentMoves, finalKey, immediate, moves) }, { 0, site, firstReadSite });
    uint32_t thunk = siblingUnderNameWithColumn(finalKey) ? appendWordsOfThunk(m_argumentMoves, finalKey, mentionNamesSiblingOfCallee | immediate.mention, moves) : appendArgumentMovesOfThunk(m_argumentMoves, finalKey, std::nullopt, moves);
    moveNumberedImmediate(m_relocations, immediate, destination);
    return emitCall(jit, finalKey, { SymbolSpace::ArgumentThunk, thunk }, { 0, site, firstReadSite });
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

void StubCalls::tailCall(CCallHelpers&, StubKey key, NumberedImmediate immediate)
{
    emitJump(prepare(key, immediate));
}

void StubCalls::callFunction(CCallHelpers& jit, Typed::CodeUnit callee, uint16_t entry, OptionalIndex site)
{
    emitCall(jit, StubKey(), { SymbolSpace::Function, indexOf(callee), entry }, { 0, site, { } });
}

uint32_t StubCalls::callRegister(CCallHelpers& jit, GPRReg address, OptionalIndex site)
{
    jit.call(address, JSEntryPtrTag);
    m_calls.append({ 0, site, { }, jit.label() });
    return safeCast<uint32_t>(m_calls.size() - 1);
}

void StubCalls::jumpToFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry)
{
    emitJump({ SymbolSpace::Function, indexOf(callee), entry });
}

void StubCalls::moveAddressOfFunction(Typed::CodeUnit callee, uint16_t entry, GPRReg destination)
{
    m_relocations.moveAddress({ SymbolSpace::Function, indexOf(callee), entry }, destination, PCRelativeReach::Far);
}

void StubCalls::moveAddressOfStub(CCallHelpers& jit, StubKey key, std::optional<uint32_t> immediate, GPRReg destination)
{
    m_relocations.moveAddress({ SymbolSpace::Stub, indexOf(prepare(jit, key, immediate)) }, destination, PCRelativeReach::Far);
}

void StubCalls::routeTransfersThroughEnd()
{
    if (!m_relocations.exceedsBranchReach()) [[likely]]
        return;
    for (RecordedCall& call : m_calls) {
        if (!call.returnAddress)
            call.returnAddress = m_relocations.returnAddressOf(call.relocation);
    }
    m_relocations.routeTransfersThroughEnd();
}

void StubCalls::describeCalls(std::span<const Relocation> relocations, CodeBody& body, LinkBuffer* linkBuffer)
{
    for (size_t index = 0; index < m_stubKeys.size(); ++index) {
        if (std::optional<uint32_t> mention = m_mentionsOfImmediates[index])
            body.rareLists.ensure().numberedStubKeys.append({ safeCast<uint32_t>(index), *mention });
    }
    m_mentionsOfImmediates.clear();
    m_placesOfStubKeys.clear();
    m_indexesOfCallees.clear();
    body.stubKeys = WTF::move(m_stubKeys);
    body.callees = WTF::move(m_callees);
    body.argumentMoves = ArgumentMovesOfUnit(m_argumentMoves.span());
    m_argumentMoves.clear();
    size_t numberOfReturnSites = body.returnSites.size();
    body.returnSites.reserveCapacity(numberOfReturnSites + m_calls.size());
    body.returnSites.grow(numberOfReturnSites + m_calls.size());
    std::span<ReturnSite> returnSites = body.returnSites.mutableSpan();
    uint32_t previousReturnOffset = 0;
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
        RELEASE_ASSERT(returnOffset >= previousReturnOffset);
        previousReturnOffset = returnOffset;
        returnSites[numberOfReturnSites++] = { returnOffset, call.site };
        if (call.firstReadSite)
            body.firstReadSites.append({ returnOffset, *call.firstReadSite });
    }
    ASSERT(std::ranges::is_sorted(body.firstReadSites, { }, &FirstReadSite::returnOffset));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
