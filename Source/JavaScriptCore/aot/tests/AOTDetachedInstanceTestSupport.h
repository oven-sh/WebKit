/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTNativeTest.h"
#include "AOTRealmTestSupport.h"
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

enum class TestRealm : bool { Plain, Locked };

class DetachedInstance {
    WTF_MAKE_NONCOPYABLE(DetachedInstance);
public:
    explicit DetachedInstance(TestRealm realm)
        : m_realm(realm == TestRealm::Locked ? m_realms.locked() : m_realms.plain())
        , m_instance(static_cast<Instance*>(fastZeroedMalloc(sizeof(Instance))))
    {
        m_instance->vm = &m_realms.vm();
        m_instance->globalObject = m_realm;
    }

    ~DetachedInstance() { fastFree(m_instance); }

    VM& vm() { return m_realms.vm(); }
    JSGlobalObject* globalObject() { return m_realm; }
    Instance& instance() { return *m_instance; }
    JSValue evaluate(ASCIILiteral source) { return m_realms.evaluate(m_realm, source); }

private:
    Realms m_realms;
    JSGlobalObject* m_realm;
    Instance* m_instance;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
