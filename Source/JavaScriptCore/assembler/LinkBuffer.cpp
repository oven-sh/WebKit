/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "LinkBuffer.h"

#if ENABLE(ASSEMBLER)

#include "CodeBlock.h"
#include "Disassembler.h"
#include "GdbJIT.h"
#include "JITCode.h"
#include "Options.h"
#include "PerfLog.h"
#include "WasmCallee.h"
#include "YarrJIT.h"
#include <algorithm>
#include <array>
#include <span>
#include <wtf/ScopedPrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

size_t LinkBuffer::s_profileCummulativeLinkedSizes[LinkBuffer::numberOfProfiles];
size_t LinkBuffer::s_profileCummulativeLinkedCounts[LinkBuffer::numberOfProfiles];

WTF_MAKE_TZONE_ALLOCATED_IMPL(IRDumpDebugInfo);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SourceCodeDumpDebugInfo);
WTF_MAKE_TZONE_ALLOCATED_IMPL(LinkBuffer);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(LinkBuffer::Records);

static const char* LEGACY_NODELETE profileName(LinkBuffer::Profile profile)
{
#define RETURN_LINKBUFFER_PROFILE_NAME(name) case LinkBuffer::Profile::name: return #name;
    switch (profile) {
        FOR_EACH_LINKBUFFER_PROFILE(RETURN_LINKBUFFER_PROFILE_NAME)
    }
    RELEASE_ASSERT_NOT_REACHED();
#undef RETURN_LINKBUFFER_PROFILE_NAME
    return "";
}

LinkBuffer::CodeRef<LinkBufferPtrTag> LinkBuffer::finalizeCodeWithoutDisassemblyImpl(ASCIILiteral simpleName)
{
    performFinalization();

    ASSERT(m_didAllocate);
    CodeRef<LinkBufferPtrTag> codeRef(m_executableMemory ? CodeRef<LinkBufferPtrTag>(*m_executableMemory) : CodeRef<LinkBufferPtrTag>::createSelfManagedCodeRef(m_code));

    logJITCodeForJITDump(codeRef, simpleName);

    return codeRef;
}

void LinkBuffer::logJITCodeForJITDump(CodeRef<LinkBufferPtrTag>& codeRef, ASCIILiteral simpleName)
{
    if (!Options::useJITDump() && !Options::useGdbJITInfo()) [[likely]]
        return;
    if (m_isRewriting)
        return;

    auto dumpSimpleName = [&](StringPrintStream& out, ASCIILiteral simpleName) {
        if (simpleName.isNull())
            out.print("unspecified");
        else
            out.print(simpleName);
    };

    StringPrintStream out;
    out.print("JSC-", profileName(m_profile), ": ");
    switch (m_profile) {
    case Profile::Baseline:
    case Profile::DFG:
    case Profile::FTL: {
        if (m_ownerUID)
            static_cast<CodeBlock*>(m_ownerUID)->dumpSimpleName(out);
        else
            dumpSimpleName(out, simpleName);
        break;
    }
#if ENABLE(WEBASSEMBLY)
    case Profile::WasmOMG:
    case Profile::WasmBBQ: {
        if (m_ownerUID)
            uncheckedDowncast<Wasm::Callee>(reinterpret_cast<NativeCallee*>(m_ownerUID))->dumpSimpleName(out);
        else
            dumpSimpleName(out, simpleName);
        break;
    }
#endif
#if ENABLE(YARR_JIT)
    case Profile::YarrJIT: {
        if (m_ownerUID)
            static_cast<Yarr::YarrCodeBlock*>(m_ownerUID)->dumpSimpleName(out);
        else
            dumpSimpleName(out, simpleName);
        break;
    }
#endif
    default:
        dumpSimpleName(out, simpleName);
        break;
    }
    auto finalName = out.toUTF8CString();

    if (Options::useGdbJITInfo()) [[unlikely]]
        GdbJIT::log(finalName, codeRef);

    if (Options::useJITDump()) [[unlikely]]
        PerfLog::log(finalName, codeRef, WTF::move(m_irDumpDebugInfo), WTF::move(m_sourceCodeDebugInfo));
}

