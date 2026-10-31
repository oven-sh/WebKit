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
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTIRProgram.h"
#include "AOTProgramFile.h"
#include "AOTWasmModules.h"
#include "B3RelocatableValue.h"
#include "JSLock.h"
#include "Options.h"
#include "VM.h"
#include "WasmCodeInImage.h"
#include "WasmModule.h"
#include "WasmOperations.h"
#include <algorithm>
#include <array>
#include <bmalloc/Gigacage.h>
#include "AOTBitVector.h"
#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

namespace {

class TestVM {
public:
    TestVM()
        : m_vm(VM::create(HeapType::Small))
        , m_locker(*m_vm)
    {
    }

    ~TestVM() { m_vm = nullptr; }

    VM& get() { return *m_vm; }

private:
    RefPtr<VM> m_vm;
    JSLockHolder m_locker;
};

using CompiledModule = std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason>;

constexpr uint8_t emptyModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
};

constexpr uint8_t invalidModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0xff,
};

constexpr uint8_t structTypeModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x08, 0x02, 0x5f, 0x01, 0x7f, 0x00, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,
};

constexpr uint8_t callModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x00,
    0x0a, 0x0b, 0x02, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x04, 0x00, 0x10, 0x00, 0x0b,
};

constexpr uint8_t tailCallModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x00,
    0x0a, 0x0b, 0x02, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x04, 0x00, 0x12, 0x00, 0x0b,
};

constexpr uint8_t onlyTailCallModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x12, 0x00, 0x0b,
};

constexpr uint8_t callToInterpretedFunctionModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x04, 0x03, 0x00, 0x00, 0x00,
    0x0a, 0x10, 0x03, 0x04, 0x00, 0x12, 0x01, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x04, 0x00, 0x10, 0x00, 0x0b,
};

constexpr uint8_t callToCompiledFunctionModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x04, 0x03, 0x00, 0x00, 0x00,
    0x0a, 0x10, 0x03, 0x04, 0x00, 0x12, 0x01, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x04, 0x00, 0x10, 0x01, 0x0b,
};

constexpr uint8_t throwModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x01,
    0x0d, 0x03, 0x01, 0x00, 0x00,
    0x0a, 0x0b, 0x02, 0x04, 0x00, 0x08, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x02, 0x0b,
};

constexpr uint8_t referenceTypeTestModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0a, 0x02, 0x60, 0x01, 0x70, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x05, 0x04, 0x00, 0x00, 0x00, 0x01,
    0x0a, 0x2f, 0x04,
    0x07, 0x00, 0x20, 0x00, 0xfb, 0x14, 0x01, 0x0b,
    0x0a, 0x00, 0x20, 0x00, 0xfb, 0x16, 0x01, 0x1a, 0x41, 0x00, 0x0b,
    0x15, 0x00, 0x02, 0x64, 0x01, 0x20, 0x00, 0xfb, 0x18, 0x01, 0x00, 0x70, 0x01, 0x1a, 0x41, 0x00, 0x0f, 0x0b, 0x1a, 0x41, 0x01, 0x0b,
    0x04, 0x00, 0x41, 0x01, 0x0b,
};

constexpr uint8_t unlistedOperationModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02, 0x60, 0x01, 0x6f, 0x00, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x01,
    0x0a, 0x0e, 0x02, 0x07, 0x00, 0x20, 0x00, 0xfb, 0x1a, 0x1a, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b,
};

constexpr uint8_t bulkMemoryModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x00,
    0x03, 0x03, 0x02, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x0a, 0x1a, 0x02,
    0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfc, 0x0a, 0x00, 0x00, 0x0b,
    0x0b, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfc, 0x0b, 0x00, 0x0b,
};

constexpr uint8_t importAndExportModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7e,
    0x02, 0x07, 0x01, 0x01, 0x6d, 0x01, 0x66, 0x00, 0x00,
    0x03, 0x04, 0x03, 0x00, 0x00, 0x01,
    0x07, 0x11, 0x04, 0x01, 0x66, 0x00, 0x00, 0x01, 0x61, 0x00, 0x01, 0x01, 0x62, 0x00, 0x02, 0x01, 0x63, 0x00, 0x03,
    0x0a, 0x10, 0x03, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x04, 0x00, 0x41, 0x02, 0x0b, 0x04, 0x00, 0x42, 0x03, 0x0b,
};

constexpr uint8_t memoryModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,
};

constexpr uint8_t branchTableModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x46, 0x01, 0x44, 0x00,
    0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40,
    0x20, 0x00, 0x0e, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x0b, 0x41, 0x07, 0x0f,
    0x0b, 0x41, 0x29, 0x0f,
    0x0b, 0x41, 0x03, 0x0f,
    0x0b, 0x41, 0x1d, 0x0f,
    0x0b, 0x41, 0x0d, 0x0f,
    0x0b, 0x41, 0x3b, 0x0f,
    0x0b, 0x41, 0x11, 0x0f,
    0x0b, 0x41, 0x25, 0x0f,
    0x0b, 0x41, 0x17,
    0x0b,
};

constexpr double linkedConstant = 0.1;

