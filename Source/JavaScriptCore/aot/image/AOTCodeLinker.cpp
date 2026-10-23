/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeLinker.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "MacroAssembler.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint32_t bodiesPerChunk = 64;

namespace {

struct SymbolLists {
    std::span<const StubKey> stubKeys;
    std::span<const ArgumentMoves> argumentMoves;
};

struct LinkScope {
    FunctionID caller;
    size_t sharedCodeCopy;
    std::span<const VeneerTarget> veneers;
    size_t firstVeneer;
    OptionalIndex position;
    SymbolLists lists;
    std::span<const ResolvedCallee> callees;
};

class CodeLinker {
    WTF_MAKE_NONCOPYABLE(CodeLinker);
public:
    CodeLinker(const CodeLinkerInputs&, std::span<uint8_t> code, BuildContext&);

    void copyConstants() const;
    uint64_t linkSharedCode(uint32_t copy) const;
    uint64_t linkBody(uint32_t position) const;

private:
    std::optional<size_t> resolve(const Relocation&, const LinkScope&) const;
    size_t resolveThroughVeneers(const VeneerTarget&, RelocationKind, const LinkScope&) const;
    std::optional<size_t> resolveInConstantPool(const Vector<Vector<uint32_t, 0>>& offsetsByBody, const Relocation&, const LinkScope&) const;
    std::nullopt_t reportNotApplied(const Relocation&, const LinkScope&) const;

    uint64_t link(size_t start, size_t end, std::span<const Relocation>, const LinkScope&) const;
    uint64_t writeVeneers(size_t firstVeneer, std::span<const VeneerTarget>) const;
    uint64_t linkOwnBodies(uint32_t position, const LinkScope&) const;
    void writeJumpTables(uint32_t position) const;

    SymbolLists outlinedCodeLists() const;
    void copyCode(size_t offset, std::span<const uint8_t>) const;

    const CodeLayoutInputs& m_inputs;
    const CodeLayout& m_layout;
    const Outlining& m_outlining;
    const ConstantPool& m_constants;
    const InstanceSizes m_instanceSizes;
    const CodePositions m_positions;
    std::span<uint8_t> m_code;
    BuildContext& m_context;
};

} // anonymous namespace

CodeLinker::CodeLinker(const CodeLinkerInputs& inputs, std::span<uint8_t> code, BuildContext& context)
    : m_inputs(inputs.layoutInputs)
    , m_layout(inputs.layout)
    , m_outlining(inputs.outlining)
    , m_constants(inputs.constants)
    , m_instanceSizes(inputs.instanceSizes)
    , m_positions { inputs.layout.bodyStarts.span(), inputs.layout.regExpCodeOffset }
    , m_code(code)
    , m_context(context)
{
    RELEASE_ASSERT(code.size() == m_layout.codeSize);
}

void CodeLinker::copyCode(size_t offset, std::span<const uint8_t> bytes) const
{
    if (bytes.empty())
        return;
    RELEASE_ASSERT(isSumSmallerThanOrEqual(offset, bytes.size(), m_code.size()));
    memcpySpan(m_code.subspan(offset), bytes);
}

void CodeLinker::copyConstants() const
{
    copyCode(m_layout.constantsOffset, m_constants.bytes.span());
}

SymbolLists CodeLinker::outlinedCodeLists() const
{
    return { m_inputs.outlinedCode.stubKeys.span(), m_inputs.outlinedCode.argumentMoves.span() };
}

std::nullopt_t CodeLinker::reportNotApplied(const Relocation& relocation, const LinkScope& scope) const
{
    m_context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = scope.caller, .count = relocation.offset });
    return std::nullopt;
}

size_t CodeLinker::resolveThroughVeneers(const VeneerTarget& target, RelocationKind kind, const LinkScope& scope) const
{
    if (transfersControl(kind)) {
        auto veneer = std::ranges::find(scope.veneers, target);
        if (veneer != scope.veneers.end())
            return scope.firstVeneer + (veneer - scope.veneers.begin()) * m_inputs.templates.sizeOfVeneer();
    }
    return targetOffset(m_inputs, m_positions, target);
}

std::optional<size_t> CodeLinker::resolveInConstantPool(const Vector<Vector<uint32_t, 0>>& offsetsByBody, const Relocation& relocation, const LinkScope& scope) const
{
    if (!scope.position || relocation.symbol >= offsetsByBody[*scope.position].size())
        return reportNotApplied(relocation, scope);
    return static_cast<size_t>(m_layout.constantsOffset) + offsetsByBody[*scope.position][relocation.symbol];
}

