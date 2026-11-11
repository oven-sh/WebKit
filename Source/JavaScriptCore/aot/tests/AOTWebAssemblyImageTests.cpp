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
#include "AOTIRParser.h"
#include "AOTIRProgram.h"
#include "AOTImageBuilder.h"
#include "AOTImageTestSupport.h"
#include "AOTProgramFile.h"
#include "AOTWasmModules.h"
#include "JSCPtrTag.h"
#include "JSLock.h"
#include "VM.h"
#include "WasmCodeInImage.h"
#include "WasmHandlerInfo.h"
#include "WasmOperations.h"
#include <algorithm>
#include <array>
#include <bmalloc/Gigacage.h>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

using ImageTests::testTemplates;

namespace {

constexpr size_t trapping = static_cast<size_t>(Wasm::ImageCodeFlavor::Trapping);
constexpr size_t checking = static_cast<size_t>(Wasm::ImageCodeFlavor::Checking);

constexpr uint8_t callModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x00,
    0x0a, 0x0b, 0x02, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x04, 0x00, 0x10, 0x00, 0x0b,
};

constexpr uint8_t memoryModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0a, 0x02, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x04, 0x03, 0x00, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x0a, 0x15, 0x03, 0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x07, 0x0b, 0x06, 0x00, 0x20, 0x00, 0x10, 0x00, 0x0b,
};

constexpr uint8_t catchModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x03, 0x02, 0x00, 0x01,
    0x0d, 0x03, 0x01, 0x00, 0x00,
    0x0a, 0x14, 0x02, 0x04, 0x00, 0x08, 0x00, 0x0b, 0x0d, 0x00, 0x06, 0x7f, 0x10, 0x00, 0x41, 0x01, 0x07, 0x00, 0x41, 0x02, 0x0b, 0x0b,
};

constexpr uint8_t moduleWithoutFunctions[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
};

constexpr uint8_t invalidModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
};

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

constexpr std::array declineReasons {
    Wasm::ImageDeclineReason::None,
#define LIST_DECLINE_REASON(reason) Wasm::ImageDeclineReason::reason,
    FOR_EACH_WASM_IMAGE_DECLINE_REASON(LIST_DECLINE_REASON)
#undef LIST_DECLINE_REASON
};

} // anonymous namespace