constexpr uint8_t doubleConstantModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7c,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x0d, 0x01, 0x0b, 0x00, 0x44, 0x9a, 0x99, 0x99, 0x99, 0x99, 0x99, 0xb9, 0x3f, 0x0b,
};

#if CPU(X86_64)
constexpr uint8_t vectorAndScalarModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x00,
    0x0a, 0x1e, 0x02,
    0x17, 0x00, 0xfd, 0x0c, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xfd, 0x1b, 0x00, 0x0b,
    0x04, 0x00, 0x41, 0x01, 0x0b,
};
#endif

constexpr ASCIILiteral twoImportersText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}]
module m1 path="other.js" displayPath="other.js" body=f1
entry module=m0
webassemblymodule module=m0
webassemblymodule module=m1
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="other" module=m1 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

constexpr uint32_t codeUnitAlignment = 16;

constexpr std::array declineReasons {
    Wasm::ImageDeclineReason::None,
    Wasm::ImageDeclineReason::UnsupportedCPU,
    Wasm::ImageDeclineReason::InvalidModule,
    Wasm::ImageDeclineReason::NoFunctions,
    Wasm::ImageDeclineReason::HasGCTypes,
    Wasm::ImageDeclineReason::CodeTooLarge,
    Wasm::ImageDeclineReason::UsesExceptions,
    Wasm::ImageDeclineReason::UsesTailCalls,
    Wasm::ImageDeclineReason::TestsReferenceTypes,
    Wasm::ImageDeclineReason::CallsUnlistedOperation,
    Wasm::ImageDeclineReason::NamesTypeOfOtherModule,
    Wasm::ImageDeclineReason::SavesVectorRegister,
    Wasm::ImageDeclineReason::SavedRegisterOrder,
    Wasm::ImageDeclineReason::ProcessAddressInCode,
    Wasm::ImageDeclineReason::CodeGeneratorFailed,
};
static_assert(declineReasons.size() == static_cast<size_t>(Wasm::ImageDeclineReason::CodeGeneratorFailed) + 1);

} // anonymous namespace

static const CodeTemplates& templates()
{
    static NeverDestroyed<CodeTemplates> templates = [] {
        auto created = CodeTemplates::create(AssemblerPolicy::forImage(CPUFeatureSet::host()), nullptr);
        AOT_TEST_CHECK(created.has_value());
        return WTF::move(*created);
    }();
    return templates;
}

static CompiledModule compileModule(std::span<const uint8_t> source, BuildContext& context)
{
    TestVM vm;
    return compileWasmModule(vm.get(), source, templates(), context);
}

static WebAssemblyImageInputs layOut(Vector<Wasm::ModuleForImage>&& modules)
{
    WasmSectionBuilder builder;
    for (Wasm::ModuleForImage& module : modules)
        AOT_TEST_CHECK(builder.append(WTF::move(module)));
    return builder.finish();
}

static void checkModule(const Wasm::ModuleForImage& module)
{
    AOT_TEST_CHECK(module.functionsEnd <= module.code.size());

    uint32_t previousStub = 0;
    auto checkStub = [&](uint32_t stub) {
        AOT_TEST_CHECK(!(stub % codeUnitAlignment));
        AOT_TEST_CHECK(previousStub < stub);
        previousStub = stub;
    };
    for (uint32_t stub : module.importStubs)
        checkStub(stub);
    for (const Wasm::JSEntrypointInImage& entrypoint : module.jsEntrypoints)
        checkStub(entrypoint.code);
    AOT_TEST_CHECK(previousStub < module.functions[0].entry);

    size_t numberOfInterpretedFunctions = 0;
    for (size_t index = 0; index < module.functions.size(); ++index) {
        const Wasm::FunctionInImage& function = module.functions[index];
        AOT_TEST_CHECK(!(function.entry % codeUnitAlignment));
        AOT_TEST_CHECK(function.entry < module.functionsEnd);
        AOT_TEST_CHECK(!index || module.functions[index - 1].entry < function.entry);
        if (function.hasCode) {
            AOT_TEST_CHECK(function.savedRegisters < module.savedRegisters.size());
            continue;
        }
        AOT_TEST_CHECK(!function.savedRegisters);
        ++numberOfInterpretedFunctions;
    }

    AOT_TEST_CHECK(module.interpretedFunctions.size() == numberOfInterpretedFunctions);
    for (auto [function, reason] : module.interpretedFunctions) {
        AOT_TEST_CHECK(!module.functions[function].hasCode);
        AOT_TEST_CHECK(reason != Wasm::ImageDeclineReason::None);
    }
}

static void checkOnlyInterpretedFunction(const Wasm::ModuleForImage& module, uint32_t function, Wasm::ImageDeclineReason reason)
{
    AOT_TEST_CHECK(module.interpretedFunctions.size() == 1);
    AOT_TEST_CHECK(module.interpretedFunctions[0].first == function);
    AOT_TEST_CHECK(module.interpretedFunctions[0].second == reason);
}

static Wasm::ModuleForImage compileValidModule(std::span<const uint8_t> source, BuildContext& context)
{
    CompiledModule module = compileModule(source, context);
    AOT_TEST_CHECK(module.has_value());
    checkModule(*module);
    return WTF::move(*module);
}

