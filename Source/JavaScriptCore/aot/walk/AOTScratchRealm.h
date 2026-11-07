/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTFoldingOfConstants.h"
#include "AOTRuntimeHooks.h"
#include "AOTSchemaInlineBuiltins.h"
#include "JSCJSValue.h"
#include "JSLock.h"
#include "Strong.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;
class VM;

namespace AOT { namespace Typed {

class TypeArena;

class ScratchRealm {
    WTF_MAKE_NONCOPYABLE(ScratchRealm);
public:
    struct Operand {
        PrimitiveKnownToFolding primitive;
    };

    struct Answer {
        PrimitiveKnownToFolding primitive;
        String text;
        bool isStoredLikeLiteral { true };
    };

    ScratchRealm();
    ~ScratchRealm();

    static double bytesOfAnswerSetByNumber(std::optional<InlineBuiltin> codeInLine, uint64_t bytesOfOperands, double number);

    std::optional<Answer> call(const TypeArena&, BuiltinID function, const Operand& receiver, std::span<const Operand> arguments);

    auto run(NOESCAPE const Invocable<void(JSGlobalObject*)> auto& functor)
    {
        makeAtFirstQuestion();
        JSLockHolder locker(*m_vm);
        return functor(m_realm.get());
    }

private:
    void makeAtFirstQuestion();
    JSValue toJSValue(const TypeArena&, const Operand&);

    std::optional<RuntimeHooks::NoHelperThreadsScope> m_noHelperThreads;
    RefPtr<VM> m_vm;
    Strong<JSGlobalObject> m_realm;
    std::optional<JSLockHolder> m_lockOfWholeLife;
};

} } // namespace AOT::Typed
} // namespace JSC

#endif // ENABLE(AOT)
