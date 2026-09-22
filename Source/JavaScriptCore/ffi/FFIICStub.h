/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)

#include "CodeLocation.h"
#include "JITCode.h"
#include <wtf/Forward.h>
#include <wtf/RefPtr.h>

namespace JSC {

class JSGlobalObject;
class VM;

namespace FFI {

class Signature;

// The stub bakes the target in, and callers link straight to its entry, so a closed function cannot be
// kept out of it by a check the callers make. close() overwrites the start of the fast path with a jump
// to the stub's slow path, which reaches ffiCall() and throws. An open function pays nothing for this.
class ICStubCode final : public DirectJITCode {
public:
    ICStubCode(CodeRef<JSEntryPtrTag>, CodeLocationLabel<JSInternalPtrTag> fastPath, CodeLocationLabel<JSInternalPtrTag> slowPath);

    void close();

private:
    CodeLocationLabel<JSInternalPtrTag> m_fastPath;
    CodeLocationLabel<JSInternalPtrTag> m_slowPath;
};

RefPtr<ICStubCode> generateICStubCode(VM&, JSGlobalObject*, Signature&, void* target);

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)