std::optional<size_t> CodeLinker::resolve(const Relocation& relocation, const LinkScope& scope) const
{
    const SharedCode& sharedCode = m_inputs.sharedCode;
    const OutlinedCode& outlinedCode = m_inputs.outlinedCode;
    uint32_t symbol = relocation.symbol;
    if ((relocation.kind == RelocationKind::InstanceOffset) != (relocation.space == SymbolSpace::SiteCache))
        return reportNotApplied(relocation, scope);

    switch (relocation.space) {
    case SymbolSpace::Function: {
        if (symbol >= scope.callees.size() || !scope.callees[symbol].body)
            return reportNotApplied(relocation, scope);
        ResolvedCallee callee = scope.callees[symbol];
        return resolveThroughVeneers({ SymbolSpace::Function, relocation.entry, *callee.body, callee.alias }, relocation.kind, scope);
    }
    case SymbolSpace::RegExp: {
        const auto& entries = m_inputs.regExpSection.entries;
        if (symbol >= entries.size() || relocation.entry >= entries[symbol].size())
            return reportNotApplied(relocation, scope);
        return resolveThroughVeneers({ SymbolSpace::RegExp, relocation.entry, symbol, { } }, relocation.kind, scope);
    }
    case SymbolSpace::Stub: {
        if (symbol >= scope.lists.stubKeys.size())
            return reportNotApplied(relocation, scope);
        if (std::optional<uint32_t> offset = sharedCode.blob.offsetOf(scope.lists.stubKeys[symbol]))
            return scope.sharedCodeCopy + *offset;
        m_context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated, .function = scope.caller });
        return std::nullopt;
    }
    case SymbolSpace::ArgumentThunk:
        if (symbol >= scope.lists.argumentMoves.size())
            return reportNotApplied(relocation, scope);
        return scope.sharedCodeCopy + sharedCode.argumentThunksOffset + sharedCode.thunkOffset(scope.lists.argumentMoves[symbol]);
    case SymbolSpace::OutlinedBody: {
        if (symbol >= m_outlining.bodies.size())
            return reportNotApplied(relocation, scope);
        uint32_t number = outlinedCode.indexWithinKind[symbol];
        if (m_outlining.bodies[symbol].singleCaller)
            return m_layout.ownBodyOffsets[number];
        return scope.sharedCodeCopy + sharedCode.sharedBodiesOffset + outlinedCode.sharedBodies.offsets[number];
    }
    case SymbolSpace::Constant:
        return resolveInConstantPool(m_constants.constantOffsetsByBody, relocation, scope);
    case SymbolSpace::JumpTable:
        return resolveInConstantPool(m_constants.jumpTableOffsetsByBody, relocation, scope);
    case SymbolSpace::SiteCache:
        if (symbol >= m_instanceSizes.numberOfSiteCaches)
            return reportNotApplied(relocation, scope);
        return InstanceLayout::offsetOfSiteCache(m_instanceSizes, symbol);
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return reportNotApplied(relocation, scope);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t CodeLinker::link(size_t start, size_t end, std::span<const Relocation> relocations, const LinkScope& scope) const
{
    auto resolveInsideCode = [&](const Relocation& relocation) -> std::optional<size_t> {
        size_t size = MacroAssembler::relocationSize(relocation.kind);
        if (!size || !isSumSmallerThanOrEqual<size_t>(relocation.offset, size, end))
            return std::nullopt;
        return resolve(relocation, scope);
    };
    auto reportOutOfReach = [&](const Relocation& relocation) {
        reportNotApplied(relocation, scope);
    };
    return applyRelocations(m_code, start, relocations, resolveInsideCode, reportOutOfReach);
}

uint64_t CodeLinker::writeVeneers(size_t firstVeneer, std::span<const VeneerTarget> targets) const
{
    const RelocatableCode& veneer = m_inputs.templates.veneer;
    uint64_t numberApplied = 0;
    size_t at = firstVeneer;
    for (const VeneerTarget& target : targets) {
        copyCode(at, veneer.bytes.span());
        if (applyRelocation(m_code, at, veneer.relocations[0], targetOffset(m_inputs, m_positions, target)))
            ++numberApplied;
        at += veneer.bytes.size();
    }
    return numberApplied;
}

