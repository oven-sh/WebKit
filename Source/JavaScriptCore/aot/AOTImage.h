/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTProgram.h"
#include "AOTRuntime.h"
#include "CodeSpecializationKind.h"
#include "YarrFlags.h"
#include "YarrJIT.h"
#include <wtf/Lock.h>
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class ScriptExecutable;
class SourceProvider;
class UnlinkedCodeBlock;
class VM;

namespace AOT {

static constexpr uint64_t imageMagic = 0x3130544f414e5542ULL;
static constexpr size_t imagePageSize = 16 * KB;
static constexpr size_t imageFunctionAlignment = sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16;
static constexpr unsigned maxStubCopiesPerImage = 8;
static constexpr unsigned numberOfAdapters = 6;

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
    uint32_t shapesOffset;
    uint32_t numberOfShapes;
    uint32_t shapeSlotsOffset;
    uint32_t slotRangesOffset;
    uint32_t numberOfSlotRanges;
    uint32_t slotTypesOffset;
    uint32_t fieldRangesOffset;
    uint32_t fieldRecordsOffset;
    uint32_t fieldTypesOffset;
    uint32_t fieldLayoutIDsOffset;
    uint32_t fieldsStartOffset;
    uint32_t fieldsOffset;
    uint32_t layoutIDsByFieldIDOffset;
    uint32_t largestFieldID;
    uint32_t inlineSlotCountsOffset;
    uint32_t auditsTypes;
    uint32_t selectorsOffset;
    uint32_t numberOfSelectors;
    uint32_t selectorRowsOffset;
    uint32_t selectorTextOffset;
    uint32_t selectorsInOrderOffset;
    uint32_t numberOfSelectorsInOrder;
    uint32_t intrinsicHash;
    uint32_t dispatchOffset;
    uint32_t dispatchSize;
    uint32_t quotesOffset;
    uint32_t quotesTextOffset;
    uint32_t quoteTextBlockSize;
    uint32_t quoteBlockSize;
    uint32_t numberOfProgramIdentifiers;
    uint32_t numberOfProgramConstants;
    uint32_t numberOfNamesWithLikelySlots;
    uint32_t regExpsOffset;
    uint32_t numberOfRegExps;
    uint32_t regExpTextOffset;
    uint32_t functionNumbersOffset;
    uint32_t functionStartsOffset;
    uint32_t codeGranulesOffset;
    uint32_t callSitesOffset;
    uint32_t framesOffset;
    uint32_t functionsEnd;
    uint32_t stubsSize;
    uint32_t numberOfStubCopies;
    uint32_t stubCopies[maxStubCopiesPerImage];
    uint32_t returnsIntoAdapters[numberOfAdapters];
    uint32_t stubOffsets[numberOfStubs];
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
};

struct ImageShape {
    uint16_t numberOfProperties;
    uint16_t inlineCapacity;
    uint32_t slots;
    uint16_t layoutID;
    uint16_t reserved;
    uint16_t inlineSlots;
    uint16_t hasIds;
};

struct ImageSelector {
    uint32_t text;
    uint32_t length : 31;
    uint32_t is8Bit : 1;
};

struct ImageDispatchEntry {
    static constexpr unsigned locationBits = 12;
    static uint32_t encode(uint32_t selector, int32_t location) { return selector << locationBits | (static_cast<uint32_t>(location) & ((1u << locationBits) - 1)); }
};

struct ImageEnvironment {
    uint32_t distance;
    uint32_t size;
};

struct CompiledCode {
    Vector<uint8_t> bytes;
    CompiledFunctionInfo info;
    Vector<String> remarks;
};

JS_EXPORT_PRIVATE uint64_t imageStamp();
void didUseLinkTimeConstant(unsigned);

class ImageBuilder {
    WTF_MAKE_TZONE_ALLOCATED(ImageBuilder);
    WTF_MAKE_NONCOPYABLE(ImageBuilder);
public:
    ImageBuilder() = default;

