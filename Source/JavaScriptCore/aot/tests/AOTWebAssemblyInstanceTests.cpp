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
#include "AOTCodeTemplates.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTIRProgram.h"
#include "AOTProgramFile.h"
#include "AOTRealmTestSupport.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTWasmModuleCompiler.h"
#include "AOTWasmSection.h"
#include "JSCInlines.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyModule.h"
#include "Options.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "WasmCodeInImage.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WebAssemblyModuleRecord.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

constexpr ASCIILiteral moduleLoaderOrderTestName = "webassembly-instance/imported-memory-in-module-loader-order"_s;

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

// (type $bytes (array (mut i8))) (func (export "makeBytes") (param i32) (result (ref $bytes)) local.get 0 array.new_default $bytes)
constexpr uint8_t moduleWithGCTypes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0a, 0x02, 0x5e, 0x78, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x64, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x07, 0x0d, 0x01, 0x09, 0x6d, 0x61, 0x6b, 0x65, 0x42, 0x79, 0x74, 0x65, 0x73, 0x00, 0x00,
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0xfb, 0x07, 0x00, 0x0b,
};

// (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
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
  b0():
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

static std::unique_ptr<CompileOutput> compileProgramWithModule(std::span<const uint8_t> module, uint32_t numberOfModulesWithCode = 1)
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(programText, Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);
    program->webAssemblyModules[0].bytes = module;

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty());
    AOT_TEST_CHECK(output->contents.numberOfWebAssemblyModules == numberOfModulesWithCode);
    return output;
}

static ChildProcessResult runInShellWithTripwire(std::span<const uint8_t> moduleOfProgram, uint32_t numberOfModulesWithCode, ASCIILiteral source)
{
    std::unique_ptr<CompileOutput> output = compileProgramWithModule(moduleOfProgram, numberOfModulesWithCode);
    TemporaryFile image(".image"_s, output->image.span());
    ModuleFolder folder(makeString(image.path(), ".modules"_s));
    String mainPath = folder.write("main.js"_s, byteCast<uint8_t>(source.span8()));

    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), "--useBBQJIT=false"_s, "--useOMGJIT=false"_s, "--crashIfInterpreterIsUsed=true"_s, mainPath };
    return ChildProcessLauncher(shellPath(), { }).run(arguments.span());
}

static void checkRunsModuleFromOtherBytes(std::span<const uint8_t> moduleOfProgram, uint32_t numberOfModulesWithCode)
{
    ChildProcessResult result = runInShellWithTripwire(moduleOfProgram, numberOfModulesWithCode, callsModuleFromOtherBytes);
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shellPath(), " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    AOT_TEST_CHECK(makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s) == "42\n"_s);
}

static void testTripwireAcceptsBytesTheBuildNeverSaw()
{
    checkRunsModuleFromOtherBytes(importedMemoryModule, 1);
    checkRunsModuleFromOtherBytes(moduleWithGCTypes, 0);
}

static void testTripwireReportsDeclinedModule()
{
    ChildProcessResult result = runInShellWithTripwire(moduleWithGCTypes, 0, callsModuleWithGCTypes);
    AOT_TEST_CHECK(!result.exitedWith(EXIT_SUCCESS));
    AOT_TEST_CHECK(result.output.contains("crashIfInterpreterIsUsed: a WebAssembly function without code in the image"_s));
}

static void testImportedMemoryInShell()
{
    std::unique_ptr<CompileOutput> output = compileProgramWithModule(importedMemoryModule);
    TemporaryFile image(".image"_s, output->image.span());
    ModuleFolder folder(makeString(image.path(), ".modules"_s));
    folder.write("module.wasm"_s, importedMemoryModule);
    folder.write("imports.js"_s, byteCast<uint8_t>(importsSource.span8()));
    String mainPath = folder.write("main.js"_s, byteCast<uint8_t>(mainSource.span8()));

    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), "--useBBQJIT=false"_s, "--useOMGJIT=false"_s };
    if (Options::useWasmFastMemory())
        arguments.append("--crashIfInterpreterIsUsed=true"_s);
    arguments.append("-m"_s);
    arguments.append(mainPath);

    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shellPath(), " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    AOT_TEST_CHECK(makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s) == "7\n"_s);
}

static bool canRunInThisProcess()
{
    return !Options::useBBQJIT() && !Options::useOMGJIT() && !Wasm::CodeInImage::isInProcess();
}

