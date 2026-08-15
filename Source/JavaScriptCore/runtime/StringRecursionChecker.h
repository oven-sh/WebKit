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

#if USE(BUN_JSC_ADDITIONS)

#include "VM.h"
#include <wtf/Noncopyable.h>

namespace JSC {

// Cycle detection for the array to string conversions (Array.prototype.join, Array.prototype.toString,
// Array.prototype.toLocaleString and the ToString / ToPrimitive fast path for arrays): while an object is
// being converted, converting it again further down the same conversion produces the empty string instead
// of recursing until the stack overflows. The specification has no such rule, which is why upstream removed
// this class (https://bugs.webkit.org/show_bug.cgi?id=320820), but V8 and SpiderMonkey both implement it,
// so `a = [1]; a[1] = a; a.join()` is "1," in Node.js and in every browser. Bun keeps the V8 behavior.
//
// Stack overflow is not handled here: the conversions that can recurse without crossing a JS frame check
// VM::isSafeToRecurseSoft() themselves (JSArray::fastToString), and the others are reached through calls
// that perform the usual stack check.
class StringRecursionChecker {
    WTF_MAKE_NONCOPYABLE(StringRecursionChecker);

public:
    StringRecursionChecker(VM&, JSObject* thisObject);
    ~StringRecursionChecker();

    // True if thisObject is already being converted by a caller. The caller must then return the empty string
    // without converting anything; this checker registered nothing and its destructor does nothing.
    bool isRecursive() const { return m_isRecursive; }

private:
    VM& m_vm;
    JSObject* m_thisObject;
    bool m_isRecursive;
};

// The common case is a single conversion with nothing nested inside it, so the first object is kept in a plain
// field and the hash set is only touched once a conversion is nested inside another one.
inline StringRecursionChecker::StringRecursionChecker(VM& vm, JSObject* thisObject)
    : m_vm(vm)
    , m_thisObject(thisObject)
{
    if (!vm.stringRecursionCheckFirstObject) [[likely]] {
        vm.stringRecursionCheckFirstObject = thisObject;
        m_isRecursive = false;
    } else if (vm.stringRecursionCheckFirstObject == thisObject)
        m_isRecursive = true;
    else
        m_isRecursive = !vm.stringRecursionCheckVisitedObjects.add(thisObject).isNewEntry;
}

inline StringRecursionChecker::~StringRecursionChecker()
{
    if (m_isRecursive)
        return;

    if (m_vm.stringRecursionCheckFirstObject == m_thisObject) [[likely]]
        m_vm.stringRecursionCheckFirstObject = nullptr;
    else {
        ASSERT(m_vm.stringRecursionCheckVisitedObjects.contains(m_thisObject));
        m_vm.stringRecursionCheckVisitedObjects.remove(m_thisObject);
    }
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
