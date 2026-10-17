/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTProgram.h"
#include "AOTRuntime.h"
#include "CPU.h"
#include "CodeSpecializationKind.h"
#include "YarrFlags.h"
#include "YarrJIT.h"
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/PageBlock.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class ScriptExecutable;
class SourceProvider;
class UnlinkedCodeBlock;
class VM;

namespace AOT {

static constexpr uint64_t imageMagic = 0x3130544f414e5542ULL;
static constexpr size_t imagePageSize = std::max<size_t>(CeilingOnPageSize, 16 * KB);
static constexpr size_t imageFunctionAlignment = isX86() ? 2 : sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16;
static constexpr unsigned maxStubCopiesPerImage = 48;
static constexpr unsigned numberOfAdapters = 9;
static constexpr unsigned numberOfTargetFindingStubs = 3;
static constexpr unsigned maxReturnsIntoHostFunctionFrames = numberOfTargetFindingStubs + 1;

struct ImageHeader {
    uint64_t magic;
    uint64_t stamp;
    uint64_t size;
    uint64_t codeOffset;
    uint64_t codeSize;
    uint64_t linkTimeConstantsUsed[4];
    uint32_t tableOffset;
    uint32_t tableCapacity;
    uint32_t recordsOffset;
    uint32_t recordsSize;
    uint32_t numberOfFunctions;
    uint32_t totalDataSizeIn16Bytes;
    uint32_t environmentsSize;
    uint32_t environmentsOffset;
    uint32_t numberOfEnvironments;
    uint32_t intrinsicHash;
    uint32_t quotesOffset;
    uint32_t quoteStringsOffset;
    uint32_t quoteStringBlockSize;
    uint32_t quoteBlockSize;
    uint32_t numberOfProgramIdentifiers;
    uint32_t numberOfProgramConstants;
    uint32_t regExpsOffset;
    uint32_t numberOfRegExps;
    uint32_t regExpSourceOffset;
    uint32_t functionStartsOffset;
    uint32_t codeGranulesOffset;
    uint32_t callSitesOffset;
    uint32_t faultingAccessesOffset;
    uint32_t numberOfFaultingAccesses;
    uint32_t numberOfFaultingAccessesWithWideName;
    uint32_t numberOfFaultingAccessThrowSites;
    uint32_t numberOfTrapsAtThrowSites;
    uint32_t framesOffset;
    uint32_t typedDataOffset;
    uint32_t typedDataSize;
    uint32_t functionsEnd;
    uint32_t stubsSize;
    uint32_t numberOfStubCopies;
    uint32_t stubCopies[maxStubCopiesPerImage];
    uint32_t returnsIntoAdapters[numberOfAdapters];
    uint32_t returnsFromFindingTargets[maxReturnsIntoHostFunctionFrames];
    uint32_t stubOffsets[numberOfStubs];
    uint32_t codeGranuleShift;
    uint32_t webAssemblyDirectoryOffset;
    uint32_t webAssemblyCodeOffset;
};

struct ImageRegExp {
    static uint32_t hashOf(const String& pattern, OptionSet<Yarr::Flags> flags) { return pattern.hash() * 31 + significantFlags(flags).toRaw(); }
    static OptionSet<Yarr::Flags> significantFlags(OptionSet<Yarr::Flags> flags) { return flags - OptionSet<Yarr::Flags> { Yarr::Flags::Global, Yarr::Flags::HasIndices }; }

    uint32_t hash;
    uint32_t text;
    uint32_t length : 31;
    uint32_t is8Bit : 1;
    uint32_t flags;
    uint32_t codeFor8Bit;
    uint32_t codeFor16Bit;
    uint32_t numSubpatterns;
    uint32_t minimumSize;
    uint8_t specificPattern;
    uint8_t retainsPatternDetails;
    uint8_t hasNoCode : 1;
    uint8_t sourceIsInPieces : 1;
    uint8_t sourceIsDigest : 1;
    uint8_t sourceIsPattern;

    static constexpr unsigned maxLengthWithCode = 65536;
    static constexpr unsigned minLengthStoredAsImageString = 4096;
    static constexpr unsigned maxParenthesesDepthWithoutPattern = 64;
};

struct ImageEnvironment {
    uint32_t distance;
    uint32_t size;
};

struct DataInCode {
    uint32_t offset;
    uint32_t numberOfWords : 31;
    uint32_t isTable : 1;
};

struct CompiledCode {
    Vector<uint8_t> bytes;
    Vector<uint8_t> instructions;
    Vector<DataInCode> dataInCode;
    uint32_t appendedEntriesStart { std::numeric_limits<uint32_t>::max() };
    CompiledFunctionInfo info;
    String whyDeclined;
    String bytesByOperation;
    bool mayBeDumped { true };
    bool isRewritten { false };
    bool hasCodeAnnotations { false };
    bool stubCallsAreCleared { false };
    bool rareCodeMayBeOutlined { false };
    bool allCodeMayBeOutlined { false };
    uint32_t rareCodeStart { std::numeric_limits<uint32_t>::max() };
    Vector<CompiledCode> continuations;
};

JS_EXPORT_PRIVATE uint64_t imageStamp();

class ImageBuilder {
    WTF_MAKE_TZONE_ALLOCATED(ImageBuilder);
    WTF_MAKE_NONCOPYABLE(ImageBuilder);
public:
    ImageBuilder() = default;

