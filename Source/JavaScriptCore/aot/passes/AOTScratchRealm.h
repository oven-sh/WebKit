/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include "AOTKnownValue.h"
#include "AOTRegExpFlags.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "RegExpObject.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT { namespace Typed {

// A realm of the build's own, on the thread that folds. A built-in that depends on its operands alone answers there what it answers in the program.
class ScratchRealm {
    WTF_MAKE_NONCOPYABLE(ScratchRealm);
public:
    struct Operand {
        KnownValue primitive;
        std::optional<RegExpLiteralInType> regExp;
        // An array of the realm is made of them, anew for each call.
        std::optional<std::span<const StringID>> listOfTexts { };
    };

    // The text is set for a string. It is a copy that no table of the realm knows.
    struct Answer {
        KnownValue primitive;
        String text;
        // A literal is flat, and is held in 16 bits only if it has a character outside Latin-1. Whoever asks for the characters alone need not ask this.
        bool isStoredLikeLiteral { true };
    };

    ScratchRealm()
        : m_vm(VM::create(HeapType::Small))
    {
        JSLockHolder locker(*m_vm);
        m_realm.set(*m_vm, JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())));
    }

    ~ScratchRealm()
    {
        // The lock holder is left with the last reference, so the VM dies under its lock.
        JSLockHolder locker(*m_vm);
        m_realm.clear();
        m_vm = nullptr;
    }

    // How long the answer of a built-in can be where a number among its operands sets that: asked before the answer is made. A number is a place in a text everywhere else, and the answer is
    // measured when it is there. `bytesOfOperands`: of the receiver and the texts among the arguments. `number`: the first argument.
    static double bytesOfAnswerSetByNumber(BuiltinID function, uint64_t bytesOfOperands, double number)
    {
        if (!(number > 1))
            return static_cast<double>(bytesOfOperands);
        if (function == Builtins::StringPrototypeRepeat)
            return static_cast<double>(bytesOfOperands) * number;
        if (function == Builtins::StringPrototypePadStart || function == Builtins::StringPrototypePadEnd)
            return std::max(static_cast<double>(bytesOfOperands), number * sizeof(char16_t));
        return static_cast<double>(bytesOfOperands);
    }

    // An answer that is a list of strings is given to whoever names a place for it, and is no answer for anybody else.
    std::optional<Answer> call(const Program& program, BuiltinID function, KnownValue receiver, std::span<const Operand> arguments, Vector<String>* listOfTexts = nullptr)
    {
        return call(program, function, Operand { receiver, std::nullopt }, arguments, listOfTexts);
    }

    std::optional<Answer> call(const Program& program, BuiltinID function, const Operand& receiver, std::span<const Operand> arguments, Vector<String>* listOfTexts = nullptr)
    {
        VM& vm = *m_vm;
        JSLockHolder locker(vm);
        JSGlobalObject* realm = m_realm.get();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        auto clearException = makeScopeExit([&] {
            scope.clearException();
        });

        auto callee = findInRealm(realm, BuiltinCellKey::object(function));
        if (!callee)
            return std::nullopt;
        CallData callData = getCallData(*callee);
        if (callData.type == CallData::Type::None)
            return std::nullopt;

        MarkedArgumentBuffer list;
        auto valueOf = [&](const Operand& operand) -> JSValue {
            if (operand.regExp)
                return createRegExp(program, *operand.regExp);
            if (!operand.listOfTexts)
                return toJSValue(program, operand.primitive);
            MarkedArgumentBuffer texts;
            for (StringID text : *operand.listOfTexts)
                texts.append(toJSValue(program, KnownValue::string(text)));
            if (texts.hasOverflowed())
                return { };
            JSValue array = constructArray(realm, static_cast<ArrayAllocationProfile*>(nullptr), texts);
            return scope.exception() ? JSValue() : array;
        };
        for (const Operand& operand : arguments) {
            JSValue argument = valueOf(operand);
            if (!argument)
                return std::nullopt;
            list.append(argument);
        }
        if (list.hasOverflowed())
            return std::nullopt;
        JSValue thisValue = valueOf(receiver);
        if (!thisValue)
            return std::nullopt;

        NakedPtr<Exception> exception;
        JSValue result = JSC::call(realm, *callee, callData, thisValue, list, exception);
        if (exception)
            return std::nullopt;

        if (result.isString()) {
            bool mayHaveNarrowParts = !asString(result)->is8Bit() && asString(result)->isRope() && !asString(result)->isSubstring();
            auto text = asString(result)->value(realm);
            if (scope.exception())
                return std::nullopt;
            return Answer { { }, text->isolatedCopy(), !mayHaveNarrowParts && (text->is8Bit() || !text->containsOnlyLatin1()) };
        }
        if (JSArray* list = listOfTexts ? dynamicDowncast<JSArray>(result) : nullptr) {
            for (unsigned index = 0; index < list->length(); ++index) {
                JSValue element = list->getIndex(realm, index);
                if (scope.exception() || !element.isString())
                    return std::nullopt;
                auto text = asString(element)->value(realm);
                if (scope.exception())
                    return std::nullopt;
                listOfTexts->append(text->isolatedCopy());
            }
            return Answer { };
        }
        if (result.isUndefined())
            return Answer { KnownValue::undefined(), { } };
        if (result.isNull())
            return Answer { KnownValue::null(), { } };
        if (result.isBoolean())
            return Answer { KnownValue::boolean(result.asBoolean()), { } };
        if (KnownValue number = result.isNumber() ? KnownValue::computedNumber(result.asNumber()) : KnownValue(); number.kind() != KnownValue::Kind::Unknown)
            return Answer { number, { } };
        return std::nullopt;
    }

    // For what asks the engine itself. What it hands back holds nothing of the realm.
    auto run(NOESCAPE const Invocable<void(JSGlobalObject*)> auto& functor)
    {
        JSLockHolder locker(*m_vm);
        return functor(m_realm.get());
    }

