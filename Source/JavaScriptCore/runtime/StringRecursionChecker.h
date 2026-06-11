/*
 *  Copyright (C) 2011, 2016 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "CallFrame.h"
#include "GetVM.h"
#include "VMInlines.h"
#include <wtf/StackStats.h>

namespace JSC {

class StringRecursionChecker {
    WTF_MAKE_NONCOPYABLE(StringRecursionChecker);

public:
    StringRecursionChecker(JSGlobalObject*, JSObject* thisObject);
    ~StringRecursionChecker();

    JSValue earlyReturnValue() const; // 0 if everything is OK, value to return for failure cases

private:
    JSValue throwStackOverflowError();
    JSValue NODELETE emptyString();
    JSValue performCheck();

    // TSAN r0 family vm-string-caches (UG §K: VM-level caches are NOT blessed
    // racy): the recursion-detection state used to live solely on the shared
    // VM (stringRecursionCheckFirstObject, a plain JSObject*, and
    // stringRecursionCheckVisitedObjects, a non-concurrent hash set). Under
    // GIL-off, two lites running toString/JSON.stringify/join concurrently
    // raced on both (plain read/write of firstObject at performCheck and the
    // ~StringRecursionChecker reset; concurrent add/remove corrupting the
    // hash set's buckets across a rehash). The state tracks recursion along a
    // SINGLE thread's call stack, so per-thread is the semantically correct
    // scope — sharing it across lites also produced spurious "" early returns
    // when two threads stringified the same object. GIL-off routes to a
    // per-thread instance (each thread touches only its own thread_local
    // state, so no cross-thread access remains and no ordering is needed);
    // flag-off keeps the VM members untouched behind one predicted-false
    // Config-page test (same mode-split shape as VM::liveNumericStrings()).
    // Lifetime: entries exist only while a checker for them is on this
    // thread's stack (the destructor removes them), matching the VM-member
    // set's invariant — the raw JSObject* pointers are kept live by the
    // conservatively-scanned stack frames of the active recursion; neither
    // copy of the state is (or needs to be) visited by GC.
    struct PerThreadState {
        JSObject* firstObject { nullptr };
        UncheckedKeyHashSet<JSObject*> visitedObjects;
    };
    static PerThreadState& gilOffPerThreadState()
    {
        static thread_local PerThreadState state;
        return state;
    }

    JSGlobalObject* m_globalObject;
    JSObject* m_thisObject;
    // Slots selected once in performCheck() (per-thread under GIL-off, the
    // shared VM members otherwise); the destructor must undo its registration
    // in the SAME state the constructor used.
    JSObject** m_firstObjectSlot { nullptr };
    UncheckedKeyHashSet<JSObject*>* m_visitedObjects { nullptr };
    JSValue m_earlyReturnValue;

    StackStats::CheckPoint stackCheckpoint;
};

inline JSValue StringRecursionChecker::performCheck()
{
    VM& vm = getVM(m_globalObject);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return throwStackOverflowError();

    if (vm.gilOffWithProcessGate()) [[unlikely]] {
        auto& state = gilOffPerThreadState();
        m_firstObjectSlot = &state.firstObject;
        m_visitedObjects = &state.visitedObjects;
    } else {
        m_firstObjectSlot = &vm.stringRecursionCheckFirstObject;
        m_visitedObjects = &vm.stringRecursionCheckVisitedObjects;
    }

    bool alreadyVisited = false;
    if (!*m_firstObjectSlot)
        *m_firstObjectSlot = m_thisObject;
    else if (*m_firstObjectSlot == m_thisObject)
        alreadyVisited = true;
    else
        alreadyVisited = !m_visitedObjects->add(m_thisObject).isNewEntry;

    if (alreadyVisited)
        return emptyString(); // Return empty string to avoid infinite recursion.
    return JSValue(); // Indicate success.
}

inline StringRecursionChecker::StringRecursionChecker(JSGlobalObject* globalObject, JSObject* thisObject)
    : m_globalObject(globalObject)
    , m_thisObject(thisObject)
    , m_earlyReturnValue(performCheck())
{
}

inline JSValue StringRecursionChecker::earlyReturnValue() const
{
    return m_earlyReturnValue;
}

inline StringRecursionChecker::~StringRecursionChecker()
{
    if (m_earlyReturnValue)
        return;

    // Use the slots performCheck() selected; re-deriving them here could
    // disagree (and the stack-overflow early return leaves them null, but
    // that path sets m_earlyReturnValue, so we never get here without them).
    ASSERT(m_firstObjectSlot && m_visitedObjects);
    if (*m_firstObjectSlot == m_thisObject)
        *m_firstObjectSlot = nullptr;
    else {
        ASSERT(m_visitedObjects->contains(m_thisObject));
        m_visitedObjects->remove(m_thisObject);
    }
}

} // namespace JSC
