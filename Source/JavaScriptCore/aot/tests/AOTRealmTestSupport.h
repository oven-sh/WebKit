/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "ErrorType.h"
#include "Identifier.h"
#include "JSArray.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "Strong.h"
#include <optional>
#include <wtf/RefPtr.h>

namespace JSC::AOT {

#define AOT_TEST_CHECK_FOR_SOURCE(condition, source) do { \
        if (!(condition)) [[unlikely]] \
            ::JSC::AOT::failNativeTest(__FILE__, __LINE__, (source).characters()); \
    } while (false)

enum class RuntimeValidation : bool { Off, On };

class Realms {
public:
    explicit Realms(RuntimeValidation = RuntimeValidation::Off);
    ~Realms();

    VM& vm() { return *m_vm; }
    JSGlobalObject* locked() { return m_locked.get(); }
    JSGlobalObject* plain() { return m_plain.get(); }

    JSValue evaluate(JSGlobalObject*, ASCIILiteral source);
    bool throwsTypeError(JSGlobalObject* realm, ASCIILiteral source) { return throws(realm, source, ErrorType::TypeError); }
    bool throwsSyntaxError(JSGlobalObject* realm, ASCIILiteral source) { return throws(realm, source, ErrorType::SyntaxError); }
    JSObject* object(JSGlobalObject* realm, ASCIILiteral expression) { return asObject(evaluate(realm, expression)); }
    JSArray* array(JSGlobalObject* realm, ASCIILiteral expression) { return uncheckedDowncast<JSArray>(evaluate(realm, expression)); }
    JSObject* holder(JSGlobalObject*, ASCIILiteral expression);
    Identifier key(JSGlobalObject*, ASCIILiteral expression);

private:
    bool throws(JSGlobalObject*, ASCIILiteral source, ErrorType);

    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
    Strong<JSGlobalObject> m_plain;
    Strong<JSGlobalObject> m_locked;
};

} // namespace JSC::AOT

#endif // ENABLE(AOT)