private:
    JSValue toJSValue(const Program& program, KnownValue value)
    {
        switch (value.kind()) {
        case KnownValue::Kind::Undefined:
            return jsUndefined();
        case KnownValue::Kind::Null:
            return jsNull();
        case KnownValue::Kind::Boolean:
            return jsBoolean(value.asBoolean());
        case KnownValue::Kind::Integer:
        case KnownValue::Kind::Double:
            return jsNumber(value.asNumber());
        case KnownValue::Kind::String: {
            String text = program.strings.toWTFString(value.asString());
            return jsString(*m_vm, text.is8Bit() || !text.containsOnlyLatin1() ? text : String::make8Bit(text.span16()));
        }
        case KnownValue::Kind::Unknown:
        case KnownValue::Kind::Object:
        case KnownValue::Kind::Function:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    JSValue createRegExp(const Program& program, RegExpLiteralInType literal)
    {
        RegExp* regExp = RegExp::create(*m_vm, program.strings.toWTFString(literal.pattern), toYarrFlags(literal.flags()));
        if (!regExp->isValid())
            return { };
        return RegExpObject::create(*m_vm, m_realm->regExpStructure(), regExp);
    }

    RefPtr<VM> m_vm;
    Strong<JSGlobalObject> m_realm;
};

class ScratchRealms {
    WTF_MAKE_NONCOPYABLE(ScratchRealms);
public:
    ScratchRealms() = default;

    ScratchRealm& ofCurrentThread()
    {
        Locker locker { m_lock };
        return *m_realms.ensure(&Thread::currentSingleton(), [] {
            return makeUniqueWithoutFastMallocCheck<ScratchRealm>();
        }).iterator->value;
    }

private:
    Lock m_lock;
    HashMap<Thread*, std::unique_ptr<ScratchRealm>> m_realms WTF_GUARDED_BY_LOCK(m_lock);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
