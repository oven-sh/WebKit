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

#include "WriteBarrier.h"

namespace JSC {

class PyType;

namespace Python {

#define FOR_EACH_PYTHON_IO_TYPE(v) \
    v(ioBase) v(rawIOBase) v(bufferedIOBase) v(textIOBase) v(fileIO) v(bytesIO) v(bytesIOBuffer) v(bufferedReader) v(bufferedWriter) v(bufferedRWPair) v(bufferedRandom) \
    v(incrementalNewlineDecoder) v(textIOWrapper) v(stringIO) v(unsupportedOperation)

// What _io has for each realm: _PyIO_State of CPython's Modules/_io/_iomodule.h. The classes are made when one of them is first wanted.
struct IOModuleState {
#define DECLARE(name) WriteBarrier<PyType> name;
    FOR_EACH_PYTHON_IO_TYPE(DECLARE)
#undef DECLARE

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
#define VISIT(name) visitor.append(name);
        FOR_EACH_PYTHON_IO_TYPE(VISIT)
#undef VISIT
    }
};

} } // namespace JSC::Python
