/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpCompiler.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "YarrJIT.h"
#include "YarrPattern.h"
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/StackCheck.h>

namespace JSC::AOT {

static constexpr size_t dataAlignment = 8;
static constexpr int64_t regExpReachSlack = 16;

static void invalidateIfTempRegister([[maybe_unused]] CCallHelpers& jit, [[maybe_unused]] GPRReg reg)
{
#if CPU(ARM64)
    if (reg == MacroAssembler::dataTempRegister)
        (void)jit.getCachedDataTempRegisterIDAndInvalidate();
    else if (reg == MacroAssembler::memoryTempRegister)
        (void)jit.getCachedMemoryTempRegisterIDAndInvalidate();
#endif
}

static Yarr::JITFailureReason toJITFailureReason(LinkFailure failure)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        return Yarr::JITFailureReason::ProcessAddressInCode;
    case LinkFailure::OutOfPCRelativeReach:
        return Yarr::JITFailureReason::OutOfPCRelativeReach;
    case LinkFailure::OutOfMemory:
        return Yarr::JITFailureReason::ExecutableMemoryAllocationFailure;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static size_t sizeWithPrivateCopies(const RelocatableCode& body, const Vector<RegExpSymbol>& symbols)
{
    size_t size = body.bytes.size();
    for (auto& symbol : symbols)
        size += maxPrivateCopySize(symbol);
    return size;
}

ImageCodeAddressing::ImageCodeAddressing(CCallHelpers& jit, PCRelativeReach reach, const AssemblerPolicy& policy, BuildContext* context)
    : CodeAddressing(jit)
    , m_recorder(jit, policy)
    , m_reach(reach)
    , m_context(context)
    , m_returnAddressesAreInReach(std::make_shared<bool>(true))
{
}

void ImageCodeAddressing::moveAddressOfSymbol(RegExpSymbol&& symbol, GPRReg dest, PCRelativeReach reach)
{
    invalidateIfTempRegister(m_jit, dest);
    m_recorder.moveAddress(symbolFor(WTF::move(symbol)), dest, reach);
}

void ImageCodeAddressing::moveAddress(Yarr::SharedTableID id, GPRReg dest)
{
    moveAddressOfSymbol(id, dest, PCRelativeReach::Far);
}

void ImageCodeAddressing::moveAddress(const Yarr::ClassTable& table, GPRReg dest)
{
    moveAddressOfSymbol(Yarr::ClassTable(table), dest, m_reach);
}

void ImageCodeAddressing::moveAddressOfData(std::span<const uint8_t> bytes, GPRReg dest)
{
    RELEASE_ASSERT(!bytes.empty());
    moveAddressOfSymbol(PlainData { Vector<uint8_t>(bytes) }, dest, m_reach);
}

MacroAssembler::Jump ImageCodeAddressing::branchTestTableByte(MacroAssembler::ResultCondition condition, GPRReg character, RegExpSymbol&& table, PCRelativeReach reach, uint8_t bit)
{
    GPRReg scratch = m_jit.scratchRegister();
    RELEASE_ASSERT(character != scratch);
    m_recorder.moveAddress(symbolFor(WTF::move(table)), scratch, reach);
#if CPU(ARM64)
    m_jit.load8(MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesOne), scratch);
    return m_jit.branchTest32(condition, scratch, MacroAssembler::TrustedImm32(1 << bit));
#elif CPU(X86_64)
    return m_jit.branchTest8(condition, MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesOne), MacroAssembler::TrustedImm32(1 << bit));
#else
#error "AOT images need a table test for this CPU."
#endif
}

MacroAssembler::Jump ImageCodeAddressing::branchTestBuiltInClassTable(MacroAssembler::ResultCondition condition, GPRReg character, Yarr::CharacterClass::Table table)
{
    std::optional<uint8_t> bitPlane = Yarr::builtInClassBitPlane(table);
    if (!bitPlane) {
        fail(Yarr::JITFailureReason::UnlistedSharedTable);
        return m_jit.jump();
    }
    return branchTestTableByte(condition, character, Yarr::SharedTableID::BuiltInClassBitPlanes, PCRelativeReach::Far, *bitPlane);
}

MacroAssembler::Jump ImageCodeAddressing::branchTestLatin1Table(MacroAssembler::ResultCondition condition, GPRReg character, const Yarr::CharacterClass::ByteTable& table)
{
    WTF::BitSet<Yarr::CharacterClass::latin1TableSize> members;
    for (size_t index = 0; index < table.data.size(); ++index) {
        if (table.data[index])
            members.set(index);
    }
    uint8_t bit = Latin1BitPlane::preferredBit(members);
    return branchTestTableByte(condition, character, Latin1BitPlane { members, bit }, m_reach, bit);
}

void ImageCodeAddressing::canonicalizeLatin1(GPRReg character)
{
    GPRReg scratch = m_jit.scratchRegister();
    RELEASE_ASSERT(character != scratch);
    m_recorder.moveAddress(symbolFor(Yarr::SharedTableID::Latin1Canonicalization), scratch, PCRelativeReach::Far);
    m_jit.load16(MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesTwo), character);
}

