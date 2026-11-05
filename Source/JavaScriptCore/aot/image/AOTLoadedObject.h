/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && (OS(LINUX) || OS(FREEBSD))

#include "JSExportMacros.h"
#include <link.h>
#include <optional>
#include <span>
#include <type_traits>

namespace JSC { namespace AOT {

using ProgramHeader = std::remove_const_t<std::remove_pointer_t<decltype(dl_phdr_info::dlpi_phdr)>>;

struct LoadedObject {
    JS_EXPORT_PRIVATE const ProgramHeader* findSegment(uintptr_t address) const;

    uintptr_t loadBias { 0 };
    std::span<const ProgramHeader> programHeaders;
};

class LoadedObjectSearch {
public:
    explicit LoadedObjectSearch(uintptr_t address)
        : m_address(address)
    {
    }

    JS_EXPORT_PRIVATE int visit(const LoadedObject&);
    const std::optional<LoadedObject>& found() const { return m_found; }

private:
    uintptr_t m_address;
    std::optional<LoadedObject> m_found;
};

JS_EXPORT_PRIVATE const LoadedObject& loadedExecutable();

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && (OS(LINUX) || OS(FREEBSD))