    void add(ImageKey, uint64_t rank, CompiledCode&&, String&& nameForMap = { });
    static void clearStubCalls(CompiledCode&);
    size_t growFunctions(size_t number);
    void set(size_t place, ImageKey, uint64_t rank, CompiledCode&&, String&& nameForMap);
    size_t numberOfFunctions() const { return m_functions.size(); }
    void clear() { m_functions.clear(); }
    void setEnvironments(Vector<ImageEnvironment>&& environments, uint32_t size) { m_environments = WTF::move(environments); m_environmentsSize = size; }
    void setProgramIdentifierIndices(const IdentifierIndices* numbers, uint32_t number)
    {
        m_programIdentifierIndices = numbers;
        m_numberOfProgramIdentifiers = number;
    }
    void setNumberOfProgramConstants(uint32_t number) { m_numberOfProgramConstants = number; }
    void setTypedData(Vector<uint8_t>&& data) { m_typedData = WTF::move(data); }
    void setWebAssembly(Vector<uint8_t>&& directory, Vector<uint8_t>&& code)
    {
        m_webAssemblyDirectory = WTF::move(directory);
        m_webAssemblyCode = WTF::move(code);
    }
    class PrecompiledRegExps;
    void takePrecompiledRegExps(PrecompiledRegExps&);
    void requestRegExp(VM&, const String& pattern, OptionSet<Yarr::Flags>, bool isFoundByEvaluator = false);
    bool compileOneRequestedRegExp();
    void didRequestAllRegExps();
    void compileRegExpsUntilAllAreRequested();
    Vector<uint8_t> finish();
    const String& failureReason() const { return m_failureReason; }
    Vector<FunctionReportableSites> takeReportableSites() { return std::exchange(m_reportableSites, { }); }

private:
    struct Function {
        ImageKey key;
        uint64_t rank;
        CompiledCode code;
        String nameForMap;
    };
    class Functions {
        WTF_MAKE_NONCOPYABLE(Functions);
    public:
        Functions() = default;
        Functions(Functions&& other) { swap(other); }
        Functions& operator=(Functions&& other)
        {
            Functions(WTF::move(other)).swap(*this);
            return *this;
        }
        ~Functions() { clear(); }
        size_t size() const { return m_size; }
        bool isEmpty() const { return !m_size; }
        Function& operator[](size_t index)
        {
            RELEASE_ASSERT(index < m_size);
            return m_first[index];
        }
        const Function& operator[](size_t index) const
        {
            RELEASE_ASSERT(index < m_size);
            return m_first[index];
        }
        Function* begin() { return m_first; }
        Function* end() { return m_first + m_size; }
        const Function* begin() const { return m_first; }
        const Function* end() const { return m_first + m_size; }
        std::span<const Function> span() const { return { m_first, m_size }; }
        void append(Function&&);
        void growOnPool(size_t newSize);
        void clear();

