/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTVector.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTDriver.h"
#include "AOTIRParser.h"
#include "AOTIRProgram.h"
#include "AOTImageFormat.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTWasmInterface.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

constexpr uint8_t importedMemoryModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x2f, 0x02,
    0x0c, 0x2e, 0x2f, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x73, 0x2e, 0x6a, 0x73, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x00,
    0x0c, 0x2e, 0x2f, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x73, 0x2e, 0x6a, 0x73, 0x06, 0x72, 0x65, 0x73, 0x75, 0x6c, 0x74, 0x03, 0x7f, 0x01,
    0x03, 0x02, 0x01, 0x00,
    0x08, 0x01, 0x00,
    0x0a, 0x08, 0x01, 0x06, 0x00, 0x41, 0x07, 0x24, 0x00, 0x0b,
};

constexpr uint8_t moduleWithGCTypes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0a, 0x02, 0x5e, 0x78, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x64, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x07, 0x0d, 0x01, 0x09, 0x6d, 0x61, 0x6b, 0x65, 0x42, 0x79, 0x74, 0x65, 0x73, 0x00, 0x00,
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0xfb, 0x07, 0x00, 0x0b,
};

// g and wg leave by a tail call of an import. h and wh add the imported global, k and wk add it to what g and wg return. Those with a w take ten arguments.
constexpr uint8_t moduleWithTailCalls[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x14, 0x02, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x0a, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x02, 0x14, 0x03, 0x01, 0x6d, 0x01, 0x66, 0x00, 0x00, 0x01, 0x6d, 0x01, 0x77, 0x00, 0x01, 0x01, 0x6d, 0x01, 0x76, 0x03, 0x7f, 0x00,
    0x03, 0x07, 0x06, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
    0x07, 0x1c, 0x06, 0x01, 0x67, 0x00, 0x02, 0x01, 0x68, 0x00, 0x03, 0x01, 0x6b, 0x00, 0x04, 0x02, 0x77, 0x67, 0x00, 0x05, 0x02, 0x77, 0x68, 0x00, 0x06, 0x02, 0x77, 0x6b, 0x00, 0x07,
    0x0a, 0x60, 0x06,
    0x09, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6a, 0x12, 0x00, 0x0b,
    0x07, 0x00, 0x20, 0x00, 0x23, 0x00, 0x6a, 0x0b,
    0x09, 0x00, 0x20, 0x00, 0x10, 0x02, 0x23, 0x00, 0x6a, 0x0b,
    0x18, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x03, 0x20, 0x04, 0x20, 0x05, 0x20, 0x06, 0x20, 0x07, 0x20, 0x08, 0x20, 0x09, 0x20, 0x00, 0x12, 0x01, 0x0b,
    0x0d, 0x00, 0x20, 0x09, 0x20, 0x08, 0x41, 0x0a, 0x6c, 0x6a, 0x23, 0x00, 0x6a, 0x0b,
    0x1b, 0x00, 0x41, 0x00, 0x41, 0x01, 0x41, 0x02, 0x41, 0x03, 0x41, 0x04, 0x41, 0x05, 0x41, 0x06, 0x41, 0x07, 0x41, 0x08, 0x41, 0x09, 0x10, 0x05, 0x23, 0x00, 0x6a, 0x0b,
};