static std::unique_ptr<Typed::Program> parseProgramWithTwoImporters(std::span<const uint8_t> moduleOfMain, std::span<const uint8_t> moduleOfOther, BuildContext& context)
{
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(twoImportersText, context);
    if (!program) {
        context.diagnostics().finish(nullptr);
        Typed::printDiagnostics(context.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(program->webAssemblyModules.size() == 2);
    program->webAssemblyModules[0].bytes = moduleOfMain;
    program->webAssemblyModules[1].bytes = moduleOfOther;
    return program;
}

static WasmModuleOrDecline compileModule(std::span<const uint8_t> source, BuildContext& context)
{
    RefPtr<VM> vm = VM::create(HeapType::Small);
    JSLockHolder locker(*vm);
    WasmModuleOrDecline module = compileWasmModule(*vm, source, testTemplates(), context);
    vm = nullptr;
    return module;
}

static Wasm::ModuleForImage compileModuleWithCode(std::span<const uint8_t> source)
{
    BuildContext context(makeDefaultCompileOptions());
    WasmModuleOrDecline module = compileModule(source, context);
    if (!module) {
        context.diagnostics().finish(nullptr);
        Typed::printDiagnostics(context.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(module);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    return WTF::move(*module);
}

static void checkFunctionsLieInCode(const Wasm::ModuleForImage& module, const Vector<Wasm::FunctionInImage>& functions)
{
    for (const Wasm::FunctionInImage& function : functions) {
        AOT_TEST_CHECK(function.size);
        AOT_TEST_CHECK(function.entry <= module.code.size() && function.size <= module.code.size() - function.entry);
        AOT_TEST_CHECK(function.savedRegisters < module.savedRegisters.size());
        AOT_TEST_CHECK(function.firstHandler <= module.handlers.size() && function.numberOfHandlers <= module.handlers.size() - function.firstHandler);
    }
}

static Wasm::DirectoryInImage headerOf(const WebAssemblyImageInputs& section)
{
    AOT_TEST_CHECK(section.directory.size() >= sizeof(Wasm::DirectoryInImage));
    return reinterpretCastSpanStartTo<Wasm::DirectoryInImage>(section.directory.span());
}

static std::span<const Wasm::ModuleInImage> modulesOf(const WebAssemblyImageInputs& section)
{
    size_t size = headerOf(section).numberOfModules * sizeof(Wasm::ModuleInImage);
    AOT_TEST_CHECK(section.directory.size() >= sizeof(Wasm::DirectoryInImage) + size);
    return spanReinterpretCast<const Wasm::ModuleInImage>(section.directory.span().subspan(sizeof(Wasm::DirectoryInImage), size));
}

static std::span<const Wasm::DeclinedModuleInImage> declinedModulesOf(const WebAssemblyImageInputs& section)
{
    size_t offset = sizeof(Wasm::DirectoryInImage) + headerOf(section).numberOfModules * sizeof(Wasm::ModuleInImage);
    size_t size = headerOf(section).numberOfDeclinedModules * sizeof(Wasm::DeclinedModuleInImage);
    AOT_TEST_CHECK(section.directory.size() >= offset + size);
    return spanReinterpretCast<const Wasm::DeclinedModuleInImage>(section.directory.span().subspan(offset, size));
}

template<typename Record>
static bool holdsOneRecordOf(std::span<const Record> records, std::span<const uint8_t> source)
{
    auto digest = Wasm::CodeInImage::digestOf(source);
    return std::ranges::count_if(records, [&](const Record& record) {
        return record.digest == digest && record.sourceSize == source.size();
    }) == 1;
}

static void testSectionWithoutModules()
{
    BuildContext context(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = parseProgramWithTwoImporters(callModule, catchModule, context);
    program->webAssemblyModules.clear();
    AOT_TEST_CHECK(!mayHaveWebAssemblyModules(*program));

    WebAssemblyImageInputs section = compileWebAssemblyModules(*program, testTemplates(), context);
    AOT_TEST_CHECK(section.directory.isEmpty());
    AOT_TEST_CHECK(section.code.isEmpty());
    AOT_TEST_CHECK(section.codeAlignment == 1);
    AOT_TEST_CHECK(!section.numberOfModules);

    context.diagnostics().finish(program.get());
    AOT_TEST_CHECK(context.diagnostics().userDiagnostics().isEmpty());
}

static void testSectionWithDifferentModules()
{
    BuildContext context(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = parseProgramWithTwoImporters(callModule, catchModule, context);
    AOT_TEST_CHECK(mayHaveWebAssemblyModules(*program));

    WebAssemblyImageInputs section = compileWebAssemblyModules(*program, testTemplates(), context);
    AOT_TEST_CHECK(section.numberOfModules == 2);
    AOT_TEST_CHECK(section.codeAlignment == WasmSectionBuilder::codeAlignment);
    AOT_TEST_CHECK(headerOf(section).numberOfModules == 2 && !headerOf(section).numberOfDeclinedModules);
    AOT_TEST_CHECK(headerOf(section).catchRunway < section.code.size());
    Vector<uint64_t> statistics = context.statistics().values();
    AOT_TEST_CHECK(statistics[static_cast<unsigned>(StatisticID::WasmCodeBytesEmitted)] == section.code.size());
    AOT_TEST_CHECK(statistics[static_cast<unsigned>(StatisticID::WasmFunctionsCompiled)] == 4);
    AOT_TEST_CHECK(statistics[static_cast<unsigned>(StatisticID::WasmSecondCopyBytesEmitted)] < section.code.size());
    AOT_TEST_CHECK(holdsOneRecordOf(modulesOf(section), std::span<const uint8_t>(callModule)));
    AOT_TEST_CHECK(holdsOneRecordOf(modulesOf(section), std::span<const uint8_t>(catchModule)));
    for (const Wasm::ModuleInImage& module : modulesOf(section)) {
        AOT_TEST_CHECK(module.numberOfFunctions == 2);
        for (uint32_t functions : module.functions)
            AOT_TEST_CHECK(functions + module.numberOfFunctions * sizeof(Wasm::FunctionInImage) <= section.directory.size());
    }

    context.diagnostics().finish(program.get());
    AOT_TEST_CHECK(context.diagnostics().userDiagnostics().isEmpty());
}

static void testModuleImportedTwiceIsCompiledOnce()
{
    BuildContext context(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = parseProgramWithTwoImporters(callModule, callModule, context);

    WebAssemblyImageInputs section = compileWebAssemblyModules(*program, testTemplates(), context);
    AOT_TEST_CHECK(section.numberOfModules == 1);
    AOT_TEST_CHECK(holdsOneRecordOf(modulesOf(section), std::span<const uint8_t>(callModule)));
}

static void testEveryFunctionHasCode()
{
    Wasm::ModuleForImage module = compileModuleWithCode(callModule);
    AOT_TEST_CHECK(module.functions[trapping].size() == 2);
    checkFunctionsLieInCode(module, module.functions[trapping]);
    AOT_TEST_CHECK(module.functions[trapping][0].entry != module.functions[trapping][1].entry);
    AOT_TEST_CHECK(module.catchRunway < module.code.size());
    AOT_TEST_CHECK(module.handlers.isEmpty());
    AOT_TEST_CHECK(!module.usesVectors);
}

static void testModuleWithoutMemoryHasOneFlavor()
{
    Wasm::ModuleForImage module = compileModuleWithCode(callModule);
    AOT_TEST_CHECK(module.functions[checking].isEmpty());
}

static void testMemoryModuleHasCopyPerFlavor()
{
    Wasm::ModuleForImage module = compileModuleWithCode(memoryModule);
    AOT_TEST_CHECK(module.functions[trapping].size() == 3 && module.functions[checking].size() == 3);
    checkFunctionsLieInCode(module, module.functions[trapping]);
    checkFunctionsLieInCode(module, module.functions[checking]);

    AOT_TEST_CHECK(module.functions[trapping][0].entry != module.functions[checking][0].entry);
    AOT_TEST_CHECK(module.functions[trapping][0].size < module.functions[checking][0].size);
    AOT_TEST_CHECK(module.functions[trapping][1].entry != module.functions[checking][1].entry);
    AOT_TEST_CHECK(module.functions[trapping][2].entry != module.functions[checking][2].entry);
}

static void testHandlersLieInTheirFunction()
{
    Wasm::ModuleForImage module = compileModuleWithCode(catchModule);
    const Wasm::FunctionInImage& thrower = module.functions[trapping][0];
    const Wasm::FunctionInImage& catcher = module.functions[trapping][1];
    AOT_TEST_CHECK(!thrower.numberOfHandlers);
    AOT_TEST_CHECK(catcher.numberOfHandlers == 1);

    const Wasm::HandlerInImage& handler = module.handlers[catcher.firstHandler];
    AOT_TEST_CHECK(static_cast<Wasm::HandlerType>(handler.type) == Wasm::HandlerType::Catch);
    AOT_TEST_CHECK(handler.start < handler.end);
    AOT_TEST_CHECK(!handler.exceptionIndexOrDelegateTarget);
    AOT_TEST_CHECK(handler.code > catcher.entry && handler.code < catcher.entry + catcher.size);
}

static Vector<uint8_t> moduleOfSwitches(uint8_t numberOfFunctions)
{
    static constexpr uint8_t body[] = {
        0x34, 0x00,
        0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40,
        0x20, 0x00, 0x0e, 0x0a, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05,
        0x0b, 0x41, 0x00, 0x0f, 0x0b, 0x41, 0x01, 0x0f, 0x0b, 0x41, 0x02, 0x0f, 0x0b, 0x41, 0x03, 0x0f, 0x0b, 0x41, 0x04, 0x0f,
        0x0b, 0x41, 0x05, 0x0b,
    };
    static_assert(body[0] == sizeof(body) - 1);
    size_t sizeOfCodeSection = 1 + numberOfFunctions * sizeof(body);
    RELEASE_ASSERT(numberOfFunctions < 0x7f && sizeOfCodeSection >= 0x80 && sizeOfCodeSection < 0x4000);
    Vector<uint8_t> module { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f };
    module.appendList({ static_cast<uint8_t>(0x03), static_cast<uint8_t>(numberOfFunctions + 1), numberOfFunctions });
    for (uint8_t function = 0; function < numberOfFunctions; ++function)
        module.append(static_cast<uint8_t>(0x00));
    module.appendList({ static_cast<uint8_t>(0x0a), static_cast<uint8_t>((sizeOfCodeSection & 0x7f) | 0x80), static_cast<uint8_t>(sizeOfCodeSection >> 7), numberOfFunctions });
    for (uint8_t function = 0; function < numberOfFunctions; ++function)
        module.append(std::span<const uint8_t>(body));
    return module;
}

static void testSameBytesEachTime()
{
    Vector<uint8_t> switchModule = moduleOfSwitches(16);
    for (std::span<const uint8_t> source : { std::span<const uint8_t>(callModule), std::span<const uint8_t>(memoryModule), std::span<const uint8_t>(catchModule), std::span<const uint8_t>(switchModule.span()) }) {
        Wasm::ModuleForImage first = compileModuleWithCode(source);
        Wasm::ModuleForImage second = compileModuleWithCode(source);
        AOT_TEST_CHECK(first.code == second.code);
        AOT_TEST_CHECK(equalSpans(asBytes(first.functions[trapping].span()), asBytes(second.functions[trapping].span())));
        AOT_TEST_CHECK(equalSpans(asBytes(first.functions[checking].span()), asBytes(second.functions[checking].span())));
        AOT_TEST_CHECK(equalSpans(asBytes(first.handlers.span()), asBytes(second.handlers.span())));
    }
}

static RelocatableCode codeThatCalls(std::optional<uint32_t> symbol)
{
    BuildContext context(makeDefaultCompileOptions());
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    if (symbol)
        recorder.call({ SymbolSpace::WebAssembly, *symbol });
    jit.ret();
    auto code = recorder.finalize(LinkBuffer::Profile::WasmThunk, &context);
    AOT_TEST_CHECK(code);
    return WTF::move(*code);
}

static WasmModuleCode moduleCodeOfThreeFunctions()
{
    WasmModuleCode module;
    for (RelocatableCode& stub : module.stubs)
        stub = codeThatCalls(std::nullopt);
    module.functions = Vector<WasmFunctionCode>(3);
    module.functionsThatCheck = Vector<WasmFunctionCode>(3);
    module.savedRegisters.append(Wasm::SavedRegistersInImage { 0, 0, 0 });
    return module;
}

static void testCallGoesToCopyOfItsFlavor()
{
    WasmModuleCode module = moduleCodeOfThreeFunctions();
    module.functions[0].code = codeThatCalls(std::nullopt);
    module.functions[0].dependsOnFlavor = true;
    module.functionsThatCheck[0].code = codeThatCalls(std::nullopt);
    module.functions[1].code = codeThatCalls(Wasm::imageSymbolOf(Wasm::FunctionCodeIndex(0)));
    module.functions[1].directCallees.set(0);
    module.functions[2].code = codeThatCalls(Wasm::imageSymbolOf(Wasm::ImageStub::ThrowException));
    size_t sizeOfCaller = module.functions[1].code.bytes.size();

    BuildContext context(makeDefaultCompileOptions());
    WasmModuleOrDecline linked = linkWasmModule(WTF::move(module), testTemplates(), context);
    AOT_TEST_CHECK(linked);
    const auto& whenTrapping = linked->functions[trapping];
    const auto& whenChecking = linked->functions[checking];
    AOT_TEST_CHECK(whenTrapping.size() == 3 && whenChecking.size() == 3);
    AOT_TEST_CHECK(whenTrapping[0].entry != whenChecking[0].entry);
    AOT_TEST_CHECK(whenTrapping[1].entry != whenChecking[1].entry);
    AOT_TEST_CHECK(whenTrapping[2].entry == whenChecking[2].entry);

    std::span<const uint8_t> code = linked->code.span();
    bool callersAreAlike = equalSpans(code.subspan(whenTrapping[1].entry, sizeOfCaller), code.subspan(whenChecking[1].entry, sizeOfCaller));
    bool distancesAreAlike = static_cast<int64_t>(whenTrapping[0].entry) - whenTrapping[1].entry == static_cast<int64_t>(whenChecking[0].entry) - whenChecking[1].entry;
    AOT_TEST_CHECK(callersAreAlike == distancesAreAlike);
}

static void testCallOfUnknownSymbolIsRefused()
{
    WasmModuleCode module = moduleCodeOfThreeFunctions();
    module.functions[0].code = codeThatCalls(Wasm::imageSymbolOf(Wasm::FunctionCodeIndex(3)));
    module.functions[1].code = codeThatCalls(std::nullopt);
    module.functions[2].code = codeThatCalls(std::nullopt);

    BuildContext context(makeDefaultCompileOptions());
    WasmModuleOrDecline linked = linkWasmModule(WTF::move(module), testTemplates(), context);
    AOT_TEST_CHECK(!linked);
    AOT_TEST_CHECK(context.diagnostics().hasErrors());
}

static void testPointerTableSlots()
{
    Wasm::ImagePointerTable table { 3, 2, 2 };

    Vector<uint32_t> slots;
#define APPEND_SLOT(operation) slots.append(Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operation));
    FOR_EACH_WASM_IMAGE_CODE_OPERATION(APPEND_SLOT)
#undef APPEND_SLOT
    slots.append(Wasm::ImagePointerTable::genericJSEntrypoint);
    slots.append(Wasm::ImagePointerTable::defaultJSCall);
    slots.append(Wasm::ImagePointerTable::wasmToJSCallee);
    slots.append(Wasm::ImagePointerTable::restoreFrameCallee);
    slots.append(Wasm::ImagePointerTable::restoreFrameReturnAddress);
    slots.append(Wasm::ImagePointerTable::throwFromFaultHandler);
    slots.append(Wasm::ImagePointerTable::structureIDBase);
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
    AOT_TEST_CHECK(slotOfOperation(Wasm::operationWasmToJSException) == Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationWasmToJSException));
    AOT_TEST_CHECK(slotOfOperation(Wasm::operationIterateResults) == Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationIterateResults));
    AOT_TEST_CHECK(!slotOfOperation(Wasm::operationGetWasmTableSize));
}

static void testDiagnosticKinds()
{
    AOT_TEST_CHECK(diagnosticKindOf(Wasm::ImageDeclineReason::None) == UserDiagnosticKind::Unknown);
    for (size_t first = 0; first < declineReasons.size(); ++first) {
        UserDiagnosticKind kind = diagnosticKindOf(declineReasons[first]);
        for (size_t second = first + 1; second < declineReasons.size(); ++second)
            AOT_TEST_CHECK(kind != diagnosticKindOf(declineReasons[second]));
    }
    AOT_TEST_CHECK(severityOf(diagnosticKindOf(Wasm::ImageDeclineReason::FunctionNotCompiled)) == DiagnosticSeverity::Error);
    AOT_TEST_CHECK(severityOf(diagnosticKindOf(Wasm::ImageDeclineReason::CodeTooLarge)) == DiagnosticSeverity::Error);
    AOT_TEST_CHECK(severityOf(diagnosticKindOf(Wasm::ImageDeclineReason::InvalidModule)) == DiagnosticSeverity::Warning);
}

static void testModulesWithNothingToRunAreDeclined()
{
    BuildContext context(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = parseProgramWithTwoImporters(invalidModule, moduleWithoutFunctions, context);

    WebAssemblyImageInputs section = compileWebAssemblyModules(*program, testTemplates(), context);
    AOT_TEST_CHECK(section.code.isEmpty() && !section.numberOfModules);
    AOT_TEST_CHECK(declinedModulesOf(section).size() == 2);
    AOT_TEST_CHECK(holdsOneRecordOf(declinedModulesOf(section), std::span<const uint8_t>(invalidModule)));
    AOT_TEST_CHECK(holdsOneRecordOf(declinedModulesOf(section), std::span<const uint8_t>(moduleWithoutFunctions)));

    context.diagnostics().finish(program.get());
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    const Vector<UserDiagnostic>& diagnostics = context.diagnostics().userDiagnostics();
    AOT_TEST_CHECK(diagnostics.size() == 1);
    AOT_TEST_CHECK(diagnostics[0].kind == diagnosticKindOf(Wasm::ImageDeclineReason::InvalidModule) && !diagnostics[0].location.module);
}

} } // namespace JSC::AOT

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

namespace JSC { namespace AOT {

std::span<const NativeTest> webAssemblyImageTests()
{
#if ENABLE(WEBASSEMBLY_OMGJIT)
    static constexpr NativeTest tests[] = {
        { "webassembly-image/section-without-modules"_s, testSectionWithoutModules },
        { "webassembly-image/section-with-different-modules"_s, testSectionWithDifferentModules },
        { "webassembly-image/module-imported-twice-is-compiled-once"_s, testModuleImportedTwiceIsCompiledOnce },
        { "webassembly-image/every-function-has-code"_s, testEveryFunctionHasCode },
        { "webassembly-image/module-without-memory-has-one-flavor"_s, testModuleWithoutMemoryHasOneFlavor },
        { "webassembly-image/memory-module-has-copy-per-flavor"_s, testMemoryModuleHasCopyPerFlavor },
        { "webassembly-image/handlers-lie-in-their-function"_s, testHandlersLieInTheirFunction },
        { "webassembly-image/same-bytes-each-time"_s, testSameBytesEachTime },
        { "webassembly-image/call-goes-to-copy-of-its-flavor"_s, testCallGoesToCopyOfItsFlavor },
        { "webassembly-image/call-of-unknown-symbol-is-refused"_s, testCallOfUnknownSymbolIsRefused },
        { "webassembly-image/pointer-table-slots"_s, testPointerTableSlots },
        { "webassembly-image/operation-slots"_s, testOperationSlots },
        { "webassembly-image/diagnostic-kinds"_s, testDiagnosticKinds },
        { "webassembly-image/modules-with-nothing-to-run-are-declined"_s, testModulesWithNothingToRunAreDeclined },
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