LinkBuffer::CodeRef<LinkBufferPtrTag> LinkBuffer::finalizeCodeWithDisassemblyImpl(bool dumpDisassembly, ASCIILiteral simpleName, const char* format, ...)
{
    CodeRef<LinkBufferPtrTag> result = finalizeCodeWithoutDisassemblyImpl(simpleName);

    if (!dumpDisassembly && !Options::logJIT())
        return result;

    bool justDumpingHeader = !dumpDisassembly || m_alreadyDisassembled;

    ScopedPrintStream out;
    out.printf("Generated JIT code for ");
    va_list argList;
    va_start(argList, format);

    if (m_isThunk) {
        va_list preflightArgs;
        va_copy(preflightArgs, argList);
        size_t stringLength = vsnprintf(nullptr, 0, format, preflightArgs);
        va_end(preflightArgs);

        constexpr auto prefix = "thunk: "_s;
        std::span<char> buffer;
        size_t length = stringLength + prefix.length() + 1;
        auto label = ASCIICString::newUninitialized(length, buffer);
        memcpySpan(buffer, prefix.span8());
        vsnprintf(buffer.subspan(prefix.length()).data(), stringLength + 1, format, argList);
        out.printf("%s", buffer.data());

        registerLabel(result.code().untaggedPtr(), WTF::move(label));
    } else
        out.vprintf(format, argList);

    va_end(argList);

    uint8_t* executableAddress = result.code().untaggedPtr<uint8_t*>();
    out.printf(": [%p, %p) %zu bytes%s\n", executableAddress, executableAddress + result.size(), result.size(), justDumpingHeader ? "." : ":");

    if (justDumpingHeader) {
        if (!Options::logJIT())
            out.reset();
        return result;
    }
    
    void* codeStart = entrypoint<DisassemblyPtrTag>().untaggedPtr();
    void* codeEnd = std::bit_cast<uint8_t*>(codeStart) + size();

    disassemble(result.retaggedCode<DisassemblyPtrTag>(), m_size, codeStart, codeEnd, "    ", out);
    return result;
}

#if ENABLE(BRANCH_COMPACTION)

class BranchCompactionLinkBuffer;

using ThreadSpecificBranchCompactionLinkBuffer = ThreadSpecific<BranchCompactionLinkBuffer, WTF::CanBeGCThread::True>;

static ThreadSpecificBranchCompactionLinkBuffer* threadSpecificBranchCompactionLinkBufferPtr;

static ThreadSpecificBranchCompactionLinkBuffer& threadSpecificBranchCompactionLinkBuffer()
{
static std::once_flag flag;
    std::call_once(
        flag,
        [] () {
            threadSpecificBranchCompactionLinkBufferPtr = new ThreadSpecificBranchCompactionLinkBuffer();
        });
    return *threadSpecificBranchCompactionLinkBufferPtr;
}

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER_AND_EXPORT(BranchCompactionLinkBuffer, WTF_INTERNAL);
DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(BranchCompactionLinkBuffer);

class BranchCompactionLinkBuffer {
    WTF_MAKE_NONCOPYABLE(BranchCompactionLinkBuffer);
public:
    BranchCompactionLinkBuffer()
    {
    }

    BranchCompactionLinkBuffer(size_t size, uint8_t* userBuffer = nullptr)
    {
        if (userBuffer) {
            m_data = userBuffer;
            m_size = size;
            m_bufferProvided = true;
            return;
        }

        auto& threadSpecific = threadSpecificBranchCompactionLinkBuffer();

        if (threadSpecific->size() >= size)
            takeBufferIfLarger(*threadSpecific);
        else {
            m_size = size;
            m_data = static_cast<uint8_t*>(BranchCompactionLinkBufferMalloc::malloc(size));
        }
    }

    ~BranchCompactionLinkBuffer()
    {
        if (m_bufferProvided)
            return;

        size_t cacheLimit = Options::maximumCachedAssemblerBufferSize();
        if (!cacheLimit || m_size <= cacheLimit) {
            auto& threadSpecific = threadSpecificBranchCompactionLinkBuffer();
            threadSpecific->takeBufferIfLarger(*this);
        }

        if (m_data)
            BranchCompactionLinkBufferMalloc::free(m_data);
    }

    uint8_t* LEGACY_NODELETE data()
    {
        return m_data;
    }

private:
    void takeBufferIfLarger(BranchCompactionLinkBuffer& other)
    {
        if (size() >= other.size())
            return;

        if (m_data)
            BranchCompactionLinkBufferMalloc::free(m_data);

        m_data = other.m_data;
        m_size = other.m_size;

        other.m_data = nullptr;
        other.m_size = 0;
    }

    size_t size()
    {
        return m_size;
    }

    uint8_t* m_data { nullptr };
    size_t m_size { 0 };
    bool m_bufferProvided { false };
};

static ALWAYS_INLINE void recordLinkOffsets(AssemblerData& assemblerData, int32_t regionStart, int32_t regionEnd, int32_t offset)
{
#if OS(DARWIN)
    memset_pattern4(std::bit_cast<uint8_t*>(assemblerData.buffer()) + regionStart, &offset, regionEnd - regionStart);
#else
    int32_t ptr = regionStart / sizeof(int32_t);
    const int32_t end = regionEnd / sizeof(int32_t);
    int32_t* offsets = reinterpret_cast_ptr<int32_t*>(assemblerData.buffer());
    while (ptr < end)
        offsets[ptr++] = offset;
#endif
}