    private:
        void swap(Functions&);
        void reserveCapacity(size_t);
        Function* m_first { nullptr };
        size_t m_size { 0 };
        size_t m_capacity { 0 };
    };
    Lock m_lock;
    String m_failureReason;
    Functions m_functions;
    Vector<ImageEnvironment> m_environments;
    uint32_t m_environmentsSize { 0 };
    Vector<FunctionReportableSites> reportableSites();
    Vector<FunctionReportableSites> m_reportableSites;
    struct RegExpCode {
        String pattern;
        OptionSet<Yarr::Flags> flags;
        Yarr::YarrCodeForImage code[2];
        unsigned numSubpatterns { 0 };
        unsigned minimumSize { 0 };
        Yarr::SpecificPattern specificPattern { Yarr::SpecificPattern::None };
        bool retainsPatternDetails { false };
        bool hasNoCode { false };
        bool sourceIsPattern { false };
        bool isFoundByEvaluator { false };
    };
    struct RegExpRequest {
        String pattern;
        OptionSet<Yarr::Flags> flags;
        bool isFoundByEvaluator { false };
    };
    struct RegExpOutcome {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RegExpOutcome);
        std::optional<RegExpCode> code;
        bool fallsBackToInterpreter { false };
    };
    using RegExpOutcomes = UncheckedKeyHashMap<String, std::unique_ptr<RegExpOutcome>>;
    static String flagsAndPattern(OptionSet<Yarr::Flags>, const String& pattern);
    static std::optional<RegExpCode> compileRegExp(VM&, const RegExpRequest&, bool& fallsBackToInterpreter);
    bool takeAndCompileOneRegExp();
    void compileRemainingRegExps();
    uint32_t m_numberOfProgramIdentifiers { 0 };
    const IdentifierIndices* m_programIdentifierIndices { nullptr };
    uint32_t m_numberOfProgramConstants { 0 };
    Vector<uint8_t> m_typedData;
    Vector<uint8_t> m_webAssemblyDirectory;
    Vector<uint8_t> m_webAssemblyCode;
    Vector<RegExpCode> m_regExps;
    RegExpOutcomes m_precompiledRegExps;
    unsigned m_numberOfRegExpsNotCompiled { 0 };
    UncheckedKeyHashSet<String> m_requestedRegExps;
    Vector<RegExpRequest> m_regExpRequests;
    size_t m_nextRegExpRequest { 0 };
    std::atomic<size_t> m_numberOfRegExpsWaiting { 0 };
    bool m_didRequestAllRegExps { false };
    Condition m_regExpIsRequested;
    VM* m_regExpVM { nullptr };
    Seconds m_timeInRegExpCompiler;
    UncheckedKeyHashSet<const Thread*> m_regExpCompilerThreads;
};

using RegExpSourcesAndFlags = Vector<std::pair<String, OptionSet<Yarr::Flags>>>;

class ImageBuilder::PrecompiledRegExps {
    WTF_MAKE_NONCOPYABLE(PrecompiledRegExps);
public:
    PrecompiledRegExps() = default;
    void compile(VM&, const RegExpSourcesAndFlags&);

private:
    friend class ImageBuilder;
    Lock m_lock;
    bool m_isClaimed { false };
    RegExpOutcomes m_outcomes;
};

class DiscoveredRegExps {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DiscoveredRegExps);
    WTF_MAKE_NONCOPYABLE(DiscoveredRegExps);
public:
    explicit DiscoveredRegExps(std::span<const uint8_t> typedProgramAsWritten);
    ~DiscoveredRegExps();
    bool isEvaluated();
    const RegExpSourcesAndFlags& waitForEvaluatedSources();
    ImageBuilder::PrecompiledRegExps& precompiledRegExps() { return m_precompiledRegExpSet; }

private:
    ImageBuilder::PrecompiledRegExps m_precompiledRegExpSet;
    Lock m_lock;
    Condition m_didEvaluate;
    bool m_isEvaluated { false };
    RegExpSourcesAndFlags m_evaluatedSources;
    RefPtr<Thread> m_thread;
};

class Image {
    WTF_MAKE_TZONE_ALLOCATED(Image);
    WTF_MAKE_NONCOPYABLE(Image);
public:
    JS_EXPORT_PRIVATE static Image* registerImage(std::span<const uint8_t> data, const void* code);

    static bool hasAny();
    static bool containsCode(const void*);
    JS_EXPORT_PRIVATE static bool containsCharactersOf(const StringImpl&);
    static bool isBackedByStaticData(const StringImpl& string)
    {
        switch (string.bufferOwnership()) {
        case StringImpl::BufferInternal:
            if (string.is8Bit())
                return std::bit_cast<uintptr_t>(string.span8().data()) != std::bit_cast<uintptr_t>(&string) + StringImpl::headerSize<Latin1Character>();
            return std::bit_cast<uintptr_t>(string.span16().data()) != std::bit_cast<uintptr_t>(&string) + StringImpl::headerSize<char16_t>();
        case StringImpl::BufferSubstring:
            return containsCharactersOf(string);
        case StringImpl::BufferOwned:
        case StringImpl::BufferExternal:
            break;
        }
        return false;
    }
    static Image* withCode();
    JS_EXPORT_PRIVATE static std::pair<const uint8_t*, const uint8_t*> webAssembly();
    JS_EXPORT_PRIVATE static bool usesNumberedIntrinsics();
    const void* code() const { return m_code; }
    JS_EXPORT_PRIVATE static uint32_t environmentsSize();
    JS_EXPORT_PRIVATE static uint32_t numberOfFunctions();
    static size_t totalDataSize();
    JS_EXPORT_PRIVATE static ImageEnvironment environmentOf(uint32_t graphModule);
    static const void* stubAddress(Stub);
    static std::pair<Image*, const ImageFunction*> find(const ImageKey&);

