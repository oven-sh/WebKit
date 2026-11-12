/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeRole.h"
#include "AOTCodeTemplates.h"
#include "AOTIRProgram.h"
#include "AOTImage.h"
#include "AOTImageBuilder.h"
#include "AOTNativeTest.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStages.h"
#include "AOTStubCalls.h"
#include "AOTStubCatalog.h"
#include "AOTTestHarness.h"
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
#include <wtf/Noncopyable.h>
#include <wtf/PtrTag.h>
#include <wtf/RefPtr.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using Typed::FunctionID;

inline constexpr std::array<uint8_t, 16> testIdentity { 'a', 'o', 't', '-', 'i', 'm', 'a', 'g', 'e', '-', 't', 'e', 's', 't', 's', 1 };

struct TestOptions {
    unsigned threadCount { 1 };
    const HostThreadPool* hostPool { nullptr };
    Wire::CheckSet checks { };
    Wire::OutputOptions output { };
    Wire::AdmittedCode admittedCode { };
    Wire::ExcludedCode excludedCode { Wire::ExcludedCode::CodeOutsideImage };
};

Wire::CompileOptions wireOptions(const TestOptions& = { });
String programText(unsigned numberOfFunctions);
String runtimeFunctionsText(unsigned libraryFunction);

AssemblerPolicy testAssemblerPolicy();
const CodeTemplates& testTemplates();

inline CodeUnit bodyUnitOf(uint32_t function) { return { FunctionID(function), Typed::CodeRole::Body }; }

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
    Typed::StagedBuild& staged() { return *m_staged; }
    const PreparedProgram& prepared() const { return *m_prepared; }
    Typed::Program& program() { return *m_prepared->program; }
    const Typed::Program& program() const { return *m_prepared->program; }
    const Typed::Facts& facts() const { return *m_prepared->facts; }
    const Typed::Plan& plan() const { return *m_prepared->plan; }
    const StubCatalog& stubCatalog() const { return *m_prepared->stubCatalog; }
    CodeUnit genericEntrypointOf(uint32_t function) const;

    Vector<uint8_t> encodeTables();
    TestImage build(Vector<CompiledCode>&&, std::unique_ptr<PreparedFunctionSources> = nullptr);

    unsigned count(InternalErrorKind);
    unsigned count(UserDiagnosticKind, LimitID = LimitID::None);
    uint64_t statistic(StatisticID id) { return m_context->statistics().values()[static_cast<unsigned>(id)]; }

private:
    void finishDiagnostics();

    std::unique_ptr<BuildContext> m_context;
    std::optional<Typed::StagedBuild> m_staged;
    const PreparedProgram* m_prepared { nullptr };
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
    explicit CodeSketch(CodeUnit);

    CodeSketch& withFrame(unsigned frameSizeInBytes = 16);

    uint32_t plain(uint32_t word, unsigned count = 1);
    uint32_t pinned(uint32_t word, unsigned count = 1);
    uint32_t pinnedBytes(unsigned count);
    uint32_t noOperationBytes(unsigned count) { return pinnedBytes(count); }
    uint32_t callStub(StubKey, OptionalIndex site = { });
    uint32_t jumpToStub(StubKey);
    uint32_t callThunk(const ArgumentMoves&, OptionalIndex site = { });
    uint32_t callThunkAgain(uint32_t index, OptionalIndex site = { });
    uint32_t callFunction(CodeUnit callee, OptionalIndex site = { }, uint16_t entry = primaryEntry);
    uint32_t callFunction(uint32_t callee, OptionalIndex site = { }, uint16_t entry = primaryEntry) { return callFunction(bodyUnitOf(callee), site, entry); }
    uint32_t jumpToFunction(CodeUnit callee, uint16_t entry = primaryEntry);
    uint32_t jumpToFunction(uint32_t callee, uint16_t entry = primaryEntry) { return jumpToFunction(bodyUnitOf(callee), entry); }
    uint32_t end();
    uint32_t jumpTo(uint32_t target);

    uint32_t calleeIndex(CodeUnit callee);
    uint32_t calleeIndex(uint32_t callee) { return calleeIndex(bodyUnitOf(callee)); }
    uint32_t stubKeyIndex(StubKey);
    OptionalIndex addSite(unsigned inlineFrame = 0, uint32_t line = 1, uint32_t column = 1);


    CompiledCode& code() { return m_code; }
    uint32_t size() const { return static_cast<uint32_t>(m_code.body.bytes.size()); }

    CompiledCode take();
    CompiledCode takeUnchecked();

private:
    uint32_t append(std::span<const uint8_t>);
    uint32_t appendCall(CodeSymbol, OptionalIndex site);
    uint32_t appendJump(CodeSymbol);
    void finishFrame();

    CompiledCode m_code;
    Vector<ArgumentMoves> m_argumentMoves;
    uint32_t m_sizeAfterLastEnd { 0 };
    bool m_hasFrame { false };
};

struct Assembly {
    CCallHelpers& jit;
    RelocationRecorder& relocations;
    StubCalls& calls;
    MentionList& mentions;
};

template<typename Emit, typename AfterLink>
CompiledCode assemble(uint32_t function, const StubCatalog& catalog, const Emit& emit, const AfterLink& afterLink)
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, testAssemblerPolicy());
    StubCalls calls(catalog, relocations);
    MentionList mentions;
    Assembly assembly { jit, relocations, calls, mentions };
    emit(assembly);

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "test"_s);
    auto linked = relocations.link(buffer, nullptr);
    AOT_TEST_CHECK(linked);

    CompiledCode code = CodeSketch(function).takeUnchecked();
    code.body.relocations = WTF::move(*linked);
    calls.describeCalls(code.body.relocations.span(), code.body);
    mentions.moveTo(code.body);
    code.body.bytes.append(buffer.bytes());

    afterLink(buffer.linkBuffer(), code);
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

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
