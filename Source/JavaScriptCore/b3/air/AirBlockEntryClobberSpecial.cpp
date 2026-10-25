/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirBlockEntryClobberSpecial.h"

#if ENABLE(B3_JIT)

#include "CCallHelpers.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace B3 { namespace Air {

WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(BlockEntryClobberSpecial);

BlockEntryClobberSpecial::BlockEntryClobberSpecial(const RegisterSet& clobberedRegisters)
    : m_clobberedRegisters(clobberedRegisters)
{
}

BlockEntryClobberSpecial::~BlockEntryClobberSpecial() = default;

void BlockEntryClobberSpecial::forEachArg(Inst&, NOESCAPE const ScopedLambda<Inst::EachArgCallback>&)
{
}

bool BlockEntryClobberSpecial::isValid(Inst& inst)
{
    return inst.args().size() == 1;
}

bool BlockEntryClobberSpecial::admitsStack(Inst&, unsigned)
{
    return false;
}

bool BlockEntryClobberSpecial::admitsExtendedOffsetAddr(Inst&, unsigned)
{
    return false;
}

void BlockEntryClobberSpecial::reportUsedRegisters(Inst&, const RegisterSet&)
{
}

MacroAssembler::Jump BlockEntryClobberSpecial::generate(Inst&, CCallHelpers&, GenerationContext&)
{
    return { };
}

RegisterSet BlockEntryClobberSpecial::extraEarlyClobberedRegs(Inst&)
{
    return m_clobberedRegisters;
}

RegisterSet BlockEntryClobberSpecial::extraClobberedRegs(Inst&)
{
    return { };
}

bool BlockEntryClobberSpecial::hasFixedForms(Inst&)
{
    return true;
}

bool BlockEntryClobberSpecial::needsFrame(Inst&)
{
    return true;
}

bool BlockEntryClobberSpecial::clobbersAtBlockEntry(Inst&)
{
    return true;
}

bool BlockEntryClobberSpecial::hasNonArgEffects(Inst&)
{
    return true;
}

bool BlockEntryClobberSpecial::hasNonArgNonControlEffects(Inst&)
{
    return true;
}

void BlockEntryClobberSpecial::dumpImpl(PrintStream& out) const
{
    out.print("BlockEntryClobber");
}

void BlockEntryClobberSpecial::deepDumpImpl(PrintStream& out) const
{
    out.print("Clobbers on entry: ", m_clobberedRegisters);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
