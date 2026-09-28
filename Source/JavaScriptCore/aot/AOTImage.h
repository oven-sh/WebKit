/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTRuntime.h"
#include "CodeSpecializationKind.h"
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
static constexpr size_t imageFunctionAlignment = 16;

struct ImageHeader {
    uint64_t magic;
    uint64_t stamp; // Of the engine the code was compiled by and for: it has that build's offsets and table indices in it.
    uint64_t size; // Of everything.
    uint64_t codeOffset;
    uint64_t codeSize;
    uint32_t tableOffset;
    uint32_t tableCapacity; // A power of two.
    uint32_t recordsOffset;
    uint32_t recordsSize;
    uint32_t numberOfFunctions;
    uint32_t environmentsSize; // See Instance: how much there is below it.
    uint32_t environmentsOffset; // ImageEnvironment, by which module of the graph of modules that the program was linked as.
    uint32_t numberOfEnvironments;
    // Shapes and selectors are numbered from one.
    uint32_t shapesOffset; // ImageShape, by number.
    uint32_t numberOfShapes; // One more than the last.
    uint32_t namesOfShapesOffset; // uint32_t: selectors.
    uint32_t selectorsOffset; // ImageSelector, by number.
    uint32_t numberOfSelectors;
    uint32_t rowsOfSelectorsOffset; // uint32_t, by number: the entry of the dispatch table for a shape is at this plus the number of the shape.
    uint32_t textOfSelectorsOffset;
    uint32_t selectorsInOrderOffset; // uint32_t, numberOfSelectors - 1 of them: by length, and then by what they say. The 8 bit ones first.
    uint32_t hashOfIntrinsics; // ImmutableIntrinsics::hash(), if the code goes by their numbers. Zero: it does not.
    uint32_t dispatchOffset; // uint32_t: ImageDispatchEntry.
    uint32_t dispatchSize; // In entries.
    uint32_t stubOffsets[numberOfStubs]; // From the start of the code, which starts with a copy of the stubs.
};

struct ImageShape {
    uint32_t names; // Where its names start, among the names of shapes.
    uint16_t numberOfProperties;
    uint16_t inlineCapacity;
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
    Vector<uint8_t> finish();

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
    // Of the image that has any.
    JS_EXPORT_PRIVATE static uint32_t environmentsSize();
    JS_EXPORT_PRIVATE static uint32_t numberOfFunctionsOfImageWithEnvironments();
    JS_EXPORT_PRIVATE static ImageEnvironment environmentOf(uint32_t moduleOfGraph);
    static const void* addressOfStub(Stub); // In any image. Null if there is none.
    static std::pair<Image*, const ImageFunction*> find(const ImageKey&);

    static Image* withShapes(); // The image, if it has any.
    static Image& of(const ImageFunction&); // The one it is in.
    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(m_data.data() + offset); }
    AtomString nameOfSelector(uint32_t) const;
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
        void* entry; // That checks the number of arguments.
        uint32_t index; // CodeHeader::index
        const Site* sites; // Where they are going to be.
        const ImageFunction* function; // Likewise.
        uint32_t numSlots;
        bool startsCold;
    };
    JS_EXPORT_PRIVATE static std::optional<ImageView> tryCreate(std::span<const uint8_t> data, const void* address);
    JS_EXPORT_PRIVATE std::optional<Function> find(const ImageKey&) const;
    JS_EXPORT_PRIVATE void* addressOfStub(Stub) const;
    size_t numberOfFunctions() const { return header().numberOfFunctions; }

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
ImageCode findInImage(ScriptExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSScope*);
// Of the module that the scope is in, or is: whether code that takes its imports for what they were when it was compiled
// (ImageFunction::usesStaticImports) may.
bool moduleIsLinkedAsCompiled(JSScope*);
Ref<JITCode> codeFromImage(ImageCode, UnlinkedCodeBlock*);
Ref<JITCode> codeOfFunctionFromImage(ImageCode, CodeSpecializationKind);
bool canDoWithoutUnlinkedCode(JSGlobalObject*, ImageCode); // There are FunctionFacts.

// Options::aotWriteImage(): everything the process compiles goes to Options::aotImagePath() when it exits.
void addToImageBeingWritten(ScriptExecutable*, CodeSpecializationKind, const JITCode&);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