template <typename InstructionType>
void LinkBuffer::copyCompactAndLinkCode(MacroAssembler& macroAssembler, JITCompilationEffort effort)
{
    allocate(macroAssembler, effort);
    const size_t initialSize = macroAssembler.m_assembler.codeSize();
    if (didFailToAllocate())
        return;

    auto& jumpsToLink = macroAssembler.jumpsToLink();
    m_assemblerStorage = macroAssembler.m_assembler.buffer().releaseAssemblerData();
    uint8_t* inData = std::bit_cast<uint8_t*>(m_assemblerStorage.buffer());
#if ENABLE(JIT_SIGN_ASSEMBLER_BUFFER)
    ARM64EHash<ShouldSign::No> verifyUncompactedHash;
    m_assemblerHashesStorage = macroAssembler.m_assembler.buffer().releaseAssemblerHashes();
    uint32_t* inHashes = std::bit_cast<uint32_t*>(m_assemblerHashesStorage.buffer());
#endif

    uint8_t* codeOutData = m_code.dataLocation<uint8_t*>();
// It is important not to spill this to the stack, so we don't make a local.
#define shouldCopyDirectlyToJITRegion (!m_shouldPerformBranchCompaction || g_jscConfig.useFastJITPermissions)

    BranchCompactionLinkBuffer outBuffer(m_size, shouldCopyDirectlyToJITRegion ? codeOutData : 0);
    uint8_t* outData = outBuffer.data();

#if CPU(ARM64)
    RELEASE_ASSERT(roundUpToMultipleOf<sizeof(unsigned)>(outData) == outData);
    RELEASE_ASSERT(roundUpToMultipleOf<sizeof(unsigned)>(codeOutData) == codeOutData);
#endif

    int readPtr = 0;
    int writePtr = 0;
    unsigned jumpCount = jumpsToLink.size();

    auto read = [&](const InstructionType* ptr) -> InstructionType {
        InstructionType value = *ptr;
#if ENABLE(JIT_SIGN_ASSEMBLER_BUFFER)
        unsigned index = (std::bit_cast<uint8_t*>(ptr) - inData) / 4;
        uint32_t hash = verifyUncompactedHash.update(value, index);
        RELEASE_ASSERT(inHashes[index] == hash);
#endif
        return value;
    };

    if (g_jscConfig.useFastJITPermissions)
        threadSelfRestrict<MemoryRestriction::kRwxToRw>();
#if ENABLE(MPROTECT_RX_TO_RWX)
    ExecutableAllocator::singleton().startWriting(outData, initialSize);
#endif

    if (m_shouldPerformBranchCompaction) {
        for (unsigned i = 0; i < jumpCount; ++i) {
            auto& linkRecord = jumpsToLink[i];
            int offset = readPtr - writePtr;
            ASSERT(!(offset & 1));

            // Copy the instructions from the last jump to the current one.
            size_t regionSize = linkRecord.from() - readPtr;
            InstructionType* copySource = reinterpret_cast_ptr<InstructionType*>(inData + readPtr);
            InstructionType* copyEnd = reinterpret_cast_ptr<InstructionType*>(inData + readPtr + regionSize);
            InstructionType* copyDst = reinterpret_cast_ptr<InstructionType*>(outData + writePtr);
            ASSERT(!(regionSize % 2));
            ASSERT(!(readPtr % 2));
            ASSERT(!(writePtr % 2));
            while (copySource != copyEnd) {
                InstructionType insn = read(copySource++);
                *copyDst++ = insn;
            }
            recordLinkOffsets(m_assemblerStorage, readPtr, linkRecord.from(), offset);
            readPtr += regionSize;
            writePtr += regionSize;

            // Calculate absolute address of the jump target, in the case of backwards
            // branches we need to be precise, forward branches we are pessimistic
            const uint8_t* target;
            const intptr_t to = linkRecord.to(&macroAssembler.m_assembler);
            if (linkRecord.isThunk())
                target = std::bit_cast<uint8_t*>(to);
            else if (to >= linkRecord.from())
                target = codeOutData + to - offset; // Compensate for what we have collapsed so far
            else
                target = codeOutData + to - executableOffsetFor(to);

            JumpLinkType jumpLinkType = MacroAssembler::computeJumpType(linkRecord, codeOutData + writePtr, target);
            // Compact branch if we can...
            if (MacroAssembler::canCompact(linkRecord.type())) {
                // Step back in the write stream
                int32_t delta = MacroAssembler::jumpSizeDelta(linkRecord.type(), jumpLinkType);
                if (delta) {
                    writePtr -= delta;
                    recordLinkOffsets(m_assemblerStorage, linkRecord.from() - delta, readPtr, readPtr - writePtr);
                }
            }
            linkRecord.setFrom(&macroAssembler.m_assembler, writePtr);
        }
    } else {
        if (ASSERT_ENABLED) {
            for (unsigned i = 0; i < jumpCount; ++i)
                ASSERT(!MacroAssembler::canCompact(jumpsToLink[i].type()));
        }
    }

    // Copy everything after the last jump
    {
        InstructionType* dst = std::bit_cast<InstructionType*>(outData + writePtr);
        InstructionType* src = std::bit_cast<InstructionType*>(inData + readPtr);
        size_t bytes = initialSize - readPtr;

        RELEASE_ASSERT(!(std::bit_cast<uintptr_t>(dst) % sizeof(InstructionType)));
        RELEASE_ASSERT(!(std::bit_cast<uintptr_t>(src) % sizeof(InstructionType)));
        RELEASE_ASSERT(!(bytes % sizeof(InstructionType)));

        for (size_t i = 0; i < bytes; i += sizeof(InstructionType)) {
            InstructionType insn = read(src++);
            *dst++ = insn;
        }
    }

    recordLinkOffsets(m_assemblerStorage, readPtr, initialSize, readPtr - writePtr);

    for (unsigned i = 0; i < jumpCount; ++i) {
        auto& linkRecord = jumpsToLink[i];
        uint8_t* location = codeOutData + linkRecord.from();
        const intptr_t to = linkRecord.to(&macroAssembler.m_assembler);
        uint8_t* target = nullptr;
        if (linkRecord.isThunk())
            target = std::bit_cast<uint8_t*>(to);
        else
            target = codeOutData + to - executableOffsetFor(to);
        BranchEncoding encoding;
        if (shouldCopyDirectlyToJITRegion)
            encoding = MacroAssembler::link<memcpyRepatch>(linkRecord, outData + linkRecord.from(), location, target);
        else
            encoding = MacroAssembler::link<jitMemcpyRepatch>(linkRecord, outData + linkRecord.from(), location, target);
        if (m_records) [[unlikely]]
            didLinkRecord(linkRecord, to, encoding);
    }

    size_t compactSize = writePtr + initialSize - readPtr;
    if (!m_executableMemory) {
        size_t nopSizeInBytes = initialSize - compactSize;

        if (shouldCopyDirectlyToJITRegion)
            Assembler::fillNops<memcpyRepatch>(outData + compactSize, nopSizeInBytes);
        else
            Assembler::fillNops<jitMemcpyRepatch>(outData + compactSize, nopSizeInBytes);
    }
    if (g_jscConfig.useFastJITPermissions)
        threadSelfRestrict<MemoryRestriction::kRwxToRx>();

    if (m_executableMemory) {
        m_size = compactSize;
        m_executableMemory->shrink(m_size);
    }

#undef shouldCopyDirectlyToJITRegion

#if ENABLE(CODE_GENERATION)
    if (g_jscConfig.useFastJITPermissions) {
        ASSERT(codeOutData == outData);
        if (Options::dumpJITMemoryPath()) [[unlikely]]
            dumpJITMemory(outData, outData, m_size);
    } else {
        ASSERT(codeOutData != outData);
        performJITMemcpy<jitMemcpyRepatch>(codeOutData, outData, m_size);
    }
#else
    ASSERT(codeOutData != outData);
    performJITMemcpy<jitMemcpyRepatch>(codeOutData, outData, m_size);
#endif

#if ENABLE(MPROTECT_RX_TO_RWX)
    ExecutableAllocator::singleton().finishWriting(outData, initialSize);
#endif

    jumpsToLink.clear();

#if DUMP_LINK_STATISTICS
    dumpLinkStatistics(codeOutData, initialSize, m_size);
#endif
#if DUMP_CODE
    dumpCode(codeOutData, m_size);
#endif
}
#endif // ENABLE(BRANCH_COMPACTION)