static Wasm::ModuleForImage compileValidModule(std::span<const uint8_t> source)
{
    BuildContext context(makeDefaultCompileOptions());
    return compileValidModule(source, context);
}

static Wasm::ImageDeclineReason compileDeclinedModule(std::span<const uint8_t> source, BuildContext& context)
{
    CompiledModule module = compileModule(source, context);
    AOT_TEST_CHECK(!module);
    return module.error();
}

static Wasm::ImageDeclineReason compileDeclinedModule(std::span<const uint8_t> source)
{
    BuildContext context(makeDefaultCompileOptions());
    return compileDeclinedModule(source, context);
}

static Wire::CompileOptions optionsWith(ASCIILiteral word)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    AOT_TEST_CHECK(!applyOptionWord(options, word));
    return options;
}

static uint64_t countOf(BuildContext& context, StatisticID statistic)
{
    return context.statistics().values()[static_cast<size_t>(statistic)];
}

template<typename T>
static std::span<const T> arrayInDirectory(const WebAssemblyImageInputs& section, uint32_t offset, uint32_t count)
{
    return spanReinterpretCast<const T>(section.directory.span().subspan(offset, count * sizeof(T)));
}

static std::span<const Wasm::ModuleInImage> recordsOf(const WebAssemblyImageInputs& section)
{
    uint32_t numberOfModules = reinterpretCastSpanStartTo<Wasm::DirectoryInImage>(section.directory.span()).numberOfModules;
    return arrayInDirectory<Wasm::ModuleInImage>(section, sizeof(Wasm::DirectoryInImage), numberOfModules);
}

static std::span<const Wasm::FunctionInImage> functionsOf(const WebAssemblyImageInputs& section, const Wasm::ModuleInImage& record)
{
    return arrayInDirectory<Wasm::FunctionInImage>(section, record.functions, record.numberOfFunctions);
}

static std::span<const uint32_t> importStubsOf(const WebAssemblyImageInputs& section, const Wasm::ModuleInImage& record)
{
    return arrayInDirectory<uint32_t>(section, record.importStubs, record.numberOfImportedFunctions);
}

static std::span<const Wasm::JSEntrypointInImage> jsEntrypointsOf(const WebAssemblyImageInputs& section, const Wasm::ModuleInImage& record)
{
    return arrayInDirectory<Wasm::JSEntrypointInImage>(section, record.jsEntrypoints, record.numberOfJSEntrypoints);
}

static void checkRecord(const WebAssemblyImageInputs& section, const Wasm::ModuleInImage& record, std::span<const uint8_t> source)
{
    AOT_TEST_CHECK(record.digest == Wasm::CodeInImage::digestOf(source));
    AOT_TEST_CHECK(record.sourceSize == source.size());

    for (uint32_t offset : { record.functions, record.importStubs, record.jsEntrypoints, record.savedRegisters }) {
        AOT_TEST_CHECK(!(offset % 8));
        AOT_TEST_CHECK(offset <= section.directory.size());
    }

    auto functions = functionsOf(section, record);
    for (size_t index = 1; index < functions.size(); ++index)
        AOT_TEST_CHECK(functions[index - 1].entry < functions[index].entry);
    AOT_TEST_CHECK(functions.back().entry < record.functionsEnd);
    AOT_TEST_CHECK(record.functionsEnd <= section.code.size());

    for (uint32_t stub : importStubsOf(section, record))
        AOT_TEST_CHECK(stub < functions.front().entry);
    for (const Wasm::JSEntrypointInImage& entrypoint : jsEntrypointsOf(section, record))
        AOT_TEST_CHECK(entrypoint.code < functions.front().entry);
}

static void testCallHasCode()
{
    BuildContext context(makeDefaultCompileOptions());
    Wasm::ModuleForImage module = compileValidModule(callModule, context);
    AOT_TEST_CHECK(module.functions.size() == 2);
    AOT_TEST_CHECK(module.functions[0].hasCode);
    AOT_TEST_CHECK(module.functions[1].hasCode);
    AOT_TEST_CHECK(module.interpretedFunctions.isEmpty());
    AOT_TEST_CHECK(module.digest == Wasm::CodeInImage::digestOf(callModule));
    AOT_TEST_CHECK(module.sourceSize == sizeof(callModule));
    AOT_TEST_CHECK(!module.isForSignalingMemory);
    AOT_TEST_CHECK(!module.usesVectors);
    AOT_TEST_CHECK(module.importStubs.isEmpty());
    AOT_TEST_CHECK(module.jsEntrypoints.isEmpty());
    AOT_TEST_CHECK(countOf(context, StatisticID::WasmFunctionsCompiled) == 2);
    AOT_TEST_CHECK(!countOf(context, StatisticID::WasmFunctionsInterpreted));
    AOT_TEST_CHECK(!countOf(context, hitsOf(LimitID::WasmImageCodeSize)));
}

