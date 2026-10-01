/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

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

// The machine code of a whole program, compiled before the program runs: the equivalent of a text section plus a table for finding
// each function's code. It contains no absolute addresses, so every process can use it wherever it is mapped.
//
//     ImageHeader | hash table of ImageKey | function records | padding to a page | code
//
// The image starts on a page boundary of its file, so the code can be mapped executable directly from the file.

static constexpr uint64_t imageMagic = 0x3130544f414e5542ULL; // "BUNAOT01"
static constexpr size_t imagePageSize = 16 * KB;
static constexpr size_t imageFunctionAlignment = sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16; // Also the alignment of everything else that is not a function.
static constexpr unsigned mostCopiesOfStubsInImage = 8;
static constexpr unsigned numberOfAdapters = 5;

struct ImageHeader {
    uint64_t magic;
    uint64_t stamp; // Identifies the engine build. The code embeds that build's field offsets and table indices.
    uint64_t size; // Total size.
    uint64_t codeOffset;
    uint64_t codeSize;
    uint64_t linkTimeConstantsUsed[4]; // One bit per LinkTimeConstant the code loads. Each must be present in Instance::linkTimeConstants.
    uint32_t tableOffset;
    uint32_t tableCapacity; // A power of two, or zero if the table is omitted (StaticHeap::keysOfImage()).
    uint32_t recordsOffset;
    uint32_t recordsSize;
    uint32_t numberOfFunctions;
    uint32_t environmentsSize; // Size of the area below the Instance. See Instance::placeForEnvironment().
    uint32_t environmentsOffset; // ImageEnvironment[], indexed by module index in the linked module graph.
    uint32_t numberOfEnvironments;
    // Shape and selector numbers start at one.
    uint32_t shapesOffset; // ImageShape[], by shape number.
    uint32_t numberOfShapes; // Highest shape number plus one.
    uint32_t slotsOfShapesOffset; // uint16_t[]. See ImageShape::slots.
    // Tables for TypedLayoutTable (Options::useAOTTypedFields()). See TypedLayoutTable::setSlotTypes() and setFields().
    // numberOfSlotRanges is zero if typed layouts are not used.
    uint32_t slotRangesOffset;
    uint32_t numberOfSlotRanges;
    uint32_t slotTypesOffset;
    uint32_t fieldRangesOffset; // Zero if there are no typed fields.
    uint32_t fieldRecordsOffset;
    uint32_t fieldTypesOffset;
    uint32_t fieldLayoutIDsOffset;
    uint32_t startOfFieldsOffset;
    uint32_t fieldsOffset;
    uint32_t layoutIDsByFieldIDOffset;
    uint32_t largestFieldID; // Property name IDs (VM::aotPropertyNameIDs) start above this. Both kinds of ID are stored in Structure::m_fieldIDInSlot.
    uint32_t inlineSlotCountsOffset;
    uint32_t auditsTypes; // Options::auditAOTTypedFields()
    // If numberOfIdentifiersOfProgram is nonzero, a selector number is an identifier number and StaticHeap holds the strings.
    uint32_t selectorsOffset; // ImageSelector[], by selector number.
    uint32_t numberOfSelectors; // Highest selector number plus one.
    uint32_t rowsOfSelectorsOffset; // uint32_t[], by selector number. The dispatch table entry for a shape is at this value plus the shape number.
    uint32_t textOfSelectorsOffset;
    uint32_t selectorsInOrderOffset; // uint32_t[], sorted by length and then by content, with 8-bit selectors first.
    uint32_t numberOfSelectorsInOrder;
    uint32_t hashOfIntrinsics; // ImmutableIntrinsics::hash() if the code refers to intrinsics by index, else zero.
    uint32_t dispatchOffset; // uint32_t[]. See ImageDispatchEntry.
    uint32_t dispatchSize; // Number of entries.
    uint32_t quotesOffset; // Base for ImageFunction::quotes. See Image::quoteAt().
    uint32_t textOfQuotesOffset; // UTF-8.
    // If nonzero, the data is compressed in independent blocks of this many bytes (setCodec()). The stored form is: the number of
    // blocks; the uncompressed size of the last block; the start of each block and the end of the last, relative to the first; then
    // the blocks.
    uint32_t sizeOfBlockOfTextOfQuotes;
    uint32_t sizeOfBlockOfQuotes; // As above.
    uint32_t numberOfIdentifiersOfProgram; // If nonzero, see NumbersOfIdentifiers.
    uint32_t numberOfConstantsOfProgram; // See NumbersOfConstants.
    uint32_t regExpsOffset; // ImageRegExp[], sorted by hash.
    uint32_t numberOfRegExps;
    uint32_t textOfRegExpsOffset;
    // For mapping a code address to a function (classifyAddress()).
    uint32_t numbersOfFunctionsOffset; // uint32_t[], by function index: the function's number in a Type (typeOfFunction()). Only used by Options::validateAOTInferredTypes().
    uint32_t startsOfFunctionsOffset; // uint32_t[], by function index (which is code order): the code offset of each function, plus a final sentinel.
    uint32_t granulesOfCodeOffset; // uint32_t[]: for each 1 << shiftOfGranuleOfCode bytes of code, the last function that starts at or before them.
    uint32_t callSitesOffset; // See callSiteAt().
    uint32_t framesOffset; // ImageFrame[], indexed by ImageFunction::frame.
    uint32_t endOfFunctions; // Code offset.
    uint32_t sizeOfStubs;
    uint32_t numberOfCopiesOfStubs;
    uint32_t copiesOfStubs[mostCopiesOfStubsInImage]; // Code offset of each copy.
    uint32_t returnsIntoAdapters[numberOfAdapters]; // Relative to the start of the stubs. See StubBlob::returnsIntoAdapters.
    uint32_t stubOffsets[numberOfStubs]; // Relative to the start of the code, which begins with a copy of the stubs.
};

