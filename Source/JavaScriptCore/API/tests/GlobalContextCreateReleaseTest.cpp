/*
 * Copyright (C) 2026 Oven LLC. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GlobalContextCreateReleaseTest.h"

#include "JavaScript.h"
#include <stdio.h>

// Regression test for oven-sh/bun#30434.
//
// JSGlobalContextCreate(nullptr) + JSGlobalContextRelease() is the
// standard entry/exit for non-Bun consumers of the standalone JSC C API.
// It must not crash or deadlock on teardown.
//
// The bug: VM's constructor installs the VM-owned m_atomStringTable on
// the current thread so CommonIdentifiers, Structure property names, etc.
// get interned into it, then restores the thread's previous table after
// init. But the destructor ran heap.lastChanceToFinalize() without
// reinstalling m_atomStringTable, so PropertyTable::~PropertyTable
// derefs trying to remove atoms from the WRONG (thread-original) table
// and trip AtomStringImpl::remove's RELEASE_ASSERT:
//   "The string being removed is an atom in the string table of an
//    other thread!"
//
// For VMType::Default (the VM::tryCreate path Bun itself uses),
// m_atomStringTable is the thread's own table, so the path is silent
// there. Only VMType::APIContextGroup — the C API
// JSGlobalContextCreate{,InGroup}() path — has a private m_atomStringTable.

int testGlobalContextCreateRelease()
{
    bool failed = false;

    // JSGlobalContextCreate(nullptr) — minimal repro from the bug report.
    {
        JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
        if (!ctx) {
            printf("FAIL: JSGlobalContextCreate(nullptr) returned nullptr.\n");
            failed = true;
        } else {
            JSGlobalContextRelease(ctx);
        }
    }

    // JSGlobalContextCreateInGroup(nullptr, nullptr) — same shape via the
    // explicit-group form; still the JSAPIGlobalObject::create branch.
    {
        JSGlobalContextRef ctx = JSGlobalContextCreateInGroup(nullptr, nullptr);
        if (!ctx) {
            printf("FAIL: JSGlobalContextCreateInGroup(nullptr, nullptr) returned nullptr.\n");
            failed = true;
        } else {
            JSGlobalContextRelease(ctx);
        }
    }

    // Repeated create/release on one thread — each VM has its own
    // m_atomStringTable; reaching the bug on the second iteration would
    // mean the previous destructor left the thread's atomStringTable
    // pointing at a freed table.
    for (int i = 0; i < 4; ++i) {
        JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
        if (!ctx) {
            printf("FAIL: iter %d: JSGlobalContextCreate(nullptr) returned nullptr.\n", i);
            failed = true;
            break;
        }
        JSGlobalContextRelease(ctx);
    }

    // Interleaved retain/release keeping refcount > 0 across siblings.
    {
        JSGlobalContextRef a = JSGlobalContextCreate(nullptr);
        JSGlobalContextRef b = JSGlobalContextCreate(nullptr);
        if (!a || !b) {
            printf("FAIL: interleaved JSGlobalContextCreate returned nullptr.\n");
            failed = true;
        }
        if (a)
            JSGlobalContextRelease(a);
        if (b)
            JSGlobalContextRelease(b);
    }

    if (failed)
        printf("FAIL: testGlobalContextCreateRelease\n");
    else
        printf("PASS: testGlobalContextCreateRelease\n");

    return failed ? 1 : 0;
}