static void testTailCallIsInterpreted()
{
    if (!Options::useWasmTailCalls())
        return;

    BuildContext context(makeDefaultCompileOptions());
    Wasm::ModuleForImage module = compileValidModule(tailCallModule, context);
    AOT_TEST_CHECK(module.functions.size() == 2);
    AOT_TEST_CHECK(module.functions[0].hasCode);
    checkOnlyInterpretedFunction(module, 1, Wasm::ImageDeclineReason::UsesTailCalls);
    AOT_TEST_CHECK(module.functions[1].entry < module.code.size());
    AOT_TEST_CHECK(module.functions[1].entry != module.functions[0].entry);
    AOT_TEST_CHECK(countOf(context, StatisticID::WasmFunctionsCompiled) == 1);
    AOT_TEST_CHECK(countOf(context, StatisticID::WasmFunctionsInterpreted) == 1);
}

static void testThrowIsInterpreted()
{
    Wasm::ModuleForImage module = compileValidModule(throwModule);
    AOT_TEST_CHECK(module.functions.size() == 2);
    checkOnlyInterpretedFunction(module, 0, Wasm::ImageDeclineReason::UsesExceptions);
    AOT_TEST_CHECK(module.functions[1].hasCode);
}

static void testReferenceTypeTestIsInterpreted()
{
    Wasm::ModuleForImage module = compileValidModule(referenceTypeTestModule);
    AOT_TEST_CHECK(module.functions.size() == 4);
    AOT_TEST_CHECK(module.interpretedFunctions.size() == 3);
    for (uint32_t function = 0; function < 3; ++function) {
        AOT_TEST_CHECK(module.interpretedFunctions[function].first == function);
        AOT_TEST_CHECK(module.interpretedFunctions[function].second == Wasm::ImageDeclineReason::TestsReferenceTypes);
    }
    AOT_TEST_CHECK(module.functions[3].hasCode);
}

static void testUnlistedOperationIsInterpreted()
{
    Wasm::ModuleForImage module = compileValidModule(unlistedOperationModule);
    AOT_TEST_CHECK(module.functions.size() == 2);
    checkOnlyInterpretedFunction(module, 0, Wasm::ImageDeclineReason::CallsUnlistedOperation);
    AOT_TEST_CHECK(module.functions[1].hasCode);
}

static void testBulkMemoryHasCode()
{
    Wasm::ModuleForImage module = compileValidModule(bulkMemoryModule);
    AOT_TEST_CHECK(module.functions.size() == 2);
    AOT_TEST_CHECK(module.interpretedFunctions.isEmpty());
    AOT_TEST_CHECK(module.isForSignalingMemory);
}

static void testCallToInterpretedFunction()
{
    if (!Options::useWasmTailCalls())
        return;

    Wasm::ModuleForImage module = compileValidModule(callToInterpretedFunctionModule);
    AOT_TEST_CHECK(module.functions.size() == 3);
    checkOnlyInterpretedFunction(module, 0, Wasm::ImageDeclineReason::UsesTailCalls);
    AOT_TEST_CHECK(module.functions[2].hasCode);

    Wasm::ModuleForImage other = compileValidModule(callToCompiledFunctionModule);
    AOT_TEST_CHECK(equalSpans(asBytes(module.functions.span()), asBytes(other.functions.span())));
    AOT_TEST_CHECK(module.code.size() == other.code.size());
    size_t caller = module.functions[2].entry;
    AOT_TEST_CHECK(equalSpans(module.code.span().first(caller), other.code.span().first(caller)));
    AOT_TEST_CHECK(!equalSpans(module.code.span().subspan(caller), other.code.span().subspan(caller)));
}

static void testModuleWithoutCompiledFunction()
{
    if (!Options::useWasmTailCalls())
        return;

    Wasm::ModuleForImage module = compileValidModule(onlyTailCallModule);
    AOT_TEST_CHECK(module.functions.size() == 1);
    checkOnlyInterpretedFunction(module, 0, Wasm::ImageDeclineReason::UsesTailCalls);
    AOT_TEST_CHECK(module.savedRegisters.isEmpty());
    AOT_TEST_CHECK(!module.usesVectors);
}

static void testModuleWithoutFunctions()
{
    AOT_TEST_CHECK(compileDeclinedModule(emptyModule) == Wasm::ImageDeclineReason::NoFunctions);
}

static void testInvalidModule()
{
    AOT_TEST_CHECK(compileDeclinedModule(invalidModule) == Wasm::ImageDeclineReason::InvalidModule);
}

static void testModuleWithStructType()
{
    AOT_TEST_CHECK(compileDeclinedModule(structTypeModule) == Wasm::ImageDeclineReason::HasGCTypes);
}

static void testCodeSizeLimit()
{
    BuildContext context(optionsWith("limit=WasmImageCodeSize:16"_s));
    AOT_TEST_CHECK(compileDeclinedModule(callModule, context) == Wasm::ImageDeclineReason::CodeTooLarge);
    AOT_TEST_CHECK(!countOf(context, StatisticID::WasmFunctionsCompiled));
    AOT_TEST_CHECK(countOf(context, StatisticID::WasmFunctionsInterpreted) == 2);
}

static void testThreadCountDoesNotChangeCode()
{
    Wasm::ModuleForImage parallel = compileValidModule(callModule);

    BuildContext context(optionsWith("threads=1"_s));
    Wasm::ModuleForImage serial = compileValidModule(callModule, context);
    AOT_TEST_CHECK(serial.code == parallel.code);
    AOT_TEST_CHECK(equalSpans(asBytes(serial.functions.span()), asBytes(parallel.functions.span())));
    AOT_TEST_CHECK(serial.savedRegisters == parallel.savedRegisters);
}

