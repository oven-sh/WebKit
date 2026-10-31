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
#include "AOTVector.h"
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

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)
#include <sys/mman.h>
#endif
#if OS(DARWIN) || OS(LINUX)
#include <fcntl.h>
#endif
#if OS(WINDOWS)
#include <windows.h>
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

static Vector<uint8_t> compileOnThisThread(ASCIILiteral programText, std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.executableIdentity = executableIdentityOfTests;
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
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

Vector<uint8_t> compileProgramText(ASCIILiteral programText, std::initializer_list<ASCIILiteral> optionWords)
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        image = compileOnThisThread(programText, optionWords);
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

// On Linux, on macOS and on Windows nothing makes the copy executable: the code is mapped from the file, as it is for an executable. The file stays open for as long as the process lives.
static std::optional<ImageFile> fileToMapCodeFrom(const String& path)
{
#if OS(DARWIN) || OS(LINUX)
    int descriptor = open(byteCast<char>(FileSystem::fileSystemRepresentation(path).data()), O_RDONLY | O_CLOEXEC);
    AOT_TEST_CHECK(descriptor >= 0);
    return ImageFile { descriptor, 0 };
#elif OS(WINDOWS)
    HANDLE file = CreateFileA(byteCast<char>(path.utf8().data()), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    AOT_TEST_CHECK(file != INVALID_HANDLE_VALUE);
    return ImageFile { reinterpret_cast<intptr_t>(file), 0 };
#else
    UNUSED_PARAM(path);
    return std::nullopt;
#endif
}

static void useImageAt(const String& path)
{
    std::optional<Vector<uint8_t>> file = FileSystem::readEntireFile(path);
    AOT_TEST_CHECK(file);

    Wire::ImageVerdict verdict = useImage(leakAlignedCopy(file->span()), fileToMapCodeFrom(path), executableIdentityOfTests);
    if (verdict != Wire::ImageVerdict::Accepted) {
        dataLogLn("Cannot use ", path, ": ", nameOf(verdict));
        failNativeTest(__FILE__, __LINE__, "the process accepts the image");
    }
    AOT_TEST_CHECK(Image::current()->header().options.useWideReferences() || MarkedBlockCage::require());
}

void overwriteWordInImage(const uint32_t& word, uint32_t value)
{
    auto* address = const_cast<uint32_t*>(&word);
#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)
    void* page = std::bit_cast<void*>(std::bit_cast<uintptr_t>(address) & ~(pageSize() - 1));
    AOT_TEST_CHECK(!mprotect(page, pageSize(), PROT_READ | PROT_WRITE));
    *address = value;
    AOT_TEST_CHECK(!mprotect(page, pageSize(), PROT_READ));
#elif OS(WINDOWS)
    DWORD protectionOfPage;
    DWORD protectionForStore;
    AOT_TEST_CHECK(VirtualProtect(address, sizeof(uint32_t), PAGE_READWRITE, &protectionOfPage));
    *address = value;
    AOT_TEST_CHECK(VirtualProtect(address, sizeof(uint32_t), protectionOfPage, &protectionForStore));
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

void runWithLoadedProgram(ASCIILiteral testName, ASCIILiteral programText, LoadedProgramTest body, std::initializer_list<ASCIILiteral> optionWords)
{
    if (const char* imagePath = byteCast<char>(Options::aotImagePath())) {
        if (!Image::isRegistered())
            useImageAt(String::fromUTF8(imagePath));
        LoadedProgram program;
        body(program);
        return;
    }

    TemporaryFile file(".image"_s, compileProgramText(programText, optionWords).span());
    runInChildProcess(testName, file.path());
}

void runWithLoadedProgramThatStops(ASCIILiteral testName, ASCIILiteral programText, LoadedProgramTest body, ASCIILiteral lastWords)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }

    TemporaryFile file(".image"_s, compileProgramText(programText).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, file.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (result.outcome != ChildProcessOutcome::Crashed || !result.output.contains(lastWords)) {
        dataLogLn(result.output);
        dataLogLn("The process that uses the image ", result.description());
        failNativeTest(__FILE__, __LINE__, testName.characters());
    }
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