constexpr ASCIILiteral makesTailCallsBetweenInstances =
    "const module = new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 20, 2, 96, 1, 127, 1, 127, 96, 10, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 1, 127, 2, 20, 3, 1, 109, 1, 102, 0, 0, 1, 109, 1, 119, 0, 1, 1, 109, 1, 118, 3, 127, 0, 3, 7, 6, 0, 0, 0, 1, 1, 0, 7, 28, 6, 1, 103, 0, 2, 1, 104, 0, 3, 1, 107, 0, 4, 2, 119, 103, 0, 5, 2, 119, 104, 0, 6, 2, 119, 107, 0, 7, 10, 96, 6, 9, 0, 32, 0, 65, 1, 106, 18, 0, 11, 7, 0, 32, 0, 35, 0, 106, 11, 9, 0, 32, 0, 16, 2, 35, 0, 106, 11, 24, 0, 32, 1, 32, 2, 32, 3, 32, 4, 32, 5, 32, 6, 32, 7, 32, 8, 32, 9, 32, 0, 18, 1, 11, 13, 0, 32, 9, 32, 8, 65, 10, 108, 106, 35, 0, 106, 11, 27, 0, 65, 0, 65, 1, 65, 2, 65, 3, 65, 4, 65, 5, 65, 6, 65, 7, 65, 8, 65, 9, 16, 5, 35, 0, 106, 11]));\n"
    "const make = (v, f, w) => new WebAssembly.Instance(module, { m: { f, w, v } }).exports;\n"
    "const a = make(100, () => 0, () => 0), b = make(1000, a.h, a.wh), c = make(10000, b.g, b.wg);\n"
    "print(c.k(1), c.wk(0), c.g(1));\n"_s;

constexpr ASCIILiteral callsModuleFromOtherBytes =
    "const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 7, 1, 96, 2, 127, 127, 1, 127, 3, 2, 1, 0, 7, 7, 1, 3, 97, 100, 100, 0, 0, 10, 9, 1, 7, 0, 32, 0, 32, 1, 106, 11]);\n"
    "print(new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports.add(20, 22));\n"_s;

constexpr ASCIILiteral callsModuleWithGCTypes =
    "const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 10, 2, 94, 120, 1, 96, 1, 127, 1, 100, 0, 3, 2, 1, 1, 7, 13, 1, 9, 109, 97, 107, 101, 66, 121, 116, 101, 115, 0, 0, 10, 9, 1, 7, 0, 32, 0, 251, 7, 0, 11]);\n"
    "print(typeof new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports.makeBytes(3));\n"_s;

constexpr ASCIILiteral importsSource =
    "export const memory = new WebAssembly.Memory({ initial: 1 });\n"
    "export const result = new WebAssembly.Global({ value: 'i32', mutable: true }, 0);\n"_s;

constexpr ASCIILiteral mainSource =
    "import './module.wasm';\n"
    "import { result } from './imports.js';\n"
    "print(result.value);\n"_s;

constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
webassemblymodule module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

class ModuleFolder {
    WTF_MAKE_NONCOPYABLE(ModuleFolder);
public:
    explicit ModuleFolder(String&& path)
        : m_path(WTF::move(path))
    {
        AOT_TEST_CHECK(FileSystem::makeAllDirectories(m_path));
    }

    ~ModuleFolder() { FileSystem::deleteNonEmptyDirectory(m_path); }

    String write(ASCIILiteral name, std::span<const uint8_t> contents) const
    {
        String path = FileSystem::pathByAppendingComponent(m_path, name);
        AOT_TEST_CHECK(FileSystem::overwriteEntireFile(path, contents) == contents.size());
        return path;
    }

private:
    String m_path;
};

} // anonymous namespace

static void checkImageHoldsCodeOfItsWebAssemblyModule(std::span<const uint8_t> image)
{
    AOT_TEST_CHECK(image.size() >= sizeof(ImageHeader));
    const ImageHeader& header = reinterpretCastSpanStartTo<ImageHeader>(image);
    AOT_TEST_CHECK(header.code.webAssemblyCodeSize);

    ImageSectionRange range = std::span(header.sections)[static_cast<size_t>(ImageSection::WebAssemblyDirectory)];
    AOT_TEST_CHECK(range.size >= sizeof(Wasm::DirectoryInImage));
    AOT_TEST_CHECK(range.offset <= image.size() && range.size <= image.size() - range.offset);
    Wasm::DirectoryInImage directory = reinterpretCastSpanStartTo<Wasm::DirectoryInImage>(image.subspan(range.offset, range.size));
    AOT_TEST_CHECK(directory.numberOfModules == 1);
    AOT_TEST_CHECK(!directory.numberOfDeclinedModules);
    AOT_TEST_CHECK(directory.catchRunway < header.code.webAssemblyCodeSize);
}