static void runInNewProcess(ASCIILiteral testName)
{
    ChildProcessLauncher launcher(String(programPath()), Vector<String> { "--useBBQJIT=false"_s, "--useOMGJIT=false"_s });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLog(result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
}

static Wasm::ModuleForImage compileForImage(VM& vm, std::span<const uint8_t> source)
{
    BuildContext context(makeDefaultCompileOptions());
    auto compiler = WasmModuleCompiler::create(vm, source);
    AOT_TEST_CHECK(compiler.has_value());
    auto templates = CodeTemplates::create(context.assemblerPolicy(), &context);
    AOT_TEST_CHECK(templates.has_value());
    auto module = (*compiler)->compile(*templates, context);
    AOT_TEST_CHECK(module.has_value());
    return WTF::move(*module);
}

static void setProcessImage(Wasm::ModuleForImage&& module)
{
    static NeverDestroyed<WebAssemblyImageInputs> section;
    WasmSectionBuilder builder;
    AOT_TEST_CHECK(builder.append(WTF::move(module)));
    section.get() = builder.finish();
    Wasm::CodeInImage::setImage(section->directory.span().data(), section->code.span());
    AOT_TEST_CHECK(Wasm::CodeInImage::isInProcess());
}

static JSWebAssemblyInstance* instantiateAsModuleLoader(Realms& realms, JSWebAssemblyModule* module, JSObject* imports)
{
    constexpr Wasm::CreationMode creationMode = Wasm::CreationMode::FromModuleLoader;
    VM& vm = realms.vm();
    JSGlobalObject* globalObject = realms.plain();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    RefPtr<SourceProvider> provider = StringSourceProvider::create("[wasm code]"_s, SourceOrigin { }, String(), SourceTaintedOrigin::Untainted);
    JSWebAssemblyInstance* instance = JSWebAssemblyInstance::tryCreate(vm, globalObject->webAssemblyInstanceStructure(), globalObject, globalObject->moduleLoader(), JSWebAssemblyInstance::createPrivateModuleKey(), module, nullptr, creationMode, WTF::move(provider));
    AOT_TEST_CHECK(!scope.exception());
    instance->initializeImports(globalObject, nullptr, creationMode);
    AOT_TEST_CHECK(!scope.exception());

    AOT_TEST_CHECK(instance->memory0Mode() == MemoryMode::BoundsChecking);
    instance->finalizeCreation(vm, globalObject, module->module().compileSync(vm, instance->memory0Mode()), creationMode);
    AOT_TEST_CHECK(!scope.exception());

    WebAssemblyModuleRecord* record = instance->moduleRecord();
    record->link(globalObject, nullptr);
    record->initializeImports(globalObject, imports, Wasm::CreationMode::FromJS);
    AOT_TEST_CHECK(!scope.exception());
    record->initializeExports(globalObject);
    AOT_TEST_CHECK(!scope.exception());
    return instance;
}

static void testImportedMemoryInModuleLoaderOrder()
{
    if (!canRunInThisProcess()) {
        runInNewProcess(moduleLoaderOrderTestName);
        return;
    }

    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* globalObject = realms.plain();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    Wasm::ModuleForImage moduleForImage = compileForImage(vm, importedMemoryModule);
    AOT_TEST_CHECK(moduleForImage.isForSignalingMemory);
    AOT_TEST_CHECK(moduleForImage.functions.size() == 1);
    AOT_TEST_CHECK(moduleForImage.functions[0].hasCode);
    setProcessImage(WTF::move(moduleForImage));

    auto validated = Wasm::Module::validateSync(vm, Vector<uint8_t>(std::span<const uint8_t>(importedMemoryModule)));
    AOT_TEST_CHECK(validated.has_value());
    JSWebAssemblyModule* module = JSWebAssemblyModule::create(vm, globalObject->webAssemblyModuleStructure(), WTF::move(*validated));
    const Wasm::ModuleInformation& info = module->moduleInformation();
    AOT_TEST_CHECK(info.moduleWithSkippedFunctions);

    JSObject* fastImports = realms.object(globalObject, "({ './imports.js': { memory: new WebAssembly.Memory({ initial: 1 }), result: new WebAssembly.Global({ value: 'i32', mutable: true }, 0) } })"_s);
    JSWebAssemblyInstance* compiled = instantiateAsModuleLoader(realms, module, fastImports);
    void* const* pointers = compiled->calleeGroup()->imageCodePointers();
    if (compiled->memory0Mode() == MemoryMode::Signaling) {
        AOT_TEST_CHECK(pointers);
        AOT_TEST_CHECK(info.moduleWithSkippedFunctions);
    }
    AOT_TEST_CHECK(compiled->imageCodePointers() == pointers);

    JSObject* boundsCheckedImports = realms.object(globalObject, "globalThis.boundsCheckedImports = { './imports.js': { memory: new WebAssembly.Memory({ initial: 0, maximum: 0 }), result: new WebAssembly.Global({ value: 'i32', mutable: true }, 0) } }"_s);
    JSWebAssemblyInstance* interpreted = instantiateAsModuleLoader(realms, module, boundsCheckedImports);
    AOT_TEST_CHECK(interpreted->memory0Mode() == MemoryMode::BoundsChecking);
    AOT_TEST_CHECK(!interpreted->imageCodePointers());
    AOT_TEST_CHECK(!info.moduleWithSkippedFunctions);
    AOT_TEST_CHECK(compiled->imageCodePointers() == pointers);

    interpreted->moduleRecord()->evaluate(globalObject);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(realms.evaluate(globalObject, "boundsCheckedImports['./imports.js'].result.value"_s) == jsNumber(7));
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
        { "webassembly-instance/tripwire-reports-declined-module"_s, testTripwireReportsDeclinedModule },
        { moduleLoaderOrderTestName, testImportedMemoryInModuleLoaderOrder },
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
