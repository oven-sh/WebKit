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
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

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
        NoFreeEntry,
    };

    CallbackEntryPool() = default;

    JS_EXPORT_PRIVATE static CallbackEntryPool& singleton();

    JS_EXPORT_PRIVATE std::expected<unsigned, Failure> acquire();
    JS_EXPORT_PRIVATE void bind(unsigned entry, JSFFICallback&);
    JS_EXPORT_PRIVATE void release(unsigned entry);

    JS_EXPORT_PRIVATE void* entrypoint(unsigned entry) const;
    JSFFICallback* callbackAt(unsigned entry) const { return m_callbacks[entry].load(std::memory_order_acquire); }

    JS_EXPORT_PRIVATE unsigned numberOfEntries() const;

private:
    std::optional<Failure> prepare() WTF_REQUIRES_LOCK(m_lock);

    Lock m_lock;
    bool m_didPrepare WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<Failure> m_preparationFailure WTF_GUARDED_BY_LOCK(m_lock);
    BitVector m_entriesInUse WTF_GUARDED_BY_LOCK(m_lock);
    const Stubs* m_stubs { nullptr };
    FixedVector<std::atomic<JSFFICallback*>> m_callbacks;
};

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
