/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <cstdint>
#include <optional>

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTCodeSites.h"
#include "AOTFaultTable.h"
#include "AOTImageFormat.h"
#include "AOTImageForward.h"
#include "AOTImageText.h"
#include "AOTProgramTables.h"
#include "AOTStubFrames.h"
#include "AOTStubKey.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <span>
#include <wtf/IterationStatus.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct StubReturnSite;

enum class CodeKind : uint8_t {
    NotInImage,
    Function,
    Stub,
    RegExp,
    WebAssembly,
    FFI,
};

struct CodeLocation {
    bool runsCompiledCode() const
    {
        switch (kind) {
        case CodeKind::Function:
        case CodeKind::Stub:
            return true;
        case CodeKind::NotInImage:
        case CodeKind::RegExp:
        case CodeKind::WebAssembly:
        case CodeKind::FFI:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    CodeKind kind { CodeKind::NotInImage };
    StubFrameKind stubFrameKind { StubFrameKind::Plain };
    uint32_t function { 0 };
    uint32_t offset { 0 };
};

struct ReturnSiteInfo {
    std::optional<CallSiteRecord> site;
    ImageFrame frame { };
    bool mayHaveHandler { false };
    bool hasConstructedCallees { false };
};

class Image {
    WTF_MAKE_TZONE_ALLOCATED(Image);
    WTF_MAKE_NONCOPYABLE(Image);
public:
    static const Image* current()
    {
        const Image* image = s_current.load(std::memory_order_acquire);
        if (image != differsByThread()) [[likely]]
            return image;
        return currentOfThread();
    }
    static const Image* sharedByAllThreads()
    {
        const Image* image = s_current.load(std::memory_order_acquire);
        return image != differsByThread() ? image : nullptr;
    }
    static bool isRegistered() { return !!s_current.load(std::memory_order_acquire); }
    JS_EXPORT_PRIVATE static void setCurrentOfThread(const Image*);
    static const Image** addressOfCurrentOfThread();
    JS_EXPORT_PRIVATE static void disableOnThisThread();
    static bool isDisabledOnThisThread();
    JS_EXPORT_PRIVATE static const Image* find(std::span<const uint8_t> bytes);
    static const Image* findByCodeAddress(const void*);
    static void forEachPublishedImage(const ScopedLambda<IterationStatus(const Image&)>&);

    JS_EXPORT_PRIVATE static Wire::ImageVerdict check(std::span<const uint8_t> bytes, const std::array<uint8_t, 16>& identity);
    JS_EXPORT_PRIVATE static std::optional<Wire::AdmittedCode> admittedCode(std::span<const uint8_t> bytes);
    JS_EXPORT_PRIVATE static std::optional<OptionSet<ImageFlag>> flags(std::span<const uint8_t> bytes);
    JS_EXPORT_PRIVATE static std::unique_ptr<Image> create(std::span<const uint8_t> bytes, const uint8_t* code);
    JS_EXPORT_PRIVATE static void publish(std::unique_ptr<Image>&&);

    struct PositionInFile {
        int descriptor { -1 };
        int64_t offset { 0 };
    };
    struct PlaceInExecutable {
        int64_t offset { 0 };
        uint64_t device { 0 };
        uint64_t inode { 0 };
    };
    const std::optional<PlaceInExecutable>& placeInExecutable() const { return m_placeInExecutable; }
    void setPlaceInExecutable(const PlaceInExecutable& place) { m_placeInExecutable = place; }

    const std::optional<PositionInFile>& positionOfCodeInFile() const { return m_positionOfCodeInFile; }
    void setPositionOfCodeInFile(const std::optional<PositionInFile>& position) { m_positionOfCodeInFile = position; }
    unsigned number() const { return m_number; }

    const ImageHeader& header() const { return *reinterpret_cast<const ImageHeader*>(m_bytes.data()); }
    std::span<const uint8_t> bytes() const { return m_bytes; }
    template<typename T> std::span<const T> section(ImageSection) const;
    std::span<const uint8_t> code() const { return { m_code, static_cast<size_t>(header().codeSize) }; }
    std::span<const uint8_t> regExpCode() const { return code().subspan(header().code.regExpCodeOffset, std::max(header().code.constantsOffset, header().code.regExpCodeOffset) - header().code.regExpCodeOffset); }
    std::span<const uint8_t> webAssemblyCode() const { return code().subspan(header().code.webAssemblyCodeOffset, header().code.webAssemblyCodeSize); }
    std::span<const uint8_t> ffiCode() const { return code().subspan(header().code.ffiCodeOffset, header().code.ffiCodeSize); }
    std::span<const uint8_t> ffiDirectory() const;
    const ProgramTables& programTables() const { return *reinterpret_cast<const ProgramTables*>(section<uint8_t>(ImageSection::ProgramTables).data()); }
    const ImageText& text() const { return m_text; }
    ImageText& text() { return m_text; }
    bool isObfuscated() const { return header().has(ImageFlag::IsObfuscated); }
    bool admitsUnknownJavaScript() const { return header().has(ImageFlag::AdmitsUnknownJavaScript); }
    bool loadsModulesByName() const { return header().has(ImageFlag::LoadsModulesByName); }
    bool allowsLibrarySource() const { return header().has(ImageFlag::AllowsLibrarySource); }
    bool isBuiltWithLibrary() const { return header().has(ImageFlag::IsBuiltWithLibrary); }
    bool refusesOriginalsBesideMocks() const { return header().has(ImageFlag::RefusesOriginalsBesideMocks); }
    bool loadsCodeOutsideImage() const { return !isHardenedBuild && header().has(ImageFlag::LoadsCodeOutsideImage); }
    bool showsWaysBetweenModuleObjects() const { return header().has(ImageFlag::ShowsWaysBetweenModuleObjects); }
    bool setsMainModulePath() const { return header().has(ImageFlag::SetsMainModulePath); }
    bool runsEntryPointsAgain() const { return header().has(ImageFlag::RunsEntryPointsAgain); }
    bool hasReloadableModules() const { return header().has(ImageFlag::HasReloadableModules); }
    bool letsGlobalObjectGo() const { return header().has(ImageFlag::LetsGlobalObjectGo); }
    bool showsEntriesOfRequireCache() const { return header().has(ImageFlag::ShowsEntriesOfRequireCache); }

    JS_EXPORT_PRIVATE CodeLocation locate(const void* pc) const;

    ImageFunction function(uint32_t function) const;
    const FunctionStarts& functionStarts() const LIFETIME_BOUND { return m_functionStarts; }
    const uint8_t* codeOf(uint32_t function) const;
    size_t sizeOfCodeOf(uint32_t function) const;
    const ImageFrame& frameOf(uint32_t function) const;
    SideTableReader sideTable(uint32_t function) const;
    FunctionID rootFunctionOf(uint32_t function) const;
    uint64_t closureWord(FunctionID) const;

    JS_EXPORT_PRIVATE const void* stubAddress(StubKey) const;
    enum class FrequentStub : uint8_t { EnterClosure, EnterSlimClosure, EnterModuleBody, EnterClosureForConstruct, EnterHandlerOfJob, EnterSavedFrame, RejectInFrame, RejectInFrameBelowSpilledArguments, RejectAwaitedCall };
    template<typename Functor> void* frequentStub(FrequentStub, const Functor&) const;
    JS_EXPORT_PRIVATE std::span<const StubAddressInImage> stubAddressesOf(StubFamily) const;
    std::span<const StubAddressInImage> stubAddressesSortedWith(StubFamily) const;
    bool usesCallConvention(uint32_t offsetInSharedCode) const;
    std::span<const StubReturnSite> stubReturnSites() const;
    std::span<const uint32_t> sharedCodeCopies() const { return section<uint32_t>(ImageSection::SharedCodeCopies); }
    std::span<const uint32_t> slotHandlerOffsets() const { return section<uint32_t>(ImageSection::SlotHandlers); }
    std::span<const uint32_t> methodTable() const { return section<uint32_t>(ImageSection::MethodTable).first(programTables().instanceSizes.sizeOfMethodTable / sizeof(uint32_t)); }
    std::span<const uint32_t> methodLookups() const { return section<uint32_t>(ImageSection::MethodTable).subspan(programTables().instanceSizes.sizeOfMethodTable / sizeof(uint32_t)); }
    const uint8_t* sharedCodeCopy(unsigned index) const;

    JS_EXPORT_PRIVATE bool containsCharactersOf(const StringImpl&) const;
    static bool isBackedByStaticData(const StringImpl&);

    std::optional<uint32_t> imageFunctionFromClosureWord(FunctionID) const;
    std::optional<uint32_t> imageFunctionOf(FunctionID) const;
    std::span<const uint32_t> frameMapWords(uint32_t function) const;
    std::span<const uint32_t> frameMapWordsAt(const void* returnAddress) const;
    bool hasFrameAt(uint32_t function, uint32_t offset) const;
    bool hasFrameAtReturnOffset(uint32_t function, uint32_t returnOffset) const;
    bool hasFrameAtReturnOffset(uint32_t function, const SideTableFrame&, uint32_t returnOffset) const;
    ImageFrame frameAtReturnOffset(uint32_t function, uint32_t returnOffset) const;
    ReturnSiteInfo returnSiteInfoAt(uint32_t function, uint32_t returnOffset) const;

    std::optional<CallSiteRecord> callSiteAt(uint32_t function, uint32_t returnOffset) const;
    JS_EXPORT_PRIVATE std::optional<HandlerRecord> handlerAt(uint32_t function, CallSiteIndex) const;
    std::optional<FunctionID> constructedCalleeAt(uint32_t function, CallSiteIndex) const;
    std::optional<PrologueCheck> prologueCheckAt(const void* returnAddress) const;

    std::optional<FaultSite> faultSiteAt(const void* pc) const;

    std::optional<SourcePosition> positionOf(uint32_t function, CallSiteIndex) const;

    bool mayContainModulePathWithPrefix(StringView start) const;

private:
    Image(std::span<const uint8_t> bytes, const uint8_t* code);

    static const Image* differsByThread() { return std::bit_cast<const Image*>(static_cast<uintptr_t>(1)); }
    JS_EXPORT_PRIVATE static const Image* currentOfThread();

    JS_EXPORT_PRIVATE static std::atomic<const Image*> s_current;
    static std::atomic<const Image*> s_lastPublished;

    mutable std::array<std::atomic<void*>, 9> m_frequentStubs { };
    unsigned m_number { 0 };
    const Image* m_publishedBefore { nullptr };
    std::optional<PlaceInExecutable> m_placeInExecutable;
    std::optional<PositionInFile> m_positionOfCodeInFile;
    std::span<const uint8_t> m_bytes;
    const uint8_t* m_code;
    ImageFunctions m_functions;
    FunctionStarts m_functionStarts;
    const uint32_t* m_codeGranules;
    ImageText m_text;
    static constexpr unsigned frameMapCacheBits = 8;
    mutable std::array<std::atomic<uint64_t>, 1u << frameMapCacheBits> m_frameMapCache { };
    mutable std::once_flag m_didSortFunctionsByID;
    mutable Vector<uint32_t> m_functionsByID;
};

template<typename T>
std::span<const T> Image::section(ImageSection section) const
{
    ImageSectionRange range = header().sections[static_cast<unsigned>(section)];
    return { reinterpret_cast<const T*>(m_bytes.data() + range.offset), range.size / sizeof(T) };
}

inline const uint8_t* Image::codeOf(uint32_t function) const
{
    RELEASE_ASSERT(function < header().counts.functions);
    return m_code + m_functionStarts[function];
}

inline size_t Image::sizeOfCodeOf(uint32_t function) const
{
    RELEASE_ASSERT(function < header().counts.functions);
    const FunctionStarts& starts = m_functionStarts;
    return (function + 1 < header().counts.functions ? starts[function + 1] : header().code.functionsEnd) - starts[function];
}

inline FunctionID Image::rootFunctionOf(uint32_t function) const
{
    RELEASE_ASSERT(function < header().counts.functions);
    return FunctionID(m_functions.numberOf(function));
}

inline uint64_t Image::closureWord(FunctionID function) const
{
    const ClosureFunctionRangeData& range = programTables().closureFunctions;
    std::span words = section<uint64_t>(ImageSection::ClosureWords);
    uint32_t index = function.index();
    return index >= range.first && index - range.omittedWordCount < words.size() ? words[index - range.omittedWordCount] : 0;
}

inline const uint8_t* Image::sharedCodeCopy(unsigned index) const
{
    std::span copies = sharedCodeCopies();
    RELEASE_ASSERT(index < copies.size());
    return m_code + copies[index];
}

template<typename Functor>
inline void* Image::frequentStub(FrequentStub stub, const Functor& find) const
{
    std::atomic<void*>& place = m_frequentStubs[static_cast<size_t>(stub)];
    void* address = place.load(std::memory_order_relaxed);
    if (!address) [[unlikely]] {
        address = find();
        place.store(address, std::memory_order_relaxed);
    }
    return address;
}

inline bool Image::isBackedByStaticData(const StringImpl& string)
{
    switch (string.bufferOwnership()) {
    case StringImpl::BufferInternal:
        if (string.is8Bit())
            return std::bit_cast<uintptr_t>(string.span8().data()) != std::bit_cast<uintptr_t>(&string) + StringImpl::headerSize<Latin1Character>();
        return std::bit_cast<uintptr_t>(string.span16().data()) != std::bit_cast<uintptr_t>(&string) + StringImpl::headerSize<char16_t>();
    case StringImpl::BufferSubstring: {
        const Image* image = current();
        return image && image->containsCharactersOf(string);
    }
    case StringImpl::BufferOwned:
    case StringImpl::BufferExternal:
        break;
    }
    return false;
}

JS_EXPORT_PRIVATE bool isCompiledCodePC(const void*);
JS_EXPORT_PRIVATE std::optional<uint32_t> imageCodeOffset(const void* address);
JS_EXPORT_PRIVATE uintptr_t imageCodeStart();
JS_EXPORT_PRIVATE std::optional<ImageFrame> imageFrameAtReturnAddress(const void* returnAddress);
JS_EXPORT_PRIVATE bool isInImageStub(const void* address);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#else // ENABLE(AOT)

namespace JSC { namespace AOT {

class Image {
public:
    static constexpr const Image* current() { return nullptr; }
    static constexpr bool isRegistered() { return false; }
};

inline bool isCompiledCodePC(const void*) { return false; }
inline std::optional<uint32_t> imageCodeOffset(const void*) { return std::nullopt; }
inline uintptr_t imageCodeStart() { return 0; }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