// Compiled code for a regular expression, used for any RegExp with the same pattern and flags however it is created.
struct ImageRegExp {
    static uint32_t hashOf(const String& pattern, OptionSet<Yarr::Flags> flags) { return pattern.hash() * 31 + significantFlags(flags).toRaw(); }
    // The other flags do not affect what the pattern matches at a given position, which is all the code computes.
    static OptionSet<Yarr::Flags> significantFlags(OptionSet<Yarr::Flags> flags) { return flags - OptionSet<Yarr::Flags> { Yarr::Flags::Global, Yarr::Flags::HasIndices }; }

    uint32_t hash;
    uint32_t text; // Byte offset of the pattern in the RegExp text.
    uint32_t length : 31; // In characters.
    uint32_t is8Bit : 1;
    uint32_t flags;
    // Code offsets. The code records subpattern positions (Yarr::ExecutionMode::IncludeSubpatterns).
    uint32_t codeFor8Bit;
    uint32_t codeFor16Bit;
};

struct ImageShape {
    uint16_t numberOfProperties;
    uint16_t inlineCapacity;
    uint32_t slots; // Index into ImageHeader::slotsOfShapesOffset of the first property's slot, plus one. Zero if the slots are consecutive.
    uint16_t layoutID; // KnownShape::layoutID
    uint16_t reserved;
    uint16_t inlineSlots;
    uint16_t hasIds; // The slots are followed by the TypedLayoutTable::Field::id of each property (zero if it is not a field).
};

// A property name used by property reads.
struct ImageSelector {
    uint32_t text; // Byte offset in the selector text.
    uint32_t length : 31; // In characters.
    uint32_t is8Bit : 1;
};

// One program-wide table gives the location of every property of every shape (row displacement dispatch). There is one row per
// selector. Rows overlap wherever the shapes that have one selector lack the other, so each entry records which selector owns it.
// If the entry belongs to a different selector, objects of that shape have no own property with that name.
struct ImageDispatchEntry {
    static constexpr unsigned locationBits = 12; // As AOT::locationOfProperty(): in words, relative to the object or, if negative, to its butterfly.
    static uint32_t encode(uint32_t selector, int32_t location) { return selector << locationBits | (static_cast<uint32_t>(location) & ((1u << locationBits) - 1)); }
};

// The location of a module's JSModuleEnvironment, the same in every realm that runs the module's code from the image.
struct ImageEnvironment {
    uint32_t distance; // Distance below the Instance. Zero if it has no fixed location.
    uint32_t size;
};

