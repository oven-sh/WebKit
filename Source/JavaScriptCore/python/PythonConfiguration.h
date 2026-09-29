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

#include "PythonFileOperations.h"
#include <wtf/Vector.h>
#include "JSCJSValue.h"
#include <wtf/text/WTFString.h>

namespace JSC { namespace Python {

// What whoever embeds the engine tells Python about the program: what PyConfig is to whoever embeds CPython. It is asked for once for each
// global object, through GlobalObjectMethodTable::configurePython, when Python is first used there.
struct Configuration {
    Vector<String> arguments; // sys.argv
    Vector<String> moduleSearchPaths; // sys.path
    String executable; // sys.executable
    String implementationName { "javascriptcore"_s }; // sys.implementation.name
    const FileOperations* files { systemFileOperations() }; // Null if there are to be no files.
    // What io.open_code() is: given the name of a file whose contents are to be run, a str, it returns a file that is open for reading bytes, or nothing, having thrown. It is for what wants a say in what is
    // run, or has it somewhere other than in a file. Null is open(path, "rb"). PyFile_SetOpenCodeHook() of CPython.
    JSValue (*openCode)(JSGlobalObject*, JSValue path) { nullptr };
};

} } // namespace JSC::Python