static void testSectionWithoutModules()
{
    WebAssemblyImageInputs section = layOut({ });
    AOT_TEST_CHECK(section.directory.isEmpty());
    AOT_TEST_CHECK(section.code.isEmpty());
    AOT_TEST_CHECK(section.codeAlignment == 1);
    AOT_TEST_CHECK(!section.numberOfModules);
}

static void testSectionWithDifferentModules()
{
    Wasm::ModuleForImage first = compileValidModule(callModule);
    Wasm::ModuleForImage second = compileValidModule(throwModule);
    Vector<uint8_t> firstCode = first.code;
    Vector<uint8_t> secondCode = second.code;
    uint32_t entryInSecondModule = second.functions[0].entry;

    WebAssemblyImageInputs section = layOut(Vector<Wasm::ModuleForImage>::from(WTF::move(first), WTF::move(second)));
    AOT_TEST_CHECK(section.codeAlignment == WasmSectionBuilder::codeAlignment);
    AOT_TEST_CHECK(section.numberOfModules == 2);
    auto records = recordsOf(section);
    AOT_TEST_CHECK(records.size() == 2);
    checkRecord(section, records[0], callModule);
    checkRecord(section, records[1], throwModule);

    uint32_t entryInSection = functionsOf(section, records[1])[0].entry;
    AOT_TEST_CHECK(records[0].functionsEnd <= entryInSection);
    AOT_TEST_CHECK(!functionsOf(section, records[1])[0].hasCode);
    AOT_TEST_CHECK(functionsOf(section, records[1])[1].hasCode);

    uint32_t base = entryInSection - entryInSecondModule;
    AOT_TEST_CHECK(!(base % WasmSectionBuilder::codeAlignment));
    AOT_TEST_CHECK(equalSpans(section.code.span().first(firstCode.size()), firstCode.span()));
    AOT_TEST_CHECK(equalSpans(section.code.span().subspan(base), secondCode.span()));
}

static void testImportAndJSEntrypointStubs()
{
    Wasm::ModuleForImage module = compileValidModule(importAndExportModule);
    AOT_TEST_CHECK(module.importStubs.size() == 1);
    AOT_TEST_CHECK(module.jsEntrypoints.size() == 1);
    AOT_TEST_CHECK(!module.jsEntrypoints[0].type);
    uint32_t importStub = module.importStubs[0];
    uint32_t jsEntrypoint = module.jsEntrypoints[0].code;
    uint32_t firstFunction = module.functions[0].entry;

    WebAssemblyImageInputs section = layOut(Vector<Wasm::ModuleForImage>::from(compileValidModule(callModule), WTF::move(module)));
    AOT_TEST_CHECK(section.codeAlignment == WasmSectionBuilder::codeAlignment);
    AOT_TEST_CHECK(section.numberOfModules == 2);
    auto records = recordsOf(section);
    AOT_TEST_CHECK(records.size() == 2);
    checkRecord(section, records[1], importAndExportModule);

    uint32_t base = functionsOf(section, records[1])[0].entry - firstFunction;
    AOT_TEST_CHECK(base && !(base % WasmSectionBuilder::codeAlignment));
    auto importStubs = importStubsOf(section, records[1]);
    AOT_TEST_CHECK(importStubs.size() == 1);
    AOT_TEST_CHECK(importStubs[0] == importStub + base);
    auto jsEntrypoints = jsEntrypointsOf(section, records[1]);
    AOT_TEST_CHECK(jsEntrypoints.size() == 1);
    AOT_TEST_CHECK(!jsEntrypoints[0].type);
    AOT_TEST_CHECK(jsEntrypoints[0].code == jsEntrypoint + base);
}

static void testPointerTableSlots()
{
    Wasm::ImagePointerTable table { 3, 2, 2 };

    Vector<uint32_t> slots;
#define APPEND_SLOT(operation) slots.append(Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operation));
    FOR_EACH_WASM_IMAGE_CODE_OPERATION(APPEND_SLOT)
#undef APPEND_SLOT
    slots.append(Wasm::ImagePointerTable::genericJSEntrypoint);
    slots.append(Wasm::ImagePointerTable::interpreterEntrypoint);
#if GIGACAGE_ENABLED
    slots.append(Wasm::ImagePointerTable::primitiveGigacageDisableRequest);
    slots.append(Wasm::ImagePointerTable::primitiveGigacageBase);
#endif
    for (uint32_t function = 0; function < table.numberOfFunctions; ++function)
        slots.append(table.callee(Wasm::FunctionCodeIndex(function)));
    for (uint32_t type = 0; type < table.numberOfTypes; ++type)
        slots.append(table.rtt(Wasm::TypeSignatureIndex(type)));
    for (uint32_t entry = 0; entry < table.numberOfJSEntrypoints; ++entry)
        slots.append(table.jsEntrypointCallee(entry));

    AOT_TEST_CHECK(slots.size() == table.size());
    BitVector usedSlots;
    for (uint32_t slot : slots) {
        AOT_TEST_CHECK(slot < table.size());
        AOT_TEST_CHECK(usedSlots.add(slot));
    }
}

