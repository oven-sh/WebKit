/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
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

#pragma once

#include "JSCast.h"
// #include "Structure.h"

namespace JSC {

class Microtask;
class JSArray;

JS_EXPORT_PRIVATE Ref<Microtask> createJSMicrotask(VM&, JSValue job, JSValue, JSValue, JSValue, JSValue);

#define MAKE_POOLED \
    void operator delete(void* ptr) {}

class JSMicrotask : public Microtask {
public:
    static constexpr unsigned maxArguments = 4;
    JSMicrotask(VM& vm, JSValue job, JSValue argument0, JSValue argument1, JSValue argument2, JSValue argument3)
    {
        m_job.set(vm, job);
        m_arguments[0].set(vm, argument0);
        m_arguments[1].set(vm, argument1);
        m_arguments[2].set(vm, argument2);
        m_arguments[3].set(vm, argument3);
    }
    JSMicrotask() {}

private:
    void run(JSGlobalObject*) final;

    Strong<Unknown> m_job {};
    Strong<Unknown> m_arguments[maxArguments] {};
};

class JSMicrotaskPooled : public JSMicrotask {
public:
    MAKE_POOLED
    using Base = JSMicrotask;
};

class JSMicrotaskNoArguments : public Microtask {
    static constexpr unsigned maxArguments = 0;

public:
    JSMicrotaskNoArguments(VM& vm, JSValue job)
    {
        m_job.set(vm, job);
    }
    JSMicrotaskNoArguments()
        : m_job()
    {
    }

private:
    void run(JSGlobalObject* globalObject);

    Strong<Unknown> m_job;
};

class JSMicrotaskNoArgumentsPooled : public JSMicrotaskNoArguments {
public:
    MAKE_POOLED
    using Base = JSMicrotaskNoArguments;
};

class JSMicrotask1Argument : public Microtask {
    static constexpr unsigned maxArguments = 2;

public:
    JSMicrotask1Argument(VM& vm, JSValue job, JSValue argument0)
    {
        m_args[0].set(vm, job);
        m_args[1].set(vm, argument0);
    }
    JSMicrotask1Argument()
        : m_args()
    {
    }

private:
    void run(JSGlobalObject* globalObject);

    Strong<Unknown> m_args[maxArguments];
};

class JSMicrotask1ArgumentPooled : public JSMicrotask1Argument {
public:
    using Base = JSMicrotask1Argument;
    MAKE_POOLED
};

class JSMicrotask2Argument : public Microtask {
    static constexpr unsigned maxArguments = 3;

public:
    JSMicrotask2Argument(VM& vm, JSValue job, JSValue argument0, JSValue argument1)
    {
        m_args[0].set(vm, job);
        m_args[1].set(vm, argument0);
        m_args[2].set(vm, argument1);
    }
    JSMicrotask2Argument()
        : m_args()
    {
    }

private:
    void run(JSGlobalObject* globalObject);

    Strong<Unknown> m_args[maxArguments];
};

class JSMicrotask2ArgumentPooled : public JSMicrotask2Argument {
public:
    using Base = JSMicrotask2Argument;
    MAKE_POOLED
};

class JSMicrotask3Argument : public Microtask {
    static constexpr unsigned maxArguments = 4;

public:
    JSMicrotask3Argument(VM& vm, JSValue job, JSValue argument0, JSValue argument1, JSValue argument2)
    {
        m_args[0].set(vm, job);
        m_args[1].set(vm, argument0);
        m_args[2].set(vm, argument1);
        m_args[3].set(vm, argument2);
    }
    JSMicrotask3Argument()
        : m_args()
    {
    }

private:
    void run(JSGlobalObject* globalObject);

    Strong<Unknown> m_args[maxArguments];
};

class JSMicrotask3ArgumentPooled : public JSMicrotask3Argument {
public:
    using Base = JSMicrotask3Argument;
    MAKE_POOLED
};

template<typename MicrotaskType>
class JSMicrotaskPool {
public:
    static constexpr unsigned poolSize = 256;

    JSMicrotaskPool()
    {
        poolAvailable[0] = poolAvailable[1] = poolAvailable[2] = poolAvailable[3] = std::numeric_limits<uint64_t>::max();
    }

    MicrotaskType pool[poolSize] = {};
    uint64_t poolAvailable[poolSize / 64];

    template<typename... Args>
    typename MicrotaskType::Base* allocate(VM& vm, Args... args);
    void markUnused(typename MicrotaskType::Base*);

    bool isPooled(typename MicrotaskType::Base* microtask)
    {
        return microtask >= pool && microtask < pool + poolSize;
    }

    ~JSMicrotaskPool()
    {
        for (unsigned i = 0; i < poolSize; ++i) {
            if ((poolAvailable[i / 64] & (1ULL << (i % 64))) == 0)
                pool[i].~MicrotaskType();
        }
    }

private:
    int nextIndex();
};

class JSMicrotaskPoolAllocator {
public:
    JSMicrotaskPoolAllocator()
        : noArguments()
        , oneArgument()
        , twoArguments()
        , threeArguments()
        , fourArguments()
    {
    }

    template<typename MicrotaskType>
    void markUnused(MicrotaskType* task);

    JSMicrotaskPool<JSMicrotaskNoArgumentsPooled> noArguments;
    JSMicrotaskPool<JSMicrotask1ArgumentPooled> oneArgument;
    JSMicrotaskPool<JSMicrotask2ArgumentPooled> twoArguments;
    JSMicrotaskPool<JSMicrotask3ArgumentPooled> threeArguments;
    JSMicrotaskPool<JSMicrotaskPooled> fourArguments;
};

#undef MAKE_POOLED
} // namespace JSC
