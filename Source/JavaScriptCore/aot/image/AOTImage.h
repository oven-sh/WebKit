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
#include "AOTHandlerTable.h"
#include "AOTImageFormat.h"
#include "AOTImageForward.h"
#include "AOTImageText.h"
#include "AOTPositionTable.h"
#include "AOTProgramTables.h"
#include "AOTStubFrames.h"
#include "AOTStubKey.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>
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

// A uint32_t function is an image function index, a position in ImageSection::Functions. A FunctionID is the program's.
class Image {
    WTF_MAKE_TZONE_ALLOCATED(Image);
    WTF_MAKE_NONCOPYABLE(Image);
public:
    static const Image* current() { return s_current.load(std::memory_order_acquire); }
    static bool isRegistered() { return !!current(); }

    JS_EXPORT_PRIVATE static Wire::ImageVerdict check(std::span<const uint8_t> bytes, const std::array<uint8_t, 16>& identity);
    // A host reads its start-up options before it can call useImage(). Nothing for bytes whose header check() refuses under every identity.
    JS_EXPORT_PRIVATE static std::optional<Wire::AdmittedCode> admittedCode(std::span<const uint8_t> bytes);
    JS_EXPORT_PRIVATE static std::optional<OptionSet<ImageFlag>> flags(std::span<const uint8_t> bytes);
    JS_EXPORT_PRIVATE static std::unique_ptr<Image> create(std::span<const uint8_t> bytes, const uint8_t* code);
    JS_EXPORT_PRIVATE static bool publish(std::unique_ptr<Image>&&);

    const ImageHeader& header() const { return *reinterpret_cast<const ImageHeader*>(m_bytes.data()); }
    template<typename T> std::span<const T> section(ImageSection) const;
    std::span<const uint8_t> code() const { return { m_code, static_cast<size_t>(header().codeSize) }; }
    std::span<const uint8_t> regExpCode() const;
    std::span<const uint8_t> webAssemblyCode() const;
    std::span<const uint8_t> ffiCode() const;
    const ProgramTables& programTables() const { return *reinterpret_cast<const ProgramTables*>(section<uint8_t>(ImageSection::ProgramTables).data()); }
    const ImageText& text() const { return m_text; }
    bool isObfuscated() const { return header().isObfuscated(); }
    bool admitsUnknownJavaScript() const { return header().admitsUnknownJavaScript(); }
    bool loadsModulesByName() const { return header().loadsModulesByName(); }
    bool replacesLoaderAnswers() const { return header().replacesLoaderAnswers(); }
    bool admitsLibraryText() const { return header().admitsLibraryText(); }

    JS_EXPORT_PRIVATE CodeLocation locate(const void* pc) const;

    const ImageFunction& function(uint32_t function) const;
    const uint8_t* codeOf(uint32_t function) const;
    size_t sizeOfCodeOf(uint32_t function) const;
    const ImageFrame& frameOf(uint32_t function) const;
    FunctionID rootFunctionOf(uint32_t function) const;
    uint64_t closureWord(FunctionID) const;

    JS_EXPORT_PRIVATE const void* stubAddress(StubKey) const;
    std::span<const StubReturnSite> stubReturnSites() const;
    std::span<const uint32_t> sharedCodeCopies() const { return section<uint32_t>(ImageSection::SharedCodeCopies); }
    std::span<const uint32_t> slotHandlerOffsets() const { return section<uint32_t>(ImageSection::SlotHandlers); }
    const uint8_t* sharedCodeCopy(unsigned index) const;

    JS_EXPORT_PRIVATE bool containsCharactersOf(const StringImpl&) const;
    static bool isBackedByStaticData(const StringImpl&);

    std::optional<uint32_t> imageFunctionOf(FunctionID) const;
    std::optional<FunctionID> innerEntryContaining(const void* pc) const;
    std::span<const uint32_t> frameMapWords(uint32_t function) const;
    bool hasFrameAt(uint32_t function, uint32_t offset) const;
    bool hasFrameAtReturnOffset(uint32_t function, uint32_t returnOffset) const;

    std::optional<CallSiteRecord> callSiteAt(uint32_t function, uint32_t returnOffset) const;
    std::optional<PrologueCheck> prologueCheckAt(const void* returnAddress) const;
    std::optional<CallSiteIndex> spreadSiteAt(uint32_t function, CallSiteIndex, unsigned item) const;

    std::optional<FaultSite> faultSiteAt(const void* pc) const;

    std::optional<HandlerRecord> handlerAt(uint32_t function, CallSiteIndex) const;
    std::optional<SourcePosition> positionOf(uint32_t function, CallSiteIndex) const;

private:
    Image(std::span<const uint8_t> bytes, const uint8_t* code);

    JS_EXPORT_PRIVATE static std::atomic<const Image*> s_current;

    std::span<const uint8_t> m_bytes;
    const uint8_t* m_code;
    const ImageFunction* m_functions;
    const uint32_t* m_functionStarts;
    const uint32_t* m_codeGranules;
    ImageText m_text;
};

template<typename T>
std::span<const T> Image::section(ImageSection section) const
{
    ImageSectionRange range = header().sections[static_cast<unsigned>(section)];
    return { reinterpret_cast<const T*>(m_bytes.data() + range.offset), range.size / sizeof(T) };
}

inline const ImageFunction& Image::function(uint32_t function) const
{
    RELEASE_ASSERT(function < header().counts.functions);
    return m_functions[function];
}

inline const uint8_t* Image::codeOf(uint32_t function) const
{
    RELEASE_ASSERT(function < header().counts.functions);
    return m_code + m_functionStarts[function];
}

inline size_t Image::sizeOfCodeOf(uint32_t function) const
{
    RELEASE_ASSERT(function < header().counts.functions);
    const uint32_t* starts = m_functionStarts;
    return (function + 1 < header().counts.functions ? starts[function + 1] : header().code.functionsEnd) - starts[function];
}

inline const ImageFrame& Image::frameOf(uint32_t function) const
{
    return section<ImageFrame>(ImageSection::Frames)[this->function(function).frame()];
}

inline FunctionID Image::rootFunctionOf(uint32_t function) const
{
    return FunctionID(this->function(function).function());
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
