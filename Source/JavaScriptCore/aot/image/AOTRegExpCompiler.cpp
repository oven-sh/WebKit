/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpCompiler.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTRegExpFlags.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "YarrJIT.h"
#include "YarrPattern.h"
#include <algorithm>
#include <cstdlib>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/StackCheck.h>

namespace JSC::AOT {

static constexpr size_t regExpDataAlignment = 8;
static constexpr unsigned maxRegExpPatternLengthWithCode = Typed::longestRegExpPatternWithCode;
static constexpr PCRelativeReach reachOfRegExpTables = PCRelativeReach::Far;

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

ImageCodeAddressing::ImageCodeAddressing(CCallHelpers& jit, PCRelativeReach reachWithinBody, const AssemblerPolicy& policy, BuildContext* context)
    : CodeAddressing(jit)
    , m_recorder(jit, policy)
    , m_reach(reachWithinBody)
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
    moveAddressOfSymbol(id, dest, reachOfRegExpTables);
}

void ImageCodeAddressing::moveAddress(const Yarr::ClassTable& table, GPRReg dest)
{
    moveAddressOfSymbol(Yarr::ClassTable(table), dest, reachOfRegExpTables);
}

void ImageCodeAddressing::moveAddressOfData(std::span<const uint8_t> bytes, GPRReg dest)
{
    RELEASE_ASSERT(!bytes.empty());
    moveAddressOfSymbol(PlainData { Vector<uint8_t>(bytes) }, dest, reachOfRegExpTables);
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
    return branchTestTableByte(condition, character, Yarr::SharedTableID::BuiltInClassBitPlanes, reachOfRegExpTables, *bitPlane);
}

MacroAssembler::Jump ImageCodeAddressing::branchTestLatin1Table(MacroAssembler::ResultCondition condition, GPRReg character, const Yarr::CharacterClass::ByteTable& table)
{
    WTF::BitSet<Yarr::CharacterClass::latin1TableSize> members;
    for (size_t index = 0; index < table.data.size(); ++index) {
        if (table.data[index])
            members.set(index);
    }
    uint8_t bit = Latin1BitPlane::preferredBit(members);
    return branchTestTableByte(condition, character, Latin1BitPlane { members, bit }, reachOfRegExpTables, bit);
}

void ImageCodeAddressing::canonicalizeLatin1(GPRReg character)
{
    GPRReg scratch = m_jit.scratchRegister();
    RELEASE_ASSERT(character != scratch);
    m_recorder.moveAddress(symbolFor(Yarr::SharedTableID::Latin1Canonicalization), scratch, reachOfRegExpTables);
    m_jit.load16(MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesTwo), character);
}

bool ImageCodeAddressing::tryLoadConstant(uint64_t, GPRReg)
{
    return false;
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
    if (!*m_returnAddressesAreInReach)
        return makeUnexpected(Yarr::JITFailureReason::OutOfPCRelativeReach);

    auto code = makeUnique<RegExpCode>();
    code->body = WTF::move(*body);
    code->symbols = WTF::move(m_symbols);
    code->reach = m_reach;
    return code;
}

