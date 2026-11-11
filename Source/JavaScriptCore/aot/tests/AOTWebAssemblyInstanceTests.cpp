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
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
