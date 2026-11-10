/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHostFunctionSources.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <memory>
#include <span>
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

struct ExternalFunctionBodies;
struct HostThreadPool;

inline constexpr unsigned numberOfResultSections = numberOfEnumerators<Wire::ResultSection>;
inline constexpr size_t alignmentOfResultSections = 8;

class CompileOutput {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CompileOutput);
    WTF_MAKE_NONCOPYABLE(CompileOutput);
public:
    CompileOutput() = default;

    std::span<const uint8_t> section(Wire::ResultSection section) const { return m_sections[static_cast<unsigned>(section)].span(); }
    bool hasImage() const { return !section(Wire::ResultSection::Image).empty(); }

    template<typename Record>
    std::span<const Record> records(Wire::ResultSection section) const
    {
        static_assert(std::is_trivially_copyable_v<Record> && alignof(Record) <= alignmentOfResultSections);
        return spanReinterpretCast<const Record>(this->section(section));
    }

    void setSection(Wire::ResultSection section, Vector<uint8_t>&& bytes) { m_sections[static_cast<unsigned>(section)] = WTF::move(bytes); }

    template<typename Record>
    void setRecords(Wire::ResultSection section, std::span<const Record> records)
    {
        static_assert(std::is_trivially_copyable_v<Record> && alignof(Record) <= alignmentOfResultSections);
        setSection(section, Vector<uint8_t>(asBytes(records)));
    }

    JS_EXPORT_PRIVATE Wire::TextRange appendText(std::span<const uint8_t> utf8);
    JS_EXPORT_PRIVATE Wire::TextRange appendText(const String&);

private:
    std::array<Vector<uint8_t>, numberOfResultSections> m_sections;
};

struct ProgramOwner {
    void (*release)(void* owner) { nullptr };
    void* owner { nullptr };
    const ExternalFunctionBodies* externalBodies { nullptr };
    HostFunctionSources functionSources { };
    void (*releaseBytes)(void* owner) { nullptr };
    HostStringEncoders stringEncoders { };
};

JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions&, const HostThreadPool* = nullptr, ProgramOwner = { });

JS_EXPORT_PRIVATE size_t imageAlignment();
JS_EXPORT_PRIVATE size_t processPeakResidentMegabytes();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
