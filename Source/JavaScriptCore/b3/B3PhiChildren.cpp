/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
#include "B3PhiChildren.h"

#if ENABLE(B3_JIT)

#include "B3ValueInlines.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace B3 {

WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(PhiChildren);

PhiChildren::PhiChildren(Procedure& proc)
{
    if (!proc.hasEverAdded(Upsilon))
        return;
    m_ranges.grow(proc.values().size());
    Vector<UpsilonValue*, 64> upsilons;
    for (Value* value : proc.values()) {
        if (UpsilonValue* upsilon = value->as<UpsilonValue>()) {
            upsilons.append(upsilon);
            if (!m_ranges[upsilon->phi()->index()].size++)
                m_phis.append(upsilon->phi());
        }
    }
    unsigned begin = 0;
    for (Value* phi : m_phis) {
        Range& range = m_ranges[phi->index()];
        range.begin = begin;
        begin += std::exchange(range.size, 0);
    }
    m_upsilons.grow(upsilons.size());
    for (UpsilonValue* upsilon : upsilons) {
        Range& range = m_ranges[upsilon->phi()->index()];
        m_upsilons[range.begin + range.size++] = upsilon;
    }
}

PhiChildren::~PhiChildren() = default;

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