void LinkBuffer::linkCode(MacroAssembler& macroAssembler, JITCompilationEffort effort)
{
    m_policy = macroAssembler.policy();
    if (m_policy.needsLinkRecords()) [[unlikely]] {
#if CPU(X86_64) || CPU(ARM64)
        m_records.reset(new Records);
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }

    // Ensure that the end of the last invalidation point does not extend beyond the end of the buffer.
    macroAssembler.label();

#if ENABLE(CODE_GENERATION)
#if !ENABLE(BRANCH_COMPACTION)
#if defined(ASSEMBLER_HAS_CONSTANT_POOL) && ASSEMBLER_HAS_CONSTANT_POOL
    macroAssembler.m_assembler.buffer().flushConstantPool(false);
#endif
#if CPU(X86_64)
    if (m_records && (m_policy.recordsLinkedSites || m_policy.shortensJumps)) [[unlikely]] {
        macroAssembler.m_assembler.assignJumpKinds();
        if (m_policy.shortensJumps)
            m_records->jumpShifts = macroAssembler.m_assembler.shortenJumps();
    }
#endif
    allocate(macroAssembler, effort);
    if (!m_didAllocate)
        return;
    ASSERT(m_code);
    AssemblerBuffer& buffer = macroAssembler.m_assembler.buffer();
    void* code = m_code.dataLocation();
#if CPU(ARM64)
    RELEASE_ASSERT(roundUpToMultipleOf<Assembler::instructionSize>(code) == code);
#endif
    performJITMemcpy<jitMemcpyRepatch>(code, buffer.data(), buffer.codeSize());
#elif CPU(ARM64)
    copyCompactAndLinkCode<uint32_t>(macroAssembler, effort);
#endif // !ENABLE(BRANCH_COMPACTION)
#else  // ENABLE(CODE_GENERATION)
UNUSED_PARAM(effort);
#endif // ENABLE(CODE_GENERATION)

    if (m_records) [[unlikely]]
        takeRecords(macroAssembler);

    m_linkTasks = WTF::move(macroAssembler.m_linkTasks);
    m_lateLinkTasks = WTF::move(macroAssembler.m_lateLinkTasks);

    linkComments(macroAssembler);
}

