/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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
#include "AirInst.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirFormTable.h"
#include "AirInstInlines.h"
#include <wtf/ListDump.h>

namespace JSC { namespace B3 { namespace Air {

void Inst::fillForms()
{
    ASSERT(kind.opcode == Patch && m_size);
    if (m_size > maxNumberOfForms + 1)
        return;
    setFormsState(FormsState::NotFixed);
    if (!args()[0].special()->hasFixedForms(*this))
        return;
    std::array<uint8_t, maxNumberOfForms + 1> forms;
    unsigned next = 0;
    bool isInOrder = true;
    forEachArgCustom(
        [&] (Arg& arg, Arg::Role role, Bank bank, Width width) {
            if (next >= m_size || &arg != &args()[next]) {
                isInOrder = false;
                return;
            }
            forms[next++] = ENCODE_INST_FORM(role, bank, width);
        });
    if (!isInOrder || next != m_size)
        return;
    if (forms[0] != ENCODE_INST_FORM(Arg::Use, GP, pointerWidth()))
        return;
    if (m_size == 1) {
        setFormsState(FormsState::NoArguments);
        return;
    }
    memcpy(formsAfterSpecial(), forms.data() + 1, m_size - 1);
}

const uint8_t* Inst::formsKeptByCode()
{
    ASSERT(kind.opcode == Patch && m_size > maxNumberOfForms + 1);
    Special* special = args()[0].special();
    if (!origin)
        return nullptr;
    Code::FormsOfPatch& kept = special->code().formsOfPatches().add(origin, Code::FormsOfPatch()).iterator->value;
    if (kept.special == special && kept.formsAfterSpecial.size() == m_size - 1)
        return kept.areFixed ? kept.formsAfterSpecial.span().data() : nullptr;
    kept.special = special;
    kept.areFixed = false;
    kept.formsAfterSpecial.resize(m_size - 1);
    if (!special->hasFixedForms(*this))
        return nullptr;
    unsigned next = 0;
    bool isInOrder = true;
    forEachArgCustom(
        [&] (Arg& arg, Arg::Role role, Bank bank, Width width) {
            if (next >= m_size || &arg != &args()[next]) {
                isInOrder = false;
                return;
            }
            uint8_t form = ENCODE_INST_FORM(role, bank, width);
            if (next)
                kept.formsAfterSpecial[next - 1] = form;
            else
                isInOrder = form == ENCODE_INST_FORM(Arg::Use, GP, pointerWidth());
            ++next;
        });
    kept.areFixed = isInOrder && next == m_size;
    return kept.areFixed ? kept.formsAfterSpecial.span().data() : nullptr;
}

bool Inst::hasCurrentForms()
{
    ASSERT(kind.opcode == Patch && m_size);
    const uint8_t* keptForms = m_size > maxNumberOfForms + 1 ? formsKeptByCode() : formsState() == FormsState::Fixed ? formsAfterSpecial() : nullptr;
    if (!keptForms)
        return true;
    std::span<const uint8_t> forms = unsafeMakeSpan(keptForms, m_size - 1);
    unsigned next = 0;
    bool isCurrent = true;
    forEachArgCustom(
        [&] (Arg&, Arg::Role role, Bank bank, Width width) {
            if (next && next < m_size)
                isCurrent &= forms[next - 1] == ENCODE_INST_FORM(role, bank, width);
            ++next;
        });
    return isCurrent && next == m_size;
}

bool Inst::hasEarlyDef()
{
    if (kind.opcode == Patch && !extraEarlyClobberedRegs().isEmpty())
        return true;
    bool result = false;
    forEachArg(
        [&] (Arg&, Arg::Role role, Bank, Width) {
            result |= Arg::isEarlyDef(role);
        });
    return result;
}

bool Inst::hasLateUseOrDef()
{
    if (kind.opcode == Patch && !extraClobberedRegs().isEmpty())
        return true;
    bool result = false;
    forEachArg(
        [&] (Arg&, Arg::Role role, Bank, Width) {
            result |= Arg::isLateUse(role) || Arg::isLateDef(role);
        });
    return result;
}

Inst::PaddingSummary Inst::paddingSummary()
{
    PaddingSummary summary;
    if (kind.opcode == Patch) {
        summary.hasEarlyDef = !extraEarlyClobberedRegs().isEmpty();
        summary.hasLateUseOrDef = !extraClobberedRegs().isEmpty();
        if (summary.hasEarlyDef && summary.hasLateUseOrDef)
            return summary;
    }
    forEachArg(
        [&] (Arg&, Arg::Role role, Bank, Width) {
            summary.hasEarlyDef |= Arg::isEarlyDef(role);
            summary.hasLateUseOrDef |= Arg::isLateUse(role) || Arg::isLateDef(role);
        });
    return summary;
}

bool Inst::hasArgEffects()
{
    bool result = false;
    forEachArg(
        [&] (Arg&, Arg::Role role, Bank, Width) {
            if (Arg::isAnyDef(role))
                result = true;
        });
    return result;
}

unsigned Inst::jsHash() const
{
    // FIXME: This should do something for flags.
    // https://bugs.webkit.org/show_bug.cgi?id=162751
    unsigned result = static_cast<unsigned>(kind.opcode);
    
    for (const Arg& arg : args())
        result += arg.jsHash();
    
    return result;
}

void Inst::dump(PrintStream& out) const
{
    out.print(kind, " ", listDump(args()));
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
