/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <cstddef>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

class AbstractSlotVisitor;
class JSCell;
class SlotVisitor;

class StoredCellLog {
    WTF_MAKE_NONCOPYABLE(StoredCellLog);
public:
    static constexpr unsigned minVectorLengthOfOwner = 1024;
    static constexpr size_t minCapacity = 4096;

    StoredCellLog() = default;
    ~StoredCellLog();

    static constexpr ptrdiff_t offsetOfCursor() { return OBJECT_OFFSETOF(StoredCellLog, m_cursor); }
    static constexpr ptrdiff_t offsetOfEnd() { return OBJECT_OFFSETOF(StoredCellLog, m_end); }

    JS_EXPORT_PRIVATE bool tryRemember(const JSCell* owner, JSCell* stored);

    void visitAndReset(SlotVisitor&);
    void visitAndReset(AbstractSlotVisitor&) { }

    size_t size() const { return m_cursor - m_begin; }
    size_t capacity() const { return m_end - m_begin; }

private:
    static unsigned vectorLengthOf(const JSCell* owner);
    bool tryGrow() WTF_REQUIRES_LOCK(m_lock);

    JSCell** m_cursor { nullptr };
    JSCell** m_end { nullptr };
    JSCell** m_begin { nullptr };
    Lock m_lock;
};

} // namespace JSC