CodeSymbol ImageCodeAddressing::symbolFor(RegExpSymbol&& symbol)
{
    static constexpr size_t mostSymbolsSearchedOneByOne = 16;
    auto keyOf = [](const RegExpSymbol& symbol) {
        return computeHash(symbol) >> 1;
    };
    size_t index = notFound;
    if (m_symbols.size() <= mostSymbolsSearchedOneByOne)
        index = m_symbols.find(symbol);
    else {
        if (m_symbolsByHash.isEmpty()) {
            for (uint32_t known = 0; known < m_symbols.size(); ++known)
                m_symbolsByHash.add(keyOf(m_symbols[known]), Vector<uint32_t, 1>()).iterator->value.append(known);
        }
        if (auto iterator = m_symbolsByHash.find(keyOf(symbol)); iterator != m_symbolsByHash.end()) {
            for (uint32_t known : iterator->value) {
                if (m_symbols[known] == symbol) {
                    index = known;
                    break;
                }
            }
        }
    }
    if (index == notFound) {
        index = m_symbols.size();
        if (!m_symbolsByHash.isEmpty())
            m_symbolsByHash.add(keyOf(symbol), Vector<uint32_t, 1>()).iterator->value.append(static_cast<uint32_t>(index));
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

static size_t maxRegExpTableSize(const RegExpSymbol& symbol)
{
    return WTF::switchOn(symbol,
        [](Yarr::SharedTableID) -> size_t { return 0; },
        [](const PlainData& data) -> size_t { return roundUpToMultipleOf<regExpDataAlignment>(data.bytes.size()) + regExpDataAlignment; },
        [](const Latin1BitPlane&) -> size_t { return Yarr::CharacterClass::latin1TableSize + regExpDataAlignment; },
        [](const Yarr::ClassTable& table) -> size_t {
            Yarr::ClassTablePacker packer;
            packer.add(table);
            packer.pack();
            return packer.bytes().size() + regExpDataAlignment;
        },
        [](const Yarr::SharedRoutine&) -> size_t { return 0; },
        [](const Literal64&) -> size_t { return sizeof(Literal64::value) + regExpDataAlignment; });
}

static size_t sizeOfRegExpBodyAndTables(const RelocatableCode& body, const Vector<RegExpSymbol>& symbols)
{
    size_t size = body.bytes.size();
    for (auto& symbol : symbols)
        size += maxRegExpTableSize(symbol);
    return size;
}

size_t CompiledRegExp::codeSize() const
{
    size_t size = 0;
    for (auto& codeForCharSize : code) {
        if (codeForCharSize)
            size += sizeOfRegExpBodyAndTables(codeForCharSize->body, codeForCharSize->symbols);
    }
    return size;
}

static RegExpDeclineReason regExpDeclineReasonFor(Yarr::JITFailureReason reason)
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

static RegExpCodeOrFailureReason generateRegExpCode(const RegExpCompilerInputs& inputs, const Yarr::YarrPattern& pattern, Yarr::CharSize charSize, Yarr::CodeSizePolicy sizePolicy, PCRelativeReach reachWithinBody)
{
    Yarr::YarrCodeBlock codeBlock(nullptr);
    Yarr::CodeGenerationPolicy policy;
    policy.sizePolicy = sizePolicy;
    Yarr::CodeGenerationRequest request {
        .pattern = pattern,
        .patternString = inputs.pattern,
        .charSize = charSize,
        .executionMode = Yarr::ExecutionMode::IncludeSubpatterns,
        .sampleString = std::nullopt,
        .codeBlock = &codeBlock,
        .policy = policy,
        .createCodeAddressing = [&](CCallHelpers& jit) -> std::unique_ptr<Yarr::CodeAddressing> {
            return makeUnique<ImageCodeAddressing>(jit, reachWithinBody, inputs.assemblerPolicy, inputs.context);
        },
    };

    auto generated = Yarr::generateCode(request);
    if (!generated)
        return makeUnexpected(generated.error());

    RegExpCodeOrFailureReason code = static_cast<ImageCodeAddressing&>(generated->addressing()).takeCode();
    if (code) {
        (*code)->hasElidedFrame = generated->hasElidedFrame();
        (*code)->handsLongSearchesOver = generated->handsLongSearchesOver();
        (*code)->analysisLimitHits = pattern.m_firstFollowSets.limitHits() + generated->classTableLimitHits();
    }
    return code;
}

NEVER_INLINE CompiledRegExp compileRegExp(const RegExpCompilerInputs& inputs)
{
    const RegExpSummary& summary = inputs.summary;
    if (summary.metadata == RegExpMetadata::Unknown)
        return { .form = RegExpRecordForm::NoMetadata, .reason = summary.exceedsParenthesesDepth ? RegExpDeclineReason::NestedTooDeep : RegExpDeclineReason::NotSupportedByCodeGenerator };
    if (!summary.atom.isNull())
        return { .form = RegExpRecordForm::Atom };
    if (inputs.pattern.length() > maxRegExpPatternLengthWithCode)
        return { .form = RegExpRecordForm::NoCode, .reason = RegExpDeclineReason::PatternTooLong };

    RELEASE_ASSERT(StackCheck(Thread::currentSingleton().stack(), regExpCompilerStackSize).isSafeToRecurse());
    Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
    Yarr::YarrPattern yarrPattern(inputs.pattern, inputs.flags, error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::Performed);
    if (Yarr::hasError(error) || yarrPattern.containsUnsignedLengthPattern())
        return { .form = RegExpRecordForm::NoCode, .reason = RegExpDeclineReason::NotSupportedByCodeGenerator };
    RELEASE_ASSERT(!yarrPattern.m_firstFollowSets.didExhaustStack());

    CompiledRegExp result { .form = RegExpRecordForm::Code };
    for (auto charSize : { Yarr::CharSize::Char8, Yarr::CharSize::Char16 }) {
        if (charSize == Yarr::CharSize::Char16 && inputs.codeFor16BitSubjects == RegExpCodeFor16BitSubjects::WhereWideCharactersCanBeInMatchedSubject && summary.matchesOnlyWhole8BitSubjects)
            break;
        RegExpCodeOrFailureReason code = generateRegExpCode(inputs, yarrPattern, charSize, Yarr::CodeSizePolicy::Compact, PCRelativeReach::Near);
        if (!code && code.error() == Yarr::JITFailureReason::OutOfPCRelativeReach)
            code = generateRegExpCode(inputs, yarrPattern, charSize, Yarr::CodeSizePolicy::MostCompact, PCRelativeReach::Far);
        if (!code) {
            RELEASE_ASSERT(code.error() != Yarr::JITFailureReason::ParenthesisNestedTooDeep);
            if (inputs.context && needsProcessAddress(code.error()))
                inputs.context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
            return { .form = RegExpRecordForm::NoCode, .reason = regExpDeclineReasonFor(code.error()) };
        }
        result.code[charSize == Yarr::CharSize::Char16] = WTF::move(*code);
    }
    return result;
}

uint32_t CompiledRegExps::numberWithForm(RegExpRecordForm form) const
{
    return static_cast<uint32_t>(std::ranges::count(results, form, &CompiledRegExp::form));
}

static LimitID limitBehindRegExpDecline(RegExpDeclineReason reason)
{
    switch (reason) {
    case RegExpDeclineReason::SectionFull:
        return LimitID::RegExpSectionSize;
    case RegExpDeclineReason::NotSupportedByCodeGenerator:
    case RegExpDeclineReason::NestedTooDeep:
    case RegExpDeclineReason::CodeTooLarge:
    case RegExpDeclineReason::PatternTooLong:
        return LimitID::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void applyRegExpSectionSizeLimit(CompiledRegExps& compiled, BuildContext& context)
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

static void reportRegExpDecline(const RegExpRequest& request, RegExpDeclineReason reason, BuildContext& context)
{
    context.diagnostics().add(UserDiagnostic {
        .kind = UserDiagnosticKind::RegExpWithoutCode,
        .limit = limitBehindRegExpDecline(reason),
        .count = static_cast<uint32_t>(reason),
        .text = request.pattern,
    });
}

static void countRegExpResult(Statistics& statistics, const CompiledRegExp& result)
{
    switch (result.form) {
    case RegExpRecordForm::Code:
        statistics.count(StatisticID::RegExpsCompiled);
        return;
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        statistics.count(StatisticID::RegExpsInterpreted);
        return;
    case RegExpRecordForm::Atom:
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
        auto compile = [&] {
            const RegExpSummary& summary = summaries.summaryOf(request.pattern, request.flags, pattern);
            compiled.results[index] = compileRegExp({ pattern, request.flags, summary, context.assemblerPolicy(), &context, RegExpCodeFor16BitSubjects::WhereWideCharactersCanBeInMatchedSubject });
        };
        if (hasStackForRegExpCompiler())
            compile();
        else
            runOnBuildThread(compile);
    });

    applyRegExpSectionSizeLimit(compiled, context);

    for (size_t index = 0; index < allRequests.size(); ++index) {
        const RegExpRequest& request = allRequests[index];
        const CompiledRegExp& result = compiled.results[index];
        if (result.reason)
            reportRegExpDecline(request, *result.reason, context);
        countRegExpResult(context.statistics(), result);
        context.statistics().count(StatisticID::RegExpCandidatesCompiled, request.origin != RegExpRequestOrigin::Literal && result.form == RegExpRecordForm::Code);

        const RegExpSummary* summary = summaries.summaryIfComputed(request.pattern, request.flags);
        RELEASE_ASSERT(summary);
        compiled.numberWithUnicodePropertyLookup += summary->hasUnicodePropertyLookup;
    }
    return compiled;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
