/*
 *  Copyright (C) 1999-2001, 2004 Harri Porten (porten@kde.org)
 *  Copyright (c) 2007-2021 Apple Inc. All rights reserved.
 *  Copyright (C) 2009 Torch Mobile, Inc.
 *  Copyright (C) 2010 Peter Varga (pvarga@inf.u-szeged.hu), University of Szeged
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "RegExp.h"

#include "AOTRemovableFeature.h"
#include "Lexer.h"
#include "RegExpCache.h"
#include "RegExpInlines.h"
#include "SourceCharacters.h"
#include "WeakInlines.h"
#include "YarrCharacterSetBits.h"
#include "YarrJIT.h"
#include "YarrPattern.h"
#include "YarrSyntaxChecker.h"
#include <wtf/Assertions.h>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

#include "AOTRegExpInProcessLoader.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeCounters.h"

namespace JSC {

const ClassInfo RegExp::s_info = { "RegExp"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(RegExp) };

#if REGEXP_FUNC_TEST_DATA_GEN
const char* const RegExpFunctionalTestCollector::s_fileName = "/tmp/RegExpTestsData";
RegExpFunctionalTestCollector* RegExpFunctionalTestCollector::s_instance = 0;

RegExpFunctionalTestCollector* RegExpFunctionalTestCollector::get()
{
    if (!s_instance)
        s_instance = new RegExpFunctionalTestCollector();

    return s_instance;
}

void RegExpFunctionalTestCollector::outputOneTest(RegExp* regExp, StringView s, int startOffset, int* ovector, int result)
{
    if ((!m_lastRegExp) || (m_lastRegExp != regExp)) {
        m_lastRegExp = regExp;
        fputc('/', m_file);
        outputEscapedString(regExp->pattern(), true);
        fputc('/', m_file);
        SAFE_FPRINTF(m_file, "%s\n", Yarr::flagsString(regExp->flags()));
    }

    fprintf(m_file, " \"");
    outputEscapedString(s);
    fprintf(m_file, "\", %d, %d, (", startOffset, result);
    for (unsigned i = 0; i <= regExp->numSubpatterns(); i++) {
        int subpatternBegin = ovector[i * 2];
        int subpatternEnd = ovector[i * 2 + 1];
        if (subpatternBegin == -1 || subpatternEnd < subpatternBegin) {
            subpatternBegin = -1;
            subpatternEnd = -1;
        }
        fprintf(m_file, "%d, %d", subpatternBegin, subpatternEnd);
        if (i < regExp->numSubpatterns())
            fputs(", ", m_file);
    }

    fprintf(m_file, ")\n");
    fflush(m_file);
}

RegExpFunctionalTestCollector::RegExpFunctionalTestCollector()
{
    m_file = fopen(s_fileName, "r+");
    if  (!m_file)
        m_file = fopen(s_fileName, "w+");

    fseek(m_file, 0L, SEEK_END);
}

RegExpFunctionalTestCollector::~RegExpFunctionalTestCollector()
{
    fclose(m_file);
    s_instance = 0;
}

void RegExpFunctionalTestCollector::outputEscapedString(StringView s, bool escapeSlash)
{
    int len = s.length();
    
    for (int i = 0; i < len; ++i) {
        char16_t c = s[i];

        switch (c) {
        case '\0':
            fputs("\\0", m_file);
            break;
        case '\a':
            fputs("\\a", m_file);
            break;
        case '\b':
            fputs("\\b", m_file);
            break;
        case '\f':
            fputs("\\f", m_file);
            break;
        case '\n':
            fputs("\\n", m_file);
            break;
        case '\r':
            fputs("\\r", m_file);
            break;
        case '\t':
            fputs("\\t", m_file);
            break;
        case '\v':
            fputs("\\v", m_file);
            break;
        case '/':
            if (escapeSlash)
                fputs("\\/", m_file);
            else
                fputs("/", m_file);
            break;
        case '\"':
            fputs("\\\"", m_file);
            break;
        case '\\':
            fputs("\\\\", m_file);
            break;
        case '\?':
            fputs("\?", m_file);
            break;
        default:
            if (c > 0x7f)
                fprintf(m_file, "\\u%04x", c);
            else 
                fputc(c, m_file);
            break;
        }
    }
}
#endif

RegExp::RegExp(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags)
    : JSCell(vm, vm.regExpStructure.get())
    , m_patternString(patternString)
    , m_flags(flags)
{
    ASSERT(m_flags != Yarr::Flags::DeletedValue);
}

// Shorter patterns are built eagerly: that is cheap and is what recognizes the m_specificPattern shapes.
static constexpr unsigned maxEagerlyConstructedPatternLength = 64;
// Deeper patterns are built eagerly so that YarrPatternConstructor's recursion limit still reports at construction.
static constexpr unsigned maxLazilyConstructedParenthesesDepth = 64;

static void countParsedPattern(VM& vm)
{
#if ENABLE(AOT)
    if (vm.hasCompiledCode()) [[unlikely]]
        AOT::count(vm, AOT::RuntimeCounterID::RegExpPatternsParsed);
#else
    UNUSED_PARAM(vm);
#endif
}

void RegExp::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
#if ENABLE(AOT)
    if (vm.hasCompiledCode()) [[unlikely]] {
        if (const AOT::ImageRegExp* record = AOT::findRegExpRecord(vm, m_patternString, m_flags)) {
            finishCreationFromImage(vm, *record);
            if (record->form() != AOT::RegExpRecordForm::NoMetadata)
                return;
        }
    }
#endif
    // Four of five patterns that a compiled program makes out of text are never matched.
    unsigned maxEagerLength = vm.hasCompiledCode() ? 0 : maxEagerlyConstructedPatternLength;
    if (Options::useLazyRegExpPatternConstruction() && m_patternString.length() > maxEagerLength) {
        countParsedPattern(vm);
        Yarr::SyntaxSummary summary = Yarr::checkSyntax(m_patternString, m_flags);
        m_constructionErrorCode = summary.error;
        if (!isValid()) {
            m_state = ParseError;
            return;
        }
        bool reportsConstructionErrors = summary.hasCountedQuantifier && m_patternString.length() <= maxEagerlyConstructedPatternLength;
        if (!summary.hasNamedCaptureGroups && !summary.isOnlyPatternCharacters && summary.maxParenthesesDepth <= maxLazilyConstructedParenthesesDepth && !reportsConstructionErrors) {
            m_numSubpatterns = summary.numSubpatterns;
            m_ovector = FixedVector<int>(offsetVectorBaseForNamedCaptures());
            return;
        }
    }

    countParsedPattern(vm);
    Yarr::YarrPattern pattern(m_patternString, m_flags, m_constructionErrorCode, Yarr::ExecutionMode::IncludeSubpatterns, vm.hasCompiledCode() ? Yarr::PatternAnalysis::CharacterClassForms : Yarr::PatternAnalysis::Skipped);
    if (!isValid()) {
        m_state = ParseError;
        return;
    }

    updateMetadataFromPattern(pattern);

    unsigned offsetVectorSize = offsetVectorBaseForNamedCaptures();
    if (hasNamedCaptures())
        offsetVectorSize += m_rareData->m_numDuplicateNamedCaptureGroups;
    m_ovector = FixedVector<int>(offsetVectorSize);
}

#if ENABLE(AOT)
static constexpr uint16_t imageRecordIsFoundByPattern = std::numeric_limits<uint16_t>::max();

const AOT::ImageRegExp* RegExp::imageRecord() const
{
    if (!m_imageRecordNumber)
        return nullptr;
    VM& vm = this->vm();
    if (m_imageRecordNumber == imageRecordIsFoundByPattern) [[unlikely]]
        return AOT::findRegExpRecord(vm, m_patternString, m_flags);
    return &AOT::RegExpRecords(*vm.aotState().image).at(AOT::RegExpRecordID(static_cast<uint32_t>(m_imageRecordNumber - 1)));
}

void RegExp::finishCreationFromImage(VM& vm, const AOT::ImageRegExp& record)
{
    // A number says nothing without its image, and any image may run when the record is asked for.
    uint32_t index = vm.aotState().hasSeveralImages ? imageRecordIsFoundByPattern : AOT::RegExpRecords(*vm.aotState().image).idOf(record).index();
    m_imageRecordNumber = index + 1 < imageRecordIsFoundByPattern ? index + 1 : imageRecordIsFoundByPattern;
    if (!record.isLiteral)
        AOT::count(vm, AOT::RuntimeCounterID::RegExpPredictedTextsCreated);
    if (record.form() == AOT::RegExpRecordForm::NoMetadata)
        return;

    m_numSubpatterns = record.numSubpatterns;
    m_minimumSize = record.minimumSize;
    m_specificPattern = record.specificPattern();
    if (record.form() == AOT::RegExpRecordForm::Atom) {
        m_atom = AOT::regExpAtomOf(vm, record);
        m_state = ImageAtom;
    }
    if (record.hasNamedGroups) {
        auto groups = AOT::decodeRegExpNamedGroups(vm, record);
        auto rareData = makeUnique<RareData>();
        rareData->m_numDuplicateNamedCaptureGroups = groups.numDuplicateNamedCaptureGroups;
        rareData->m_captureGroupNames = WTF::move(groups.captureGroupNames);
        rareData->m_namedGroupToParenIndices = WTF::move(groups.namedGroupToParenIndices);
        WTF::storeStoreFence();
        m_rareData = WTF::move(rareData);
    }
    m_ovector = FixedVector<int>(offsetVectorBaseForNamedCaptures() + (m_rareData ? m_rareData->m_numDuplicateNamedCaptureGroups : 0));
}
#endif

void RegExp::updateMetadataFromPattern(Yarr::YarrPattern& pattern)
{
    m_atom = WTF::move(pattern.m_atom);
    m_specificPattern = pattern.m_specificPattern;

    m_numSubpatterns = pattern.m_numSubpatterns;
#if USE(BUN_JSC_ADDITIONS)
    // The compile paths call this again for a RegExp whose RareData finishCreation already built (the callers
    // RELEASE_ASSERT that the pattern parsed to the same subpattern count). Replacing it would free a RareData
    // the concurrent marker may be reading in visitChildren and drop the cached Structure of the groups.
    if (m_rareData)
        return;
#endif
    if (!pattern.m_captureGroupNames.isEmpty() || !pattern.m_namedGroupToParenIndices.isEmpty() || pattern.m_specificCharacterClass) {
        auto rareData = makeUnique<RareData>();
        rareData->m_numDuplicateNamedCaptureGroups = pattern.m_numDuplicateNamedCaptureGroups;
        rareData->m_captureGroupNames = FixedVector<AtomString>::map(pattern.m_captureGroupNames, [](auto& name) {
            return AtomString { name };
        });
        rareData->m_namedGroupToParenIndices.swap(pattern.m_namedGroupToParenIndices);
        rareData->m_specificCharacterClass = WTF::move(pattern.m_specificCharacterClass);
        // The concurrent GC thread reads m_rareData in visitChildren, so it must observe a fully initialized RareData.
        WTF::storeStoreFence();
        m_rareData = WTF::move(rareData);
    }
}

NEVER_INLINE void RegExp::destroy(JSCell* cell)
{
    RegExp* thisObject = static_cast<RegExp*>(cell);
#if REGEXP_FUNC_TEST_DATA_GEN
    RegExpFunctionalTestCollector::get()->clearRegExp(this);
#endif
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::destroy);
    thisObject->RegExp::~RegExp();
}

size_t RegExp::estimatedSize(JSCell* cell, VM& vm)
{
    RegExp* thisObject = static_cast<RegExp*>(cell);
    size_t regexDataSize = thisObject->m_regExpBytecode ? thisObject->m_regExpBytecode->estimatedSizeInBytes() : 0;
#if ENABLE(YARR_JIT)
    if (auto* jitCode = thisObject->m_regExpJITCode.get())
        regexDataSize += jitCode->size();
#endif
    regexDataSize += thisObject->m_ovector.byteSize();
    if (thisObject->m_firstCharacterBitmap.get())
        regexDataSize += sizeof(WTF::BitSet<256>);
    return Base::estimatedSize(cell, vm) + regexDataSize;
}

template<typename Visitor>
void RegExp::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<RegExp>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
#if !USE(BUN_JSC_ADDITIONS)
    if (thisObject->m_rareData)
        visitor.append(thisObject->m_rareData->m_cachedGroupsStructureID);
#endif
}

DEFINE_VISIT_CHILDREN(RegExp);

Structure* RegExp::ensureGroupsStructure(VM& vm, JSGlobalObject* globalObject)
{
    ASSERT(hasNamedCaptures());
#if USE(BUN_JSC_ADDITIONS)
    if (Structure* cached = m_rareData->m_cachedGroupsStructure.get()) {
#else
    if (Structure* cached = m_rareData->m_cachedGroupsStructureID.get()) {
#endif
        if (cached->realm() == globalObject)
            return cached;
    }

    Structure* baseStructure = globalObject->nullPrototypeObjectStructure();
    if (m_rareData->m_namedGroupToParenIndices.size() > baseStructure->inlineCapacity())
        return nullptr;

    Structure* structure = baseStructure;
    PropertyOffset offset = invalidOffset;
    PropertyOffset expectedOffset = 0;
    for (auto& name : m_rareData->m_captureGroupNames) {
        if (name.isEmpty())
            continue;
        structure = Structure::addPropertyTransition(vm, structure, Identifier::fromString(vm, name), 0, offset);
        // Callers store via putDirectOffset assuming sequential inline offsets.
        if (offset != expectedOffset || structure->isDictionary())
            return nullptr;
        expectedOffset++;
    }
    ASSERT(!structure->outOfLineCapacity());
#if USE(BUN_JSC_ADDITIONS)
    m_rareData->m_cachedGroupsStructure = Weak<Structure>(structure);
#else
    m_rareData->m_cachedGroupsStructureID.set(vm, this, structure);
#endif
    return structure;
}

RegExp* RegExp::createWithoutCaching(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags)
{
    RegExp* regExp = new (NotNull, allocateCell<RegExp>(vm)) RegExp(vm, patternString, flags);
    regExp->finishCreation(vm);
    return regExp;
}

#if ENABLE(AOT)
NEVER_INLINE RegExp* RegExp::createFromImage(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags, const AOT::ImageRegExp& record)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::createFromImage);
    ASSERT(AOT::ImageRegExp::significantFlags(flags) == record.flags());
    if (record.form() == AOT::RegExpRecordForm::NoMetadata)
        return RegExp::create(vm, pattern, flags);
    RegExp* regExp = new (NotNull, allocateCell<RegExp>(vm)) RegExp(vm, pattern, flags);
    regExp->Base::finishCreation(vm);
    regExp->finishCreationFromImage(vm, record);
    return regExp;
}
#endif

RegExp* RegExp::create(VM& vm, const String& patternString, OptionSet<Yarr::Flags> flags)
{
    return vm.regExpCache()->lookupOrCreate(vm, patternString, flags);
}

void RegExp::finishCreationFromCache(VM& vm, unsigned numSubpatterns, String&& atom, Yarr::SpecificPattern specificPattern)
{
    Base::finishCreation(vm);
    m_atom = WTF::move(atom);
    // The bytecode cache carries no character set: the first compile finds a class form again.
    bool needsCharacterSet = specificPattern == Yarr::SpecificPattern::CharacterClass || specificPattern == Yarr::SpecificPattern::CharacterClassPlus;
    m_specificPattern = needsCharacterSet ? Yarr::SpecificPattern::None : specificPattern;
    m_numSubpatterns = numSubpatterns;
    m_ovector = FixedVector<int>(offsetVectorBaseForNamedCaptures());
}

std::optional<Yarr::CharacterSetView> RegExp::specificCharacterClass() const
{
    switch (m_specificPattern) {
    case Yarr::SpecificPattern::None:
    case Yarr::SpecificPattern::Atom:
    case Yarr::SpecificPattern::LeadingSpacesStar:
    case Yarr::SpecificPattern::LeadingSpacesPlus:
    case Yarr::SpecificPattern::TrailingSpacesStar:
    case Yarr::SpecificPattern::TrailingSpacesPlus:
    case Yarr::SpecificPattern::Newlines:
    case Yarr::SpecificPattern::OptionalCRThenLF:
        return std::nullopt;
    case Yarr::SpecificPattern::CharacterClass:
    case Yarr::SpecificPattern::CharacterClassPlus:
        if (m_rareData && m_rareData->m_specificCharacterClass)
            return Yarr::CharacterSetView(*m_rareData->m_specificCharacterClass);
#if ENABLE(AOT)
        return AOT::regExpCharacterSetOf(vm(), *imageRecord());
#else
        break;
#endif
    }
    RELEASE_ASSERT_NOT_REACHED();
}


static Yarr::PatternAnalysis patternAnalysisFor(VM& vm)
{
    // The others: so that a shell without a compiled program can test the flat program, and what stands behind the code of an image.
    bool isPerformed = vm.hasCompiledCode() || Options::validateRegExpFlatProgram();
#if ENABLE(AOT)
    // Where nothing compiles a pattern as for an image, the JIT takes the pattern as it is written.
    isPerformed |= Options::useRegExpImageMode();
#endif
    return isPerformed ? Yarr::PatternAnalysis::Performed : Yarr::PatternAnalysis::Skipped;
}

static std::unique_ptr<Yarr::BytecodePattern> byteCodeCompilePattern(VM* vm, Yarr::YarrPattern& pattern, const String& source, Yarr::ErrorCode& errorCode)
{
    return Yarr::byteCompile(pattern, &vm->m_regExpAllocator, errorCode, &vm->m_regExpAllocatorLock, source);
}

#if ENABLE(AOT)
void RegExp::countBytecodeCompilation(VM& vm, BytecodeIsFor purpose) const
{
    if (!vm.hasCompiledCode()) [[likely]]
        return;
    if (purpose == BytecodeIsFor::LongSearch) {
        AOT::count(vm, AOT::RuntimeCounterID::RegExpsPreparedForLongSearches);
        return;
    }
    AOT::count(vm, AOT::RuntimeCounterID::RegExpsCompiledToBytecode);
    if (const AOT::ImageRegExp* record = imageRecord())
        AOT::count(vm, record->isLiteral ? AOT::RuntimeCounterID::RegExpLiteralsCompiledToBytecode : AOT::RuntimeCounterID::RegExpPredictedTextsCompiledToBytecode);
}
#endif

#if ENABLE(AOT)
void RegExp::crashIfLiteralIsInterpreted() const
{
    if (!Options::crashIfInterpreterIsUsed() || !m_imageRecordNumber || !imageRecord()->isLiteral || hasValidAtom()) [[likely]]
        return;
    dataLogLn("crashIfInterpreterIsUsed: regular expression /", m_patternString, "/", StringView { Yarr::flagsString(m_flags).span() });
    CRASH();
}
#endif

void RegExp::byteCodeCompileIfNecessary(VM* vm, BytecodeIsFor purpose)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::byteCodeCompileIfNecessary);
#if ENABLE(AOT)
    if (purpose == BytecodeIsFor::Interpreter)
        crashIfLiteralIsInterpreted();
#endif
    Locker locker { cellLock() };

    if (m_regExpBytecode)
        return;

    countParsedPattern(*vm);
    Yarr::YarrPattern pattern(m_patternString, m_flags, m_constructionErrorCode, Yarr::ExecutionMode::IncludeSubpatterns, patternAnalysisFor(*vm));
    if (hasError(m_constructionErrorCode)) {
        m_state = ParseError;
        return;
    }
    RELEASE_ASSERT(m_numSubpatterns == pattern.m_numSubpatterns); // came from the bytecode cache or Yarr::checkSyntax, not from a YarrPattern
    updateMetadataFromPattern(pattern);

#if ENABLE(AOT)
    countBytecodeCompilation(*vm, purpose);
#endif
    m_regExpBytecode = byteCodeCompilePattern(vm, pattern, m_patternString, m_constructionErrorCode);
    if (!m_regExpBytecode) {
        m_state = ParseError;
        return;
    }
}

#if ENABLE(AOT)
bool RegExp::takeCodeFromImage(VM& vm)
{
    if (hasValidAtom() && vm.hasCompiledCode()) {
        m_state = ImageAtom;
        return true;
    }
    const AOT::ImageRegExp* record = imageRecord();
    if (!record || record->form() != AOT::RegExpRecordForm::Code)
        return false;
    if (!hasCode())
        vm.regExpCache()->addToStrongCache(this);
    AOT::count(vm, record->isLiteral ? AOT::RuntimeCounterID::RegExpLiteralsGivenCode : AOT::RuntimeCounterID::RegExpPredictedTextsGivenCode);
    auto entrypoints = AOT::regExpEntrypointsOf(vm, *record);
    ensureRegExpJITCode().setCodeFromImage(entrypoints.for8Bit, entrypoints.for16Bit);
    m_state = ImageCode;
    m_minimumSize = record->minimumSize;
    return true;
}

int RegExp::matchWithImageCode(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset, int* offsetVector)
{
    ASSERT(m_state == ImageCode);
    if (startOffset > s.length())
        return -1;

    int result;
    {
        Yarr::MatchingContextHolder regExpContext(vm, this, Yarr::MatchFrom::VMThread);
        if (s.is8Bit())
            result = m_regExpJITCode->execute(s.span8(), startOffset, offsetVector, &regExpContext).start;
        else
            result = m_regExpJITCode->execute(s.span16(), startOffset, offsetVector, &regExpContext).start;
    }
    if (result != static_cast<int>(Yarr::JSRegExpResult::JITCodeFailure))
        return result;

    const AOT::ImageRegExp* record = imageRecord();
    if (record && !s.is8Bit() && record->hasCodeFor8BitSubjectsAlone) {
        std::span<const char16_t> wide = s.span16();
        if (!WTF::charactersAreAllLatin1(wide)) {
            AOT::count(vm, AOT::RuntimeCounterID::RegExpWideSubjectsRefused);
            return -1;
        }
        AOT::count(vm, AOT::RuntimeCounterID::RegExpSubjectsNarrowed);
        Vector<Latin1Character, 256> narrow(wide.size(), [&](size_t index) {
            return static_cast<Latin1Character>(wide[index]);
        });
        Yarr::MatchingContextHolder regExpContext(vm, this, Yarr::MatchFrom::VMThread);
        result = m_regExpJITCode->execute(narrow.span(), startOffset, offsetVector, &regExpContext).start;
        if (result != static_cast<int>(Yarr::JSRegExpResult::JITCodeFailure))
            return result;
    }

    bool isLongSearch = record && record->handsLongSearchesOver;
    byteCodeCompileIfNecessary(&vm, isLongSearch ? BytecodeIsFor::LongSearch : BytecodeIsFor::Interpreter);
    if (m_state == ParseError) {
        if (nullOrGlobalObject) {
            auto throwScope = DECLARE_THROW_SCOPE(vm);
            throwScope.throwException(nullOrGlobalObject, errorToThrow(nullOrGlobalObject));
        }
        if (!hasHardError(m_constructionErrorCode))
            reset();
        return -1;
    }
    if (isLongSearch)
        AOT::count(vm, AOT::RuntimeCounterID::RegExpLongSearchesHandedOver);
    else {
        AOT::count(vm, AOT::RuntimeCounterID::RegExpImageCodeGaveUp);
        countInterpretedMatch(vm);
    }
    Yarr::MatchingContextHolder regExpContext(vm, this, Yarr::MatchFrom::VMThread);
    return Yarr::interpret(m_regExpBytecode.get(), s, startOffset, reinterpret_cast<unsigned*>(offsetVector));
}

bool RegExp::collectMatchesOfOneSetOrRun(VM& vm, StringView subject, Vector<unsigned, 128>& matches)
{
    if (m_state != NotCompiled && m_state != ByteCode)
        return false;
    compileIfNecessary(vm, subject.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16, subject);
    if (m_state != ByteCode || !m_regExpBytecode)
        return false;
    return Yarr::collectMatchesOfOneSetOrRun(m_regExpBytecode.get(), subject, matches);
}

void RegExp::countInterpretedMatch(VM& vm)
{
    AOT::count(vm, AOT::RuntimeCounterID::RegExpMatchesInterpreted);
}
#endif

#if ENABLE(YARR_JIT)
bool RegExp::compileWithJIT(VM* vm, Yarr::YarrPattern& pattern, Yarr::CharSize charSize, std::optional<StringView> sampleString, Yarr::ExecutionMode mode)
{
#if ENABLE(JIT)
    auto& jitCode = ensureRegExpJITCode();
    RegExpState stateWithCode = JITCode;
#if ENABLE(AOT)
    if (Options::useRegExpImageMode()) [[unlikely]] {
        callRemovable<RemovableFeature::ImageCompiler, RemovedFeatureStub::Abort, &AOT::jitCompileThroughImage>(StringView(m_patternString), m_flags, jitCode);
        stateWithCode = ImageCode;
    } else
#endif
        callRemovable<RemovableFeature::JIT, RemovedFeatureStub::Abort, &Yarr::jitCompile>(pattern, StringView(m_patternString), charSize, sampleString, vm, jitCode, mode);
    if (jitCode.failureReason())
        return false;
    m_state = stateWithCode;
    m_minimumSize = pattern.m_body->m_minimumSize;
    return true;
#else
    UNUSED_PARAM(vm);
    UNUSED_PARAM(pattern);
    UNUSED_PARAM(charSize);
    UNUSED_PARAM(sampleString);
    UNUSED_PARAM(mode);
    return false;
#endif
}
#endif

void RegExp::compile(VM* vm, Yarr::CharSize charSize, std::optional<StringView> sampleString)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::compile);
    Locker locker { cellLock() };

#if ENABLE(AOT)
    if (takeCodeFromImage(*vm))
        return;
#endif
    countParsedPattern(*vm);
    Yarr::YarrPattern pattern(m_patternString, m_flags, m_constructionErrorCode, Yarr::ExecutionMode::IncludeSubpatterns, patternAnalysisFor(*vm));
    if (hasError(m_constructionErrorCode)) {
        m_state = ParseError;
        return;
    }
    RELEASE_ASSERT(m_numSubpatterns == pattern.m_numSubpatterns); // came from the bytecode cache or Yarr::checkSyntax, not from a YarrPattern
    updateMetadataFromPattern(pattern);

    if (!hasCode()) {
        ASSERT(m_state == NotCompiled);
        vm->regExpCache()->addToStrongCache(this);
        m_state = ByteCode;
    }

#if ENABLE(YARR_JIT)
#if ENABLE(AOT)
    crashIfLiteralIsInterpreted();
#endif
    if (!pattern.containsUnsignedLengthPattern() && Options::useRegExpJIT()
#if !ENABLE(YARR_JIT_BACKREFERENCES)
        && !pattern.m_containsBackreferences
#endif
        && (!pattern.m_containsLookbehinds || Options::useRegExpLookbehindJIT())
        ) {
        if (compileWithJIT(vm, pattern, charSize, sampleString, Yarr::ExecutionMode::IncludeSubpatterns))
            return;
    }
#else
    UNUSED_PARAM(charSize);
    UNUSED_PARAM(sampleString);
#endif

    dataLogLnIf(Options::dumpCompiledRegExpPatterns(), "Can't JIT this regular expression: \"/", m_patternString, "/\"");

    m_state = ByteCode;
#if ENABLE(AOT)
    countBytecodeCompilation(*vm, BytecodeIsFor::Interpreter);
#endif
    m_regExpBytecode = byteCodeCompilePattern(vm, pattern, m_patternString, m_constructionErrorCode);
    if (!m_regExpBytecode) {
        m_state = ParseError;
        return;
    }
    m_minimumSize = pattern.m_body->m_minimumSize;
}

const WTF::BitSet<256>* RegExp::firstCharacterBitmap(FirstCharacterFilterPosition position)
{
    ASSERT_UNUSED(position, position == FirstCharacterFilterPosition::AtStart ? !globalOrSticky() : sticky());
    const auto& result = m_firstCharacterBitmap.ensure([&] {
        auto bitmap = Yarr::computeFirstCharacterBitmap(m_patternString, m_flags);
        if (!bitmap || bitmap->isFull()) {
            WTF::BitSet<256> full;
            full.setAll();
            return makeUnique<WTF::BitSet<256>>(WTF::move(full));
        }
        return makeUnique<WTF::BitSet<256>>(*bitmap);
    });

    if (result.isFull())
        return nullptr;
    return &result;
}

int RegExp::match(JSGlobalObject* globalObject, StringView s, unsigned startOffset, std::span<int> ovector)
{
    return matchInline(globalObject, globalObject->vm(), s, startOffset, ovector);
}

bool RegExp::matchConcurrently(
    VM& vm, StringView s, unsigned startOffset, int& position, std::span<int> ovector)
{
    Locker locker { cellLock() };

#if ENABLE(AOT)
    if (m_state == ImageCode || m_state == ImageAtom)
        return false;
#endif
    if (!hasCodeFor(s.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16))
        return false;

    position = matchInline<Yarr::MatchFrom::CompilerThread>(nullptr, vm, s, startOffset, ovector);
    if (position == static_cast<int>(Yarr::JSRegExpResult::JITCodeFailure))
        return false;
    if (m_state == ParseError)
        return false;
    return true;
}

void RegExp::compileMatchOnly(VM* vm, Yarr::CharSize charSize, std::optional<StringView> sampleString)
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::compileMatchOnly);
    Locker locker { cellLock() };

#if ENABLE(AOT)
    if (takeCodeFromImage(*vm))
        return;
#endif
    bool willUseInterpreter = vm->hasCompiledCode() && !Options::useRegExpJIT();
    countParsedPattern(*vm);
    Yarr::YarrPattern pattern(m_patternString, m_flags, m_constructionErrorCode, willUseInterpreter ? Yarr::ExecutionMode::IncludeSubpatterns : Yarr::ExecutionMode::MatchOnly, patternAnalysisFor(*vm));
    if (hasError(m_constructionErrorCode)) {
        m_state = ParseError;
        return;
    }
    RELEASE_ASSERT(m_numSubpatterns == pattern.m_numSubpatterns); // came from the bytecode cache or Yarr::checkSyntax, not from a YarrPattern
    updateMetadataFromPattern(pattern);

    if (!hasCode()) {
        ASSERT(m_state == NotCompiled);
        vm->regExpCache()->addToStrongCache(this);
        m_state = ByteCode;
    }

#if ENABLE(YARR_JIT)
#if ENABLE(AOT)
    crashIfLiteralIsInterpreted();
#endif
    if (!pattern.containsUnsignedLengthPattern() && Options::useRegExpJIT()
#if !ENABLE(YARR_JIT_BACKREFERENCES)
        && !pattern.m_containsBackreferences
#endif
        && (!pattern.m_containsLookbehinds || Options::useRegExpLookbehindJIT())
        ) {
        if (compileWithJIT(vm, pattern, charSize, sampleString, Yarr::ExecutionMode::MatchOnly))
            return;
    }
#else
    UNUSED_PARAM(charSize);
    UNUSED_PARAM(sampleString);
#endif

    dataLogLnIf(Options::dumpCompiledRegExpPatterns(), "Can't JIT this regular expression: \"/", m_patternString, "/\"");

    m_state = ByteCode;
    // m_regExpBytecode is shared with capture-observing operations (exec/match) and the Yarr
    // interpreter has no StringList fast path, so compile it from a capture-complete pattern rather
    // than the match-only one above, whose capturing fixed-string alternations may have been
    // flattened (dropping their subpatterns) for the JIT.
    if (willUseInterpreter) {
#if ENABLE(AOT)
        countBytecodeCompilation(*vm, BytecodeIsFor::Interpreter);
#endif
        m_regExpBytecode = byteCodeCompilePattern(vm, pattern, m_patternString, m_constructionErrorCode);
        if (!m_regExpBytecode) {
            m_state = ParseError;
            return;
        }
        m_minimumSize = pattern.m_body->m_minimumSize;
        return;
    }
    countParsedPattern(*vm);
    Yarr::YarrPattern bytecodePattern(m_patternString, m_flags, m_constructionErrorCode, Yarr::ExecutionMode::IncludeSubpatterns, patternAnalysisFor(*vm));
    if (hasError(m_constructionErrorCode)) {
        m_state = ParseError;
        return;
    }
#if ENABLE(AOT)
    countBytecodeCompilation(*vm, BytecodeIsFor::Interpreter);
#endif
    m_regExpBytecode = byteCodeCompilePattern(vm, bytecodePattern, m_patternString, m_constructionErrorCode);
    if (!m_regExpBytecode) {
        m_state = ParseError;
        return;
    }
    m_minimumSize = bytecodePattern.m_body->m_minimumSize;
}

MatchResult RegExp::match(JSGlobalObject* globalObject, StringView s, unsigned startOffset)
{
    return matchInline(globalObject, globalObject->vm(), s, startOffset);
}

bool RegExp::matchConcurrently(VM& vm, StringView s, unsigned startOffset, MatchResult& result)
{
    Locker locker { cellLock() };

#if ENABLE(AOT)
    if (m_state == ImageCode || m_state == ImageAtom)
        return false;
#endif
    if (!hasMatchOnlyCodeFor(s.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16))
        return false;

    result = matchInline<Yarr::MatchFrom::CompilerThread>(nullptr, vm, s, startOffset);
    if (result.start == static_cast<size_t>(Yarr::JSRegExpResult::JITCodeFailure))
        return false;
    return true;
}

bool RegExp::wasUsedInCurrentFullCollectionCycle(VM& vm) const
{
    return m_lastUseEpoch == currentUseEpoch(vm);
}

NEVER_INLINE void RegExp::deleteCode()
{
    JSC_REMOVABLE_DEFINITION(RegExp, &RegExp::deleteCode);
    Locker locker { cellLock() };
    
    if (!hasCode())
        return;
    m_state = NotCompiled;
    m_minimumSize = 0;
    // m_atom and m_specificPattern stay: they describe the pattern (every compile sets them to the same values), the fast paths that
    // use them never run the code, and RegExpCachedResult needs the atom of a one-character match it has yet to reify.
#if ENABLE(YARR_JIT)
    if (m_regExpJITCode)
        m_regExpJITCode->clear(locker);
#endif
    m_regExpBytecode = nullptr;
}

#if ENABLE(YARR_JIT)
Yarr::YarrCodeBlock& RegExp::ensureRegExpJITCode()
{
    if (!m_regExpJITCode) {
        auto result = makeUnique<Yarr::YarrCodeBlock>(this);
        WTF::storeStoreFence();
        m_regExpJITCode = WTF::move(result);
    }
    return *m_regExpJITCode.get();
}
#endif

#if ENABLE(YARR_JIT_DEBUG)
void RegExp::matchCompareWithInterpreter(StringView s, int startOffset, int* offsetVector, int jitResult)
{
    int offsetVectorSize = (m_numSubpatterns + 1) * 2;
    Vector<int> interpreterOvector(offsetVectorSize);
    int* interpreterOffsetVector = interpreterOvector.data();
    int interpreterResult = 0;
    int differences = 0;

    // Initialize interpreterOffsetVector with the return value (index 0) and the 
    // first subpattern start indices (even index values) set to -1.
    // No need to init the subpattern end indices.
    for (unsigned j = 0, i = 0; i < m_numSubpatterns + 1; j += 2, i++)
        interpreterOffsetVector[j] = -1;

    interpreterResult = Yarr::interpret(m_regExpBytecode.get(), s, startOffset, reinterpret_cast<unsigned*>(interpreterOffsetVector));

    if (jitResult != interpreterResult)
        differences++;

    for (unsigned j = 2, i = 0; i < m_numSubpatterns; j +=2, i++)
        if ((offsetVector[j] != interpreterOffsetVector[j])
            || ((offsetVector[j] >= 0) && (offsetVector[j+1] != interpreterOffsetVector[j+1])))
            differences++;

    if (differences) {
        dataLog("RegExp Discrepency for ", toSourceString(), "\n    string input ");
        unsigned segmentLen = s.length() - static_cast<unsigned>(startOffset);

        SAFE_DATALOGF((segmentLen < 150) ? "\"%s\"\n" : "\"%148s...\"\n", s.utf8() + startOffset);

        if (jitResult != interpreterResult) {
            dataLogF("    JIT result = %d, interpreted result = %d\n", jitResult, interpreterResult);
            differences--;
        } else {
            dataLogF("    Correct result = %d\n", jitResult);
        }

        if (differences) {
            for (unsigned j = 2, i = 0; i < m_numSubpatterns; j +=2, i++) {
                if (offsetVector[j] != interpreterOffsetVector[j])
                    dataLogF("    JIT offset[%d] = %d, interpreted offset[%d] = %d\n", j, offsetVector[j], j, interpreterOffsetVector[j]);
                if ((offsetVector[j] >= 0) && (offsetVector[j+1] != interpreterOffsetVector[j+1]))
                    dataLogF("    JIT offset[%d] = %d, interpreted offset[%d] = %d\n", j+1, offsetVector[j+1], j+1, interpreterOffsetVector[j+1]);
            }
        }
    }
}
#endif

#if ENABLE(REGEXP_TRACING)
void RegExp::printTraceHeader()
{
    dataLogF("\nRegExp Tracing\n");
    dataLogF("Regular Expression");
    for (unsigned i = 0; i < SameLineFormatedRegExpnWidth - 16; ++i)
        dataLogF(" ");
    dataLogF("    8 Bit       16 Bit     match()    Matches    Average\n");
    dataLogF(" <Match only / Match>");
    for (unsigned i = 0; i < RegExp::SameLineFormatedRegExpnWidth - 21; ++i)
        dataLogF(" ");
    dataLogF("    JIT Addr     JIT Addr     calls      found   String len\n");
    for (unsigned i = 0; i < RegExp::SameLineFormatedRegExpnWidth; ++i)
        dataLogF("-");

    dataLogF("+------------+------------+----------+----------+-----------\n");
}

void RegExp::printTraceData()
{
    UTF8CString formattedRegExp = ""_s;
    char rawPatternBuffer[SameLineFormatedRegExpnWidth + 1];
    String rawPattern;

    auto patternCStr = pattern().utf8(); // Hold a reference so it doesn't get destroyed.
    auto patternStr = patternCStr.legacyCStringPointer();
    auto patternLength = pattern().length();

    auto appendRawPatternBuffer = [&] (size_t& index) {
        if (!index)
            return;

        rawPatternBuffer[index] = '\0';

        rawPattern = makeString(rawPattern, rawPatternBuffer);

        index = 0;
    };

    // Escape literal TAB characters.
    size_t dstIdx = 0;
    for (size_t srcIdx = 0; srcIdx < patternLength; ++srcIdx) {
        auto c = patternStr[srcIdx];
        if (c == '\t') {
            rawPatternBuffer[dstIdx++] = '\\';
            if (dstIdx >= SameLineFormatedRegExpnWidth)
                appendRawPatternBuffer(dstIdx);
            c = 't';
        }

        rawPatternBuffer[dstIdx++] = c;
        if (dstIdx >= SameLineFormatedRegExpnWidth)
            appendRawPatternBuffer(dstIdx);
    }

    appendRawPatternBuffer(dstIdx);

    if (rawPattern.length() + Yarr::flagsString(flags()).span().size() + 2 <= SameLineFormatedRegExpnWidth)
        formattedRegExp = makeString('/', rawPattern, '/', Yarr::flagsString(flags()).span()).utf8();
    else
        SAFE_DATALOGF("/%s/%s\n", rawPattern.utf8(), Yarr::flagsString(flags()));

    constexpr int addrWidth = 12;
#if ENABLE(YARR_JIT)
    constexpr char hexDigits[] = "0123456789abcdef";
    constexpr char fallback[] = "fallback  ";
    constexpr char dashes[] = "----    ";

    String jit8BitMatchOnlyAddr;
    String jit16BitMatchOnlyAddr;
    String jit8BitMatchAddr;
    String jit16BitMatchAddr;

    auto formatAddress = [&] (void* addr) {
        constexpr int jitAddrSignificantWidth = addrWidth - 2;
        uintptr_t addrAsUint = reinterpret_cast<uintptr_t>(addr);

        String formatResult;
        for (int digit = jitAddrSignificantWidth; digit; --digit) {
            auto nibble = (addrAsUint >> ((digit - 1) * 4)) & 0xf;
            if (!formatResult.length()) {
                if (!nibble && digit > 8)
                    continue;

                formatResult = makeString("0x");
            }
            formatResult = makeString(formatResult, hexDigits[nibble]);
        }

        return formatResult;
    };

    switch (m_state) {
    case ParseError:
    case NotCompiled:
#if ENABLE(AOT)
    case ImageCode:
    case ImageAtom:
#endif
        break;
    case ByteCode:
        jit8BitMatchOnlyAddr = makeString(fallback);
        jit16BitMatchOnlyAddr = makeString(dashes);
        jit8BitMatchAddr = makeString(fallback);
        jit16BitMatchAddr = makeString(dashes);
        break;
    case JITCode: {
        Yarr::YarrCodeBlock& codeBlock = *m_regExpJITCode.get();
        jit8BitMatchOnlyAddr = formatAddress(codeBlock.get8BitMatchOnlyAddr());
        jit16BitMatchOnlyAddr = formatAddress(codeBlock.get16BitMatchOnlyAddr());
        jit8BitMatchAddr = formatAddress(codeBlock.get8BitMatchAddr());
        jit16BitMatchAddr = formatAddress(codeBlock.get16BitMatchAddr());
        break;
    }
    }
#else
    constexpr char jitOff[] = "JIT Off";
    String jit8BitMatchOnlyAddr = makeString(jitOff);
    String jit16BitMatchOnlyAddr;
    String jit8BitMatchAddr = makeString(jitOff);
    String jit16BitMatchAddr;
#endif
    unsigned averageMatchOnlyStringLen = (unsigned)(m_rtMatchOnlyTotalSubjectStringLen / m_rtMatchOnlyCallCount);
    unsigned averageMatchStringLen = (unsigned)(m_rtMatchTotalSubjectStringLen / m_rtMatchCallCount);

    SAFE_DATALOGF("%-*.*s %*.*s %*.*s %10d %10d %10u\n", SameLineFormatedRegExpnWidth, SameLineFormatedRegExpnWidth, formattedRegExp, addrWidth, addrWidth, jit8BitMatchOnlyAddr.utf8(), addrWidth, addrWidth, jit16BitMatchOnlyAddr.utf8(), m_rtMatchOnlyCallCount, m_rtMatchOnlyFoundCount, averageMatchOnlyStringLen);
    for (unsigned i = 0; i < SameLineFormatedRegExpnWidth; ++i)
        dataLog(" ");
    SAFE_DATALOGF(" %*.*s %*.*s %10d %10d %10u\n", addrWidth, addrWidth, jit8BitMatchAddr.utf8(), addrWidth, addrWidth, jit16BitMatchAddr.utf8(), m_rtMatchCallCount, m_rtMatchFoundCount, averageMatchStringLen);
}
#endif

void RegExp::dumpToStream(const JSCell* cell, PrintStream& out)
{
    // This function can be called concurrently. So we must not ref m_pattern.
    auto* regExp = uncheckedDowncast<RegExp>(cell);
    out.print(toUTF8CString("/", regExp->pattern().impl(), "/", StringView { Yarr::flagsString(regExp->flags()).span() }));
}

template <typename CharacterType>
static inline void appendLineTerminatorEscape(StringBuilder&, CharacterType);

template <>
inline void appendLineTerminatorEscape<Latin1Character>(StringBuilder& builder, Latin1Character lineTerminator)
{
    if (lineTerminator == '\n')
        builder.append('n');
    else
        builder.append('r');
}

template <>
inline void appendLineTerminatorEscape<char16_t>(StringBuilder& builder, char16_t lineTerminator)
{
    if (lineTerminator == '\n')
        builder.append('n');
    else if (lineTerminator == '\r')
        builder.append('r');
    else if (lineTerminator == 0x2028)
        builder.append("u2028"_s);
    else
        builder.append("u2029"_s);
}

template <typename CharacterType>
static inline String escapePattern(const String& pattern, std::span<const CharacterType> characters)
{
    bool previousCharacterWasBackslash = false;
    bool inBrackets = false;
    bool shouldEscape = false;

    // 15.10.6.4 specifies that RegExp.prototype.toString must return '/' + source + '/',
    // and also states that the result must be a valid RegularExpressionLiteral. '//' is
    // not a valid RegularExpressionLiteral (since it is a single line comment), and hence
    // source cannot ever validly be "". If the source is empty, return a different Pattern
    // that would match the same thing.
    if (characters.empty())
        return "(?:)"_s;

    auto contains = [&](CharacterType character) {
        return WTF::find(characters, character) != notFound;
    };
    bool hasCharacterToEscape = contains('/') || contains('\n') || contains('\r');
    if constexpr (sizeof(CharacterType) > 1)
        hasCharacterToEscape = hasCharacterToEscape || contains(0x2028) || contains(0x2029);
    if (!hasCharacterToEscape)
        return pattern;

    // early return for strings that don't contain a forwards slash and LineTerminator
    for (auto ch : characters) {
        if (!previousCharacterWasBackslash) {
            if (inBrackets) {
                if (ch == ']')
                    inBrackets = false;
            } else {
                if (ch == '/') {
                    shouldEscape = true;
                    break;
                }
                if (ch == '[')
                    inBrackets = true;
            }
        }

        if (isLineTerminator<CharacterType>(ch)) {
            shouldEscape = true;
            break;
        }

        if (previousCharacterWasBackslash)
            previousCharacterWasBackslash = false;
        else
            previousCharacterWasBackslash = ch == '\\';
    }

    if (!shouldEscape)
        return pattern;

    previousCharacterWasBackslash = false;
    inBrackets = false;
    StringBuilder result;
    for (auto ch : characters) {
        if (!previousCharacterWasBackslash) {
            if (inBrackets) {
                if (ch == ']')
                    inBrackets = false;
            } else {
                if (ch == '/')
                    result.append('\\');
                else if (ch == '[')
                    inBrackets = true;
            }
        }

        // escape LineTerminator
        if (isLineTerminator<CharacterType>(ch)) {
            if (!previousCharacterWasBackslash)
                result.append('\\');

            appendLineTerminatorEscape<CharacterType>(result, ch);
        } else
            result.append(ch);

        if (previousCharacterWasBackslash)
            previousCharacterWasBackslash = false;
        else
            previousCharacterWasBackslash = ch == '\\';
    }

    return result.toString();
}

String RegExp::escapedPattern(const String& pattern)
{
    return pattern.is8Bit() ? escapePattern(pattern, pattern.span8()) : escapePattern(pattern, pattern.span16());
}

String RegExp::escapedPattern() const
{
#if ENABLE(AOT)
    if (m_imageRecordNumber && imageRecord()->sourceIsPattern)
        return m_patternString;
#endif
    return escapedPattern(m_patternString);
}

String RegExp::toSourceString() const
{
    return makeString('/', escapedPattern(), '/', Yarr::flagsString(flags()).span());
}

} // namespace JSC
