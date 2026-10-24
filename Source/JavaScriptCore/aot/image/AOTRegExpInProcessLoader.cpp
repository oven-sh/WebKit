/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpInProcessLoader.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTCodeTemplates.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRecords.h"
#include "AOTRegExpSummary.h"
#include "AOTThreshold.h"
#include "CPUFeatureSet.h"
#include "ExecutableAllocator.h"
#include "MacroAssembler.h"
#include <array>
#include <atomic>
#include <bit>
#include <expected>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

#if ENABLE(JIT_SCAN_ASSEMBLER_BUFFER_FOR_ZEROES)

// Tables hold runs of zero bytes, which performJITMemcpy rejects in this configuration.
const uint8_t* loadRegExpSection(const RegExpSection&)
{
    return nullptr;
}

#else

AOT_THRESHOLD(size_t, maxLoadedRegExpBytes, 64 * MB);
static std::atomic<size_t> loadedRegExpBytes { 0 };

const uint8_t* loadRegExpSection(const RegExpSection& section)
{
    auto bytes = section.bytes.span();
    size_t size = roundUpToMultipleOf<RegExpSection::alignment>(bytes.size()) + RegExpSection::alignment;
    if (loadedRegExpBytes.fetch_add(size, std::memory_order_relaxed) + size > maxLoadedRegExpBytes) {
        loadedRegExpBytes.fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }

    auto handle = ExecutableAllocator::singleton().allocate(size, JITCompilationCanFail);
    if (!handle) {
        loadedRegExpBytes.fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }

    auto* start = std::bit_cast<uint8_t*>(roundUpToMultipleOf<RegExpSection::alignment>(handle->startAsInteger()));
    performJITMemcpy<jitMemcpyRepatch>(start, bytes.data(), bytes.size());
    MacroAssembler::cacheFlush(start, bytes.size());
    UNUSED_VARIABLE(handle.leakRef());
    return start;
}

#endif // ENABLE(JIT_SCAN_ASSEMBLER_BUFFER_FOR_ZEROES)

static Yarr::JITFailureReason failureReasonFor(RegExpDeclineReason reason)
{
    switch (reason) {
    case RegExpDeclineReason::NestedTooDeep:
        return Yarr::JITFailureReason::ParenthesisNestedTooDeep;
    case RegExpDeclineReason::CodeTooLarge:
    case RegExpDeclineReason::PatternTooLong:
    case RegExpDeclineReason::CandidateBudgetExhausted:
    case RegExpDeclineReason::SectionFull:
        return Yarr::JITFailureReason::GeneratedCodeSizeTooLarge;
    case RegExpDeclineReason::NotSupportedByCodeGenerator:
    case RegExpDeclineReason::TextKnownAtRunTime:
        return Yarr::JITFailureReason::UnlistedSharedTable;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Yarr::JITFailureReason> failureReasonFor(const CompiledRegExp& compiled)
{
    switch (compiled.form) {
    case RegExpRecordForm::Code:
        return std::nullopt;
    case RegExpRecordForm::Atom:
        return Yarr::JITFailureReason::UnlistedSharedTable;
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        return failureReasonFor(*compiled.reason);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::expected<RegExpEntrypoints, Yarr::JITFailureReason> compileThroughImage(StringView pattern, OptionSet<Yarr::Flags> flags)
{
    if (!hasStackForRegExpCompiler())
        return makeUnexpected(Yarr::JITFailureReason::ParenthesisNestedTooDeep);

    AssemblerPolicy policy = AssemblerPolicy::forImage(CPUFeatureSet::buildTarget());
    RegExpSummary summary = summarizeRegExp(pattern, flags);
    std::array<CompiledRegExp, 1> compiled { compileRegExp({ pattern, flags, summary, policy, nullptr }) };
    if (auto reason = failureReasonFor(compiled[0]))
        return makeUnexpected(*reason);

    auto templates = CodeTemplates::create(policy, nullptr);
    if (!templates)
        return makeUnexpected(Yarr::JITFailureReason::ExecutableMemoryAllocationFailure);

    auto section = layOutRegExpSection(compiled, policy, *templates, nullptr);
    if (!section)
        return makeUnexpected(Yarr::JITFailureReason::GeneratedCodeSizeTooLarge);

    auto* base = loadRegExpSection(*section);
    if (!base)
        return makeUnexpected(Yarr::JITFailureReason::ExecutableMemoryAllocationFailure);

    auto [offsetFor8Bit, offsetFor16Bit] = section->entries[0];
    return RegExpEntrypoints { base + offsetFor8Bit, base + offsetFor16Bit };
}

void jitCompileThroughImage(StringView pattern, OptionSet<Yarr::Flags> flags, Yarr::YarrCodeBlock& codeBlock)
{
    auto entrypoints = compileThroughImage(pattern, ImageRegExp::significantFlags(flags));
    if (!entrypoints) {
        codeBlock.setFallBackWithFailureReason(entrypoints.error());
        return;
    }
    codeBlock.setCodeFromImage(entrypoints->for8Bit, entrypoints->for16Bit);
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
