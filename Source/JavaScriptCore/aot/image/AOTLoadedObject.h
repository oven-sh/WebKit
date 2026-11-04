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

// An ELF file that the process has loaded, as dl_iterate_phdr() describes it.
struct LoadedObject {
    // The loadable segment that holds the address among its bytes from the file.
    JS_EXPORT_PRIVATE const ProgramHeader* findSegment(uintptr_t address) const;

    uintptr_t loadBias { 0 };
    std::span<const ProgramHeader> programHeaders;
};

// Finds the object that holds an address. The order in which a process lists its objects is up to its C library: glibc, musl and FreeBSD begin with the executable, bionic begins with the dynamic linker.
class LoadedObjectSearch {
public:
    explicit LoadedObjectSearch(uintptr_t address)
        : m_address(address)
    {
    }

    // Returns what a callback of dl_iterate_phdr() returns: zero to be given the next object.
    JS_EXPORT_PRIVATE int visit(const LoadedObject&);
    const std::optional<LoadedObject>& found() const { return m_found; }

private:
    uintptr_t m_address;
    std::optional<LoadedObject> m_found;
};

// The engine is linked into the executable: it is the object that holds the engine.
JS_EXPORT_PRIVATE const LoadedObject& loadedExecutable();

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && (OS(LINUX) || OS(FREEBSD))