bool ImageCodeAddressing::tryLoadConstant(uint64_t constant, GPRReg dest)
{
    AOT_THRESHOLD(unsigned, minInstructionsReplacedByLoad, 3);
    if (!isARM64() || m_reach != PCRelativeReach::Near || ARM64LogicalImmediate::create64(constant).isValid())
        return false;
    unsigned zeroHalfwords = 0;
    unsigned onesHalfwords = 0;
    for (unsigned shift = 0; shift < 64; shift += 16) {
        uint16_t halfword = static_cast<uint16_t>(constant >> shift);
        zeroHalfwords += !halfword;
        onesHalfwords += halfword == 0xffff;
    }
    if (4 - (zeroHalfwords >= onesHalfwords ? zeroHalfwords : onesHalfwords) < minInstructionsReplacedByLoad)
        return false;
    invalidateIfTempRegister(m_jit, dest);
    m_recorder.load64(symbolFor(Literal64 { constant }), dest);
    return true;
}

void ImageCodeAddressing::callRoutine(const Yarr::SharedRoutine& routine)
{
    if (!Yarr::canGenerateSharedRoutine(routine)) {
        fail(Yarr::JITFailureReason::UnlistedSharedTable);
        return;
    }
    m_recorder.call(symbolFor(Yarr::SharedRoutine(routine)));
}

Yarr::ReturnAddressSite ImageCodeAddressing::storeReturnAddress(MacroAssembler::Address slot)
{
    GPRReg scratch = m_jit.scratchRegister();
    RELEASE_ASSERT(slot.base != scratch);
    MacroAssembler::Label site = m_jit.movePCRelativeAddress(scratch, m_reach);
    m_jit.storePtr(scratch, slot);
    return { { }, site };
}

void ImageCodeAddressing::linkReturnAddresses(WTF::Vector<Yarr::ReturnAddressRecord>&& records)
{
    if (records.isEmpty())
        return;
    m_jit.addLinkTask([records = WTF::move(records), kind = addressKindFor(m_reach), areInReach = m_returnAddressesAreInReach](LinkBuffer& linkBuffer) {
        for (auto& record : records) {
            if (!linkBuffer.link(record.site.pcRelativeAddress, kind, record.target))
                *areInReach = false;
        }
    });
}

RegExpCodeOrFailureReason ImageCodeAddressing::takeCode()
{
    if (auto reason = failureReason())
        return makeUnexpected(*reason);

    auto body = m_recorder.finalize(LinkBuffer::Profile::YarrJIT, m_context);
    if (!body)
        return makeUnexpected(toJITFailureReason(body.error()));
    // layOutRegExpSection relies on this: a body and a private copy of every symbol, placed side by side, span less than the near reach.
    if (!*m_returnAddressesAreInReach || (m_reach == PCRelativeReach::Near && static_cast<int64_t>(sizeWithPrivateCopies(*body, m_symbols)) >= MacroAssembler::relocationReach(RelocationKind::AddressNear) - regExpReachSlack))
        return makeUnexpected(Yarr::JITFailureReason::OutOfPCRelativeReach);

    auto code = makeUnique<RegExpCode>();
    code->body = WTF::move(*body);
    code->symbols = WTF::move(m_symbols);
    code->reach = m_reach;
    return code;
}

CodeSymbol ImageCodeAddressing::symbolFor(RegExpSymbol&& symbol)
{
    size_t index = m_symbols.find(symbol);
    if (index == notFound) {
        index = m_symbols.size();
        m_symbols.append(WTF::move(symbol));
    }
    return { SymbolSpace::RegExp, static_cast<uint32_t>(index) };
}

RegExpCodeOrFailureReason compileSharedRoutine(const Yarr::SharedRoutine& routine, const AssemblerPolicy& policy, BuildContext* context)
{
    if (!Yarr::canGenerateSharedRoutine(routine))
        return makeUnexpected(Yarr::JITFailureReason::UnlistedSharedTable);

    CCallHelpers jit;
    ImageCodeAddressing addressing(jit, PCRelativeReach::Far, policy, context);
    Yarr::generateSharedRoutine(jit, routine, addressing);
    auto code = addressing.takeCode();
    if (code) {
        bool callsSharedRoutine = (*code)->symbols.containsIf([](const RegExpSymbol& symbol) {
            return WTF::holdsAlternative<Yarr::SharedRoutine>(symbol);
        });
        RELEASE_ASSERT(!callsSharedRoutine);
    }
    return code;
}

