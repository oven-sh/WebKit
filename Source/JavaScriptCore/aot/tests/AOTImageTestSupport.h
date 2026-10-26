/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeGenerationDriver.h"
#include "AOTCodeTemplates.h"
#include "AOTCodeUnits.h"
#include "AOTIRProgram.h"
#include "AOTImage.h"
#include "AOTImageBuilder.h"
#include "AOTNativeTest.h"
#include "AOTPipeline.h"
#include "AOTRelocation.h"
#include "AOTStubCalls.h"
#include "AOTStubCatalog.h"
#include "AOTVector.h"
#include "AssemblerPolicy.h"
#include "CCallHelpers.h"
#include "ExecutableAllocator.h"
#include "ExecutableMemoryHandle.h"
#include "LinkBuffer.h"
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/PtrTag.h>
#include <wtf/RefPtr.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

// The offsets are numbered anew behind the change: packed units have none.
void changeUnits(CodeBody&, NOESCAPE const Invocable<void(Vector<CodeUnit>&)> auto& change);

using Typed::FunctionID;

inline constexpr uint64_t loweredReach = 128 * KB;
inline constexpr uint64_t loweredSharedCodeReach = 1 * MB;
inline constexpr std::array<uint8_t, 16> testIdentity { 'a', 'o', 't', '-', 'i', 'm', 'a', 'g', 'e', '-', 't', 'e', 's', 't', 's', 1 };

struct TestOptions {
    LimitID loweredLimit { LimitID::None };
    uint64_t loweredLimitValue { 0 };
    unsigned threadCount { 1 };
    uint32_t layoutSeed { 0 };
    Wire::CheckSet checks { };
    Wire::ReportSet reports { };
    Wire::HeatOverride heat { Wire::HeatOverride::None };
    Wire::OutputOptions output { };
    Wire::AdmittedCode admittedCode { };
};

Wire::CompileOptions wireOptions(const TestOptions& = { });
String programText(unsigned numberOfFunctions);
String runtimeFunctionsText(unsigned libraryFunction);

AssemblerPolicy testAssemblerPolicy();
const CodeTemplates& testTemplates();

class AlignedBytes {
public:
    AlignedBytes() = default;
    explicit AlignedBytes(std::span<const uint8_t>);
    AlignedBytes(AlignedBytes&&);
    AlignedBytes& operator=(AlignedBytes&&);
    ~AlignedBytes();

    std::span<uint8_t> span() { return m_bytes; }
    std::span<const uint8_t> span() const { return m_bytes; }

private:
    std::span<uint8_t> m_bytes;
};

struct TestImage {
    uint32_t indexOf(uint32_t function) const;
    const uint8_t* address(uint32_t function, uint32_t offset) const { return image->codeOf(indexOf(function)) + offset; }

    BuiltImage built;
    AlignedBytes bytes;
    std::unique_ptr<Image> image;
};

class TestBuild {
    WTF_MAKE_NONCOPYABLE(TestBuild);
public:
    explicit TestBuild(unsigned numberOfFunctions, const TestOptions& = { });
    TestBuild(const String& text, const TestOptions&);
    ~TestBuild();

    BuildContext& context() { return *m_context; }
    const Typed::PipelineOutput& pipeline() const { return *m_pipeline; }
    const Typed::Program& program() const { return *m_pipeline->program; }
    FunctionID genericEntrypointOf(uint32_t function) const;

    const CodeGenerationPlan& plan();
    const StubCatalog& stubCatalog() { return *plan().stubCatalog; }

    Vector<uint8_t> encodeTables();
    TestImage build(Vector<CompiledCode>&&);
    TestImage build(Vector<CompiledCode>&&, CompiledRegExps&&, WebAssemblyImageInputs&&, FFIStubs&& = { });

    unsigned count(InternalErrorKind);
    unsigned count(UserDiagnosticKind, LimitID = LimitID::None);
    uint64_t statistic(StatisticID id) { return m_context->statistics().values()[static_cast<unsigned>(id)]; }

private:
    void finishDiagnostics();

    std::unique_ptr<BuildContext> m_context;
    std::unique_ptr<Typed::PipelineOutput> m_pipeline;
    std::optional<CodeGenerationPlan> m_plan;
    BitVector m_namedStrings;
    BitVector m_stringsOnlyShown;
    bool m_isFinished { false };
};

