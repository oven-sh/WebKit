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

// The code of a whole program, compiled before the program is run: what a linker would call the text section, and the table to
// find a function's code in it. Nothing in it is an address, so it is used where it is mapped, by every process that maps it.
//
//     ImageHeader | hash table of ImageKey | function records | padding to a page | code
//
// The image starts on a page boundary of the file it is in, so the code can be mapped from the file, executable.

static constexpr uint64_t imageMagic = 0x3130544f414e5542ULL; // "BUNAOT01"
static constexpr size_t imagePageSize = 16 * KB;
static constexpr size_t imageFunctionAlignment = sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16; // And of whatever else is not a function.
static constexpr unsigned mostCopiesOfStubsInImage = 8;
static constexpr unsigned numberOfAdapters = 5;

struct ImageHeader {
    uint64_t magic;
    uint64_t stamp; // Of the engine the code was compiled by and for: it has that build's offsets and table indices in it.
    uint64_t size; // Of everything.
    uint64_t codeOffset;
    uint64_t codeSize;
    uint32_t tableOffset;
    uint32_t tableCapacity; // A power of two. Or nothing: see StaticHeap::keysOfImage().
    uint32_t recordsOffset;
    uint32_t recordsSize;
    uint32_t numberOfFunctions;
    uint32_t environmentsSize; // See Instance: how much there is below it.
    uint32_t environmentsOffset; // ImageEnvironment, by which module of the graph of modules that the program was linked as.
    uint32_t numberOfEnvironments;
    // Shapes and selectors are numbered from one.
    uint32_t shapesOffset; // ImageShape, by number.
    uint32_t numberOfShapes; // One more than the last.
    uint32_t slotsOfShapesOffset; // uint16_t: see ImageShape::slots.
    // Options::aotTypesFields(): SlotsOfBornObjects. The index (uint32_t, by the number of the shape), and what it is an index of. No index: it is not gone by.
    uint32_t indexOfHeldInSlotsOffset;
    uint32_t sizeOfIndexOfHeldInSlots;
    uint32_t heldInSlotsOffset;
    // With numberOfIdentifiersOfProgram, a selector is the number of the identifier, and what it says is for StaticHeap to know.
    uint32_t selectorsOffset; // ImageSelector, by number.
    uint32_t numberOfSelectors; // One more than the last.
    uint32_t rowsOfSelectorsOffset; // uint32_t, by number: the entry of the dispatch table for a shape is at this plus the number of the shape.
    uint32_t textOfSelectorsOffset;
    uint32_t selectorsInOrderOffset; // uint32_t: by length, and then by what they say. The 8 bit ones first.
    uint32_t numberOfSelectorsInOrder;
    uint32_t hashOfIntrinsics; // ImmutableIntrinsics::hash(), if the code goes by their numbers. Zero: it does not.
    uint32_t dispatchOffset; // uint32_t: ImageDispatchEntry.
    uint32_t dispatchSize; // In entries.
    uint32_t quotesOffset; // What ImageFunction::quotes is from. See Image::quoteAt().
    uint32_t textOfQuotesOffset; // UTF-8.
    uint32_t numberOfIdentifiersOfProgram; // Not zero: see NumbersOfIdentifiers.
    uint32_t numberOfConstantsOfProgram; // See NumbersOfConstants.
    uint32_t regExpsOffset; // ImageRegExp, in the order of their hashes.
    uint32_t numberOfRegExps;
    uint32_t textOfRegExpsOffset;
    // For telling what an address is in (whatIsAt()).
    uint32_t numbersOfFunctionsOffset; // uint32_t, by index: what the function goes by in a type (typeOfFunction()). Only Options::aotVerifiesFacts() looks.
    uint32_t startsOfFunctionsOffset; // uint32_t, by index, which is the order they are in: where each starts, in the code. And one more, which is beyond everything.
    uint32_t granulesOfCodeOffset; // uint32_t: for each 1 << shiftOfGranuleOfCode bytes of the code, the last function to start no later than they do.
    uint32_t callSitesOffset; // See callSiteAt().
    uint32_t endOfFunctions; // In the code.
    uint32_t sizeOfStubs;
    uint32_t numberOfCopiesOfStubs;
    uint32_t copiesOfStubs[mostCopiesOfStubsInImage]; // Where each is, in the code.
    uint32_t returnsIntoAdapters[numberOfAdapters]; // From the start of the stubs: StubBlob::returnsIntoAdapters.
    uint32_t stubOffsets[numberOfStubs]; // From the start of the code, which starts with a copy of the stubs.
};

