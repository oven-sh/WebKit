/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTStubDescriptor.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static constexpr size_t functionsInChunk = 1024;
static constexpr unsigned shardBits = 6;
static constexpr unsigned numberOfShards = 1 << shardBits;

using ArgumentMovesList = Vector<const ArgumentMoves*>;
using ShardedArgumentMoves = std::array<ArgumentMovesList, numberOfShards>;

static unsigned numberOfChunks(const LinkedFunctions& functions)
{
    return static_cast<unsigned>((functions.size() + functionsInChunk - 1) / functionsInChunk);
}

static std::span<const CompiledCode> chunkOf(const LinkedFunctions& functions, unsigned chunk)
{
    return functions.span().subspan(chunk * functionsInChunk, std::min(functionsInChunk, functions.size() - chunk * functionsInChunk));
}

static std::optional<StubKey> targetStubOf(const Relocation& relocation, const Vector<StubKey>& stubKeys, const Vector<ArgumentMoves>& argumentMoves)
{
    switch (relocation.space) {
    case SymbolSpace::Stub:
        return stubKeys[relocation.symbol];
    case SymbolSpace::ArgumentThunk:
        return StubKey::fromBits(argumentMoves[relocation.symbol][0]);
    case SymbolSpace::Function:
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::SiteCache:
    case SymbolSpace::RegExp:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

[[nodiscard]] static bool appendTargetStubs(Vector<StubKey>& keys, const Vector<Relocation>& relocations, const Vector<StubKey>& stubKeys, const Vector<ArgumentMoves>& argumentMoves)
{
    bool everyStubExists = true;
    for (const Relocation& relocation : relocations) {
        std::optional<StubKey> key = targetStubOf(relocation, stubKeys, argumentMoves);
        if (!key)
            continue;
        if (exists(*key))
            keys.append(*key);
        else
            everyStubExists = false;
    }
    return everyStubExists;
}

static void appendCalledKeys(Vector<StubKey>& keys, const CompiledCode& function, BuildContext& context)
{
    const CodeBody& body = function.body;
    if (!appendTargetStubs(keys, body.relocations, body.stubKeys, body.argumentMoves))
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated, .function = function.function });
}

static void appendJumpKeys(Vector<StubKey>& keys, const OutlinedBodies& bodies, const OutlinedCode& outlinedCode, BuildContext& context)
{
    if (!appendTargetStubs(keys, bodies.relocations, outlinedCode.stubKeys, outlinedCode.argumentMoves))
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated });
}

static Vector<StubKey> collectCalledKeys(const LinkedFunctions& functions, const OutlinedCode& outlinedCode, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<Vector<StubKey>> keysByChunk(numberOfChunks(functions));
    forEachIndexInParallel(loops, keysByChunk.size(), [&](unsigned chunk) {
        Vector<StubKey>& chunkKeys = keysByChunk[chunk];
        for (auto& function : chunkOf(functions, chunk))
            appendCalledKeys(chunkKeys, function, context);
        sortAndRemoveDuplicates(loops, chunkKeys);
    });
    Vector<StubKey> keys;
    for (auto& chunkKeys : keysByChunk)
        keys.appendVector(chunkKeys);
    appendJumpKeys(keys, outlinedCode.sharedBodies, outlinedCode, context);
    appendJumpKeys(keys, outlinedCode.ownBodies, outlinedCode, context);
    return keys;
}

static bool isLessThan(const ArgumentMoves* a, const ArgumentMoves* b)
{
    return std::ranges::lexicographical_compare(*a, *b);
}

static bool isEqual(const ArgumentMoves* a, const ArgumentMoves* b)
{
    return *a == *b;
}

static unsigned shardOf(const ArgumentMoves& moves)
{
    uint64_t hash = moves.size();
    for (uint64_t word : moves)
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
    return static_cast<unsigned>(hash >> (64 - shardBits));
}