unsigned count(BuildContext&, InternalErrorKind);
unsigned count(BuildContext&, UserDiagnosticKind, LimitID = LimitID::None);
LinkedFunctions createLinkedFunctions(Vector<CompiledCode>&&, BuildContext&);

template<typename... Code>
Vector<CompiledCode> bodies(Code... code)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(sizeof...(code));
    (result.append(WTF::move(code)), ...);
    return result;
}

class CodeSketch {
public:
    explicit CodeSketch(uint32_t function);

    CodeSketch& withFrame(unsigned frameSizeInBytes = 16);

    uint32_t plain(uint32_t word, unsigned count = 1);
    uint32_t pinned(uint32_t word, unsigned count = 1);
    uint32_t pinnedBytes(unsigned count);
    uint32_t callStub(StubKey, OptionalIndex site = { });
    uint32_t jumpToStub(StubKey);
    uint32_t callThunk(const ArgumentMoves&, OptionalIndex site = { });
    uint32_t callFunction(uint32_t callee, OptionalIndex site = { }, uint16_t entry = primaryEntry);
    uint32_t jumpToFunction(uint32_t callee, uint16_t entry = primaryEntry);
    uint32_t end();
    uint32_t jumpTo(uint32_t target);
    uint32_t padding(unsigned bytes);

    uint32_t calleeIndex(uint32_t callee);
    uint32_t stubKeyIndex(StubKey);
    OptionalIndex addSite(unsigned inlineFrame = 0, uint32_t line = 1, uint32_t column = 1);

    Relocation& lastRelocation() { return m_code.body.relocations.last(); }
    CompiledCode& code() { return m_code; }
    uint32_t size() const { return static_cast<uint32_t>(m_code.body.bytes.size()); }

    CompiledCode take();
    CompiledCode takeUnchecked();

private:
    uint32_t append(CodeUnit::Kind, std::span<const uint8_t>);
    uint32_t appendCall(CodeSymbol, OptionalIndex site);
    uint32_t appendJump(CodeSymbol);
    void finishFrame();

    CompiledCode m_code;
    bool m_hasFrame { false };
    bool m_hasPadding { false };
};

struct Assembly {
    CCallHelpers& jit;
    RelocationRecorder& relocations;
    StubCalls& calls;
    CodeUnitRecorder& units;
};

template<typename Emit, typename AfterLink>
CompiledCode assemble(uint32_t function, const StubCatalog& catalog, const Emit& emit, const AfterLink& afterLink)
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, testAssemblerPolicy());
    CodeUnitRecorder units;
    StubCalls calls(catalog, relocations, units);
    Assembly assembly { jit, relocations, calls, units };
    emit(assembly);
    CCallHelpers::Label end = jit.label();

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "test"_s);
    auto linked = relocations.link(buffer, nullptr);
    AOT_TEST_CHECK(linked);

    CompiledCode code = CodeSketch(function).takeUnchecked();
    code.body.relocations = WTF::move(*linked);
    calls.describeCalls(code.body.relocations.span(), code.body);
    code.body.units = units.link(buffer, code.body.relocations.span());
    code.body.localSites = buffer.linkBuffer().linkedSites();
    code.body.bytes.append(buffer.bytes());
    code.body.paddingStart = buffer.linkBuffer().offsetOf(end);

    afterLink(buffer.linkBuffer(), code);
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
    return code;
}

class ExecutableCopy {
public:
    explicit ExecutableCopy(std::span<const uint8_t>);

    const uint8_t* start() const;

    template<typename Result, typename... Arguments>
    Result call(size_t offset, Arguments... arguments) const
    {
        using Function = Result(SYSV_ABI *)(Arguments...);
        return std::bit_cast<Function>(tagCodePtr<CFunctionPtrTag>(const_cast<uint8_t*>(start() + offset)))(arguments...);
    }

private:
    RefPtr<ExecutableMemoryHandle> m_handle;
};

template<void (*run)()>
void whenExecutable()
{
    if (ExecutableAllocator::singleton().isValid())
        run();
}

void changeUnits(CodeBody& body, NOESCAPE const Invocable<void(Vector<CodeUnit>&)> auto& change)
{
    Vector<CodeUnit> units = body.units.unpack();
    change(units);
    uint32_t offset = 0;
    for (CodeUnit& unit : units) {
        unit.offset = offset;
        offset += unit.length;
    }
    body.units = units;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