void LinkBuffer::allocate(MacroAssembler& macroAssembler, JITCompilationEffort effort)
{
    size_t initialSize = macroAssembler.m_assembler.codeSize();
    if (m_code) {
        if (initialSize > m_size)
            return;
        
        size_t nopsToFillInBytes = m_size - initialSize;
        macroAssembler.emitNops(nopsToFillInBytes);
        m_didAllocate = true;
        return;
    }
    
    if (macroAssembler.policy().trapsWithIllegalInstruction) {
        macroAssembler.emitNops(roundUpToMultipleOf<jitAllocationGranule>(initialSize) - initialSize);
        initialSize = macroAssembler.m_assembler.codeSize();
    }
    while (initialSize % jitAllocationGranule) {
        macroAssembler.breakpoint();
        initialSize = macroAssembler.m_assembler.codeSize();
    }

#if ENABLE(JIT_SIGN_ASSEMBLER_BUFFER)
    macroAssembler.m_assembler.buffer().arm64eHash().deallocatePinForCurrentThread();
#endif

    m_executableMemory = ExecutableAllocator::singleton().allocate(initialSize, effort);
    if (!m_executableMemory)
        return;
    m_code = CodePtr<LinkBufferPtrTag>(m_executableMemory->start().retaggedPtr<LinkBufferPtrTag>());
    m_size = initialSize;
    m_didAllocate = true;
}

void LinkBuffer::linkComments(MacroAssembler& assembler)
{
    if (!Options::needDisassemblySupport()) [[likely]]
        return;
    if (!m_executableMemory)
        return;
    AssemblyCommentRegistry::CommentMap map;
    for (auto& comment : assembler.m_comments) {
        void* commentLocation = locationOf<DisassemblyPtrTag>(comment.first).dataLocation();
        auto key = reinterpret_cast<uintptr_t>(commentLocation);

        auto& string = comment.second;
        auto addResult = map.ensure(key, [&] {
            return string.isolatedCopy();
        });
        if (!addResult.isNewEntry)
            addResult.iterator->value = makeString(addResult.iterator->value, "\n; "_s, string);
    }

    AssemblyCommentRegistry::singleton().registerCodeRange(m_executableMemory->start().untaggedPtr(), m_executableMemory->end().untaggedPtr(), WTF::move(map));
}

#if CPU(X86_64) || CPU(ARM64)

#if CPU(X86_64)

static_assert(MacroAssembler::relocationSize(RelocationKind::Jump) == X86JumpShortener::longJumpLength(X86JumpKind::Unconditional));
static_assert(MacroAssembler::relocationSize(RelocationKind::ConditionalJump) == X86JumpShortener::longJumpLength(X86JumpKind::Conditional));
static_assert(MacroAssembler::relocationSize(RelocationKind::ShortJump) == X86JumpShortener::shortJumpLength);
static_assert(MacroAssembler::relocationSize(RelocationKind::ShortConditionalJump) == X86JumpShortener::shortJumpLength);

#elif CPU(ARM64)

