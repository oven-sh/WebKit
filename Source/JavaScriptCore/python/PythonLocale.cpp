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


#include "config.h"
#include "PythonLocale.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyStateObject.h"

namespace JSC { namespace Python {

#if OS(UNIX)

namespace {

struct LocaleState final : NativeState {
    PYTHON_NATIVE_STATE(LocaleState);
    ~LocaleState()
    {
        if (characters)
            freelocale(characters);
    }
    locale_t characters { nullptr };
    bool isLookedFor { false };
};

template<typename Visitor> void LocaleState::visit(Visitor&) { }

}

locale_t characterLocale(JSGlobalObject* globalObject)
{
    auto& state = globalObject->pyRealm()->moduleState<LocaleState>();
    if (state.isLookedFor)
        return state.characters;
    state.isLookedFor = true;
    state.characters = newlocale(LC_CTYPE_MASK, "", nullptr);
    // _Py_CoerceLegacyCLocale(): if the environment asks for none in particular, or for one that there is not, it is one in which text is UTF-8. Which it asks for is found as setlocale() finds it.
    const char* asked = nullptr;
    for (const char* variable : { "LC_ALL", "LC_CTYPE", "LANG" }) {
        asked = getenv(variable);
        if (asked && *asked)
            break;
        asked = nullptr;
    }
    if (!state.characters || !asked || !strcmp(asked, "C") || !strcmp(asked, "POSIX")) {
        for (const char* name : { "C.UTF-8", "C.utf8", "UTF-8" }) {
            if (locale_t coerced = newlocale(LC_CTYPE_MASK, name, nullptr)) {
                if (state.characters)
                    freelocale(state.characters);
                state.characters = coerced;
                break;
            }
        }
    }
    return state.characters;
}

#endif // OS(UNIX)

} } // namespace JSC::Python