static ArgumentMovesList collectArgumentMoves(const LinkedFunctions& functions, const OutlinedCode& outlinedCode, const ParallelLoops& loops)
{
    Vector<ShardedArgumentMoves> shardsByChunk(numberOfChunks(functions));
    forEachIndexInParallel(loops, shardsByChunk.size(), [&](unsigned chunk) {
        ArgumentMovesList chunkMoves;
        for (auto& function : chunkOf(functions, chunk)) {
            for (const Relocation& relocation : function.body.relocations) {
                if (relocation.space == SymbolSpace::ArgumentThunk)
                    chunkMoves.append(&function.body.argumentMoves[relocation.symbol]);
            }
        }
        sortAndRemoveDuplicates(loops, chunkMoves, isLessThan, isEqual);
        for (auto* moves : chunkMoves)
            shardsByChunk[chunk][shardOf(*moves)].append(moves);
    });
    ShardedArgumentMoves mergedShards;
    forEachIndexInParallel(loops, numberOfShards, [&](unsigned shard) {
        for (auto& shards : shardsByChunk)
            mergedShards[shard].appendVector(shards[shard]);
        sortAndRemoveDuplicates(loops, mergedShards[shard], isLessThan, isEqual);
    });
    ArgumentMovesList argumentMoves;
    for (auto& shardMoves : mergedShards)
        argumentMoves.appendVector(shardMoves);
    for (auto& moves : outlinedCode.argumentMoves)
        argumentMoves.append(&moves);
    sortAndRemoveDuplicates(loops, argumentMoves, isLessThan, isEqual);
    return argumentMoves;
}

uint32_t SharedCode::thunkOffset(const ArgumentMoves& moves) const
{
    auto found = std::ranges::lower_bound(argumentMoves, &moves, isLessThan);
    RELEASE_ASSERT(found != argumentMoves.end() && **found == moves);
    return argumentThunks.offsets[found - argumentMoves.begin()];
}

[[nodiscard]] static bool computeOffsetsInCopy(SharedCode& sharedCode, const OutlinedCode& outlinedCode, BuildContext& context)
{
    uint64_t blobSize = sharedCode.blob.code.bytes.size();
    uint64_t argumentThunksSize = sharedCode.argumentThunks.code.bytes.size();
    uint64_t argumentThunksOffset = WTF::roundUpToMultipleOf<imageStubsAlignment>(blobSize);
    uint64_t argumentThunksEnd = argumentThunksSize ? argumentThunksOffset + argumentThunksSize : blobSize;
    uint64_t sharedBodiesOffset = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(argumentThunksEnd);
    uint64_t veneersOffset = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(sharedBodiesOffset + outlinedCode.sharedBodies.bytes.size());
    if (veneersOffset > std::min<uint64_t>(context.limit(LimitID::CodeBytes), UINT32_MAX)) {
        context.reportLimitExceeded(LimitID::CodeBytes, veneersOffset);
        return false;
    }
    sharedCode.argumentThunksOffset = static_cast<uint32_t>(argumentThunksOffset);
    sharedCode.sharedBodiesOffset = static_cast<uint32_t>(sharedBodiesOffset);
    sharedCode.veneersOffset = static_cast<uint32_t>(veneersOffset);
    return true;
}

SharedCode selectSharedCode(const LinkedFunctions& functions, const OutlinedCode& outlinedCode, const StubCatalog& catalog, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<StubKey> keys = collectCalledKeys(functions, outlinedCode, context);
    if (context.diagnostics().hasErrors())
        return { };

    SharedCode result;
    result.argumentMoves = collectArgumentMoves(functions, outlinedCode, loops);
    auto argumentThunks = generateArgumentThunks(context, catalog, result.argumentMoves);
    if (!argumentThunks) {
        reportLinkFailure(argumentThunks.error(), { }, context);
        return { };
    }
    result.argumentThunks = WTF::move(*argumentThunks);
    keys.appendVector(result.argumentThunks.stubKeys);
    sortAndRemoveDuplicates(loops, keys);

    auto blob = buildStubBlob(context, catalog, keys.span(), entriesUsedByFunctions);
    if (!blob) {
        reportLinkFailure(blob.error(), { }, context);
        return { };
    }
    result.blob = WTF::move(*blob);
    if (!computeOffsetsInCopy(result, outlinedCode, context))
        return { };
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
