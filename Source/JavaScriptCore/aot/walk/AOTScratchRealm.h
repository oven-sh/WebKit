/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTFoldingOfConstants.h"
#include "AOTRuntimeHooks.h"
#include "AOTSchemaEnums.h"
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

// A realm of the build's own, on the thread that asks. Nothing that a call leaves in it changes what a later call answers.
class ScratchRealm {
    WTF_MAKE_NONCOPYABLE(ScratchRealm);
public:
    struct RegExpLiteral {
        StringID pattern;
        OptionSet<RegExpFlags> flags;
    };

    struct Operand {
        PrimitiveKnownToFolding primitive;
        // A RegExp of the realm is made of it, anew for each call.
        std::optional<RegExpLiteral> regExp { };
    };

    // The text is set for a string. It is a copy that no table of the realm knows.
    struct Answer {
        PrimitiveKnownToFolding primitive;
        String text;
        // A literal is flat, and is held in 16 bits only if it has a character outside Latin-1.
        bool isStoredLikeLiteral { true };
    };

    // It makes nothing: the VM and the realm are made at the first question. AOTWalkStringFoldsByEngine.cpp.
    ScratchRealm();
    ~ScratchRealm();

    // How long the answer of a built-in can be where a number among its operands sets that: asked before the answer is made. `bytesOfOperands`: of the receiver and the texts among the arguments. `number`: the first argument.
    static double bytesOfAnswerSetByNumber(std::optional<InlineBuiltin> codeInLine, uint64_t bytesOfOperands, double number);

    // Each primitive among the operands is known, and a text among them is handed over as a literal is stored.
    // Nothing: the realm has no such function, the call throws, or what it answers is no undefined, null, boolean, number or string.
    std::optional<Answer> call(const TypeArena&, BuiltinID function, const Operand& receiver, std::span<const Operand> arguments);

    // What it hands back holds nothing of the realm.
    auto run(NOESCAPE const Invocable<void(JSGlobalObject*)> auto& functor)
    {
        makeAtFirstQuestion();
        JSLockHolder locker(*m_vm);
        return functor(m_realm.get());
    }

private:
    void makeAtFirstQuestion();
    JSValue toJSValue(const TypeArena&, const Operand&);

    // From before the VM is made until it is dead.
    std::optional<RuntimeHooks::NoHelperThreadsScope> m_noHelperThreads;
    RefPtr<VM> m_vm;
    Strong<JSGlobalObject> m_realm;
    // Held by the thread of the worker as long as the VM lives.
    std::optional<JSLockHolder> m_lockOfWholeLife;
};

} } // namespace AOT::Typed
} // namespace JSC

#endif // ENABLE(AOT)
