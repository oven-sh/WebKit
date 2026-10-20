/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageTestSupport.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCompileOptionWords.h"
#include "AOTDerivedFunctions.h"
#include "AOTIRParser.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageFormat.h"
#include "AOTProgramFile.h"
#include "AOTProgramTableEncoder.h"
#include "AOTRegExpRequests.h"
#include "AOTTextEncoder.h"
#include "AOTWire.h"
#include "CPUFeatureSet.h"
#include "ExecutableAllocator.h"
#include <algorithm>
#include <limits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using Kind = CodeUnit::Kind;

Wire::CompileOptions wireOptions(const TestOptions& testOptions)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    memcpySpan(std::span { options.executableIdentity }, std::span { testIdentity });
    options.loweredLimit = testOptions.loweredLimit;
    options.loweredLimitValue = testOptions.loweredLimitValue;
    options.threadCount = testOptions.threadCount;
    options.layoutSeed = testOptions.layoutSeed;
    options.checks = testOptions.checks;
    options.reports = testOptions.reports;
    options.heatOverride = testOptions.heat;
    options.output = testOptions.output;
    options.admittedCode = testOptions.admittedCode;
    return options;
}

String programText(unsigned numberOfFunctions)
{
    AOT_TEST_CHECK(numberOfFunctions >= 1);
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
        "entry\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (unsigned k = 1; k < numberOfFunctions; ++k) {
        text.append("    v"_s, 2 * k, " = NewClosure function=f"_s, k, "\n"_s);
        text.append("    v"_s, 2 * k + 1, " = Call v1, -, v"_s, 2 * k, "\n"_s);
    }
    text.append("    Return\n}\n"_s);
    for (unsigned k = 1; k < numberOfFunctions; ++k)
        text.append("function f"_s, k, " name=\"f"_s, k, "\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append(runtimeFunctionsText(numberOfFunctions));
    return text.toString();
}

String runtimeFunctionsText(unsigned libraryFunction)
{
    return Typed::standInLibraryText(libraryFunction);
}

AssemblerPolicy testAssemblerPolicy()
{
    return AssemblerPolicy::forImage(CPUFeatureSet::host());
}

static CodeTemplates createTestTemplates()
{
    std::expected<CodeTemplates, LinkFailure> templates = CodeTemplates::create(testAssemblerPolicy(), nullptr);
    AOT_TEST_CHECK(templates);
    return WTF::move(*templates);
}

const CodeTemplates& testTemplates()
{
    static const NeverDestroyed<CodeTemplates> templates { createTestTemplates() };
    return templates.get();
}

AlignedBytes::AlignedBytes(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return;
    m_bytes = { static_cast<uint8_t*>(fastAlignedMalloc(imagePageSize, bytes.size())), bytes.size() };
    memcpySpan(m_bytes, bytes);
}

AlignedBytes::AlignedBytes(AlignedBytes&& other)
    : m_bytes(std::exchange(other.m_bytes, { }))
{
}

AlignedBytes& AlignedBytes::operator=(AlignedBytes&& other)
{
    std::swap(m_bytes, other.m_bytes);
    return *this;
}

AlignedBytes::~AlignedBytes()
{
    if (!m_bytes.empty())
        fastFree(m_bytes.data());
}

uint32_t TestImage::indexOf(uint32_t function) const
{
    std::optional<uint32_t> index = image->imageFunctionOf(FunctionID(function));
    AOT_TEST_CHECK(index);
    return *index;
}

TestBuild::TestBuild(unsigned numberOfFunctions, const TestOptions& options)
    : TestBuild(programText(numberOfFunctions), options)
{
}

TestBuild::TestBuild(const String& text, const TestOptions& options)
    : m_context(makeUnique<BuildContext>(wireOptions(options)))
{
    AOT_TEST_CHECK(m_context->threadCount() == options.threadCount);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(text, Typed::IRForm::AsRead, *m_context);
    AOT_TEST_CHECK(program);
    m_pipeline = Typed::runPipelineUntil(PassID::CheckPlacement, WTF::move(program), *m_context);
    AOT_TEST_CHECK(m_pipeline);
}