uint64_t CodeLinker::linkSharedCode(uint32_t copy) const
{
    const SharedCode& sharedCode = m_inputs.sharedCode;
    const RelocatableCode& blob = sharedCode.blob.code;
    const RelocatableCode& argumentThunks = sharedCode.argumentThunks.code;
    const OutlinedBodies& sharedBodies = m_inputs.outlinedCode.sharedBodies;
    std::span<const VeneerTarget> veneers = m_layout.sharedCodeVeneers.span();
    size_t argumentThunksStart = static_cast<size_t>(copy) + sharedCode.argumentThunksOffset;
    size_t sharedBodiesStart = static_cast<size_t>(copy) + sharedCode.sharedBodiesOffset;
    size_t firstVeneer = static_cast<size_t>(copy) + sharedCode.veneersOffset;
    RELEASE_ASSERT(sharedCode.veneersOffset + veneers.size() * m_inputs.templates.sizeOfVeneer() <= m_layout.sharedCodeSize);

    auto scopeFor = [&](SymbolLists lists, std::span<const ResolvedCallee> callees) {
        return LinkScope { { }, copy, veneers, firstVeneer, { }, lists, callees };
    };
    copyCode(copy, blob.bytes.span());
    copyCode(argumentThunksStart, argumentThunks.bytes.span());
    copyCode(sharedBodiesStart, sharedBodies.bytes.span());
    uint64_t numberApplied = writeVeneers(firstVeneer, veneers);
    numberApplied += link(copy, blob.bytes.size(), blob.relocations.span(), scopeFor({ }, { }));
    if (uint32_t firstCopy = m_layout.sharedCodeCopies[0]; copy != firstCopy) {
        const RelocatableCode& veneer = m_inputs.templates.veneer;
        for (const StubEntryRange& entry : sharedCode.blob.entriesInFrontOfSingleReturnSites) {
            RELEASE_ASSERT(entry.offset + veneer.bytes.size() <= entry.end);
            copyCode(static_cast<size_t>(copy) + entry.offset, veneer.bytes.span());
            if (applyRelocation(m_code, static_cast<size_t>(copy) + entry.offset, veneer.relocations[0], static_cast<size_t>(firstCopy) + entry.offset))
                ++numberApplied;
        }
    }
    numberApplied += link(argumentThunksStart, argumentThunks.bytes.size(), argumentThunks.relocations.span(), scopeFor({ sharedCode.argumentThunks.stubKeys.span(), { } }, { }));
    numberApplied += link(sharedBodiesStart, sharedBodies.bytes.size(), sharedBodies.relocations.span(), scopeFor(outlinedCodeLists(), m_inputs.outlinedCode.callees.span()));
    return numberApplied;
}

uint64_t CodeLinker::linkOwnBodies(uint32_t position, const LinkScope& scope) const
{
    const OutlinedBodies& ownBodies = m_inputs.outlinedCode.ownBodies;
    uint64_t numberApplied = 0;
    for (uint32_t number : m_inputs.outlinedCode.ownBodiesByCaller[position]) {
        size_t begin = ownBodies.offsets[number];
        size_t end = begin + ownBodies.sizeOf(number);
        size_t start = m_layout.ownBodyOffsets[number];
        copyCode(start, ownBodies.bytes.subspan(begin, end - begin));
        // The difference may wrap around: only its sum with an offset of the list is a position.
        numberApplied += link(start - begin, end, ownBodies.relocationsOf(number), scope);
    }
    return numberApplied;
}

void CodeLinker::writeJumpTables(uint32_t position) const
{
    const CompiledCode& function = m_inputs.functions[position];
    const Vector<uint32_t, 0>& tableOffsets = m_constants.jumpTableOffsetsByBody[position];
    RELEASE_ASSERT(tableOffsets.size() == function.body.jumpTables.size());
    int64_t bodyStart = m_layout.bodyStarts[position];
    for (size_t index = 0; index < tableOffsets.size(); ++index) {
        std::span<const uint32_t> targets = function.body.jumpTables[index].targets.span();
        size_t table = static_cast<size_t>(m_layout.constantsOffset) + tableOffsets[index];
        RELEASE_ASSERT(isSumSmallerThanOrEqual(table, targets.size_bytes(), m_code.size()));
        std::span<uint8_t> words = m_code.subspan(table, targets.size_bytes());
        for (uint32_t target : targets) {
            int64_t distance = bodyStart + target - static_cast<int64_t>(table);
            if (!isRepresentableAs<int32_t>(distance)) {
                m_context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = function.function, .count = target });
                return;
            }
            WTF::unalignedStore<int32_t>(words.data(), static_cast<int32_t>(distance));
            words = words.subspan(sizeof(int32_t));
        }
    }
}

