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

#include "SourceOrigin.h"
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

// All that is done before a program is run and can go wrong, if it has not been done: init_interp_main() of CPython's Python/pylifecycle.c. After it, what is in files can be imported, codecs are found by name,
// sys.stdin and the like are there, and `site` has been imported. Whatever runs Python from outside calls it first. It may throw.
JS_EXPORT_PRIVATE void startPython(JSGlobalObject*);

// Runs a program as `python file.py` does, as the module __main__, and reports what it does not catch. Returns the status that the process is to end with, if this is all that it does.
JS_EXPORT_PRIVATE int runMain(JSGlobalObject*, std::span<const uint8_t>, const SourceOrigin&, const String& filename);

// What is done when there is no more to run, of what Py_FinalizeEx() does: what has been written and not sent on is sent on. It is for the host to say when that is, since a program in two languages is not over when
// the Python that began it is. False if it could not all be, for which CPython ends the process with the status 120. It does nothing if Python was never started.
JS_EXPORT_PRIVATE bool finalizePython(JSGlobalObject*);

} } // namespace JSC::Python