TestBuild::~TestBuild()
{
    finishDiagnostics();
    m_context->destroyLater(WTF::move(m_pipeline));
}

FunctionID TestBuild::genericEntrypointOf(uint32_t function) const
{
    FunctionID entrypoint = m_pipeline->derivedFunctions->find(Typed::derivedFunctionKey(Typed::FunctionRole::GenericEntrypoint, FunctionID(function), 0));
    AOT_TEST_CHECK(entrypoint);
    return entrypoint;
}

const CodeGenerationPlan& TestBuild::plan()
{
    if (!m_plan)
        m_plan.emplace(prepareCodeGeneration(*m_pipeline, context()));
    return *m_plan;
}

Vector<uint8_t> TestBuild::encodeTables()
{
    Typed::EncodedProgramTables tables = Typed::encodeProgramTables({ *m_pipeline, plan().instanceSizes, plan().stringConstants }, context());
    m_namedStrings = WTF::move(tables.namedStrings);
    return WTF::move(tables.bytes);
}

TestImage TestBuild::build(Vector<CompiledCode>&& code)
{
    return build(WTF::move(code), { }, { });
}

TestImage TestBuild::build(Vector<CompiledCode>&& code, CompiledRegExps&& regExps, WebAssemblyImageInputs&& webAssembly, FFIStubs&& ffiStubs)
{
    Vector<uint8_t> tables = encodeTables();
    TestImage result {
        buildImage(ImageInputs {
            .pipeline = *m_pipeline,
            .stubCatalog = stubCatalog(),
            .instanceSizes = plan().instanceSizes,
            .functions = createLinkedFunctions(WTF::move(code), context()),
            .entriesUsedByFunctions = { },
            .programTables = WTF::move(tables),
            .text = encodeText({ program(), plan().uses.strings, context().identifierNumbering(), plan().stringConstants, m_namedStrings }, context()),
            .regExpRequests = *m_pipeline->regExpRequests,
            .regExps = WTF::move(regExps),
            .webAssembly = WTF::move(webAssembly),
            .ffiStubs = WTF::move(ffiStubs),
        }, context()),
        { },
        nullptr,
    };
    if (result.built.image.isEmpty()) {
        AOT_TEST_CHECK(context().diagnostics().hasErrors());
        return result;
    }

    AOT_TEST_CHECK(!context().diagnostics().hasErrors());
    result.bytes = AlignedBytes(result.built.image.span());
    result.built.image.clear();
    std::span<const uint8_t> bytes = std::as_const(result.bytes).span();
    AOT_TEST_CHECK(Image::check(bytes, testIdentity) == Wire::ImageVerdict::Accepted);
    const ImageHeader& header = *reinterpret_cast<const ImageHeader*>(bytes.data());
    result.image = Image::create(bytes, bytes.data() + header.codeOffset);
    return result;
}

void TestBuild::finishDiagnostics()
{
    if (m_isFinished)
        return;
    m_isFinished = true;
    m_context->diagnostics().finish(m_pipeline->program.get());
}

static unsigned countFinished(const Diagnostics& diagnostics, InternalErrorKind kind)
{
    return static_cast<unsigned>(std::ranges::count(diagnostics.internalErrors(), kind, &InternalError::kind));
}

static unsigned countFinished(const Diagnostics& diagnostics, UserDiagnosticKind kind, LimitID limit)
{
    return static_cast<unsigned>(std::ranges::count_if(diagnostics.userDiagnostics(), [&](const UserDiagnostic& diagnostic) {
        return diagnostic.kind == kind && diagnostic.limit == limit;
    }));
}

unsigned TestBuild::count(InternalErrorKind kind)
{
    finishDiagnostics();
    return countFinished(m_context->diagnostics(), kind);
}

unsigned TestBuild::count(UserDiagnosticKind kind, LimitID limit)
{
    finishDiagnostics();
    return countFinished(m_context->diagnostics(), kind, limit);
}