static LinkedSite twoWordJump(uint32_t secondWordOffset, uint32_t target, RelocationKind direct, Assembler::BranchEncoding encoding)
{
    uint32_t offset = secondWordOffset - static_cast<uint32_t>(Assembler::instructionSize);
    switch (encoding) {
    case Assembler::BranchEncoding::DirectThenNop:
        return { offset, target, direct };
    case Assembler::BranchEncoding::InvertedThenJump:
        return { offset, target, RelocationKind::InvertedJumpThenJump };
    case Assembler::BranchEncoding::Direct:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#endif

void LinkBuffer::takeRecords(MacroAssembler& macroAssembler)
{
    if (!m_didAllocate || !recordsLinks())
        return;

    m_records->patchablePointerCount = std::exchange(macroAssembler.m_patchablePointerCount, 0);
    m_records->wideImmediates = WTF::move(macroAssembler.m_wideImmediates);
    for (WideImmediate& immediate : m_records->wideImmediates)
        immediate.offset = applyOffset(AssemblerLabel(immediate.offset)).offset();

#if CPU(X86_64)
    if (!m_policy.recordsLinkedSites)
        return;
    m_records->emittedJumps = macroAssembler.m_assembler.takeEmittedJumps();
    Vector<X86LinkedJump> jumps = macroAssembler.m_assembler.takeLinkedJumps();
    m_records->linkedSites.reserveInitialCapacity(jumps.size());
    for (const X86LinkedJump& jump : jumps) {
        bool isConditional = jump.kind == X86JumpKind::Conditional;
        RelocationKind kind = jump.isShort
            ? (isConditional ? RelocationKind::ShortConditionalJump : RelocationKind::ShortJump)
            : (isConditional ? RelocationKind::ConditionalJump : RelocationKind::Jump);
        m_records->linkedSites.append(LinkedSite { jump.from - MacroAssembler::relocationSize(kind), jump.to, kind });
    }
#endif
}

void LinkBuffer::recordExternalLink(AssemblerLabel label, const void* target)
{
    RELEASE_ASSERT(!m_didFinalize);
    if (m_policy.recordsProcessAddresses)
        m_records->externalLinks.append(ExternalLink { label.offset(), reinterpret_cast<uintptr_t>(target) });
}

void LinkBuffer::didLinkCall(Call call, AssemblerLabel, const void* target)
{
    if (!call.isFlagSet(Call::Near) || !isInBuffer(target))
        recordExternalLink(call.m_label, target);
}

void LinkBuffer::didLinkJump(Jump jump, AssemblerLabel emittedLabel, BranchEncoding encoding, const void* target)
{
    if (!isInBuffer(target)) {
        recordExternalLink(jump.m_label, target);
        return;
    }
    if (!m_policy.recordsLinkedSites)
        return;

    RELEASE_ASSERT(!m_didFinalize);
    uint32_t offset = jump.m_label.offset();
    uint32_t targetOffset = static_cast<uint32_t>(static_cast<const uint8_t*>(target) - static_cast<const uint8_t*>(code()));
#if CPU(X86_64)
    UNUSED_PARAM(encoding);
    const X86EmittedJump& emitted = findEmittedJump(m_records->emittedJumps.span(), emittedLabel.offset());
    RelocationKind kind = emitted.kind == X86JumpKind::Conditional ? RelocationKind::ConditionalJump : RelocationKind::Jump;
    m_records->linkedSites.append(LinkedSite { offset - MacroAssembler::relocationSize(kind), targetOffset, kind });
#elif CPU(ARM64)
    UNUSED_PARAM(emittedLabel);
    switch (jump.m_type) {
    case Assembler::JumpNoCondition:
    case Assembler::JumpNoConditionFixedSize:
        m_records->linkedSites.append(LinkedSite { offset, targetOffset, RelocationKind::Jump });
        return;
    case Assembler::JumpCondition:
    case Assembler::JumpConditionFixedSize:
    case Assembler::JumpCompareAndBranch:
    case Assembler::JumpCompareAndBranchFixedSize:
        m_records->linkedSites.append(twoWordJump(offset, targetOffset, RelocationKind::ConditionalJumpThenNop, encoding));
        return;
    case Assembler::JumpTestBit:
    case Assembler::JumpTestBitFixedSize:
        m_records->linkedSites.append(twoWordJump(offset, targetOffset, RelocationKind::TestBitJumpThenNop, encoding));
        return;
    case Assembler::JumpFixed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

#if ENABLE(BRANCH_COMPACTION)
void LinkBuffer::didLinkRecord(const LinkRecord& record, intptr_t to, BranchEncoding encoding)
{
    uint32_t from = static_cast<uint32_t>(record.from());
    if (record.isThunk()) {
        recordExternalLink(AssemblerLabel(from), std::bit_cast<const void*>(to));
        return;
    }
    if (!m_policy.recordsLinkedSites)
        return;

    uint32_t target = static_cast<uint32_t>(to - executableOffsetFor(to));
    switch (record.linkType()) {
    case Assembler::LinkInvalid:
        return;
    case Assembler::LinkJumpNoCondition:
        m_records->linkedSites.append(LinkedSite { from, target, RelocationKind::Jump });
        return;
    case Assembler::LinkJumpConditionDirect:
    case Assembler::LinkJumpCompareAndBranchDirect:
        m_records->linkedSites.append(LinkedSite { from, target, RelocationKind::ConditionalJump });
        return;
    case Assembler::LinkJumpTestBitDirect:
        m_records->linkedSites.append(LinkedSite { from, target, RelocationKind::TestBitJump });
        return;
    case Assembler::LinkJumpCondition:
    case Assembler::LinkJumpCompareAndBranch:
        m_records->linkedSites.append(twoWordJump(from, target, RelocationKind::ConditionalJumpThenNop, encoding));
        return;
    case Assembler::LinkJumpTestBit:
        m_records->linkedSites.append(twoWordJump(from, target, RelocationKind::TestBitJumpThenNop, encoding));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

bool LinkBuffer::link(Label site, RelocationKind kind, Label target)
{
    RELEASE_ASSERT(kind == RelocationKind::AddressNear || kind == RelocationKind::AddressFar || kind == RelocationKind::Load64);
    RELEASE_ASSERT(!m_didFinalize);

    constexpr size_t maxSize = std::max({ MacroAssembler::relocationSize(RelocationKind::AddressNear), MacroAssembler::relocationSize(RelocationKind::AddressFar), MacroAssembler::relocationSize(RelocationKind::Load64) });
    uint32_t siteOffset = offsetOf(site);
    uint32_t targetOffset = offsetOf(target);
    size_t size = MacroAssembler::relocationSize(kind);
    RELEASE_ASSERT(size && siteOffset + size <= m_size && targetOffset <= m_size);

    uint8_t* siteAddress = static_cast<uint8_t*>(code()) + siteOffset;
    alignas(uint32_t) std::array<uint8_t, maxSize> instructions;
    memcpy(instructions.data(), siteAddress, size);
    if (!MacroAssembler::setRelocationDistance(kind, std::span { instructions }.first(size), static_cast<int64_t>(targetOffset) - static_cast<int64_t>(siteOffset)))
        return false;

#if CPU(ARM64)
    // performJITMemcpy writes eight bytes as one 64-bit store, and a site is only aligned to four.
    for (size_t offset = 0; offset < size; offset += Assembler::instructionSize)
        performJITMemcpy<jitMemcpyRepatch>(siteAddress + offset, instructions.data() + offset, Assembler::instructionSize);
#else
    performJITMemcpy<jitMemcpyRepatch>(siteAddress, instructions.data(), size);
#endif

    if (m_policy.recordsLinkedSites)
        m_records->linkedSites.append(LinkedSite { siteOffset, targetOffset, kind });
    return true;
}

// Each step of a link appends its sites in ascending order, so the list is a few ascending runs. No two sites have one offset.
static void sortByOffset(Vector<LinkedSite>& sites)
{
    constexpr size_t maxRuns = 16;
    Vector<size_t, maxRuns> runEnds;
    for (size_t i = 1; i < sites.size() && runEnds.size() < maxRuns; ++i) {
        if (sites[i].offset < sites[i - 1].offset)
            runEnds.append(i);
    }
    if (runEnds.size() == maxRuns) {
        std::ranges::sort(sites, { }, &LinkedSite::offset);
        return;
    }
    runEnds.append(sites.size());
    for (size_t run = 1; run < runEnds.size(); ++run)
        std::ranges::inplace_merge(sites.begin(), sites.begin() + runEnds[run - 1], sites.begin() + runEnds[run], { }, &LinkedSite::offset);
}

NEVER_INLINE void LinkBuffer::finalizeRecords()
{
    std::ranges::sort(m_records->externalLinks, { }, &ExternalLink::offset);
    sortByOffset(m_records->linkedSites);
    const Vector<LinkedSite>& linkedSites = m_records->linkedSites;
    for (size_t i = 1; i < linkedSites.size(); ++i) {
        const LinkedSite& previous = linkedSites[i - 1];
        RELEASE_ASSERT(previous.offset + MacroAssembler::relocationSize(previous.kind) <= linkedSites[i].offset);
    }
}

#if CPU(X86_64)
uint32_t LinkBuffer::jumpShiftAt(uint32_t offset) const
{
    return shortenedJumpShiftAt(m_records->jumpShifts.span(), offset, m_records->jumpShiftCursor);
}
#endif

#else

void LinkBuffer::takeRecords(MacroAssembler&) { RELEASE_ASSERT_NOT_REACHED(); }
void LinkBuffer::finalizeRecords() { RELEASE_ASSERT_NOT_REACHED(); }
void LinkBuffer::recordExternalLink(AssemblerLabel, const void*) { RELEASE_ASSERT_NOT_REACHED(); }
void LinkBuffer::didLinkCall(Call, AssemblerLabel, const void*) { RELEASE_ASSERT_NOT_REACHED(); }
void LinkBuffer::didLinkJump(Jump, AssemblerLabel, BranchEncoding, const void*) { RELEASE_ASSERT_NOT_REACHED(); }

#endif // CPU(X86_64) || CPU(ARM64)

void LinkBuffer::RecordsDeleter::operator()(Records* records) const
{
    delete records;
}

void LinkBuffer::performFinalization()
{
#if ENABLE(MPROTECT_RX_TO_RWX)
    ExecutableAllocator::singleton().startWriting(code(), m_size);
#endif
    for (auto& task : m_linkTasks)
        task->run(*this);
    for (auto& task : m_lateLinkTasks)
        task->run(*this);
#if ENABLE(MPROTECT_RX_TO_RWX)
    ExecutableAllocator::singleton().finishWriting(code(), m_size);
#endif

    if (m_records) [[unlikely]]
        finalizeRecords();
    m_didFinalize = true;

#ifndef NDEBUG
    ASSERT(!m_completed);
    ASSERT(isValid());
    m_completed = true;
#endif
    
    s_profileCummulativeLinkedSizes[static_cast<unsigned>(m_profile)] += m_size;
    s_profileCummulativeLinkedCounts[static_cast<unsigned>(m_profile)]++;
    if (m_cacheFlushOnFinalize == CacheFlushOnFinalize::Yes)
        MacroAssembler::cacheFlush(code(), m_size);
}

#if DUMP_LINK_STATISTICS
void LinkBuffer::dumpLinkStatistics(void* code, size_t initializeSize, size_t finalSize)
{
    static unsigned linkCount = 0;
    static unsigned totalInitialSize = 0;
    static unsigned totalFinalSize = 0;
    linkCount++;
    totalInitialSize += initialSize;
    totalFinalSize += finalSize;
    dataLogF("link %p: orig %u, compact %u (delta %u, %.2f%%)\n", 
            code, static_cast<unsigned>(initialSize), static_cast<unsigned>(finalSize),
            static_cast<unsigned>(initialSize - finalSize),
            100.0 * (initialSize - finalSize) / initialSize);
    dataLogF("\ttotal %u: orig %u, compact %u (delta %u, %.2f%%)\n", 
            linkCount, totalInitialSize, totalFinalSize, totalInitialSize - totalFinalSize,
            100.0 * (totalInitialSize - totalFinalSize) / totalInitialSize);
}
#endif

#if DUMP_CODE
void LinkBuffer::dumpCode(void* code, size_t size)
{
}
#endif

void LinkBuffer::clearProfileStatistics()
{
    for (unsigned i = 0; i < numberOfProfiles; ++i) {
        s_profileCummulativeLinkedSizes[i] = 0;
        s_profileCummulativeLinkedCounts[i] = 0;
    }
}

void LinkBuffer::dumpProfileStatistics(std::optional<PrintStream*> outStream)
{
    struct Stat {
        Profile profile;
        size_t size;
        size_t count;
    };

    Stat sortedStats[numberOfProfiles];
    PrintStream& out = outStream ? *outStream.value() : WTF::dataFile();

    size_t totalOfAllProfilesSize = 0;
    auto dumpStat = [&] (const Stat& stat) {
        char formattedName[21];
        snprintf(formattedName, 21, "%20s", profileName(stat.profile));

        const char* largerUnit = nullptr;
        double sizeInLargerUnit = stat.size;
        if (stat.size > 1 * MB) {
            largerUnit = "MB";
            sizeInLargerUnit = sizeInLargerUnit / MB;
        } else if (stat.size > 1 * KB) {
            largerUnit = "KB";
            sizeInLargerUnit = sizeInLargerUnit / KB;
        }

        if (largerUnit)
            out.print("  ", formattedName, ": ", stat.size, " (", sizeInLargerUnit, " ", largerUnit, ")");
        else
            out.print("  ", formattedName, ": ", stat.size);

        if (!stat.count)
            out.println();
        else
            out.println(" count ", stat.count, " avg size ", (stat.size / stat.count));
    };

    for (unsigned i = 0; i < numberOfProfiles; ++i) {
        sortedStats[i].profile = static_cast<Profile>(i);
        sortedStats[i].size = s_profileCummulativeLinkedSizes[i];
        sortedStats[i].count = s_profileCummulativeLinkedCounts[i];
        totalOfAllProfilesSize += s_profileCummulativeLinkedSizes[i];
    }
    sortedStats[static_cast<unsigned>(Profile::Total)].size = totalOfAllProfilesSize;
    std::sort(&sortedStats[0], &sortedStats[numberOfProfilesExcludingTotal],
        [] (Stat& a, Stat& b) -> bool {
            return a.size > b.size;
        });

    out.println("Cummulative LinkBuffer profile sizes:");
    for (unsigned i = 0; i < numberOfProfiles; ++i)
        dumpStat(sortedStats[i]);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(ASSEMBLER)