    void add(ImageKey, uint64_t rank, CompiledCode&&, String&& nameForMap = { });
    size_t numberOfFunctions() const { return m_functions.size(); }
    void clear() { m_functions.clear(); }
    void setEnvironments(Vector<ImageEnvironment>&& environments, uint32_t size) { m_environments = WTF::move(environments); m_environmentsSize = size; }
    void setProgramIdentifierIndices(const IdentifierIndices* numbers, uint32_t number)
    {
        m_programIdentifierIndices = numbers;
        m_numberOfProgramIdentifiers = number;
    }
    void setNumberOfProgramConstants(uint32_t number) { m_numberOfProgramConstants = number; }
    void setNumberOfNamesWithLikelySlots(uint32_t number) { m_numberOfNamesWithLikelySlots = number; }
    bool addRegExp(VM&, const String& pattern, OptionSet<Yarr::Flags>);
    Vector<uint8_t> finish();
    Vector<FunctionReportableSites> takeReportableSites() { return std::exchange(m_reportableSites, { }); }

private:
    struct Function {
        ImageKey key;
        uint64_t rank;
        CompiledCode code;
        String nameForMap;
    };
    Lock m_lock;
    Vector<Function> m_functions;
    Vector<ImageEnvironment> m_environments;
    uint32_t m_environmentsSize { 0 };
    Vector<FunctionReportableSites> reportableSites();
    Vector<FunctionReportableSites> m_reportableSites;
    struct RegExpCode {
        String pattern;
        OptionSet<Yarr::Flags> flags;
        Yarr::YarrCodeForImage code[2];
    };
    uint32_t m_numberOfProgramIdentifiers { 0 };
    uint32_t m_numberOfNamesWithLikelySlots { 0 };
    const IdentifierIndices* m_programIdentifierIndices { nullptr };
    uint32_t m_numberOfProgramConstants { 0 };
    Vector<RegExpCode> m_regExps;
    unsigned m_numberOfRegExpsNotCompiled { 0 };
    UncheckedKeyHashMap<String, bool> m_requestedRegExps;
};

class Image {
    WTF_MAKE_TZONE_ALLOCATED(Image);
    WTF_MAKE_NONCOPYABLE(Image);
public:
    JS_EXPORT_PRIVATE static Image* registerImage(std::span<const uint8_t> data, const void* code);

    static bool hasAny();
    static bool containsCode(const void*);
    static Image* withCode();
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
    };
    JS_EXPORT_PRIVATE static std::optional<CodeForRegExp> codeForRegExp(const String& pattern, OptionSet<Yarr::Flags>);

    static Image* withShapes();
    static Image& of(const ImageFunction&);
    std::optional<std::pair<String, bool>> quoteAt(const ImageFunction&, unsigned bytecodeOffset) const;
    String quoteText(uint64_t start, size_t length) const;
    bool constructsAt(const ImageFunction&, unsigned bytecodeOffset) const;
    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(m_data.data() + offset); }
    uint32_t selectorNamed(VM&, const StringImpl&) const;

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
    };
    JS_EXPORT_PRIVATE static std::optional<ImageView> tryCreate(std::span<const uint8_t> data, const void* address);
    JS_EXPORT_PRIVATE std::optional<Function> find(const ImageKey&) const;
    JS_EXPORT_PRIVATE uint32_t stubCodeOffset(Stub) const;
    bool keysAreOmitted() const { return !header().tableCapacity; }
    JS_EXPORT_PRIVATE std::span<const ImageKey> keys() const;
    JS_EXPORT_PRIVATE uint32_t functionIndexWith(const ImageKey&) const;
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
std::optional<ImageKey> imageKeyFor(ScriptExecutable*, CodeSpecializationKind);
JS_EXPORT_PRIVATE ImageKey imageKeyForTopLevelCode(uint32_t module);
unsigned codeHash(std::span<const uint8_t>);

struct ImageCode {
    Image* image { nullptr };
    const ImageFunction* function { nullptr };
    explicit operator bool() const { return !!function; }
};
JS_EXPORT_PRIVATE void collectQuotes(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text, unsigned sourceOffset);
JS_EXPORT_PRIVATE void collectConstructSites(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text = { }, unsigned sourceOffset = 0);
ImageCode findInImage(ScriptExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSScope*);
bool moduleIsLinkedAsCompiled(JSScope*);
Ref<JITCode> codeFromImage(ImageCode, UnlinkedCodeBlock*);
Ref<JITCode> jitCodeForImageFunction(ImageCode, CodeSpecializationKind);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
