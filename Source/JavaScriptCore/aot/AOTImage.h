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
    uint32_t reserved;
    uint32_t stubOffsets[numberOfStubs]; // From the start of the code, which starts with a copy of the stubs.
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
    Vector<uint8_t> finish();

private:
    struct Function {
        ImageKey key;
        uint64_t rank;
        CompiledCode code;
    };
    Lock m_lock;
    Vector<Function> m_functions;
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
    static const void* addressOfStub(Stub); // In any image. Null if there is none.
    static std::pair<Image*, const ImageFunction*> find(const ImageKey&);

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
void installFromImage(CodeBlock*, ImageCode);

// Options::aotWriteImage(): everything the process compiles goes to Options::aotImagePath() when it exits.
void addToImageBeingWritten(CodeBlock*, const JITCode&);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