// The code for a regular expression, however the program comes by one that says that.
struct ImageRegExp {
    static uint32_t hashOf(const String& pattern, OptionSet<Yarr::Flags> flags) { return pattern.hash() * 31 + flagsThatMatter(flags).toRaw(); }
    // The others make no difference to what the pattern matches at a given place, which is all that the code says.
    static OptionSet<Yarr::Flags> flagsThatMatter(OptionSet<Yarr::Flags> flags) { return flags - OptionSet<Yarr::Flags> { Yarr::Flags::Global, Yarr::Flags::HasIndices }; }

    uint32_t hash;
    uint32_t text; // Where the pattern is, in bytes, in the text of them.
    uint32_t length : 31; // In characters.
    uint32_t is8Bit : 1;
    uint32_t flags;
    // From the start of the code. It records where the subpatterns are (Yarr::ExecutionMode::IncludeSubpatterns).
    uint32_t codeFor8Bit;
    uint32_t codeFor16Bit;
};

struct ImageShape {
    uint16_t numberOfProperties;
    uint16_t inlineCapacity;
    uint32_t slots; // Where, among ImageHeader::slotsOfShapesOffset, the slot of each property is, plus one. Zero: they are one after the other.
};

// A name that properties are read by.
struct ImageSelector {
    uint32_t text; // Where it is, in bytes, in the text of selectors.
    uint32_t length : 31; // In characters.
    uint32_t is8Bit : 1;
};

// One table for the whole program says where every property of every shape is. The rows, one for each selector, are laid over one
// another wherever the shapes that have the one do not have the other, so an entry says whose it is. If it is somebody else's, an
// object of that shape has no property of that name of its own.
struct ImageDispatchEntry {
    static constexpr unsigned locationBits = 12; // As AOT::locationOfProperty(): in words, from the object or, if negative, its butterfly.
    static uint32_t encode(uint32_t selector, int32_t location) { return selector << locationBits | (static_cast<uint32_t>(location) & ((1u << locationBits) - 1)); }
};

// Where a module's JSModuleEnvironment is, in every realm that runs the module's code from the image.
struct ImageEnvironment {
    uint32_t distance; // Below the Instance. Zero: nowhere in particular.
    uint32_t size;
};

// What a compilation makes.
struct CompiledCode {
    Vector<uint8_t> bytes;
    CompiledFunctionInfo info;
};

JS_EXPORT_PRIVATE uint64_t imageStamp();

class ImageBuilder {
    WTF_MAKE_TZONE_ALLOCATED(ImageBuilder);
    WTF_MAKE_NONCOPYABLE(ImageBuilder);
public:
    ImageBuilder() = default;

    // Any thread. The code is laid out in the order of `rank`, not of the calls.
    void add(ImageKey, uint64_t rank, CompiledCode&&);
    size_t numberOfFunctions() const { return m_functions.size(); }
    void clear() { m_functions.clear(); } // Of functions.
    void setEnvironments(Vector<ImageEnvironment>&& environments, uint32_t size) { m_environments = WTF::move(environments); m_environmentsSize = size; }
    // They are there until finish() is done. `number`: one more than the last.
    void setNumbersOfIdentifiersOfProgram(const NumbersOfIdentifiers* numbers, uint32_t number)
    {
        m_numbersOfIdentifiersOfProgram = numbers;
        m_numberOfIdentifiersOfProgram = number;
    }
    void setNumberOfConstantsOfProgram(uint32_t number) { m_numberOfConstantsOfProgram = number; }
    // The thread that has the VM. False: there is not going to be code for it.
    bool addRegExp(VM&, const String& pattern, OptionSet<Yarr::Flags>);
    Vector<uint8_t> finish();
    Vector<ReportableSitesOfFunction> takeReportableSites() { return std::exchange(m_reportableSites, { }); } // After that. By the index of the function.

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
        Yarr::YarrCodeForImage code[2]; // 8 bit, 16 bit.
    };
    uint32_t m_numberOfIdentifiersOfProgram { 0 };
    const NumbersOfIdentifiers* m_numbersOfIdentifiersOfProgram { nullptr };
    uint32_t m_numberOfConstantsOfProgram { 0 };
    Vector<RegExpCode> m_regExps;
    UncheckedKeyHashMap<String, bool> m_regExpsAsked; // By the flags and the pattern.
};

// An image that is ready to be run. There is one list of them for the process, and they stay.
class Image {
    WTF_MAKE_TZONE_ALLOCATED(Image);
    WTF_MAKE_NONCOPYABLE(Image);
public:
    // `data` is the image, readable, for as long as the process lives; `code` is where its code is mapped executable. Null if
    // it is not an image, or not one for this engine.
    JS_EXPORT_PRIVATE static Image* registerImage(std::span<const uint8_t> data, const void* code);
    // For a shell, and for where a file cannot be mapped executable: reads the file, copies its code to memory of the JIT's.
    static Image* registerImageFromFile(const char* path);