static void testOperationSlots()
{
    auto slotOfOperation = [](auto* operation) {
        return Wasm::ImagePointerTable::slotOfOperation(std::bit_cast<void*>(tagCFunction<OperationPtrTag>(operation)));
    };
    AOT_TEST_CHECK(slotOfOperation(Wasm::operationThrowExceptionFromOMG) == Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationThrowExceptionFromOMG));
    AOT_TEST_CHECK(slotOfOperation(Wasm::operationPopcount64) == Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationPopcount64));
    AOT_TEST_CHECK(!slotOfOperation(Wasm::operationGetWasmTableSize));
}

static void testRuntimeFunctionSlots()
{
    static_assert(B3::numberOfRuntimeFunctions == 10);

    AOT_TEST_CHECK(Wasm::runtimeFunctionSlot(B3::RuntimeFunction::MemoryMove) == Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationMemoryMove));
    AOT_TEST_CHECK(Wasm::runtimeFunctionSlot(B3::RuntimeFunction::MemoryFill) == Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationMemoryFill));

    constexpr B3::RuntimeFunction functionsWithoutSlot[] = {
        B3::RuntimeFunction::GCSafeMemoryMove,
        B3::RuntimeFunction::FmodDouble,
        B3::RuntimeFunction::CeilDouble,
        B3::RuntimeFunction::CeilFloat,
        B3::RuntimeFunction::FloorDouble,
        B3::RuntimeFunction::FloorFloat,
        B3::RuntimeFunction::TruncDouble,
        B3::RuntimeFunction::TruncFloat,
    };
    for (B3::RuntimeFunction function : functionsWithoutSlot)
        AOT_TEST_CHECK(!Wasm::runtimeFunctionSlot(function));
}

static void testDiagnosticKinds()
{
    AOT_TEST_CHECK(diagnosticKindOf(Wasm::ImageDeclineReason::None) == UserDiagnosticKind::Unknown);
    for (size_t first = 0; first < declineReasons.size(); ++first) {
        UserDiagnosticKind kind = diagnosticKindOf(declineReasons[first]);
        AOT_TEST_CHECK(kind == UserDiagnosticKind::Unknown || severityOf(kind) == DiagnosticSeverity::Warning);
        for (size_t second = first + 1; second < declineReasons.size(); ++second)
            AOT_TEST_CHECK(kind != diagnosticKindOf(declineReasons[second]));
    }
}

static void testJumpTableIsLinked()
{
    Wasm::ModuleForImage module = compileValidModule(branchTableModule);
    AOT_TEST_CHECK(module.functions.size() == 1 && module.functions[0].hasCode);
    AOT_TEST_CHECK(module.functionsEnd < module.code.size());

    size_t tableOffset = roundUpToMultipleOf<sizeof(int32_t)>(module.functionsEnd);
    AOT_TEST_CHECK(!((module.code.size() - tableOffset) % sizeof(int32_t)));
    auto words = spanReinterpretCast<const int32_t>(module.code.span().subspan(tableOffset));
    AOT_TEST_CHECK(words.size() >= 8);
    for (int32_t word : words) {
        int64_t target = static_cast<int64_t>(tableOffset) + word;
        AOT_TEST_CHECK(target >= module.functions[0].entry && target < module.functionsEnd);
    }
}

static void testConstantIsLinked()
{
    Wasm::ModuleForImage module = compileValidModule(doubleConstantModule);
    AOT_TEST_CHECK(module.functions.size() == 1 && module.functions[0].hasCode);
    if (module.functionsEnd == module.code.size())
        return;
    AOT_TEST_CHECK(!std::ranges::search(module.code.span().subspan(module.functionsEnd), asByteSpan(linkedConstant)).empty());
}

static void testUnknownSymbolSpace()
{
    WasmFunctionCode function;
    function.code.bytes = Vector<uint8_t>(FillWith { }, 16, 0);
    function.code.relocations.append(Relocation { 0, RelocationKind::Call, SymbolSpace::Stub, primaryEntry, 0 });
    WasmModuleCode code;
    code.throwStub = Vector<uint8_t>(FillWith { }, 4, 0);
    code.functions.append(WTF::move(function));

    BuildContext context(makeDefaultCompileOptions());
    CompiledModule module = linkWasmModule(WTF::move(code), templates(), context);
    AOT_TEST_CHECK(!module && module.error() == Wasm::ImageDeclineReason::CodeGeneratorFailed);
    context.diagnostics().finish(nullptr);
    AOT_TEST_CHECK(context.diagnostics().internalErrors().size() == 1);
    AOT_TEST_CHECK(context.diagnostics().internalErrors()[0].kind == InternalErrorKind::RelocationNotApplied);
}

static WasmFunctionCode makeFunctionCalling(uint32_t callee, bool usesVectors)
{
    WasmFunctionCode function;
    function.usesVectors = usesVectors;
    function.callTargets.append({ Wasm::ImageCallTarget::Kind::Function, Wasm::FunctionCodeIndex(callee) });
    function.callTargets.append({ Wasm::ImageCallTarget::Kind::ThrowStub, Wasm::FunctionCodeIndex(0) });
    for (uint32_t target = 0; target < function.callTargets.size(); ++target) {
        function.code.relocations.append(Relocation { static_cast<uint32_t>(function.code.bytes.size()), RelocationKind::Call, SymbolSpace::WebAssembly, primaryEntry, target });
        function.code.bytes.appendVector(templates().nearCall);
    }
    function.code.bytes.appendVector(templates().returnToCaller);
    return function;
}