unsigned count(BuildContext& context, InternalErrorKind kind)
{
    context.diagnostics().finish(nullptr);
    return countFinished(context.diagnostics(), kind);
}

unsigned count(BuildContext& context, UserDiagnosticKind kind, LimitID limit)
{
    context.diagnostics().finish(nullptr);
    return countFinished(context.diagnostics(), kind, limit);
}

LinkedFunctions createLinkedFunctions(Vector<CompiledCode>&& code, BuildContext& context)
{
    LinkedFunctions functions;
    functions.grow(code.size(), context);
    for (size_t i = 0; i < code.size(); ++i)
        functions[i] = WTF::move(code[i]);
    return functions;
}

CodeSketch::CodeSketch(uint32_t function)
{
    m_code.function = FunctionID(function);
    m_code.body.entries.append(CodeEntry { 0, primaryEntry });
    m_code.sites.positions.append(SitePosition { 1, 1, ModuleID(0u), 0, 0 });
}

CodeSketch& CodeSketch::withFrame(unsigned frameSizeInBytes)
{
    m_hasFrame = true;
    m_code.frame.frameSizeInBytes = frameSizeInBytes;
    return *this;
}

static Vector<uint8_t> filledBytes(uint8_t value, size_t count)
{
    return Vector<uint8_t>(FillWith { }, count, value);
}

uint32_t CodeSketch::append(Kind kind, std::span<const uint8_t> bytes)
{
    AOT_TEST_CHECK(bytes.size() <= std::numeric_limits<uint16_t>::max());
    uint32_t offset = size();
    m_code.body.bytes.append(bytes);
    m_code.body.units.append(CodeUnit { offset, static_cast<uint16_t>(bytes.size()), kind });
    return offset;
}

uint32_t CodeSketch::appendCall(CodeSymbol symbol, OptionalIndex site)
{
    uint32_t offset = append(Kind::Call, testTemplates().nearCall.span());
    m_code.body.relocations.append(Relocation { offset, RelocationKind::Call, symbol.space, symbol.entry, symbol.index });
    m_code.body.returnSites.append(ReturnSite { offset + static_cast<uint32_t>(sizeOfNearCall), site });
    return offset;
}

uint32_t CodeSketch::appendJump(CodeSymbol symbol)
{
    uint32_t offset = append(Kind::Pinned, testTemplates().nearJump.span());
    m_code.body.relocations.append(Relocation { offset, RelocationKind::Jump, symbol.space, symbol.entry, symbol.index });
    return offset;
}

uint32_t CodeSketch::plain(uint32_t word, unsigned count)
{
    uint32_t offset = size();
    for (unsigned i = 0; i < count; ++i)
        append(Kind::Plain, asByteSpan(word));
    return offset;
}

uint32_t CodeSketch::pinned(uint32_t word, unsigned count)
{
    Vector<uint8_t> bytes;
    for (unsigned i = 0; i < count; ++i)
        bytes.append(asByteSpan(word));
    return append(Kind::Pinned, bytes.span());
}

uint32_t CodeSketch::pinnedBytes(unsigned count)
{
#if CPU(ARM64)
    Vector<uint8_t> bytes = filledBytes(0, WTF::roundUpToMultipleOf<sizeof(uint32_t)>(count));
    fillWithNops(bytes.mutableSpan());
    return append(Kind::Pinned, bytes.span());
#else
    return append(Kind::Pinned, filledBytes(0x90, count).span());
#endif
}

uint32_t CodeSketch::callStub(StubKey key, OptionalIndex site)
{
    return appendCall({ SymbolSpace::Stub, stubKeyIndex(key) }, site);
}

uint32_t CodeSketch::jumpToStub(StubKey key)
{
    return appendJump({ SymbolSpace::Stub, stubKeyIndex(key) });
}

uint32_t CodeSketch::callThunk(const ArgumentMoves& moves, OptionalIndex site)
{
    uint32_t index = static_cast<uint32_t>(m_code.body.argumentMoves.size());
    m_code.body.argumentMoves.append(moves);
    return appendCall({ SymbolSpace::ArgumentThunk, index }, site);
}