    static bool hasAny();
    static bool containsCode(const void*); // Any image's.
    static Image* withCode(); // The one that has any.
    const void* code() const { return m_code; }
    // Of the image that has any.
    JS_EXPORT_PRIVATE static uint32_t environmentsSize();
    JS_EXPORT_PRIVATE static uint32_t numberOfFunctionsOfImageWithEnvironments();
    JS_EXPORT_PRIVATE static ImageEnvironment environmentOf(uint32_t moduleOfGraph);
    static const void* addressOfStub(Stub); // In any image. Null if there is none.
    static std::pair<Image*, const ImageFunction*> find(const ImageKey&);

    struct CodeForRegExp {
        const void* for8Bit;
        const void* for16Bit;
    };
    JS_EXPORT_PRIVATE static std::optional<CodeForRegExp> codeForRegExp(const String& pattern, OptionSet<Yarr::Flags>); // In any image.

    static Image* withShapes(); // The image, if it has any.
    static Image& of(const ImageFunction&); // The one it is in.
    // What the source says where the function is at that offset in its bytecode: the text, and whether it is exactly that.
    std::optional<std::pair<String, bool>> quoteAt(const ImageFunction&, unsigned bytecodeOffset) const;
    bool constructsAt(const ImageFunction&, unsigned bytecodeOffset) const;
    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(m_data.data() + offset); }
    uint32_t selectorNamed(const StringImpl&) const; // Zero: none.

    const uint8_t* codeFor(const ImageFunction& function) const { return static_cast<const uint8_t*>(m_code) + function.codeOffset; }
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

// An image that has just been made, for whoever needs to know where what is in it is going to be once it is mapped at `address`.
class ImageView {
public:
    struct Function {
        uint64_t entry; // An EntryWord.
        uint32_t index;
        const Site* sites; // Where they are going to be.
        const ImageFunction* function; // Likewise.
        uint32_t numSlots;
        bool startsCold;
        bool hasSiteConstants;
    };
    JS_EXPORT_PRIVATE static std::optional<ImageView> tryCreate(std::span<const uint8_t> data, const void* address);
    JS_EXPORT_PRIVATE std::optional<Function> find(const ImageKey&) const;
    JS_EXPORT_PRIVATE void* addressOfStub(Stub) const;
    // The image goes without its table of keys, which is what follows it in `data`. Whoever this is for keeps what is wanted of it.
    bool keysAreLeftOut() const { return !header().tableCapacity; }
    JS_EXPORT_PRIVATE std::span<const ImageKey> keys() const; // The table: those with no record are empty places in it.
    JS_EXPORT_PRIVATE uint32_t indexOfFunctionWith(const ImageKey&) const; // One of the table's own.
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

// The number an image knows the provider's module by; zero if it is in no image.
uint32_t moduleIDFor(SourceProvider&);
std::optional<ImageKey> imageKeyFor(ScriptExecutable*, CodeSpecializationKind);
JS_EXPORT_PRIVATE ImageKey imageKeyForTopLevelCode(uint32_t module); // A program, or the code of a module itself.
unsigned hashOfCode(std::span<const uint8_t>); // For telling whether two compilations came out the same.

// The code an image has for the function, if any has: asked before the function gets a CodeBlock, which is made for it
// (CodeBlock::LinkMode::ForCodeFromImage), and then given it.
struct ImageCode {
    Image* image { nullptr };
    const ImageFunction* function { nullptr };
    explicit operator bool() const { return !!function; }
};
// Fills in info.quotes, from info.quotableSites. `text` is what the function is in, and its own source starts at sourceOffset.
JS_EXPORT_PRIVATE void collectQuotes(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text, unsigned sourceOffset);
// `text` is what the function is in, if that is known, and its own source starts at sourceOffset.
JS_EXPORT_PRIVATE void collectConstructSites(CompiledFunctionInfo&, UnlinkedCodeBlock*, StringView text = { }, unsigned sourceOffset = 0);
ImageCode findInImage(ScriptExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSScope*);
// Of the module that the scope is in, or is: whether code that takes its imports for what they were when it was compiled
// (ImageFunction::usesStaticImports) may.
bool moduleIsLinkedAsCompiled(JSScope*);
Ref<JITCode> codeFromImage(ImageCode, UnlinkedCodeBlock*);
Ref<JITCode> codeOfFunctionFromImage(ImageCode, CodeSpecializationKind);
bool canDoWithoutUnlinkedCode(JSGlobalObject*, ImageCode); // There are FunctionFacts.

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
