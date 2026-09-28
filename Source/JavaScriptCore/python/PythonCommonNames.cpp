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
#include "PythonCommonNames.h"

#include "VM.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/SymbolImpl.h>

namespace JSC { namespace Python {

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(CommonNames);

CommonNames::CommonNames(VM& vm)
    :
#define INITIALIZE(name) dunder_##name(Identifier::fromString(vm, "__" #name "__"_s)),
    FOR_EACH_PYTHON_DUNDER_NAME(INITIALIZE)
#undef INITIALIZE
#define INITIALIZE(name) private_##name(Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "py" #name ""_s))),
    FOR_EACH_PYTHON_PRIVATE_NAME(INITIALIZE)
#undef INITIALIZE
#define INITIALIZE(name, attribute) field_##name(Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, attribute ""_s))),
    FOR_EACH_PYTHON_EXCEPTION_FIELD(INITIALIZE)
#undef INITIALIZE
    globals(Identifier::fromString(vm, ".globals"_s))
    , m_binaryMethods { &dunder_add, &dunder_sub, &dunder_mul, &dunder_matmul, &dunder_truediv, &dunder_mod, &dunder_pow, &dunder_lshift, &dunder_rshift, &dunder_or, &dunder_xor, &dunder_and, &dunder_floordiv }
    , m_reflectedMethods { &dunder_radd, &dunder_rsub, &dunder_rmul, &dunder_rmatmul, &dunder_rtruediv, &dunder_rmod, &dunder_rpow, &dunder_rlshift, &dunder_rrshift, &dunder_ror, &dunder_rxor, &dunder_rand, &dunder_rfloordiv }
    , m_inPlaceMethods { &dunder_iadd, &dunder_isub, &dunder_imul, &dunder_imatmul, &dunder_itruediv, &dunder_imod, &dunder_ipow, &dunder_ilshift, &dunder_irshift, &dunder_ior, &dunder_ixor, &dunder_iand, &dunder_ifloordiv }
{
}

} } // namespace JSC::Python