static Wasm::ModuleForImage linkByHand(WasmFunctionCode&& first, WasmFunctionCode&& second)
{
    WasmModuleCode code;
    code.throwStub = templates().returnToCaller;
    code.savedRegisters.append(Wasm::SavedRegistersInImage { 0, 0, 0 });
    code.functions.append(WTF::move(first));
    code.functions.append(WTF::move(second));

    BuildContext context(makeDefaultCompileOptions());
    CompiledModule module = linkWasmModule(WTF::move(code), templates(), context);
    AOT_TEST_CHECK(module.has_value());
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    checkModule(*module);
    AOT_TEST_CHECK(module->functions.size() == 2);
    AOT_TEST_CHECK(module->functionsEnd == module->code.size());
    return WTF::move(*module);
}

static void testLinkByHand()
{
    Wasm::ModuleForImage module = linkByHand(makeFunctionCalling(1, false), makeFunctionCalling(0, true));
    AOT_TEST_CHECK(module.interpretedFunctions.isEmpty());
    AOT_TEST_CHECK(module.functions[0].hasCode && module.functions[1].hasCode);
    AOT_TEST_CHECK(!module.functions[0].usesVectors && module.functions[1].usesVectors && module.usesVectors);
    AOT_TEST_CHECK(!linkByHand(makeFunctionCalling(1, false), makeFunctionCalling(0, false)).usesVectors);

    Vector<uint8_t> expected(FillWith { }, module.code.size(), 0);
    templates().fillWithBreakpoints(expected.mutableSpan());
    memcpySpan(expected.mutableSpan(), templates().returnToCaller.span());
    for (uint32_t index = 0; index < 2; ++index) {
        WasmFunctionCode function = makeFunctionCalling(1 - index, false);
        uint32_t entry = module.functions[index].entry;
        memcpySpan(expected.mutableSpan().subspan(entry), function.code.bytes.span());
        AOT_TEST_CHECK(applyRelocation(expected.mutableSpan(), entry, function.code.relocations[0], module.functions[1 - index].entry));
        AOT_TEST_CHECK(applyRelocation(expected.mutableSpan(), entry, function.code.relocations[1], 0));
    }
    AOT_TEST_CHECK(expected == module.code);
}

static void testLinkByHandWithDeclinedFunction()
{
    WasmFunctionCode declined = makeFunctionCalling(0, true);
    declined.decline(Wasm::ImageDeclineReason::UsesTailCalls);
    declined.code.bytes = templates().returnToCaller;

    Wasm::ModuleForImage module = linkByHand(makeFunctionCalling(1, false), WTF::move(declined));
    checkOnlyInterpretedFunction(module, 1, Wasm::ImageDeclineReason::UsesTailCalls);
    AOT_TEST_CHECK(module.functions[0].hasCode);
    AOT_TEST_CHECK(!module.functions[1].hasCode && !module.functions[1].savedRegisters);
    AOT_TEST_CHECK(!module.usesVectors);
}

