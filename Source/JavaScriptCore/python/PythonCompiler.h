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

#include "CodeSpecializationKind.h"
#include "ParserModes.h"
#include "PythonFunctionInfo.h"
#include "SourceCode.h"
#include <wtf/OptionSet.h>

namespace JSC {

class JSFunction;
class JSGlobalObject;
class JSObject;
class ParserError;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class VM;
enum class CodeGenerationMode : uint8_t;

namespace Python {

// The code of a function whose source is Python, made when it is first called. What generateUnlinkedFunctionCodeBlock() does for
// JavaScript.
UnlinkedFunctionCodeBlock* generateFunctionCodeBlock(VM&, UnlinkedFunctionExecutable*, const SourceCode&, CodeSpecializationKind, OptionSet<CodeGenerationMode>, ParserError&, SourceParseMode);

// A function of no arguments that runs the body of a module, whose global variables are the properties of `namespaceObject`. All
// of the source is checked first. Null, with SyntaxError raised, if it is not Python.
JS_EXPORT_PRIVATE JSFunction* compileModule(JSGlobalObject*, const SourceCode&, JSObject* namespaceObject);

// Runs a file as `python file.py` would, as the module __main__. If an exception gets away it is reported on stderr as Python
// reports it. Returns what the process should exit with.
JS_EXPORT_PRIVATE int runMain(JSGlobalObject*, const SourceCode&);

} } // namespace JSC::Python
