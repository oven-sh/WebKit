/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirSpecial.h"
#include "RegisterSet.h"
#include <wtf/SequesteredMalloc.h>

namespace JSC { namespace B3 { namespace Air {

class BlockEntryClobberSpecial final : public Special {
    WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED(BlockEntryClobberSpecial);
public:
    JS_EXPORT_PRIVATE explicit BlockEntryClobberSpecial(const RegisterSet&);
    JS_EXPORT_PRIVATE ~BlockEntryClobberSpecial() final;

    const RegisterSet& clobberedRegisters() const { return m_clobberedRegisters; }

private:
    void forEachArg(Inst&, NOESCAPE const ScopedLambda<Inst::EachArgCallback>&) final;
    bool isValid(Inst&) final;
    bool admitsStack(Inst&, unsigned argIndex) final;
    bool admitsExtendedOffsetAddr(Inst&, unsigned argIndex) final;
    void reportUsedRegisters(Inst&, const RegisterSet&) final;
    MacroAssembler::Jump generate(Inst&, CCallHelpers&, GenerationContext&) final;
    RegisterSet extraEarlyClobberedRegs(Inst&) final;
    RegisterSet extraClobberedRegs(Inst&) final;
    bool hasFixedForms(Inst&) final;
    bool needsFrame(Inst&) final;
    bool clobbersAtBlockEntry(Inst&) final;
    bool hasNonArgEffects(Inst&) final;
    bool hasNonArgNonControlEffects(Inst&) final;
    void dumpImpl(PrintStream&) const final;
    void deepDumpImpl(PrintStream&) const final;

    RegisterSet m_clobberedRegisters;
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