    struct CodeForRegExp {
        const void* for8Bit;
        const void* for16Bit;
        unsigned numSubpatterns;
        unsigned minimumSize;
        Yarr::SpecificPattern specificPattern;
        bool retainsPatternDetails;
        bool hasCode;
        bool sourceIsPattern;
        StringView text;
        const ImageRegExp* recordWithSourceInPieces;
        bool wasFoundByDigest { false };
    };
    JS_EXPORT_PRIVATE static std::optional<CodeForRegExp> codeForRegExp(const String& pattern, OptionSet<Yarr::Flags>, bool wasFoundBefore = false);
    JS_EXPORT_PRIVATE static CodeForRegExp codeForRegExpRecord(const ImageRegExp&);
    JS_EXPORT_PRIVATE static const ImageRegExp* findRegExpWithSourceInPieces(std::span<const StringView> leaves, unsigned length, OptionSet<Yarr::Flags>);
    JS_EXPORT_PRIVATE static String regExpSource(const ImageRegExp&);

    static Image& of(const ImageFunction&);
    static Image& ofRegExp(const ImageRegExp&);
    std::optional<std::pair<String, bool>> quoteAt(const ImageFunction&, unsigned bytecodeOffset) const;
    String quoteString(uint64_t start, size_t length) const;
    bool constructsAt(const ImageFunction&, unsigned bytecodeOffset) const;
    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(m_data.data() + offset); }

    const uint8_t* codeFor(const ImageFunction& function) const { return static_cast<const uint8_t*>(m_code) + at<uint32_t>(header().functionStartsOffset)[function.index]; }
    size_t sizeOfCodeOf(const ImageFunction& function) const
    {
        const uint32_t* starts = at<uint32_t>(header().functionStartsOffset);
        return (function.index + 1 < header().numberOfFunctions ? starts[function.index + 1] : header().functionsEnd) - starts[function.index];
    }
    const ImageFrame& frameOf(const ImageFunction& function) const { return at<ImageFrame>(header().framesOffset)[function.frame]; }
    const ImageHeader& header() const { return *reinterpret_cast<const ImageHeader*>(m_data.data()); }

private:
    Image(std::span<const uint8_t> data, const void* code)
        : m_data(data)
        , m_code(code)
    {
    }
    const ImageFunction* lookup(const ImageKey&) const;

    std::span<const uint8_t> m_data;
    const void* m_code;
};

class ImageView {
public:
    struct Function {
        uint64_t entry;
        uint32_t index;
        const Site* sites;
        const ImageFunction* function;
        uint32_t numSlots;
        bool startsCold;
        bool hasSiteConstants;
        bool hasInlineFrames;
        bool hasValueEntry;
        bool hasCompatibleResultsMask;
    };
    JS_EXPORT_PRIVATE static std::optional<ImageView> tryCreate(std::span<const uint8_t> data, const void* address);
    JS_EXPORT_PRIVATE std::optional<Function> find(const ImageKey&) const;
    bool keysAreOmitted() const { return !header().tableCapacity; }
    JS_EXPORT_PRIVATE std::span<const ImageKey> keys() const;
    size_t numberOfFunctions() const { return header().numberOfFunctions; }
    uint32_t numberOfProgramIdentifiers() const { return header().numberOfProgramIdentifiers; }
    uint32_t numberOfProgramConstants() const { return header().numberOfProgramConstants; }

private:
    ImageView(std::span<const uint8_t> data, const void* address)
        : m_data(data)
        , m_address(static_cast<const uint8_t*>(address))
    {
    }
    const ImageHeader& header() const { return *reinterpret_cast<const ImageHeader*>(m_data.data()); }

    std::span<const uint8_t> m_data;
    const uint8_t* m_address;
};

uint32_t moduleIDFor(SourceProvider&);
std::optional<ImageKey> imageKeyFor(ScriptExecutable*);
JS_EXPORT_PRIVATE ImageKey imageKeyForTopLevelCode(uint32_t module);
unsigned codeHash(std::span<const uint8_t>);

struct ImageCode {
    Image* image { nullptr };
    const ImageFunction* function { nullptr };
    explicit operator bool() const { return !!function; }
};
struct FaultingAccessInImage {
    static constexpr uint32_t hasNoFrameBit = hasFramelessFunctionStubs ? 1 : isX86_64() ? 1u << 31 : 0;
    static constexpr uintptr_t reach = 16 * KB;

    std::array<uint32_t, 2> throwSite;
    bool hasNoFrame;
};
std::optional<FaultingAccessInImage> faultingAccessAt(const void* instruction);
void prepareFaultingAccesses();
JS_EXPORT_PRIVATE void collectQuotes(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text, unsigned sourceOffset);
JS_EXPORT_PRIVATE void collectConstructSites(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text = { }, unsigned sourceOffset = 0);
ImageCode findInImage(ScriptExecutable*, JSScope*);
Ref<JITCode> codeFromImage(ImageCode, UnlinkedCodeBlock*);
Ref<JITCode> jitCodeForImageFunction(ImageCode, CodeSpecializationKind);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
