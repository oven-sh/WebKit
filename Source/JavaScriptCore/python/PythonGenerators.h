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

#include "JSGenerator.h"
#include "PythonOperations.h"

namespace JSC { namespace Python {

inline JSGenerator* asGenerator(JSValue value) { return uncheckedDowncast<JSGenerator>(value.asCell()); }

// A Python generator is a JavaScript generator: a JSGenerator, resumed in the same way. So each language can iterate the other's.

// Runs it until it yields, and gives what it yielded. If it returns instead, the result is empty and `returned` is what it returned.
// If it raises, both are empty.
JSValue resumeGenerator(JSGlobalObject*, JSGenerator*, JSValue sent, JSGenerator::ResumeMode, JSValue& returned);

// A coroutine and an asynchronous generator are generators too. They differ in their class, which is their prototype.
enum class GeneratorKind : uint8_t { Generator, Coroutine, AsyncGenerator };
GeneratorKind generatorKindOf(JSGlobalObject*, JSGenerator*);
JSGenerator* newCoroutine(JSGlobalObject*, JSValue body, bool isAsyncGenerator);

// What `await value` iterates. `context` is 1 if the value is what __aenter__ gave and 2 if it is what __aexit__ gave, for the message.
JSValue getAwaitable(JSGlobalObject*, JSValue, unsigned context);
// What `async for` does first, and then each time round.
JSValue getAsyncIterator(JSGlobalObject*, JSValue);
JSValue getAsyncNext(JSGlobalObject*, JSValue iterator);
JSValue wrapAsyncYield(JSGlobalObject*, JSValue);
// An exception, from what may be the class of one, with a value to make it from. Empty if it raised.
JSValue exceptionToThrow(JSGlobalObject*, JSValue typeOrValue, JSValue value);

// generator.send(), .throw() and .close(), which raise StopIteration when it returns.
JSValue generatorSend(JSGlobalObject*, JSGenerator*, JSValue);
JSValue generatorThrow(JSGlobalObject*, JSGenerator*, JSValue exception);
// What it returned, if being closed made it return.
JSValue generatorClose(JSGlobalObject*, JSGenerator*);

} } // namespace JSC::Python
