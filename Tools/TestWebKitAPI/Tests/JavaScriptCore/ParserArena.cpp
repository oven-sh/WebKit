/*
 * Copyright (C) 2026 Peter Steinberger All rights reserved.
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

#include <JavaScriptCore/IdentifierInlines.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/NodeConstructors.h>
#include <JavaScriptCore/VM.h>

namespace TestWebKitAPI {

TEST(JavaScriptCore_ParserArena, DefineFieldReleasesIdentifier)
{
    WTF::initializeMainThread();
    JSC::initialize();
    RefPtr<JSC::VM> vm = JSC::VM::create(JSC::HeapType::Small);
    JSC::JSLockHolder locker(vm.get());
    auto identifier = JSC::Identifier::fromString(*vm, "parserArenaFieldIdentifier"_s);
    auto initialRefCount = identifier.impl()->refCount();

    for (auto type : { JSC::DefineFieldNode::Type::Name, JSC::DefineFieldNode::Type::PrivateName, JSC::DefineFieldNode::Type::ComputedName }) {
        {
            JSC::ParserArena arena;
            new (arena) JSC::DefineFieldNode(JSC::JSTokenLocation { }, identifier, nullptr, type);
            EXPECT_EQ(initialRefCount + 1, identifier.impl()->refCount());
        }
        EXPECT_EQ(initialRefCount, identifier.impl()->refCount());
    }
    identifier = { };
    vm = nullptr;
}

} // namespace TestWebKitAPI
