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

#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

// A string can be only so long, and nearly all text that is put together here has in it something that is as long as a program makes it: a name, a key, the repr() of something. makeString() and StringBuilder
// bring everything down when it comes to too much. So it is concatenate() that is used, and TextBuilder, which give a null string then, as JavaScriptCore does where the length is not its own to decide.
// A null string that goes into either makes what comes out null, so that it does not matter how far down it was that there was no room. What is done with a null string in the end:
//
//   - If it was to be what an exception says, MemoryError is raised in place of the exception. createException() sees to that, so nothing need be done where it is raised.
//   - If it was to be a str, strOrMemoryError() raises MemoryError.
//   - If it is returned as text, by something that says that it raised by returning null, textOrMemoryError() raises MemoryError.
template<typename Part> bool isNullText(const Part&) { return false; }
inline bool isNullText(const String& text) { return text.isNull(); }

// tryMakeString(), but for what it makes of a null string, which to that is an empty one.
template<typename... Parts>
String concatenate(const Parts&... parts)
{
    if ((isNullText(parts) || ...)) [[unlikely]]
        return { };
    return tryMakeString(parts...);
}

class TextBuilder final : public StringBuilder {
public:
    TextBuilder()
        : StringBuilder(OverflowPolicy::RecordOverflow)
    {
    }

    template<typename... Parts>
    void append(const Parts&... parts)
    {
        if ((isNullText(parts) || ...)) [[unlikely]]
            didOverflow();
        else
            StringBuilder::append(parts...);
    }

    // What was built. Null if there was no room for it.
    String tryFinish() { return hasOverflowed() ? String() : isEmpty() ? emptyString() : toString(); }
    // The same, and then MemoryError has been raised.
    String finish(JSGlobalObject*);
};

} } // namespace JSC::Python