RegExpCodeOrFailureReason compileFailureRoutine(const AssemblerPolicy& policy, BuildContext* context)
{
    CCallHelpers jit;
    ImageCodeAddressing addressing(jit, PCRelativeReach::Far, policy, context);
    jit.tagReturnAddress();
    jit.move(MacroAssembler::TrustedImm64(static_cast<int64_t>(Yarr::JSRegExpResult::JITCodeFailure)), GPRInfo::returnValueGPR);
    jit.move(MacroAssembler::TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();
    return addressing.takeCode();
}

size_t maxPrivateCopySize(const RegExpSymbol& symbol)
{
    return WTF::switchOn(symbol,
        [](Yarr::SharedTableID) -> size_t { return 0; },
        [](const PlainData& data) -> size_t { return roundUpToMultipleOf<dataAlignment>(data.bytes.size()) + dataAlignment; },
        [](const Latin1BitPlane&) -> size_t { return Yarr::CharacterClass::latin1TableSize + dataAlignment; },
        [](const Yarr::ClassTable& table) -> size_t {
            Yarr::ClassTablePacker packer;
            packer.add(table);
            packer.pack();
            return packer.bytes().size() + dataAlignment;
        },
        [](const Yarr::SharedRoutine&) -> size_t { return 0; },
        [](const Literal64&) -> size_t { return sizeof(Literal64::value) + dataAlignment; });
}

size_t CompiledRegExp::codeSize() const
{
    size_t size = 0;
    for (auto& codeForCharSize : code) {
        if (codeForCharSize)
            size += sizeWithPrivateCopies(codeForCharSize->body, codeForCharSize->symbols);
    }
    return size;
}

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

static RegExpCodeOrFailureReason generate(const RegExpCompilerInputs& inputs, const Yarr::YarrPattern& pattern, Yarr::CharSize charSize, PCRelativeReach reach)
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

    RegExpCodeOrFailureReason code = static_cast<ImageCodeAddressing&>(generated->addressing()).takeCode();
    if (code) {
        (*code)->hasElidedFrame = generated->hasElidedFrame();
        (*code)->analysisLimitHits = pattern.m_firstFollowSets.limitHits() + generated->classTableLimitHits();
    }
    return code;
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
        return { .form = RegExpRecordForm::NoMetadata, .reason = summary.exceedsParenthesesDepth ? RegExpDeclineReason::NestedTooDeep : RegExpDeclineReason::NotSupportedByCodeGenerator };
    if (!summary.atom.isNull())
        return { .form = RegExpRecordForm::Atom };
    if (inputs.pattern.length() > (inputs.context ? inputs.context->limit(LimitID::RegExpPatternLengthWithCode) : Limits::regExpPatternLengthWithCode))
        return { .form = RegExpRecordForm::NoCode, .reason = RegExpDeclineReason::PatternTooLong };

    // Not hasStackForRegExpCompiler(): the 16 KB it adds are for the frames between a caller that tests it and this one.
    RELEASE_ASSERT(StackCheck(Thread::currentSingleton().stack(), regExpCompilerStackSize).isSafeToRecurse());
    Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
    Yarr::YarrPattern yarrPattern(inputs.pattern, inputs.flags, error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::Performed);
    if (Yarr::hasError(error) || yarrPattern.containsUnsignedLengthPattern())
        return { .form = RegExpRecordForm::NoCode, .reason = RegExpDeclineReason::NotSupportedByCodeGenerator };
    RELEASE_ASSERT(!yarrPattern.m_firstFollowSets.didExhaustStack());

    CompiledRegExp result { .form = RegExpRecordForm::Code };
    for (auto charSize : { Yarr::CharSize::Char8, Yarr::CharSize::Char16 }) {
        if (charSize == Yarr::CharSize::Char16 && inputs.codeFor16BitSubjects == RegExpCodeFor16BitSubjects::WhereWideCharactersCanBeInMatchedSubject && matchesOnlyWhole8BitSubjects(yarrPattern))
            break;
        RegExpCodeOrFailureReason code = generate(inputs, yarrPattern, charSize, PCRelativeReach::Near);
        if (!code && code.error() == Yarr::JITFailureReason::OutOfPCRelativeReach)
            code = generate(inputs, yarrPattern, charSize, PCRelativeReach::Far);
        if (!code) {
            RELEASE_ASSERT(code.error() != Yarr::JITFailureReason::ParenthesisNestedTooDeep);
            if (inputs.context && needsProcessAddress(code.error()))
                inputs.context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
            return { .form = RegExpRecordForm::NoCode, .reason = declineReasonFor(code.error()) };
        }
        result.code[charSize == Yarr::CharSize::Char16] = WTF::move(*code);
    }
    return result;
}

uint32_t CompiledRegExps::numberWithForm(RegExpRecordForm form) const
{
    return static_cast<uint32_t>(std::ranges::count(results, form, &CompiledRegExp::form));
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
        if (chargedBytes + size > budget)
            results[index] = { .form = RegExpRecordForm::NoCode, .reason = RegExpDeclineReason::CandidateBudgetExhausted };
        else
            chargedBytes += size;
    }
}

static void applySectionSizeLimit(CompiledRegExps& compiled, BuildContext& context)
{
    uint64_t maxCodeSize = context.limit(LimitID::RegExpSectionSize) / 2;
    uint64_t totalSize = 0;
    for (CompiledRegExp& result : compiled.results) {
        uint64_t size = result.codeSize();
        if (totalSize + size > maxCodeSize)
            result = { .form = RegExpRecordForm::NoCode, .reason = RegExpDeclineReason::SectionFull };
        else
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
