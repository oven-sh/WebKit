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

// Upstream removed this guard (https://commits.webkit.org/318399@main) and lets a cyclic
// Array#join, Array#toString or Array#toLocaleString recurse until the stack overflows.
// V8 and SpiderMonkey return the empty string for an array that is already being joined, and
// code that runs under Node depends on that (three.js hashes arrays that contain themselves).
// Bun keeps the guard for arrays so that these conversions match Node.
#if USE(BUN_JSC_ADDITIONS)

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

    JSGlobalObject* m_globalObject;
    JSObject* m_thisObject;
    JSValue m_earlyReturnValue;

    StackStats::CheckPoint stackCheckpoint;
};

inline JSValue StringRecursionChecker::performCheck()
{
    VM& vm = getVM(m_globalObject);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return throwStackOverflowError();

    bool alreadyVisited = false;
    if (!vm.stringRecursionCheckFirstObject)
        vm.stringRecursionCheckFirstObject = m_thisObject;
    else if (vm.stringRecursionCheckFirstObject == m_thisObject)
        alreadyVisited = true;
    else
        alreadyVisited = !vm.stringRecursionCheckVisitedObjects.add(m_thisObject).isNewEntry;

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

    VM& vm = getVM(m_globalObject);
    if (vm.stringRecursionCheckFirstObject == m_thisObject)
        vm.stringRecursionCheckFirstObject = nullptr;
    else {
        ASSERT(vm.stringRecursionCheckVisitedObjects.contains(m_thisObject));
        vm.stringRecursionCheckVisitedObjects.remove(m_thisObject);
    }
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
