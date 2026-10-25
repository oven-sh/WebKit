/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
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

#include <wtf/text/WTFString.h>

#if USE(BUN_JSC_ADDITIONS)
#include "JSExportMacros.h"
#include <wtf/Noncopyable.h>
#endif

namespace Inspector {

// Represents a one-way connection from an Inspection or Automation target to
// a local or remote controller (such as a debugger or UI automation script).

class FrontendChannel {
#if USE(BUN_JSC_ADDITIONS)
    // A copy would be a second channel with the same identifier.
    WTF_MAKE_NONCOPYABLE(FrontendChannel);
#endif
public:

    enum class ConnectionType : bool {
        Remote,
        Local
    };

#if USE(BUN_JSC_ADDITIONS)
    FrontendChannel() = default;

    // Names the channel that sent a command, so that FrontendRouter::sendResponse() can send the
    // response to that channel alone. An asynchronous command can outlive its channel, and the
    // next channel can be allocated at the same address, so a pointer cannot do this. An
    // identifier is never used for a second channel.
    using Identifier = uint64_t;
    static constexpr Identifier noIdentifier = 0;
    Identifier identifier() const { return m_identifier; }
#endif

    virtual ~FrontendChannel() { }
    virtual ConnectionType connectionType() const = 0;
    virtual void sendMessageToFrontend(const String& message) = 0;

#if USE(BUN_JSC_ADDITIONS)
private:
    JS_EXPORT_PRIVATE static Identifier generateIdentifier();

    const Identifier m_identifier { generateIdentifier() };
#endif
};

} // namespace Inspector
