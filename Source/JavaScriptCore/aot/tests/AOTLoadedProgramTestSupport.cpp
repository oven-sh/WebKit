/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLoadedProgramTestSupport.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTImageLoading.h"
#include "AOTProgramFile.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/FastMalloc.h>
#include <wtf/FileSystem.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX) || OS(FREEBSD)
#include <sys/mman.h>
#endif

namespace JSC::AOT {

static constexpr std::array<uint8_t, 16> executableIdentityOfTests { };

LoadedProgram::LoadedProgram()
{
    AOT_TEST_CHECK(Image::isRegistered());
    AOT_TEST_CHECK(vm().hasCompiledCode());
    AOT_TEST_CHECK(Instance::of(realm()));
}

LoadedProgram::~LoadedProgram()
{
    AOT_TEST_CHECK(!vm().exceptionForInspection());
}

static Vector<uint8_t> compileOnThisThread(ASCIILiteral programText)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.executableIdentity = executableIdentityOfTests;
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
        failNativeTest(__FILE__, __LINE__, "the IR text is a valid program");
    }

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty()) {
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        failNativeTest(__FILE__, __LINE__, "the program compiles");
    }
    return WTF::move(output->image);
}

Vector<uint8_t> compileProgramText(ASCIILiteral programText)
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        image = compileOnThisThread(programText);
    });
    return image;
}

// The pages after the copy stand for what follows the image in the segment of an executable.
static std::span<const uint8_t> leakAlignedCopy(std::span<const uint8_t> bytes)
{
    std::span copy = unsafeMakeSpan(static_cast<uint8_t*>(fastAlignedMalloc(imageAlignment(), bytes.size() + imageAlignment())), bytes.size());
    memcpySpan(copy, bytes);
    return copy;
}

static void useImageAt(const String& path)
{
    std::optional<Vector<uint8_t>> file = FileSystem::readEntireFile(path);
    AOT_TEST_CHECK(file);

    Wire::ImageVerdict verdict = useImage(leakAlignedCopy(file->span()), std::nullopt, executableIdentityOfTests);
    if (verdict != Wire::ImageVerdict::Accepted) {
        dataLogLn("Cannot use ", path, ": ", nameOf(verdict));
        failNativeTest(__FILE__, __LINE__, "the process accepts the image");
    }
    AOT_TEST_CHECK(Image::current()->header().options.useWideReferences() || MarkedBlockCage::require());
}

void overwriteWordInImage(const uint32_t& word, uint32_t value)
{
    auto* address = const_cast<uint32_t*>(&word);
#if OS(LINUX) || OS(FREEBSD)
    void* page = std::bit_cast<void*>(std::bit_cast<uintptr_t>(address) & ~(pageSize() - 1));
    AOT_TEST_CHECK(!mprotect(page, pageSize(), PROT_READ | PROT_WRITE));
    *address = value;
    AOT_TEST_CHECK(!mprotect(page, pageSize(), PROT_READ));
#else
    *address = value;
#endif
}

static void runInChildProcess(ASCIILiteral testName, const String& imagePath)
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, imagePath) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that uses the image ", result.description());
        failNativeTest(__FILE__, __LINE__, testName.characters());
    }
}

void runWithLoadedProgram(ASCIILiteral testName, ASCIILiteral programText, LoadedProgramTest body)
{
    if (const char* imagePath = byteCast<char>(Options::aotImagePath())) {
        if (!Image::isRegistered())
            useImageAt(String::fromUTF8(imagePath));
        LoadedProgram program;
        body(program);
        return;
    }

    TemporaryFile file(".image"_s, compileProgramText(programText).span());
    runInChildProcess(testName, file.path());
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
