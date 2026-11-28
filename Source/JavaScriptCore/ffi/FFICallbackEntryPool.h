/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "JSExportMacros.h"
#include <atomic>
#include <expected>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/FixedVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC {

class JSFFICallback;

namespace FFI {

class Stubs;

class CallbackEntryPool {
    WTF_MAKE_NONCOPYABLE(CallbackEntryPool);
public:
    enum class Failure : uint8_t {
        NoStubs,
        CannotPrepareContext,
        CannotMapMoreEntries,
    };

    CallbackEntryPool() = default;

    JS_EXPORT_PRIVATE static CallbackEntryPool& singleton();

    JS_EXPORT_PRIVATE std::expected<unsigned, Failure> acquire();
    JS_EXPORT_PRIVATE void bind(unsigned entry, JSFFICallback&);
    JS_EXPORT_PRIVATE void release(unsigned entry);

    JS_EXPORT_PRIVATE void* entrypoint(unsigned entry) const;
    // An entry that is called reads its word through its context and asks nothing here.
    JS_EXPORT_PRIVATE JSFFICallback* callbackAt(unsigned entry) const;

    JS_EXPORT_PRIVATE unsigned numberOfEntries() const;

private:
    struct Block;

    std::optional<Failure> addBlock() WTF_REQUIRES_LOCK(m_lock);
    std::atomic<JSFFICallback*>& wordOf(unsigned entry) const WTF_REQUIRES_LOCK(m_lock);

    mutable Lock m_lock;
    std::optional<Failure> m_failureOfFirstBlock WTF_GUARDED_BY_LOCK(m_lock);
    BitVector m_entriesInUse WTF_GUARDED_BY_LOCK(m_lock);
    // No entry in front of it is free.
    size_t m_firstEntryThatMayBeFree WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    const Stubs* m_stubs WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    // A block stays for as long as the process: foreign code may hold the address of an entry that was closed.
    Vector<Block*> m_blocks WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
