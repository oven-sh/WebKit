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

#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonFileOperations.h"

// What the parts of _io share: Modules/_io/_iomodule.h of CPython.

namespace JSC { namespace Python {

static constexpr int64_t defaultBufferSize = 128 * KB; // DEFAULT_BUFFER_SIZE

// The classes are there when this returns.
IOModuleState& ioState(JSGlobalObject*);
const FileOperations* fileOperations(JSGlobalObject*);

// Each sets up some of the classes.
void initializeFileIO(JSGlobalObject*, IOModuleState&);
void initializeBytesIO(JSGlobalObject*, IOModuleState&);
void initializeBufferedIO(JSGlobalObject*, IOModuleState&);
void initializeTextIO(JSGlobalObject*, IOModuleState&);
void initializeStringIO(JSGlobalObject*, IOModuleState&);

JSValue raiseUnsupportedOperation(JSGlobalObject*, ThrowScope&, const String& message);

// object.name(...)
JSValue callMethodNamed(JSGlobalObject*, JSValue object, const Identifier& name);
JSValue callMethodNamed(JSGlobalObject*, JSValue object, const Identifier& name, JSValue);
JSValue callMethodNamed(JSGlobalObject*, JSValue object, const Identifier& name, JSValue, JSValue);

// PyErr_GetRaisedException(): what has been raised, which is raised no longer. Null if nothing has, or if it is that the thread is to do no more, which stays as it is.
Exception* takeRaisedException(VM&);
// PyErr_SetRaisedException()
void restoreRaisedException(JSGlobalObject*, Exception*);
// _PyErr_ChainExceptions1(): what was taken is raised again, unless something else has been since, and then it is the __context__ of that.
void chainRaisedExceptions(JSGlobalObject*, Exception* taken);
// _PyIO_trap_eintr(): whether what has been raised is that a system call was interrupted, which is then raised no longer.
bool trapInterruptedError(JSGlobalObject*);
// PyErr_CheckSignals(): what there is to see to when a system call has been interrupted. False if it raised.
bool checkSignals(JSGlobalObject*);

// PyNumber_AsOff_t(). If it does not fit, `overflow` is raised, or if there is none it is the most or the least that does.
std::optional<int64_t> toOffset(JSGlobalObject*, JSValue, std::optional<BuiltinType> overflow);
// `Py_buffer(accept={rwbuffer})`
Buffer writableBufferArgument(JSGlobalObject*, JSValue, ASCIILiteral function, ASCIILiteral argument = "argument"_s);
inline std::span<uint8_t> mutableSpanOf(const Buffer& buffer)
{
    auto span = buffer.span();
    return { const_cast<uint8_t*>(span.data()), span.size() };
}

// iobase_check_closed(): false, having raised, if it says that it is closed.
bool checkIsNotClosed(JSGlobalObject*, JSValue);
// _PyIOBase_check_readable() and the like: false, having raised, if it says that it is not.
bool checkIsReadable(JSGlobalObject*, JSValue);
bool checkIsWritable(JSGlobalObject*, JSValue);
bool checkIsSeekable(JSGlobalObject*, JSValue);
JSC_DECLARE_HOST_FUNCTION(ioCannotPickle); // __getstate__ and __reduce__ of what cannot be

// _PyFileIO_closed()
bool isFileIOClosed(JSValue);

// io.open(). A null String is None. Empty if it raised.
JSValue openFile(JSGlobalObject*, JSValue file, const String& mode, int buffering, const String& encoding, const String& errors, const String& newline, bool closesDescriptor, JSValue opener);
// PyFile_OpenCodeObject(): io.open_code()
JSValue openCode(JSGlobalObject*, JSValue path);

// ---- Text

// _PyIncrementalNewlineDecoder_decode(): the decode() of an IncrementalNewlineDecoder, without going by way of the method.
JSValue decodeNewlines(JSGlobalObject*, JSValue decoder, JSValue input, bool isFinal);

// How lines end, to what is reading them.
struct LineEndings {
    bool isTranslated { false }; // They have all been made "\n" already.
    bool isUniversal { false }; // Any of "\r", "\n" and "\r\n" is one.
    String readNewline; // Otherwise it is this. It is ASCII.
};
// _PyIO_find_line_ending(): how far it is to just after the first line ending in some characters. If there is none, nothing, and `consumed` is how many of them can be put aside without being looked at again.
template<typename Character>
std::optional<size_t> findLineEnding(const LineEndings&, std::span<const Character>, size_t& consumed);

// Whether it will do for the `newline` of TextIOWrapper() and StringIO(). A null String is None.
inline bool isValidNewline(const String& newline)
{
    return newline.isNull() || newline.isEmpty() || newline == "\n"_s || newline == "\r"_s || newline == "\r\n"_s;
}

// A bool that is a field of the C struct: Py_T_BOOL. False if it raised.
bool toBoolMember(JSGlobalObject*, JSValue, bool& result);

} } // namespace JSC::Python
