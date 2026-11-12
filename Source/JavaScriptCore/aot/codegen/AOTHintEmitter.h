/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class HintEmitter final : public OperationEmitter {
public:
    explicit HintEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateLoopHint(const ValueContext&);
    void generateEndCellLifetime(const ValueContext&);
    void generateReleaseArray(const ValueContext&);
    void generateReleaseList(const ValueContext&);
    void generateGuessFailsUnasked(const ValueContext&);
    void generateBeginEffectsCheck(const ValueContext&);
    void generateCheckIterationRestart(const ValueContext&);
    void generateCheckInlineeFallback(const ValueContext&);
    void generateCheckReloadBypass(const ValueContext&);
    void generateAskObjectAgain(const ValueContext&);
    bool shouldEmitLifetimeEnds();
    void emitLifetimeEnd(const OperationContext&, ValueID cell);
    void noteThatCellIsLent(const ValueContext& made, CellPointer);
    void crashIfLifetimeEnded(const OperationContext&, CellPointer);
    void pollWhereNoFrameIsSetUp(const OperationContext&);

private:
    void emitPoll(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
