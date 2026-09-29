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

#include "ImplementationVisibility.h"
#include "JSCJSValue.h"
#include "PythonFileOperations.h"
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;
class JSObject;

namespace Python {

// A module that is written in C++: `struct _inittab` of CPython.
struct BuiltinModule {
    ASCIILiteral name;
    JSObject* (*create)(JSGlobalObject*); // Null if it raised.
};

// A module that is written in Python and is not in a file: `struct _frozen` of CPython, but that it is the source that there is here, and not what comes of compiling it.
struct FrozenModule {
    ASCIILiteral name;
    std::span<const uint8_t> source; // If there is no source at all, there is no importing anything of this name, from here or from anywhere after.
    bool isPackage { false };
    ASCIILiteral originalName { }; // What it is called in the library, if that is something else.
    ImplementationVisibility visibility { ImplementationVisibility::Public };
};

// What whoever embeds the engine tells Python about the program: what PyConfig is to whoever embeds CPython. It is asked for once for each
// global object, through GlobalObjectMethodTable::configurePython, when Python is first used there.
struct Configuration {
    Vector<String> arguments; // sys.argv
    Vector<String> moduleSearchPaths; // sys.path, after what follows
    String firstSearchPath; // What sys.path begins with: where the program is, or "" for wherever the process is at the time. Null for neither. PyConfig.sys_path_0
    String executable; // sys.executable
    String implementationName { "javascriptcore"_s }; // sys.implementation.name
    const FileOperations* files { systemFileOperations() }; // Null if there are to be no files.
    Vector<BuiltinModule> builtinModules; // Besides the engine's own: PyImport_AppendInittab()
    Vector<FrozenModule> frozenModules; // Besides the engine's own, and before all of them but importlib: PyImport_FrozenModules
    bool usesFrozenModules { true }; // -X frozen_modules
    bool importsSite { true }; // Not -S
    String prefix; // sys.prefix and sys.base_prefix: what the library is installed under, if it is installed anywhere.
    String executablePrefix; // sys.exec_prefix and sys.base_exec_prefix
    String libraryDirectory; // sys._stdlib_dir: where the files of the library are. Null if they are not in a directory.
    String standardStreamEncoding { "utf-8"_s }; // PYTHONIOENCODING
    String standardStreamErrors { "surrogateescape"_s };
    // Whether sys.stdout and sys.stderr keep what is written to them until there is a good deal of it: not -u. They do not, unless it is asked for, so that what Python writes and what JavaScript writes come out in
    // the order in which they were written. What is to be kept back, so as not to ask the system so often, is for `files` to keep back, which can keep both.
    bool buffersStandardStreams { false };
    // What io.open_code() is: given the name of a file whose contents are to be run, a str, it returns a file that is open for reading bytes, or nothing, having thrown. It is for what wants a say in what is
    // run, or has it somewhere other than in a file. Null is open(path, "rb"). PyFile_SetOpenCodeHook() of CPython.
    JSValue (*openCode)(JSGlobalObject*, JSValue path) { nullptr };
};

} } // namespace JSC::Python
