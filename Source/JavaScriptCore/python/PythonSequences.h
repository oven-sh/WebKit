/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSArray.h"
#include "PyDict.h"
#include "PythonOperations.h"

namespace JSC { namespace Python {

// A list is a JSArray. These are what Python does to one.

inline JSArray* asList(JSValue value) { return uncheckedDowncast<JSArray>(value.asCell()); }
inline JSArray* tryList(JSValue value) { return isList(value) ? asList(value) : nullptr; }

JSArray* newList(JSGlobalObject*, unsigned length = 0); // Of that many Nones.
JSArray* newList(JSGlobalObject*, const ArgList&);
JSArray* listFromIterable(JSGlobalObject*, JSValue);
PyTuple* tupleFromIterable(JSGlobalObject*, JSValue);

// A hole, which only JavaScript can make, is None. So can only JavaScript make an array whose elements are not simply there to be read: one that is frozen, or
// sealed, or has a getter for an element. Those are read as JavaScript reads them, which can run code and can throw.
JSValue listGetSlow(JSGlobalObject*, JSArray*, unsigned index);
ALWAYS_INLINE JSValue listGet(JSGlobalObject* globalObject, JSArray* list, unsigned index)
{
    if (list->canGetIndexQuickly(index)) [[likely]]
        return list->getIndexQuickly(index);
    return listGetSlow(globalObject, list, index);
}

// As JavaScript does it in strict code: an array that is frozen says so.
void listSet(JSGlobalObject*, JSArray*, unsigned index, JSValue);
// For a list that has just been made here, and that nothing else has seen.
void listInitializeAt(JSGlobalObject*, JSArray*, unsigned index, JSValue);
void listAppend(JSGlobalObject*, JSArray*, JSValue);
bool listExtend(JSGlobalObject*, JSArray*, JSValue iterable);
void listInsert(JSGlobalObject*, JSArray*, unsigned index, JSValue);
void listRemoveRange(JSGlobalObject*, JSArray*, unsigned start, unsigned count);
// list[start:start + count] = values
void listReplaceRange(JSGlobalObject*, JSArray*, unsigned start, unsigned count, const ArgList& values);
JSArray* listRepeat(JSGlobalObject*, JSArray*, int64_t count);

// ---- What the built-in types do for an operator, whatever class the operands say they are. Their special methods are these.

JSValue builtinBinaryOperation(JSGlobalObject*, BinaryOperator, bool inPlace, JSValue, JSValue); // Empty if it is not for them.
JSValue builtinCompare(JSGlobalObject*, ComparisonOperator, JSValue, JSValue); // Likewise.
JSValue builtinGetItem(JSGlobalObject*, JSValue, JSValue key); // Likewise.
bool builtinSetItem(JSGlobalObject*, JSValue, JSValue key, JSValue); // False if it is not for them. An empty value deletes.
JSValue builtinGetIterator(JSGlobalObject*, JSValue);
int64_t builtinHash(JSGlobalObject*, JSValue);
JSValue setOperation(JSGlobalObject*, BinaryOperator, bool inPlace, PySet*, PySet*);
// What to look for in a set when it is asked whether `key` is in it. Empty if it raised.
JSValue keyToLookForInSet(JSGlobalObject*, JSValue key);
JSValue setCompare(JSGlobalObject*, ComparisonOperator, PySet*, PySet*);
PySet* setFromIterable(JSGlobalObject*, Structure*, JSValue);

// ---- str

// Python counts characters, and JavaScript counts UTF-16 code units. They differ only if there are surrogate pairs.
unsigned stringLength(JSGlobalObject*, JSString*);
bool stringHasSurrogatePairs(StringView);
// The code unit that the character begins at.
unsigned stringOffsetOfCharacter(StringView, unsigned character);
String reprOfString(StringView);
// By character, which is not the order of the code units: U+FFFF comes before U+10000. Negative, zero or positive.
int compareStrings(StringView, StringView);

} } // namespace JSC::Python