static std::unique_ptr<Typed::Program> parseProgramWithTwoImporters(std::span<const uint8_t> module, BuildContext& context)
{
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(twoImportersText, Typed::IRForm::AsRead, context);
    if (!program) {
        context.diagnostics().finish(nullptr);
        Typed::printDiagnostics(context.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(program->webAssemblyModules.size() == 2);
    for (auto& entry : program->webAssemblyModules)
        entry.bytes = module;
    return program;
}

static void checkProgramReportsDecline(BuildContext& context, uint32_t numberOfModules, UserDiagnosticKind kind, LimitID limit)
{
    TestVM vm;
    std::unique_ptr<Typed::Program> program = parseProgramWithTwoImporters(tailCallModule, context);
    AOT_TEST_CHECK(compileWebAssemblyModules(vm.get(), *program, templates(), context).numberOfModules == numberOfModules);
    AOT_TEST_CHECK(countOf(context, hitsOf(LimitID::WasmImageCodeSize)) == (limit == LimitID::WasmImageCodeSize ? 2u : 0u));

    context.diagnostics().finish(program.get());
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    const Vector<UserDiagnostic>& diagnostics = context.diagnostics().userDiagnostics();
    AOT_TEST_CHECK(diagnostics.size() == 2);
    for (const UserDiagnostic& diagnostic : diagnostics)
        AOT_TEST_CHECK(diagnostic.kind == kind && diagnostic.limit == limit && diagnostic.count == 1 && diagnostic.location.module < 2);
    AOT_TEST_CHECK(diagnostics[0].location.module != diagnostics[1].location.module);
}

static void testProgramReportsDeclines()
{
    if (!Options::useWasmTailCalls())
        return;

    BuildContext context(makeDefaultCompileOptions());
    checkProgramReportsDecline(context, 1, UserDiagnosticKind::WasmUsesTailCalls, LimitID::None);

    BuildContext contextWithLimit(optionsWith("limit=WasmImageCodeSize:16"_s));
    checkProgramReportsDecline(contextWithLimit, 0, UserDiagnosticKind::WasmCodeTooLarge, LimitID::WasmImageCodeSize);
}

#if CPU(X86_64)
static void testVectorsPerFunction()
{
    if (!Options::useWasmSIMD())
        return;

    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.target.features = 0;
    BuildContext context(options);
    AOT_TEST_CHECK(!context.targetFeatures().contains(CPUFeature::AVX));

    Wasm::ModuleForImage module = compileValidModule(vectorAndScalarModule, context);
    AOT_TEST_CHECK(module.functions.size() == 2);
    AOT_TEST_CHECK(module.functions[0].hasCode && module.functions[1].hasCode);
    AOT_TEST_CHECK(module.functions[0].usesVectors && !module.functions[1].usesVectors);
    AOT_TEST_CHECK(module.usesVectors);
}
#endif

static void testInterpreterFallbackIsCountedOnce()
{
    if (Wasm::CodeInImage::isInProcess())
        return;

    Wasm::ModuleForImage moduleForImage = compileValidModule(memoryModule);
    AOT_TEST_CHECK(moduleForImage.isForSignalingMemory);

    static NeverDestroyed<WebAssemblyImageInputs> section;
    section.get() = layOut(Vector<Wasm::ModuleForImage>::from(WTF::move(moduleForImage)));
    Wasm::CodeInImage::addImage(section->directory.span(), section->code.span());
    AOT_TEST_CHECK(Wasm::CodeInImage::isInProcess());

    TestVM vm;
    auto validated = Wasm::Module::validateSync(vm.get(), Vector<uint8_t>(std::span<const uint8_t>(memoryModule)));
    AOT_TEST_CHECK(validated.has_value());
    Ref<Wasm::Module> module = WTF::move(*validated);

    auto fallbackCount = [] {
        return Wasm::CodeInImage::interpreterFallbackCount(Wasm::InterpreterFallbackReason::MemoryChecksBounds);
    };
    module->compileSync(vm.get(), MemoryMode::Signaling);
    AOT_TEST_CHECK(!fallbackCount());
    module->compileSync(vm.get(), MemoryMode::BoundsChecking);
    module->compileSync(vm.get(), MemoryMode::BoundsChecking);
    AOT_TEST_CHECK(fallbackCount() == 1);
}

} } // namespace JSC::AOT

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

namespace JSC { namespace AOT {

std::span<const NativeTest> webAssemblyImageTests()
{
#if ENABLE(WEBASSEMBLY_OMGJIT)
    static constexpr NativeTest tests[] = {
        { "webassembly-image/call-has-code"_s, testCallHasCode },
        { "webassembly-image/tail-call-is-interpreted"_s, testTailCallIsInterpreted },
        { "webassembly-image/throw-is-interpreted"_s, testThrowIsInterpreted },
        { "webassembly-image/reference-type-test-is-interpreted"_s, testReferenceTypeTestIsInterpreted },
        { "webassembly-image/unlisted-operation-is-interpreted"_s, testUnlistedOperationIsInterpreted },
        { "webassembly-image/bulk-memory-has-code"_s, testBulkMemoryHasCode },
        { "webassembly-image/call-to-interpreted-function"_s, testCallToInterpretedFunction },
        { "webassembly-image/module-without-compiled-function"_s, testModuleWithoutCompiledFunction },
        { "webassembly-image/module-without-functions"_s, testModuleWithoutFunctions },
        { "webassembly-image/invalid-module"_s, testInvalidModule },
        { "webassembly-image/module-with-struct-type"_s, testModuleWithStructType },
        { "webassembly-image/code-size-limit"_s, testCodeSizeLimit },
        { "webassembly-image/thread-count-does-not-change-code"_s, testThreadCountDoesNotChangeCode },
        { "webassembly-image/section-without-modules"_s, testSectionWithoutModules },
        { "webassembly-image/section-with-different-modules"_s, testSectionWithDifferentModules },
        { "webassembly-image/import-and-js-entrypoint-stubs"_s, testImportAndJSEntrypointStubs },
        { "webassembly-image/pointer-table-slots"_s, testPointerTableSlots },
        { "webassembly-image/operation-slots"_s, testOperationSlots },
        { "webassembly-image/runtime-function-slots"_s, testRuntimeFunctionSlots },
        { "webassembly-image/diagnostic-kinds"_s, testDiagnosticKinds },
        { "webassembly-image/jump-table-is-linked"_s, testJumpTableIsLinked },
        { "webassembly-image/constant-is-linked"_s, testConstantIsLinked },
        { "webassembly-image/unknown-symbol-space"_s, testUnknownSymbolSpace },
        { "webassembly-image/link-by-hand"_s, testLinkByHand },
        { "webassembly-image/link-by-hand-with-declined-function"_s, testLinkByHandWithDeclinedFunction },
        { "webassembly-image/program-reports-declines"_s, testProgramReportsDeclines },
#if CPU(X86_64)
        { "webassembly-image/vectors-per-function"_s, testVectorsPerFunction },
#endif
        { "webassembly-image/interpreter-fallback-is-counted-once"_s, testInterpreterFallbackIsCountedOnce },
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