// The result of compiling one function.
struct CompiledCode {
    Vector<uint8_t> bytes;
    CompiledFunctionInfo info;
};

JS_EXPORT_PRIVATE uint64_t imageStamp();
void noteThatLinkTimeConstantIsUsed(unsigned); // Called while compiling for an image. Any thread.

class ImageBuilder {
    WTF_MAKE_TZONE_ALLOCATED(ImageBuilder);
    WTF_MAKE_NONCOPYABLE(ImageBuilder);
public:
    ImageBuilder() = default;

    // Any thread. Code is laid out in `rank` order, regardless of call order.
    void add(ImageKey, uint64_t rank, CompiledCode&&);
    size_t numberOfFunctions() const { return m_functions.size(); }
    void clear() { m_functions.clear(); } // Removes all functions.
    void setEnvironments(Vector<ImageEnvironment>&& environments, uint32_t size) { m_environments = WTF::move(environments); m_environmentsSize = size; }
    // `numbers` must stay valid until finish() returns. `number` is the highest identifier number plus one.
    void setNumbersOfIdentifiersOfProgram(const NumbersOfIdentifiers* numbers, uint32_t number)
    {
        m_numbersOfIdentifiersOfProgram = numbers;
        m_numberOfIdentifiersOfProgram = number;
    }
    void setNumberOfConstantsOfProgram(uint32_t number) { m_numberOfConstantsOfProgram = number; }
    // VM thread only. Returns false if the pattern cannot be compiled.
    bool addRegExp(VM&, const String& pattern, OptionSet<Yarr::Flags>);
    Vector<uint8_t> finish();
    Vector<ReportableSitesOfFunction> takeReportableSites() { return std::exchange(m_reportableSites, { }); } // Call after finish(). Indexed by function index.

private:
    struct Function {
        ImageKey key;
        uint64_t rank;
        CompiledCode code;
    };
    Lock m_lock;
    Vector<Function> m_functions;
    Vector<ImageEnvironment> m_environments;
    uint32_t m_environmentsSize { 0 };
    Vector<ReportableSitesOfFunction> reportableSites();
    Vector<ReportableSitesOfFunction> m_reportableSites;
    struct RegExpCode {
        String pattern;
        OptionSet<Yarr::Flags> flags;
        Yarr::YarrCodeForImage code[2]; // 8-bit, 16-bit.
    };
    uint32_t m_numberOfIdentifiersOfProgram { 0 };
    const NumbersOfIdentifiers* m_numbersOfIdentifiersOfProgram { nullptr };
    uint32_t m_numberOfConstantsOfProgram { 0 };
    Vector<RegExpCode> m_regExps;
    UncheckedKeyHashMap<String, bool> m_regExpsAsked; // Keyed by flags and pattern.
};

// A loaded image. Images are kept in a process-wide list and are never unloaded.
class Image {
    WTF_MAKE_TZONE_ALLOCATED(Image);
    WTF_MAKE_NONCOPYABLE(Image);
public:
    // `data` is the image, which must stay readable for the life of the process. `code` is where its code is mapped executable.
    // Returns null if `data` is not an image or was built by a different engine build.
    JS_EXPORT_PRIVATE static Image* registerImage(std::span<const uint8_t> data, const void* code);
    // For the jsc shell, and for platforms where a file cannot be mapped executable. Reads the file and copies its code into JIT
    // memory.
    static Image* registerImageFromFile(const char* path);

    static bool hasAny();
    static bool containsCode(const void*); // In any image.
    static Image* withCode(); // The image that contains code.
    const void* code() const { return m_code; }
    // These refer to the image that has module environments.
    JS_EXPORT_PRIVATE static uint32_t environmentsSize();
    JS_EXPORT_PRIVATE static uint32_t numberOfFunctionsOfImageWithEnvironments();
    JS_EXPORT_PRIVATE static ImageEnvironment environmentOf(uint32_t moduleOfGraph);
    static const void* addressOfStub(Stub); // From any image. Null if none is loaded.
    static std::pair<Image*, const ImageFunction*> find(const ImageKey&);

    struct CodeForRegExp {
        const void* for8Bit;
        const void* for16Bit;
    };
    JS_EXPORT_PRIVATE static std::optional<CodeForRegExp> codeForRegExp(const String& pattern, OptionSet<Yarr::Flags>); // From any image.