static std::unique_ptr<CompileOutput> compileProgramWithModule(std::span<const uint8_t> module)
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(programText, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);
    program->webAssemblyModules[0].bytes = module;

    std::unique_ptr<CompileOutput> output = Typed::compileWithMechanisms(WTF::move(program), build, Typed::mechanismsOfProcess());
    if (!output->hasImage())
        Typed::printDiagnosticsOf(*output, WTF::dataFile());
    AOT_TEST_CHECK(output->hasImage());
    checkImageHoldsCodeOfItsWebAssemblyModule(output->section(Wire::ResultSection::Image));
    return output;
}

static ChildProcessResult runInShellWithTripwire(std::span<const uint8_t> moduleOfProgram, ASCIILiteral source)
{
    std::unique_ptr<CompileOutput> output = compileProgramWithModule(moduleOfProgram);
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    ModuleFolder folder(makeString(image.ownPath(), ".modules"_s));
    String mainPath = folder.write("main.js"_s, byteCast<uint8_t>(source.span8()));

    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), "--useBBQJIT=false"_s, "--useOMGJIT=false"_s, "--crashIfInterpreterIsUsed=true"_s, mainPath };
    return ChildProcessLauncher(shellPath(), { }).run(arguments.span());
}

static void checkShellPrinted(const ChildProcessResult& result, ASCIILiteral expected)
{
    String printed = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
    if (!result.exitedWith(EXIT_SUCCESS) || printed != expected)
        dataLogLn(shellPath(), " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    AOT_TEST_CHECK(printed == expected);
}

static void testTripwireAcceptsBytesTheBuildNeverSaw()
{
    checkShellPrinted(runInShellWithTripwire(moduleWithGCTypes, callsModuleFromOtherBytes), "42\n"_s);
}

static void testModuleOfProgramRunsUnderTripwire()
{
    checkShellPrinted(runInShellWithTripwire(moduleWithGCTypes, callsModuleWithGCTypes), "object\n"_s);
}

static void testTailCallsBetweenInstances()
{
    checkShellPrinted(runInShellWithTripwire(moduleWithTailCalls, makesTailCallsBetweenInstances), "10103 10101 103\n"_s);
}

static void testImportedMemoryInShell()
{
    std::unique_ptr<CompileOutput> output = compileProgramWithModule(importedMemoryModule);
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    ModuleFolder folder(makeString(image.ownPath(), ".modules"_s));
    folder.write("module.wasm"_s, importedMemoryModule);
    folder.write("imports.js"_s, byteCast<uint8_t>(importsSource.span8()));
    String mainPath = folder.write("main.js"_s, byteCast<uint8_t>(mainSource.span8()));

    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), "--useBBQJIT=false"_s, "--useOMGJIT=false"_s, "-m"_s, mainPath };
    checkShellPrinted(ChildProcessLauncher(shellPath(), { }).run(arguments.span()), "7\n"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

namespace JSC { namespace AOT {

std::span<const NativeTest> webAssemblyInstanceTests()
{
#if ENABLE(WEBASSEMBLY_OMGJIT)
    static constexpr NativeTest tests[] = {
        { "webassembly-instance/imported-memory-in-shell"_s, testImportedMemoryInShell },
        { "webassembly-instance/tripwire-accepts-bytes-the-build-never-saw"_s, testTripwireAcceptsBytesTheBuildNeverSaw },
        { "webassembly-instance/module-of-program-runs-under-tripwire"_s, testModuleOfProgramRunsUnderTripwire },
        { "webassembly-instance/tail-calls-between-instances"_s, testTailCallsBetweenInstances },
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