uint32_t CodeSketch::callFunction(uint32_t callee, OptionalIndex site, uint16_t entry)
{
    return appendCall({ SymbolSpace::Function, calleeIndex(callee), entry }, site);
}

uint32_t CodeSketch::jumpToFunction(uint32_t callee, uint16_t entry)
{
    return appendJump({ SymbolSpace::Function, calleeIndex(callee), entry });
}

uint32_t CodeSketch::end()
{
    Vector<uint8_t> bytes;
    while (bytes.size() < sizeof(uint32_t))
        bytes.appendVector(testTemplates().breakpoint);
    return append(Kind::End, bytes.span());
}

uint32_t CodeSketch::jumpTo(uint32_t target)
{
    uint32_t offset = append(Kind::Pinned, testTemplates().nearJump.span());
    m_code.body.localSites.append(LinkedSite { offset, target, RelocationKind::Jump });
    return offset;
}

uint32_t CodeSketch::padding(unsigned bytes)
{
    m_code.body.paddingStart = size();
    m_hasPadding = true;
    return append(Kind::Pinned, filledBytes(0, bytes).span());
}

template<typename T>
static uint32_t findOrAppend(Vector<T>& list, T element)
{
    size_t index = list.find(element);
    if (index != notFound)
        return static_cast<uint32_t>(index);
    list.append(element);
    return static_cast<uint32_t>(list.size() - 1);
}

uint32_t CodeSketch::calleeIndex(uint32_t callee)
{
    return findOrAppend(m_code.body.callees, FunctionID(callee));
}

uint32_t CodeSketch::stubKeyIndex(StubKey key)
{
    return findOrAppend(m_code.body.stubKeys, key);
}

OptionalIndex CodeSketch::addSite(unsigned inlineFrame, uint32_t line, uint32_t column)
{
    uint32_t number = static_cast<uint32_t>(m_code.sites.positions.size());
    m_code.sites.positions.append(SitePosition { line, column, ModuleID(0u), 0, inlineFrame });
    return { number };
}

void CodeSketch::finishFrame()
{
    if (m_hasFrame && m_code.frame.frameRanges.isEmpty())
        m_code.frame.frameRanges.append(CodeRange { 0, m_code.body.paddingStart });
}

static bool endsWithoutReturning(const CompiledCode& code)
{
    if (code.body.units.last().kind == Kind::End)
        return true;
    if (code.body.relocations.isEmpty())
        return false;
    const Relocation& last = code.body.relocations.last();
    if (last.offset + MacroAssembler::relocationSize(last.kind) != code.body.bytes.size())
        return false;
    if (last.kind == RelocationKind::Jump)
        return true;
    return last.kind == RelocationKind::Call && code.frame.prologueChecks.containsIf([&](const PrologueCheck& check) {
        return check.callOffset == last.offset;
    });
}

CompiledCode CodeSketch::take()
{
    AOT_TEST_CHECK(!m_code.body.bytes.isEmpty());
    if (!m_hasPadding) {
        m_code.body.paddingStart = size();
        AOT_TEST_CHECK(endsWithoutReturning(m_code));
    }
    finishFrame();
    AOT_TEST_CHECK(verifyCodeUnits(m_code) == CodeUnitsError::None);
    return WTF::move(m_code);
}

CompiledCode CodeSketch::takeUnchecked()
{
    if (!m_hasPadding)
        m_code.body.paddingStart = size();
    finishFrame();
    return WTF::move(m_code);
}

ExecutableCopy::ExecutableCopy(std::span<const uint8_t> bytes)
    : m_handle(ExecutableAllocator::singleton().allocate(bytes.size() + 4 * KB, JITCompilationMustSucceed))
{
    uint8_t* code = const_cast<uint8_t*>(start());
    performJITMemcpy<jitMemcpyRepatch>(code, bytes.data(), bytes.size());
    MacroAssembler::cacheFlush(code, bytes.size());
}

const uint8_t* ExecutableCopy::start() const
{
    return roundUpToMultipleOf<4 * KB>(m_handle->start().untaggedPtr<uint8_t*>());
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