    static Image* withShapes(); // The image that has shapes, if any.
    static Image& of(const ImageFunction&); // The image that contains it.
    // The source text at this bytecode offset, and whether it is exact.
    std::optional<std::pair<String, bool>> quoteAt(const ImageFunction&, unsigned bytecodeOffset) const;
    String textOfQuote(uint64_t start, size_t length) const;
    bool constructsAt(const ImageFunction&, unsigned bytecodeOffset) const;
    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(m_data.data() + offset); }
    uint32_t selectorNamed(const StringImpl&) const; // Zero if there is none.

    const uint8_t* codeFor(const ImageFunction& function) const { return static_cast<const uint8_t*>(m_code) + at<uint32_t>(header().startsOfFunctionsOffset)[function.index]; }
    // Includes any padding before the next function.
    size_t sizeOfCodeOf(const ImageFunction& function) const
    {
        const uint32_t* starts = at<uint32_t>(header().startsOfFunctionsOffset);
        return (function.index + 1 < header().numberOfFunctions ? starts[function.index + 1] : header().endOfFunctions) - starts[function.index];
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

// A newly built image, for callers that need the addresses its contents will have once it is mapped at `address`.
class ImageView {
public:
    struct Function {
        uint64_t entry; // An EntryWord.
        uint32_t index;
        const Site* sites; // Address after mapping.
        const ImageFunction* function; // Address after mapping.
        uint32_t numSlots;
        bool startsCold;
        bool hasSiteConstants;
    };
    JS_EXPORT_PRIVATE static std::optional<ImageView> tryCreate(std::span<const uint8_t> data, const void* address);
    JS_EXPORT_PRIVATE std::optional<Function> find(const ImageKey&) const;
    JS_EXPORT_PRIVATE void* addressOfStub(Stub) const;
    // The image omits its key table, which instead follows the image in `data`. The caller keeps whatever part of it is needed.
    bool keysAreOmitted() const { return !header().tableCapacity; }
    JS_EXPORT_PRIVATE std::span<const ImageKey> keys() const; // The hash table. Entries with no record are empty.
    JS_EXPORT_PRIVATE uint32_t indexOfFunctionWith(const ImageKey&) const; // The key must be in the table.
    size_t numberOfFunctions() const { return header().numberOfFunctions; }
    uint32_t numberOfIdentifiersOfProgram() const { return header().numberOfIdentifiersOfProgram; }
    uint32_t numberOfConstantsOfProgram() const { return header().numberOfConstantsOfProgram; }

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

// The module number images use for this provider, or zero if it is in no image.
uint32_t moduleIDFor(SourceProvider&);
std::optional<ImageKey> imageKeyFor(ScriptExecutable*, CodeSpecializationKind);
JS_EXPORT_PRIVATE ImageKey imageKeyForTopLevelCode(uint32_t module); // For a program, or for a module's top-level code.
unsigned hashOfCode(std::span<const uint8_t>); // For checking that two compilations produced the same code.

// The code an image has for a function, if any. Looked up before the function's CodeBlock is created
// (CodeBlock::LinkMode::ForCodeFromImage).
struct ImageCode {
    Image* image { nullptr };
    const ImageFunction* function { nullptr };
    explicit operator bool() const { return !!function; }
};
// Fills in info.quotes from info.quotableSites. `text` is the source containing the function, whose own source starts at
// sourceOffset.
JS_EXPORT_PRIVATE void collectQuotes(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text, unsigned sourceOffset);
// `text` is the source containing the function, if known. The function's own source starts at sourceOffset.
JS_EXPORT_PRIVATE void collectConstructSites(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text = { }, unsigned sourceOffset = 0);
ImageCode findInImage(ScriptExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSScope*);
// Whether the module containing `scope` is linked the way it was at compile time, so that code with
// ImageFunction::usesStaticImports may run.
bool moduleIsLinkedAsCompiled(JSScope*);
Ref<JITCode> codeFromImage(ImageCode, UnlinkedCodeBlock*);
Ref<JITCode> codeOfFunctionFromImage(ImageCode, CodeSpecializationKind);
bool canRunWithoutUnlinkedCode(JSGlobalObject*, ImageCode); // True if FunctionMetadata is available.

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
