/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpCompiler.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTRegExpCodeAddressing.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "YarrJIT.h"
#include "YarrPattern.h"
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/StackCheck.h>

namespace JSC::AOT {

using CodeOrFailureReason = std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason>;

static RegExpDeclineReason declineReasonFor(Yarr::JITFailureReason reason)
{
    switch (reason) {
    case Yarr::JITFailureReason::ParenthesisNestedTooDeep:
        return RegExpDeclineReason::NestedTooDeep;
    case Yarr::JITFailureReason::GeneratedCodeSizeTooLarge:
    case Yarr::JITFailureReason::OutOfPCRelativeReach:
        return RegExpDeclineReason::CodeTooLarge;
    case Yarr::JITFailureReason::DecodeSurrogatePair:
    case Yarr::JITFailureReason::BackReference:
    case Yarr::JITFailureReason::ParenthesizedSubpattern:
    case Yarr::JITFailureReason::ExecutableMemoryAllocationFailure:
    case Yarr::JITFailureReason::OffsetTooLarge:
    case Yarr::JITFailureReason::LookbehindLayoutMismatch:
    case Yarr::JITFailureReason::LookbehindJITDisabled:
    case Yarr::JITFailureReason::UnsupportedCPU:
    case Yarr::JITFailureReason::ProcessAddressInCode:
    case Yarr::JITFailureReason::UnlistedSharedTable:
        return RegExpDeclineReason::NotSupportedByCodeGenerator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool needsProcessAddress(Yarr::JITFailureReason reason)
{
    switch (reason) {
    case Yarr::JITFailureReason::ProcessAddressInCode:
    case Yarr::JITFailureReason::UnlistedSharedTable:
        return true;
    case Yarr::JITFailureReason::DecodeSurrogatePair:
    case Yarr::JITFailureReason::BackReference:
    case Yarr::JITFailureReason::ParenthesizedSubpattern:
    case Yarr::JITFailureReason::ParenthesisNestedTooDeep:
    case Yarr::JITFailureReason::ExecutableMemoryAllocationFailure:
    case Yarr::JITFailureReason::OffsetTooLarge:
    case Yarr::JITFailureReason::GeneratedCodeSizeTooLarge:
    case Yarr::JITFailureReason::LookbehindLayoutMismatch:
    case Yarr::JITFailureReason::LookbehindJITDisabled:
    case Yarr::JITFailureReason::UnsupportedCPU:
    case Yarr::JITFailureReason::OutOfPCRelativeReach:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CompiledRegExp declinedRegExp(RegExpRecordForm form, RegExpDeclineReason reason)
{
    CompiledRegExp result;
    result.form = form;
    result.reason = reason;
    return result;
}

static uint64_t patternLengthLimit(const BuildContext* context)
{
    return context ? context->limit(LimitID::RegExpPatternLengthWithCode) : Limits::regExpPatternLengthWithCode;
}

size_t CompiledRegExp::codeSize() const
{
    size_t size = 0;
    for (auto& codeForCharSize : code) {
        if (!codeForCharSize)
            continue;
        size += codeForCharSize->body.bytes.size();
        for (auto& symbol : codeForCharSize->symbols)
            size += maxPrivateCopySize(symbol);
    }
    return size;
}

static CodeOrFailureReason generate(const RegExpCompilerInputs& inputs, const Yarr::YarrPattern& pattern, Yarr::CharSize charSize, PCRelativeReach reach)
{
    Yarr::YarrCodeBlock codeBlock(nullptr);
    Yarr::CodeGenerationPolicy policy;
    policy.sizePolicy = reach == PCRelativeReach::Near ? Yarr::CodeSizePolicy::Compact : Yarr::CodeSizePolicy::MostCompact;
    Yarr::CodeGenerationRequest request {
        .pattern = pattern,
        .patternString = inputs.pattern,
        .charSize = charSize,
        .executionMode = Yarr::ExecutionMode::IncludeSubpatterns,
        .sampleString = std::nullopt,
        .codeBlock = &codeBlock,
        .policy = policy,
        .createCodeAddressing = [&](CCallHelpers& jit) -> std::unique_ptr<Yarr::CodeAddressing> {
            return makeUnique<ImageCodeAddressing>(jit, reach, inputs.assemblerPolicy, inputs.context);
        },
    };

    auto generated = Yarr::generateCode(request);
    if (!generated)
        return makeUnexpected(generated.error());

    CodeOrFailureReason code = static_cast<ImageCodeAddressing&>(generated->addressing()).takeCode();
    if (code) {
        (*code)->reach = reach;
        (*code)->hasElidedFrame = generated->hasElidedFrame();
        (*code)->analysisLimitHits = pattern.m_firstFollowSets.limitHits() + generated->classTableLimitHits();
    }
    return code;
}

static CodeOrFailureReason generateWithinReach(const RegExpCompilerInputs& inputs, const Yarr::YarrPattern& pattern, Yarr::CharSize charSize)
{
    CodeOrFailureReason nearCode = generate(inputs, pattern, charSize, PCRelativeReach::Near);
    if (nearCode || nearCode.error() != Yarr::JITFailureReason::OutOfPCRelativeReach)
        return nearCode;
    return generate(inputs, pattern, charSize, PCRelativeReach::Far);
}

static bool consumesOnly8BitCharacters(const Yarr::PatternDisjunction& disjunction)
{
    for (auto& alternative : disjunction.m_alternatives) {
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            switch (term.type) {
            case Yarr::PatternTerm::Type::AssertionBOL:
            case Yarr::PatternTerm::Type::AssertionEOL:
            case Yarr::PatternTerm::Type::AssertionWordBoundary:
            case Yarr::PatternTerm::Type::ParentheticalAssertion:
            case Yarr::PatternTerm::Type::NumberedBackReference:
            case Yarr::PatternTerm::Type::NamedBackReference:
            case Yarr::PatternTerm::Type::NumberedForwardReference:
            case Yarr::PatternTerm::Type::NamedForwardReference:
                break;
            case Yarr::PatternTerm::Type::PatternCharacter:
                if (term.patternCharacter > 0xff)
                    return false;
                break;
            case Yarr::PatternTerm::Type::CharacterClass: {
                const Yarr::CharacterClass& characterClass = *term.characterClass;
                bool holdsWideCharacter = characterClass.hasStrings()
                    || std::ranges::any_of(characterClass.m_matches32, [](char32_t character) { return character > 0xff; })
                    || std::ranges::any_of(characterClass.m_ranges32, [](const Yarr::CharacterRange& range) { return range.end > 0xff; });
                if (term.invert() || holdsWideCharacter)
                    return false;
                break;
            }
            case Yarr::PatternTerm::Type::ParenthesesSubpattern:
                if (!consumesOnly8BitCharacters(*term.parentheses.disjunction))
                    return false;
                break;
            case Yarr::PatternTerm::Type::DotStarEnclosure:
                return false;
            }
        }
    }
    return true;
}

// A pattern that has to match its subject from end to end with characters of one byte matches no subject that holds a wider one: an address, a version, a name.
static bool matchesOnlyWhole8BitSubjects(const Yarr::YarrPattern& pattern)
{
    if (pattern.multiline() || pattern.ignoreCase() || pattern.m_containsModifiers)
        return false;
    for (auto& alternative : pattern.m_body->m_alternatives) {
        auto& terms = alternative->m_terms;
        if (terms.size() < 2 || terms.first().type != Yarr::PatternTerm::Type::AssertionBOL || terms.last().type != Yarr::PatternTerm::Type::AssertionEOL)
            return false;
    }
    return consumesOnly8BitCharacters(*pattern.m_body);
}

NEVER_INLINE CompiledRegExp compileRegExp(const RegExpCompilerInputs& inputs)
{
    const RegExpSummary& summary = inputs.summary;
    if (summary.metadata == RegExpMetadata::Unknown)
        return declinedRegExp(RegExpRecordForm::NoMetadata, summary.exceedsParenthesesDepth ? RegExpDeclineReason::NestedTooDeep : RegExpDeclineReason::NotSupportedByCodeGenerator);

    CompiledRegExp result;
    if (!summary.atom.isNull()) {
        result.form = RegExpRecordForm::Atom;
        return result;
    }
    if (inputs.pattern.length() > patternLengthLimit(inputs.context))
        return declinedRegExp(RegExpRecordForm::NoCode, RegExpDeclineReason::PatternTooLong);

    // Not hasStackForRegExpCompiler(): the 16 KB it adds are for the frames between a caller that tests it and this one.
    RELEASE_ASSERT(StackCheck(Thread::currentSingleton().stack(), regExpCompilerStackSize).isSafeToRecurse());
    Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
    Yarr::YarrPattern yarrPattern(inputs.pattern, inputs.flags, error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::Performed);
    if (Yarr::hasError(error) || yarrPattern.containsUnsignedLengthPattern())
        return declinedRegExp(RegExpRecordForm::NoCode, RegExpDeclineReason::NotSupportedByCodeGenerator);
    RELEASE_ASSERT(!yarrPattern.m_firstFollowSets.didExhaustStack());

    for (auto charSize : { Yarr::CharSize::Char8, Yarr::CharSize::Char16 }) {
        if (charSize == Yarr::CharSize::Char16 && inputs.codeFor16BitSubjects == RegExpCodeFor16BitSubjects::WhereWideCharactersCanBeInMatchedSubject && matchesOnlyWhole8BitSubjects(yarrPattern))
            break;
        CodeOrFailureReason code = generateWithinReach(inputs, yarrPattern, charSize);
        if (!code) {
            RELEASE_ASSERT(code.error() != Yarr::JITFailureReason::ParenthesisNestedTooDeep);
            if (inputs.context && needsProcessAddress(code.error()))
                inputs.context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
            return declinedRegExp(RegExpRecordForm::NoCode, declineReasonFor(code.error()));
        }
        result.code[charSize == Yarr::CharSize::Char16] = WTF::move(*code);
    }
    result.form = RegExpRecordForm::Code;
    return result;
}

uint32_t CompiledRegExps::numberWithForm(RegExpRecordForm form) const
{
    uint32_t number = 0;
    for (auto& result : results)
        number += result.form == form;
    return number;
}

static LimitID limitOf(RegExpDeclineReason reason)
{
    switch (reason) {
    case RegExpDeclineReason::NestedTooDeep:
        return LimitID::RegExpNestingDepthWithCode;
    case RegExpDeclineReason::PatternTooLong:
        return LimitID::RegExpPatternLengthWithCode;
    case RegExpDeclineReason::CandidateBudgetExhausted:
        return LimitID::RegExpCandidateCodeBytesPerSite;
    case RegExpDeclineReason::SectionFull:
        return LimitID::RegExpSectionSize;
    case RegExpDeclineReason::NotSupportedByCodeGenerator:
    case RegExpDeclineReason::CodeTooLarge:
    case RegExpDeclineReason::TextKnownAtRunTime:
        return LimitID::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void applyCandidateBudget(const RegExpRequests& requests, CompiledRegExps& compiled, BuildContext& context)
{
    uint64_t budget = context.limit(LimitID::RegExpCandidateCodeBytesPerSite);
    std::span<const RegExpRequest> candidates = requests.candidates();
    std::span<CompiledRegExp> results = compiled.results.mutableSpan().subspan(requests.literals().size());
    UncheckedKeyHashMap<std::pair<FunctionID, ValueID>, uint64_t> bytesBySite;
    for (size_t index = 0; index < candidates.size(); ++index) {
        uint64_t size = results[index].codeSize();
        if (!size || candidates[index].origin == RegExpRequestOrigin::Evaluated)
            continue;
        const RegExpSite& site = candidates[index].sites.first();
        uint64_t& chargedBytes = bytesBySite.add(std::pair { site.function, site.value }, 0).iterator->value;
        if (chargedBytes + size > budget) {
            results[index] = declinedRegExp(RegExpRecordForm::NoCode, RegExpDeclineReason::CandidateBudgetExhausted);
            continue;
        }
        chargedBytes += size;
    }
}

static void applySectionSizeLimit(CompiledRegExps& compiled, BuildContext& context)
{
    uint64_t maxCodeSize = context.limit(LimitID::RegExpSectionSize) / 2;
    uint64_t totalSize = 0;
    for (CompiledRegExp& result : compiled.results) {
        uint64_t size = result.codeSize();
        if (totalSize + size > maxCodeSize) {
            result = declinedRegExp(RegExpRecordForm::NoCode, RegExpDeclineReason::SectionFull);
            continue;
        }
        totalSize += size;
    }
}

static void reportDecline(const RegExpRequest& request, RegExpDeclineReason reason, BuildContext& context)
{
    LimitID limit = limitOf(reason);
    for (const RegExpSite& site : request.sites) {
        context.diagnostics().add(UserDiagnostic {
            .kind = UserDiagnosticKind::RegExpWithoutCode,
            .function = site.function,
            .value = site.value,
            .limit = limit,
            .count = static_cast<uint32_t>(reason),
            .location = site.location,
        });
    }
    if (limit == LimitID::None)
        return;
    const RegExpSite& firstSite = request.sites.first();
    context.gaveUp(limit, Subject { .function = firstSite.function, .value = firstSite.value, .location = firstSite.location });
}

static void countResult(Statistics& statistics, const RegExpRequest& request, const CompiledRegExp& result)
{
    bool isCandidate = request.origin != RegExpRequestOrigin::Literal;
    switch (result.form) {
    case RegExpRecordForm::Code: {
        uint64_t analysisLimitHits = 0;
        uint64_t elidedFrames = 0;
        for (auto& code : result.code) {
            if (!code)
                continue;
            analysisLimitHits += code->analysisLimitHits;
            elidedFrames += code->hasElidedFrame;
        }
        statistics.count(StatisticID::RegExpsCompiled);
        statistics.count(StatisticID::RegExpAnalysisLimitHits, analysisLimitHits);
        statistics.count(StatisticID::RegExpFramesElided, elidedFrames);
        statistics.count(StatisticID::RegExpCandidatesCompiled, isCandidate);
        return;
    }
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        statistics.count(StatisticID::RegExpsInterpreted);
        statistics.count(StatisticID::RegExpCandidatesDropped, isCandidate);
        return;
    case RegExpRecordForm::Atom:
        statistics.count(StatisticID::RegExpAtoms);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CompiledRegExps compileRegExps(const Typed::Program& program, const RegExpRequests& requests, BuildContext& context)
{
    std::span<const RegExpRequest> allRequests = requests.all();
    RegExpSummaries& summaries = context.regExpSummaries();

    CompiledRegExps compiled;
    compiled.results.grow(allRequests.size());
    forEachIndexInParallel(context.parallelLoops(), safeCast<unsigned>(allRequests.size()), [&](unsigned index) {
        const RegExpRequest& request = allRequests[index];
        Vector<char16_t> patternBuffer;
        StringView pattern = program.strings.viewOf(request.pattern, patternBuffer);
        const RegExpSummary& summary = summaries.summaryOf(request.pattern, request.flags, pattern);
        RegExpCodeFor16BitSubjects codeFor16BitSubjects = request.origin == RegExpRequestOrigin::Literal ? RegExpCodeFor16BitSubjects::Always : RegExpCodeFor16BitSubjects::WhereWideCharactersCanBeInMatchedSubject;
        compiled.results[index] = compileRegExp({ pattern, request.flags, summary, context.assemblerPolicy(), &context, codeFor16BitSubjects });
    });

    applyCandidateBudget(requests, compiled, context);
    applySectionSizeLimit(compiled, context);

    for (size_t index = 0; index < allRequests.size(); ++index) {
        const RegExpRequest& request = allRequests[index];
        const CompiledRegExp& result = compiled.results[index];
        if (result.reason)
            reportDecline(request, *result.reason, context);
        countResult(context.statistics(), request, result);

        const RegExpSummary* summary = summaries.summaryIfComputed(request.pattern, request.flags);
        RELEASE_ASSERT(summary);
        compiled.numberWithUnicodePropertyLookup += summary->hasUnicodePropertyLookup;
    }
    return compiled;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