uint64_t CodeLinker::linkBody(uint32_t position) const
{
    const CompiledCode& function = m_inputs.functions[position];
    const CodeBody& body = function.body;
    std::span<const VeneerTarget> veneers = m_layout.veneersByBody[position].span();
    size_t start = m_layout.bodyStarts[position];
    size_t sharedCodeCopy = m_layout.sharedCodeCopyByBody[position];
    size_t firstVeneer = m_layout.firstVeneerOffset(position, body.bytes.size());

    copyCode(start, body.bytes.span());
    uint64_t numberApplied = writeVeneers(firstVeneer, veneers);
    LinkScope bodyScope { function.function, sharedCodeCopy, veneers, firstVeneer, { position }, { body.stubKeys.span(), body.argumentMoves.span() }, m_inputs.callees.callees[position].span() };
    numberApplied += link(start, body.bytes.size(), body.relocations.span(), bodyScope);
    LinkScope ownBodyScope { function.function, sharedCodeCopy, veneers, firstVeneer, { }, outlinedCodeLists(), m_inputs.outlinedCode.callees.span() };
    numberApplied += linkOwnBodies(position, ownBodyScope);
    writeJumpTables(position);
    return numberApplied;
}

static uint64_t countExpectedLinks(const CodeLinkerInputs& inputs)
{
    const CodeLayoutInputs& layoutInputs = inputs.layoutInputs;
    const CodeLayout& layout = inputs.layout;
    uint64_t linksInSharedCode = layoutInputs.sharedCode.blob.code.relocations.size()
        + layoutInputs.sharedCode.argumentThunks.code.relocations.size()
        + layoutInputs.outlinedCode.sharedBodies.relocations.size()
        + layout.sharedCodeVeneers.size();
    uint64_t expected = linksInSharedCode * layout.sharedCodeCopies.size() + layoutInputs.outlinedCode.ownBodies.relocations.size();
    expected += layoutInputs.sharedCode.blob.entriesInFrontOfSingleReturnSites.size() * (layout.sharedCodeCopies.size() - 1);
    for (size_t position = 0; position < layoutInputs.functions.size(); ++position)
        expected += layoutInputs.functions[position].body.relocations.size() + layout.veneersByBody[position].size();
    return expected;
}

void linkCode(const CodeLinkerInputs& inputs, std::span<uint8_t> code, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    const CodeLayout& layout = inputs.layout;
    CodeLinker linker(inputs, code, context);
    // linkBody writes the jump tables over the zeros that the pool holds for them.
    linker.copyConstants();

    uint32_t numberOfBodies = static_cast<uint32_t>(inputs.layoutInputs.functions.size());
    uint64_t expected = 0;
    Vector<uint64_t> appliedByCopy(layout.sharedCodeCopies.size());
    Vector<uint64_t> appliedByChunk((numberOfBodies + bodiesPerChunk - 1) / bodiesPerChunk);
    runTasksInParallel(loops, {
        [&] { expected = countExpectedLinks(inputs); },
        [&] {
            forEachIndexInParallel(loops, appliedByCopy.size(), [&](unsigned index) {
                appliedByCopy[index] = linker.linkSharedCode(layout.sharedCodeCopies[index]);
            });
        },
        [&] {
            forEachIndexInParallel(loops, appliedByChunk.size(), [&](unsigned chunk) {
                uint32_t begin = chunk * bodiesPerChunk;
                uint32_t end = std::min(begin + bodiesPerChunk, numberOfBodies);
                uint64_t numberApplied = 0;
                for (uint32_t position = begin; position < end; ++position)
                    numberApplied += linker.linkBody(position);
                appliedByChunk[chunk] = numberApplied;
            });
        },
    });

    uint64_t applied = 0;
    for (uint64_t numberApplied : appliedByCopy)
        applied += numberApplied;
    for (uint64_t numberApplied : appliedByChunk)
        applied += numberApplied;
    // A relocation that got a diagnostic of its own was not applied either.
    if (!context.diagnostics().hasErrors() && expected != applied)
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = clampTo<uint32_t>(expected > applied ? expected - applied : applied - expected) });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
