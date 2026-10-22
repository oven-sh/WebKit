/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConvention.h"
#include "AOTStubAssembler.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

static constexpr size_t numberOfHeaderWords = 2;
static constexpr uint64_t hasImmediate = 1ull << 32;

#if ASSERT_ENABLED
static bool isOperand(StubKey key, const StubSignature& signature, GPRReg reg)
{
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        if (signature.operands[i] == Reg(reg))
            return true;
    }
    if (!signature.usesCallConvention)
        return false;
    if (reg == calleeGPR || reg == thisGPR || reg == countGPR)
        return true;
    for (unsigned i = 0; i < key.count(); ++i) {
        if (reg == argumentGPR(i))
            return true;
    }
    return false;
}

static bool hasValidDestinations(StubKey key, const StubSignature& signature, const ArgumentMoves& words)
{
    for (size_t i = numberOfHeaderWords; i < words.size(); i += 2) {
        GPRReg destination = static_cast<GPRReg>(words[i] >> 8);
        bool isReserved = destination == signature.immediate || RegisterSet::macroClobberedGPRs().contains(destination, IgnoreVectors);
        if (isReserved && !isOperand(key, signature, destination))
            return false;
    }
    return true;
}
#endif

static void emitRegisterMoves(CCallHelpers& jit, const ArgumentMoves& words)
{
    Vector<std::pair<GPRReg, GPRReg>, 8> pending;
    for (size_t i = numberOfHeaderWords; i < words.size(); i += 2) {
        if (static_cast<ArgumentSource>(words[i] & 0xff) == ArgumentSource::Register)
            pending.append({ static_cast<GPRReg>(words[i + 1]), static_cast<GPRReg>(words[i] >> 8) });
    }
    while (!pending.isEmpty()) {
        size_t free = pending.findIf([&](auto& move) {
            return !pending.containsIf([&](auto& other) {
                return other.first == move.second;
            });
        });
        if (free != notFound) {
            jit.move(pending[free].first, pending[free].second);
            pending.removeAt(free);
            continue;
        }
        auto [source, destination] = pending.takeLast();
        jit.swap(source, destination);
        for (auto& other : pending) {
            if (other.first == destination)
                other.first = source;
            else if (other.first == source)
                other.first = destination;
        }
        pending.removeAllMatching([](auto& move) {
            return move.first == move.second;
        });
    }
}

static void emitLoadsAndConstants(CCallHelpers& jit, const ArgumentMoves& words)
{
    for (size_t i = numberOfHeaderWords; i < words.size(); i += 2) {
        GPRReg destination = static_cast<GPRReg>(words[i] >> 8);
        switch (static_cast<ArgumentSource>(words[i] & 0xff)) {
        case ArgumentSource::Register:
            break;
        case ArgumentSource::Stack32:
            jit.load32(CCallHelpers::Address(GPRInfo::callFrameRegister, static_cast<int32_t>(words[i + 1])), destination);
            break;
        case ArgumentSource::Stack64:
            jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, static_cast<int32_t>(words[i + 1])), destination);
            break;
        case ArgumentSource::Stack32OffStackPointer:
            jit.load32(CCallHelpers::Address(CCallHelpers::stackPointerRegister, static_cast<int32_t>(words[i + 1])), destination);
            break;
        case ArgumentSource::Stack64OffStackPointer:
            jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister, static_cast<int32_t>(words[i + 1])), destination);
            break;
        case ArgumentSource::Constant:
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(words[i + 1])), destination);
            break;
        }
    }
}

static void emitImmediate(CCallHelpers& jit, const StubCatalog& catalog, StubKey key, const ArgumentMoves& words)
{
    if (!(words[1] & hasImmediate))
        return;
    std::optional<GPRReg> immediate = catalog.summaryOf(key).signature.immediate;
    RELEASE_ASSERT(immediate);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(words[1])), *immediate);
}

std::expected<ArgumentThunks, LinkFailure> generateArgumentThunks(BuildContext& context, const StubCatalog& catalog, const Vector<const ArgumentMoves*>& all)
{
    ArgumentThunks result;
    if (all.isEmpty())
        return result;
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    readTagsFromInstance(jit);
    AllowMacroScratchRegisterUsage allowScratch(jit);
    HashMap<uint64_t, uint32_t> indexOfKey;
    Vector<CCallHelpers::Label> labels;
    labels.reserveInitialCapacity(all.size());
    for (auto* moves : all) {
        auto& words = *moves;
        StubKey key = StubKey::fromBits(words[0]);
        RELEASE_ASSERT(exists(key));
        ASSERT(hasValidDestinations(key, catalog.summaryOf(key).signature, words));
        labels.append(jit.label());
        emitRegisterMoves(jit, words);
        emitLoadsAndConstants(jit, words);
        emitImmediate(jit, catalog, key, words);
        auto entry = indexOfKey.add(key.bits(), static_cast<uint32_t>(result.stubKeys.size()));
        if (entry.isNewEntry)
            result.stubKeys.append(key);
        recorder.jump({ SymbolSpace::Stub, entry.iterator->value });
    }
    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    if (buffer.didFailToAllocate())
        return makeUnexpected(LinkFailure::OutOfMemory);
    result.offsets = labels.map([&](CCallHelpers::Label label) {
        return buffer.linkBuffer().offsetOf(label);
    });
    buffer.finalize(context.options().logging.dumpsDisassembly, "AOT argument thunks"_s);
    auto relocations = recorder.link(buffer, &context);
    if (!relocations)
        return makeUnexpected(relocations.error());
    RELEASE_ASSERT(relocations->size() == all.size());
    result.code = { Vector<uint8_t>(buffer.bytes()), WTF::move(*relocations) };
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
