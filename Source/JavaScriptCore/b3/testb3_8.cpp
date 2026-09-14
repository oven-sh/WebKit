/*
 * Copyright (C) 2015-2019 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "testb3.h"

#include "AirGenerate.h"
#include <wtf/Int128.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if ENABLE(B3_JIT)

template<typename T>
void testAtomicWeakCAS()
{
    constexpr Type type = NativeTraits<T>::type;
    constexpr Width width = NativeTraits<T>::width;

    auto checkMyDisassembly = [&] (Compilation& compilation, bool fenced) {
        if (isX86()) {
            checkUsesInstruction(compilation, "lock");
            checkUsesInstruction(compilation, "cmpxchg");
        } else {
            if (isARM64_LSE())
                checkUsesInstruction(compilation, "casal");
            else {
                if (fenced) {
                    checkUsesInstruction(compilation, "ldax");
                    checkUsesInstruction(compilation, "stlx");
                } else {
                    checkUsesInstruction(compilation, "ldx");
                    checkUsesInstruction(compilation, "stx");
                }
            }
        }
    };

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* reloop = proc.addBlock();
        BasicBlock* done = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(proc, Jump, Origin());
        root->setSuccessors(reloop);
    
        reloop->appendNew<Value>(
            proc, Branch, Origin(),
            reloop->appendNew<AtomicValue>(
                proc, AtomicWeakCAS, Origin(), width,
                reloop->appendIntConstant(proc, Origin(), type, 42),
                reloop->appendIntConstant(proc, Origin(), type, 0xbeef),
                ptr));
        reloop->setSuccessors(done, reloop);
    
        done->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* reloop = proc.addBlock();
        BasicBlock* done = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(proc, Jump, Origin());
        root->setSuccessors(reloop);
    
        reloop->appendNew<Value>(
            proc, Branch, Origin(),
            reloop->appendNew<AtomicValue>(
                proc, AtomicWeakCAS, Origin(), width,
                reloop->appendIntConstant(proc, Origin(), type, 42),
                reloop->appendIntConstant(proc, Origin(), type, 0xbeef),
                ptr, 0, HeapRange(42), HeapRange()));
        reloop->setSuccessors(done, reloop);
    
        done->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, false);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* succ = proc.addBlock();
        BasicBlock* fail = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(
            proc, Branch, Origin(),
            root->appendNew<AtomicValue>(
                proc, AtomicWeakCAS, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 42),
                root->appendIntConstant(proc, Origin(), type, 0xbeef),
                ptr));
        root->setSuccessors(succ, fail);
    
        succ->appendNew<MemoryValue>(
            proc, storeOpcode(GP, width), Origin(),
            succ->appendIntConstant(proc, Origin(), type, 100),
            ptr);
        succ->appendNew<Value>(proc, Return, Origin());
    
        fail->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        while (value[0] == 42)
            invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(100));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* succ = proc.addBlock();
        BasicBlock* fail = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(
            proc, Branch, Origin(),
            root->appendNew<Value>(
                proc, Equal, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicWeakCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    ptr),
                root->appendIntConstant(proc, Origin(), Int32, 0)));
        root->setSuccessors(fail, succ);
    
        succ->appendNew<MemoryValue>(
            proc, storeOpcode(GP, width), Origin(),
            succ->appendIntConstant(proc, Origin(), type, 100),
            ptr);
        succ->appendNew<Value>(proc, Return, Origin());
    
        fail->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        while (value[0] == 42)
            invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(100));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<AtomicValue>(
                proc, AtomicWeakCAS, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 42),
                root->appendIntConstant(proc, Origin(), type, 0xbeef),
                arguments[0]));
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        while (!invoke<bool>(*code, value)) { }
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
    
        value[0] = static_cast<T>(300);
        CHECK(!invoke<bool>(*code, value));
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<Value>(
                proc, Equal, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicWeakCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    arguments[0]),
                root->appendNew<Const32Value>(proc, Origin(), 0)));
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        while (invoke<bool>(*code, value)) { }
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
    
        value[0] = static_cast<T>(300);
        CHECK(invoke<bool>(*code, value));
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<AtomicValue>(
                proc, AtomicWeakCAS, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 42),
                root->appendIntConstant(proc, Origin(), type, 0xbeef),
                arguments[0],
                42));
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        while (!invoke<bool>(*code, reinterpret_cast<intptr_t>(value) - 42)) { }
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
    
        value[0] = static_cast<T>(300);
        CHECK(!invoke<bool>(*code, reinterpret_cast<intptr_t>(value) - 42));
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }
}

template<typename T>
void testAtomicStrongCAS()
{
    constexpr Type type = NativeTraits<T>::type;
    constexpr Width width = NativeTraits<T>::width;

    auto checkMyDisassembly = [&] (Compilation& compilation, bool fenced) {
        if (isX86()) {
            checkUsesInstruction(compilation, "lock");
            checkUsesInstruction(compilation, "cmpxchg");
        } else {
            if (isARM64_LSE())
                checkUsesInstruction(compilation, "casal");
            else {
                if (fenced) {
                    checkUsesInstruction(compilation, "ldax");
                    checkUsesInstruction(compilation, "stlx");
                } else {
                    checkUsesInstruction(compilation, "ldx");
                    checkUsesInstruction(compilation, "stx");
                }
            }
        }
    };

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* succ = proc.addBlock();
        BasicBlock* fail = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(
            proc, Branch, Origin(),
            root->appendNew<Value>(
                proc, Equal, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicStrongCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    ptr),
                root->appendIntConstant(proc, Origin(), type, 42)));
        root->setSuccessors(succ, fail);
    
        succ->appendNew<MemoryValue>(
            proc, storeOpcode(GP, width), Origin(),
            succ->appendIntConstant(proc, Origin(), type, 100),
            ptr);
        succ->appendNew<Value>(proc, Return, Origin());
    
        fail->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(100));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* succ = proc.addBlock();
        BasicBlock* fail = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(
            proc, Branch, Origin(),
            root->appendNew<Value>(
                proc, Equal, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicStrongCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    ptr, 0, HeapRange(42), HeapRange()),
                root->appendIntConstant(proc, Origin(), type, 42)));
        root->setSuccessors(succ, fail);
    
        succ->appendNew<MemoryValue>(
            proc, storeOpcode(GP, width), Origin(),
            succ->appendIntConstant(proc, Origin(), type, 100),
            ptr);
        succ->appendNew<Value>(proc, Return, Origin());
    
        fail->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(100));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, false);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* succ = proc.addBlock();
        BasicBlock* fail = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
    
        Value* ptr = arguments[0];
        root->appendNew<Value>(
            proc, Branch, Origin(),
            root->appendNew<Value>(
                proc, NotEqual, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicStrongCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    ptr),
                root->appendIntConstant(proc, Origin(), type, 42)));
        root->setSuccessors(fail, succ);
    
        succ->appendNew<MemoryValue>(
            proc, storeOpcode(GP, width), Origin(),
            succ->appendIntConstant(proc, Origin(), type, 100),
            ptr);
        succ->appendNew<Value>(proc, Return, Origin());
    
        fail->appendNew<Value>(proc, Return, Origin());
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(100));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        invoke<void>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<AtomicValue>(
                proc, AtomicStrongCAS, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 42),
                root->appendIntConstant(proc, Origin(), type, 0xbeef),
                arguments[0]));
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        CHECK_EQ(invoke<typename NativeTraits<T>::CanonicalType>(*code, value), 42);
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        CHECK_EQ(invoke<typename NativeTraits<T>::CanonicalType>(*code, value), static_cast<typename NativeTraits<T>::CanonicalType>(static_cast<T>(300)));
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(-1);
        CHECK_EQ(invoke<typename NativeTraits<T>::CanonicalType>(*code, value), static_cast<typename NativeTraits<T>::CanonicalType>(static_cast<T>(-1)));
        CHECK_EQ(value[0], static_cast<T>(-1));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        // Test for https://bugs.webkit.org/show_bug.cgi?id=169867.
    
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<Value>(
                proc, BitXor, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicStrongCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    arguments[0]),
                root->appendIntConstant(proc, Origin(), type, 1)));
    
        typename NativeTraits<T>::CanonicalType one = 1;
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        CHECK_EQ(invoke<typename NativeTraits<T>::CanonicalType>(*code, value), 42 ^ one);
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        CHECK_EQ(invoke<typename NativeTraits<T>::CanonicalType>(*code, value), static_cast<typename NativeTraits<T>::CanonicalType>(static_cast<T>(300)) ^ one);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(-1);
        CHECK_EQ(invoke<typename NativeTraits<T>::CanonicalType>(*code, value), static_cast<typename NativeTraits<T>::CanonicalType>(static_cast<T>(-1)) ^ one);
        CHECK_EQ(value[0], static_cast<T>(-1));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<Value>(
                proc, Equal, Origin(),
                root->appendNew<AtomicValue>(
                    proc, AtomicStrongCAS, Origin(), width,
                    root->appendIntConstant(proc, Origin(), type, 42),
                    root->appendIntConstant(proc, Origin(), type, 0xbeef),
                    arguments[0]),
                root->appendIntConstant(proc, Origin(), type, 42)));
    
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        CHECK(invoke<bool>(*code, value));
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        CHECK(!invoke<bool>(*code, value));
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<Value>(
                proc, Equal, Origin(),
                root->appendNew<Value>(
                    proc, NotEqual, Origin(),
                    root->appendNew<AtomicValue>(
                        proc, AtomicStrongCAS, Origin(), width,
                        root->appendIntConstant(proc, Origin(), type, 42),
                        root->appendIntConstant(proc, Origin(), type, 0xbeef),
                        arguments[0]),
                    root->appendIntConstant(proc, Origin(), type, 42)),
                root->appendNew<Const32Value>(proc, Origin(), 0)));
        
        auto code = compileProc(proc);
        T value[2];
        value[0] = 42;
        value[1] = 13;
        CHECK(invoke<bool>(*code, value));
        CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        value[0] = static_cast<T>(300);
        CHECK(!invoke<bool>(*code, &value));
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);

        Value* ptr = arguments[0];
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<AtomicValue>(
                proc, AtomicStrongCAS, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 0x0f00000000000000ULL + 42),
                root->appendIntConstant(proc, Origin(), type, 0xbeef),
                ptr));

        auto code = compileProc(proc);
        T value[2];
        T result;
        value[0] = 42;
        value[1] = 13;
        result = invoke<T>(*code, value);
        if (width == Width64)
            CHECK_EQ(value[0], static_cast<T>(42));
        else
            CHECK_EQ(value[0], static_cast<T>(0xbeef));
        CHECK_EQ(value[1], 13);
        CHECK_EQ(result, static_cast<T>(42));
        value[0] = static_cast<T>(300);
        result = invoke<T>(*code, value);
        CHECK_EQ(value[0], static_cast<T>(300));
        CHECK_EQ(value[1], 13);
        CHECK_EQ(result, static_cast<T>(300));
        checkMyDisassembly(*code, true);
    }
}

template<typename T>
void testAtomicXchg(B3::Opcode opcode)
{
    constexpr Type type = NativeTraits<T>::type;
    constexpr Width width = NativeTraits<T>::width;

    auto doTheMath = [&] (T& memory, T operand) -> T {
        T oldValue = memory;
        switch (opcode) {
        case AtomicXchgAdd:
            memory += operand;
            break;
        case AtomicXchgAnd:
            memory &= operand;
            break;
        case AtomicXchgOr:
            memory |= operand;
            break;
        case AtomicXchgSub:
            memory -= operand;
            break;
        case AtomicXchgXor:
            memory ^= operand;
            break;
        case AtomicXchg:
            memory = operand;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return oldValue;
    };

    auto oldValue = [&] (T memory, T operand) -> T {
        return doTheMath(memory, operand);
    };

    auto newValue = [&] (T memory, T operand) -> T {
        doTheMath(memory, operand);
        return memory;
    };

    auto checkMyDisassembly = [&] (Compilation& compilation, bool fenced) {
        if (isX86()) {
            // AtomicXchg can be lowered to "xchg" without "lock", and this is OK since "lock" signal is asserted for "xchg" by default.
            if (AtomicXchg != opcode)
                checkUsesInstruction(compilation, "lock");
        } else {
            if (isARM64_LSE()) {
                switch (opcode) {
                case AtomicXchgAdd:
                    checkUsesInstruction(compilation, "ldaddal");
                    break;
                case AtomicXchgAnd:
                    checkUsesInstruction(compilation, "ldclral");
                    break;
                case AtomicXchgOr:
                    checkUsesInstruction(compilation, "ldsetal");
                    break;
                case AtomicXchgSub:
                    checkUsesInstruction(compilation, "ldaddal");
                    break;
                case AtomicXchgXor:
                    checkUsesInstruction(compilation, "ldeoral");
                    break;
                case AtomicXchg:
                    checkUsesInstruction(compilation, "swpal");
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }
            } else {
                if (fenced) {
                    checkUsesInstruction(compilation, "ldax");
                    checkUsesInstruction(compilation, "stlx");
                } else {
                    checkUsesInstruction(compilation, "ldx");
                    checkUsesInstruction(compilation, "stx");
                }
            }
        }
    };

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<AtomicValue>(
                proc, opcode, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 1),
                arguments[0]));

        auto code = compileProc(proc);
        T value[2];
        value[0] = 5;
        value[1] = 100;
        CHECK_EQ(invoke<T>(*code, value), oldValue(5, 1));
        CHECK_EQ(value[0], newValue(5, 1));
        CHECK_EQ(value[1], 100);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<Value>(
            proc, Return, Origin(),
            root->appendNew<AtomicValue>(
                proc, opcode, Origin(), width,
                root->appendIntConstant(proc, Origin(), type, 42),
                arguments[0]));

        auto code = compileProc(proc);
        T value[2];
        value[0] = 5;
        value[1] = 100;
        CHECK_EQ(invoke<T>(*code, value), oldValue(5, 42));
        CHECK_EQ(value[0], newValue(5, 42));
        CHECK_EQ(value[1], 100);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<AtomicValue>(
            proc, opcode, Origin(), width,
            root->appendIntConstant(proc, Origin(), type, 42),
            arguments[0]);
        root->appendNew<Value>(proc, Return, Origin());

        auto code = compileProc(proc);
        T value[2];
        value[0] = 5;
        value[1] = 100;
        invoke<T>(*code, value);
        CHECK_EQ(value[0], newValue(5, 42));
        CHECK_EQ(value[1], 100);
        checkMyDisassembly(*code, true);
    }

    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);
        root->appendNew<AtomicValue>(
            proc, opcode, Origin(), width,
            root->appendIntConstant(proc, Origin(), type, 42),
            arguments[0],
            0, HeapRange(42), HeapRange());
        root->appendNew<Value>(proc, Return, Origin());

        auto code = compileProc(proc);
        T value[2];
        value[0] = 5;
        value[1] = 100;
        invoke<T>(*code, value);
        CHECK_EQ(value[0], newValue(5, 42));
        CHECK_EQ(value[1], 100);
        checkMyDisassembly(*code, false);
    }
}

void addAtomicTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testAtomicWeakCAS<int8_t>());
    RUN(testAtomicWeakCAS<int16_t>());
    RUN(testAtomicWeakCAS<int32_t>());
    RUN(testAtomicWeakCAS<int64_t>());
    RUN(testAtomicStrongCAS<int8_t>());
    RUN(testAtomicStrongCAS<int16_t>());
    RUN(testAtomicStrongCAS<int32_t>());
    RUN(testAtomicStrongCAS<int64_t>());
    RUN(testAtomicXchg<int8_t>(AtomicXchgAdd));
    RUN(testAtomicXchg<int16_t>(AtomicXchgAdd));
    RUN(testAtomicXchg<int32_t>(AtomicXchgAdd));
    RUN(testAtomicXchg<int64_t>(AtomicXchgAdd));
    RUN(testAtomicXchg<int8_t>(AtomicXchgAnd));
    RUN(testAtomicXchg<int16_t>(AtomicXchgAnd));
    RUN(testAtomicXchg<int32_t>(AtomicXchgAnd));
    RUN(testAtomicXchg<int64_t>(AtomicXchgAnd));
    RUN(testAtomicXchg<int8_t>(AtomicXchgOr));
    RUN(testAtomicXchg<int16_t>(AtomicXchgOr));
    RUN(testAtomicXchg<int32_t>(AtomicXchgOr));
    RUN(testAtomicXchg<int64_t>(AtomicXchgOr));
    RUN(testAtomicXchg<int8_t>(AtomicXchgSub));
    RUN(testAtomicXchg<int16_t>(AtomicXchgSub));
    RUN(testAtomicXchg<int32_t>(AtomicXchgSub));
    RUN(testAtomicXchg<int64_t>(AtomicXchgSub));
    RUN(testAtomicXchg<int8_t>(AtomicXchgXor));
    RUN(testAtomicXchg<int16_t>(AtomicXchgXor));
    RUN(testAtomicXchg<int32_t>(AtomicXchgXor));
    RUN(testAtomicXchg<int64_t>(AtomicXchgXor));
    RUN(testAtomicXchg<int8_t>(AtomicXchg));
    RUN(testAtomicXchg<int16_t>(AtomicXchg));
    RUN(testAtomicXchg<int32_t>(AtomicXchg));
    RUN(testAtomicXchg<int64_t>(AtomicXchg));
}

template<typename CType, typename InputType>
void testLoad(B3::Type type, B3::Opcode opcode, InputType value)
{
    // Simple load from an absolute address.
    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();

        root->appendNewControlValue(
            proc, Return, Origin(),
            root->appendNew<MemoryValue>(
                proc, opcode, type, Origin(),
                root->appendNew<ConstPtrValue>(proc, Origin(), &value)));

        CHECK(isIdentical(compileAndRun<CType>(proc), modelLoad<CType>(value)));
    }

    // Simple load from an address in a register.
    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);

        root->appendNewControlValue(
            proc, Return, Origin(),
            root->appendNew<MemoryValue>(
                proc, opcode, type, Origin(),
                arguments[0]));

        CHECK(isIdentical(compileAndRun<CType>(proc, &value), modelLoad<CType>(value)));
    }

    // Simple load from an address in a register, at an offset.
    {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*>(proc, root);

        root->appendNewControlValue(
            proc, Return, Origin(),
            root->appendNew<MemoryValue>(
                proc, opcode, type, Origin(),
                arguments[0],
                static_cast<int32_t>(sizeof(InputType))));

        CHECK(isIdentical(compileAndRun<CType>(proc, &value - 1), modelLoad<CType>(value)));
    }

    // Load from a simple base-index with various scales.
    for (unsigned logScale = 0; logScale <= 3; ++logScale) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*, intptr_t>(proc, root);

        root->appendNewControlValue(
            proc, Return, Origin(),
            root->appendNew<MemoryValue>(
                proc, opcode, type, Origin(),
                root->appendNew<Value>(
                    proc, Add, Origin(),
                    arguments[0],
                    root->appendNew<Value>(
                        proc, Shl, Origin(),
                        arguments[1],
                        root->appendNew<Const32Value>(proc, Origin(), logScale)))));

        CHECK(isIdentical(compileAndRun<CType>(proc, &value - 2, (sizeof(InputType) * 2) >> logScale), modelLoad<CType>(value)));
    }

    // Load from a simple base-index with various scales, but commuted.
    for (unsigned logScale = 0; logScale <= 3; ++logScale) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*, intptr_t>(proc, root);

        root->appendNewControlValue(
            proc, Return, Origin(),
            root->appendNew<MemoryValue>(
                proc, opcode, type, Origin(),
                root->appendNew<Value>(
                    proc, Add, Origin(),
                    root->appendNew<Value>(
                        proc, Shl, Origin(),
                        arguments[1],
                        root->appendNew<Const32Value>(proc, Origin(), logScale)),
                    arguments[0])));

        CHECK(isIdentical(compileAndRun<CType>(proc, &value - 2, (sizeof(InputType) * 2) >> logScale), modelLoad<CType>(value)));
    }
}

template<typename T>
void testLoad(B3::Opcode opcode, int32_t value)
{
    return testLoad<T>(B3::Int32, opcode, value);
}

template<typename T>
void testLoad(B3::Type type, T value)
{
    return testLoad<T>(type, Load, value);
}

void addLoadTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testLoad(Int32, 60));
    RUN(testLoad(Int32, -60));
    RUN(testLoad(Int32, 1000));
    RUN(testLoad(Int32, -1000));
    RUN(testLoad(Int32, 1000000));
    RUN(testLoad(Int32, -1000000));
    RUN(testLoad(Int32, 1000000000));
    RUN(testLoad(Int32, -1000000000));
    RUN_BINARY(testLoad, { MAKE_OPERAND(Int64) }, int64Operands());
    RUN_BINARY(testLoad, { MAKE_OPERAND(Float) }, floatingPointOperands<float>());
    RUN_BINARY(testLoad, { MAKE_OPERAND(Double) }, floatingPointOperands<double>());

    RUN(testLoad<int8_t>(Load8S, 60));
    RUN(testLoad<int8_t>(Load8S, -60));
    RUN(testLoad<int8_t>(Load8S, 1000));
    RUN(testLoad<int8_t>(Load8S, -1000));
    RUN(testLoad<int8_t>(Load8S, 1000000));
    RUN(testLoad<int8_t>(Load8S, -1000000));
    RUN(testLoad<int8_t>(Load8S, 1000000000));
    RUN(testLoad<int8_t>(Load8S, -1000000000));
    
    RUN(testLoad<uint8_t>(Load8Z, 60));
    RUN(testLoad<uint8_t>(Load8Z, -60));
    RUN(testLoad<uint8_t>(Load8Z, 1000));
    RUN(testLoad<uint8_t>(Load8Z, -1000));
    RUN(testLoad<uint8_t>(Load8Z, 1000000));
    RUN(testLoad<uint8_t>(Load8Z, -1000000));
    RUN(testLoad<uint8_t>(Load8Z, 1000000000));
    RUN(testLoad<uint8_t>(Load8Z, -1000000000));
    
    RUN(testLoad<int16_t>(Load16S, 60));
    RUN(testLoad<int16_t>(Load16S, -60));
    RUN(testLoad<int16_t>(Load16S, 1000));
    RUN(testLoad<int16_t>(Load16S, -1000));
    RUN(testLoad<int16_t>(Load16S, 1000000));
    RUN(testLoad<int16_t>(Load16S, -1000000));
    RUN(testLoad<int16_t>(Load16S, 1000000000));
    RUN(testLoad<int16_t>(Load16S, -1000000000));
    
    RUN(testLoad<uint16_t>(Load16Z, 60));
    RUN(testLoad<uint16_t>(Load16Z, -60));
    RUN(testLoad<uint16_t>(Load16Z, 1000));
    RUN(testLoad<uint16_t>(Load16Z, -1000));
    RUN(testLoad<uint16_t>(Load16Z, 1000000));
    RUN(testLoad<uint16_t>(Load16Z, -1000000));
    RUN(testLoad<uint16_t>(Load16Z, 1000000000));
    RUN(testLoad<uint16_t>(Load16Z, -1000000000));
}

void testWasmAddressDoesNotCSE()
{
    Procedure proc;
    GPRReg pinnedGPR = GPRInfo::argumentGPR0;
    proc.pinRegister(pinnedGPR);

    BasicBlock* root = proc.addBlock();
    BasicBlock* a = proc.addBlock();
    BasicBlock* b = proc.addBlock();
    BasicBlock* c = proc.addBlock();
    BasicBlock* continuation = proc.addBlock();

    auto* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
    auto* path = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR2);

    auto* originalAddress = root->appendNew<WasmAddressValue>(proc, Origin(), pointer, pinnedGPR);
    root->appendNew<MemoryValue>(proc, Store, Origin(), originalAddress, 
        root->appendNew<WasmAddressValue>(proc, Origin(), root->appendNew<ConstPtrValue>(proc, Origin(), 6*8), pinnedGPR), 0);

    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), path);
    switchValue->setFallThrough(FrequentedBlock(c));
    switchValue->appendCase(SwitchCase(0, FrequentedBlock(a)));
    switchValue->appendCase(SwitchCase(1, FrequentedBlock(b)));

    PatchpointValue* patchpoint = b->appendNew<PatchpointValue>(proc, Void, Origin());
    patchpoint->effects = Effects::forCall();
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(RegisterSet(pinnedGPR));
    patchpoint->setGenerator(
        [&] (CCallHelpers& jit, const StackmapGenerationParams& params) {
            CHECK(!params.size());
            jit.addPtr(MacroAssembler::TrustedImm32(8), pinnedGPR);
        });

    UpsilonValue* takeA = a->appendNew<UpsilonValue>(proc, Origin(), a->appendNew<Const32Value>(proc, Origin(), 10));
    UpsilonValue* takeB = b->appendNew<UpsilonValue>(proc, Origin(), b->appendNew<Const32Value>(proc, Origin(), 20));
    UpsilonValue* takeC = c->appendNew<UpsilonValue>(proc, Origin(), c->appendNew<Const32Value>(proc, Origin(), 30));
    for (auto* i : { a, b, c }) {
        i->appendNewControlValue(proc, Jump, Origin(), FrequentedBlock(continuation));
        i->setSuccessors(FrequentedBlock(continuation));
    }

    // Continuation
    auto* takenPhi = continuation->appendNew<Value>(proc, Phi, Int32, Origin());

    auto* address2 = continuation->appendNew<WasmAddressValue>(proc, Origin(), pointer, pinnedGPR);
    continuation->appendNew<MemoryValue>(proc, Store, Origin(), takenPhi,
        continuation->appendNew<WasmAddressValue>(proc, Origin(), continuation->appendNew<ConstPtrValue>(proc, Origin(), 4*8), pinnedGPR),
        0);

    auto* returnVal = address2;
    continuation->appendNewControlValue(proc, Return, Origin(), returnVal);

    takeA->setPhi(takenPhi);
    takeB->setPhi(takenPhi);
    takeC->setPhi(takenPhi);

    auto binary = compileProc(proc);

    uint64_t* memory = new uint64_t[10];
    uintptr_t ptr = 8;

    uintptr_t finalPtr = reinterpret_cast<uintptr_t>(static_cast<void*>(memory)) + ptr;

    for (int i = 0; i < 10; ++i)
        memory[i] = 0;

    {
        uintptr_t result = invoke<uintptr_t>(*binary, memory, ptr, 0);

        CHECK_EQ(result, finalPtr);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 0ul);
        CHECK_EQ(memory[2], 0ul);
        CHECK_EQ(memory[4], 10ul);
        CHECK_EQ(memory[6], finalPtr);
    }

    memory[4] = 0;
    memory[5] = 0;
    memory[6] = 0;
    memory[7] = 0;

    {
        uintptr_t result = invoke<uintptr_t>(*binary, memory, ptr, 1);

        CHECK_EQ(result, finalPtr + 8);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 0ul);
        CHECK_EQ(memory[2], 0ul);
        CHECK_EQ(memory[5], 20ul);
        CHECK_EQ(memory[6], finalPtr);
    }

    memory[4] = 0;
    memory[5] = 0;
    memory[6] = 0;
    memory[7] = 0;
    {
        uintptr_t result = invoke<uintptr_t>(*binary, memory, ptr, 2);

        CHECK_EQ(result, finalPtr);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 0ul);
        CHECK_EQ(memory[2], 0ul);
        CHECK_EQ(memory[4], 30ul);
        CHECK_EQ(memory[6], finalPtr);
    }

    delete[] memory;
}

void testStoreAfterClobberExitsSideways()
{
    Procedure proc;
    GPRReg pinnedBaseGPR = GPRInfo::argumentGPR0;
    GPRReg pinnedSizeGPR = GPRInfo::argumentGPR1;
    proc.pinRegister(pinnedBaseGPR);
    proc.pinRegister(pinnedSizeGPR);

    // Please don't make me save anything.
    RegisterSet csrs;
    csrs.merge(RegisterSet::calleeSaveRegisters());
    csrs.exclude(RegisterSet::stackRegisters());
    csrs.forEach(
        [&] (Reg reg) {
            CHECK(reg != pinnedBaseGPR);
            CHECK(reg != pinnedSizeGPR);
            proc.pinRegister(reg);
        });

    proc.setWasmBoundsCheckGenerator([=](CCallHelpers& jit, WasmBoundsCheckValue*, GPRReg pinnedGPR) {
        CHECK_EQ(pinnedGPR, pinnedSizeGPR);

        jit.move(CCallHelpers::TrustedImm32(42), GPRInfo::returnValueGPR);
        jit.emitFunctionEpilogue();
        jit.ret();
    });

    BasicBlock* root = proc.addBlock();

    Value* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR2);
    auto* resultAddress = root->appendNew<WasmAddressValue>(proc, Origin(), pointer, pinnedBaseGPR);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const32Value>(proc, Origin(), 10), resultAddress, 0);

    pointer = root->appendNew<Value>(proc, Trunc, Origin(), pointer);
    root->appendNew<WasmBoundsCheckValue>(proc, Origin(), pinnedSizeGPR, pointer, 0);

    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const32Value>(proc, Origin(), 20), resultAddress, 0);
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Const32Value>(proc, Origin(), 30));

    auto binary = compileProc(proc);

    uint64_t* memory = new uint64_t[10];
    uint64_t ptr = 1*8;

    for (int i = 0; i < 10; ++i)
        memory[i] = 0;

    {
        int result = invoke<int>(*binary, memory, 16, ptr);

        CHECK_EQ(result, 30);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 20ul);
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, 1, ptr);

        CHECK_EQ(result, 42);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 10ul);
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    delete[] memory;
}

void testStoreAfterClobberDifferentWidth()
{
    Procedure proc;
    GPRReg pinnedBaseGPR = GPRInfo::argumentGPR0;
    proc.pinRegister(pinnedBaseGPR);

    BasicBlock* root = proc.addBlock();

    auto* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
    auto* resultAddress = root->appendNew<WasmAddressValue>(proc, Origin(), pointer, pinnedBaseGPR);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<ConstPtrValue>(proc, Origin(), -1), resultAddress, 0);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const32Value>(proc, Origin(), 20), resultAddress, 0);
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Const32Value>(proc, Origin(), 30));

    auto binary = compileProc(proc);

    uint64_t* memory = new uint64_t[10];
    uintptr_t ptr = 1*8;

    for (int i = 0; i < 10; ++i)
        memory[i] = 0;

    {
        int result = invoke<int>(*binary, memory, ptr);

        CHECK_EQ(result, 30);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], (0xFFFFFFFF00000000ul | 20ul));
        CHECK_EQ(memory[2], 0ul);
    }

    delete[] memory;
}

void testStoreAfterClobberDifferentWidthSuccessor()
{
    Procedure proc;
    GPRReg pinnedBaseGPR = GPRInfo::argumentGPR0;
    proc.pinRegister(pinnedBaseGPR);

    BasicBlock* root = proc.addBlock();
    BasicBlock* a = proc.addBlock();
    BasicBlock* b = proc.addBlock();
    BasicBlock* c = proc.addBlock();
    BasicBlock* continuation = proc.addBlock();

    auto* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
    auto* path = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR2);
    auto* resultAddress = root->appendNew<WasmAddressValue>(proc, Origin(), pointer, pinnedBaseGPR);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<ConstPtrValue>(proc, Origin(), -1), resultAddress, 0);

    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), path);
    switchValue->setFallThrough(FrequentedBlock(c));
    switchValue->appendCase(SwitchCase(0, FrequentedBlock(a)));
    switchValue->appendCase(SwitchCase(1, FrequentedBlock(b)));

    a->appendNew<MemoryValue>(proc, Store, Origin(), a->appendNew<Const32Value>(proc, Origin(), 10), resultAddress, 0);
    b->appendNew<MemoryValue>(proc, Store, Origin(), b->appendNew<Const32Value>(proc, Origin(), 20), resultAddress, 0);
    c->appendNew<MemoryValue>(proc, Store, Origin(), c->appendNew<Const32Value>(proc, Origin(), 30), resultAddress, 0);

    for (auto* i : { a, b, c }) {
        i->appendNewControlValue(proc, Jump, Origin(), FrequentedBlock(continuation));
        i->setSuccessors(FrequentedBlock(continuation));
    }

    continuation->appendNewControlValue(proc, Return, Origin(), continuation->appendNew<Const32Value>(proc, Origin(), 40));

    auto binary = compileProc(proc);

    uint64_t* memory = new uint64_t[10];
    uintptr_t ptr = 1*8;

    for (int i = 0; i < 10; ++i)
        memory[i] = 0;

    {
        int result = invoke<int>(*binary, memory, ptr, 0);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], (0xFFFFFFFF00000000ul | 10ul));
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, ptr, 1);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], (0xFFFFFFFF00000000ul | 20ul));
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, ptr, 2);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], (0xFFFFFFFF00000000ul | 30ul));
        CHECK_EQ(memory[2], 0ul);
    }

    delete[] memory;
}

void testStoreAfterClobberExitsSidewaysSuccessor()
{
    Procedure proc;
    GPRReg pinnedBaseGPR = GPRInfo::argumentGPR0;
    GPRReg pinnedSizeGPR = GPRInfo::argumentGPR1;
    proc.pinRegister(pinnedBaseGPR);
    proc.pinRegister(pinnedSizeGPR);

    // Please don't make me save anything.
    RegisterSet csrs;
    csrs.merge(RegisterSet::calleeSaveRegisters());
    csrs.exclude(RegisterSet::stackRegisters());
    csrs.forEach(
        [&] (Reg reg) {
            CHECK(reg != pinnedBaseGPR);
            CHECK(reg != pinnedSizeGPR);
            proc.pinRegister(reg);
        });

    proc.setWasmBoundsCheckGenerator([=](CCallHelpers& jit, WasmBoundsCheckValue*, GPRReg pinnedGPR) {
        CHECK_EQ(pinnedGPR, pinnedSizeGPR);

        jit.move(CCallHelpers::TrustedImm32(42), GPRInfo::returnValueGPR);
        jit.emitFunctionEpilogue();
        jit.ret();
    });

    BasicBlock* root = proc.addBlock();
    BasicBlock* a = proc.addBlock();
    BasicBlock* b = proc.addBlock();
    BasicBlock* c = proc.addBlock();
    BasicBlock* continuation = proc.addBlock();

    Value* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR2);
    auto* path = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR3);
    auto* resultAddress = root->appendNew<WasmAddressValue>(proc, Origin(), pointer, pinnedBaseGPR);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<ConstPtrValue>(proc, Origin(), -1), resultAddress, 0);

    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), path);
    switchValue->setFallThrough(FrequentedBlock(c));
    switchValue->appendCase(SwitchCase(0, FrequentedBlock(a)));
    switchValue->appendCase(SwitchCase(1, FrequentedBlock(b)));

    pointer = b->appendNew<Value>(proc, Trunc, Origin(), pointer);
    b->appendNew<WasmBoundsCheckValue>(proc, Origin(), pinnedSizeGPR, pointer, 0);

    UpsilonValue* takeA = a->appendNew<UpsilonValue>(proc, Origin(), a->appendNew<Const64Value>(proc, Origin(), 10));
    UpsilonValue* takeB = b->appendNew<UpsilonValue>(proc, Origin(), b->appendNew<Const64Value>(proc, Origin(), 20));
    UpsilonValue* takeC = c->appendNew<UpsilonValue>(proc, Origin(), c->appendNew<Const64Value>(proc, Origin(), 30));

    for (auto* i : { a, b, c }) {
        i->appendNewControlValue(proc, Jump, Origin(), FrequentedBlock(continuation));
        i->setSuccessors(FrequentedBlock(continuation));
    }

    auto* takenPhi = continuation->appendNew<Value>(proc, Phi, Int64, Origin());
    continuation->appendNew<MemoryValue>(proc, Store, Origin(), takenPhi, resultAddress, 0);
    continuation->appendNewControlValue(proc, Return, Origin(), continuation->appendNew<Const32Value>(proc, Origin(), 40));

    takeA->setPhi(takenPhi);
    takeB->setPhi(takenPhi);
    takeC->setPhi(takenPhi);

    auto binary = compileProc(proc);

    uint64_t* memory = new uint64_t[10];
    uintptr_t ptr = 1*8;

    for (int i = 0; i < 10; ++i)
        memory[i] = 0;

    {
        int result = invoke<int>(*binary, memory, 16, ptr, 0);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 10ul);
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, 16, ptr, 1);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 20ul);
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, 16, ptr, 2);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 30ul);
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, 1, ptr, 2);

        CHECK_EQ(result, 40);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], 30ul);
        CHECK_EQ(memory[2], 0ul);
    }

    memory[1] = 0;

    {
        int result = invoke<int>(*binary, memory, 1, ptr, 1);

        CHECK_EQ(result, 42);
        CHECK_EQ(memory[0], 0ul);
        CHECK_EQ(memory[1], (0xFFFFFFFFFFFFFFFFul));
        CHECK_EQ(memory[2], 0ul);
    }

    delete[] memory;
}

void testNarrowLoad()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto* value1 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
    auto* value2 = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Int64, Origin(), value1, root->appendNew<Value>(proc, ZExt32, Int64, Origin(), value2)));

    uint64_t value = 0x1000000010000000ULL;
    CHECK_EQ(compileAndRun<uint64_t>(proc, &value), 0x1000000020000000ULL);
}

void testNarrowLoadClobber()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto* address = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    auto* value1 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), address);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const64Value>(proc, Origin(), 0), address, 0);
    auto* value2 = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Int64, Origin(), value1, root->appendNew<Value>(proc, ZExt32, Int64, Origin(), value2)));

    uint64_t value = 0x1000000010000000ULL;
    CHECK_EQ(compileAndRun<uint64_t>(proc, &value), 0x1000000010000000ULL);
    CHECK_EQ(value, 0x0000000000000000ULL);
}

void testNarrowLoadClobberNarrow()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto* address = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    auto* value1 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), address);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const32Value>(proc, Origin(), 0), address, 0);
    auto* value2 = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Int64, Origin(), value1, root->appendNew<Value>(proc, ZExt32, Int64, Origin(), value2)));

    uint64_t value = 0x1000000010000000ULL;
    CHECK_EQ(compileAndRun<uint64_t>(proc, &value), 0x1000000010000000ULL);
    CHECK_EQ(value, 0x1000000000000000ULL);
}

void testNarrowLoadNotClobber()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto* address = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    auto* value1 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), address);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const32Value>(proc, Origin(), 0), address, 4);
    auto* value2 = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Int64, Origin(), value1, root->appendNew<Value>(proc, ZExt32, Int64, Origin(), value2)));

    uint64_t value = 0x1000000010000000ULL;
    CHECK_EQ(compileAndRun<uint64_t>(proc, &value), 0x1000000020000000ULL);
    CHECK_EQ(value, 0x0000000010000000ULL);
}

void testNarrowLoadUpper()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto* address = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    auto* value1 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), address);
    auto* value2 = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), address, 4);
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Int64, Origin(), value1, root->appendNew<Value>(proc, ZExt32, Int64, Origin(), value2)));

    uint64_t value = 0x2000000010000000ULL;
    CHECK_EQ(compileAndRun<uint64_t>(proc, &value), 0x2000000030000000ULL);
}

void testConstDoubleMove()
{
    // FMOV
    {
        auto encode = [](uint64_t value) -> double {
            constexpr unsigned E = 11;
            constexpr unsigned F = 64 - E - 1;
            uint64_t sign = (value & 0b10000000U) ? 1 : 0;
            uint64_t upper = (value & 0b01000000U) ? 0b01111111100U : 0b10000000000U;
            uint64_t exp = upper | ((value & 0b00110000U) >> 4);
            uint64_t frac = (value & 0b1111U) << (F - 4);
            return std::bit_cast<double>((sign << 63) | (exp << F) | frac);
        };

        for (uint8_t i = 0; i < UINT8_MAX; ++i) {
            Procedure proc;
            BasicBlock* root = proc.addBlock();
            root->appendNewControlValue(proc, Return, Origin(), root->appendNew<ConstDoubleValue>(proc, Origin(), encode(i)));
            CHECK_EQ(compileAndRun<double>(proc), encode(i));
        }
    }

    // MOVI
    {
        auto encode = [](uint64_t value) -> uint64_t {
            auto bits = [](bool flag) -> uint64_t {
                return (flag) ? 0b11111111ULL : 0b00000000ULL;
            };

            return (bits(value & (1U << 7)) << 56)
                | (bits(value & (1U << 6)) << 48)
                | (bits(value & (1U << 5)) << 40)
                | (bits(value & (1U << 4)) << 32)
                | (bits(value & (1U << 3)) << 24)
                | (bits(value & (1U << 2)) << 16)
                | (bits(value & (1U << 1)) << 8)
                | (bits(value & (1U << 0)) << 0);
        };

        for (uint8_t i = 0; i < UINT8_MAX; ++i) {
            Procedure proc;
            BasicBlock* root = proc.addBlock();
            root->appendNewControlValue(proc, Return, Origin(), root->appendNew<ConstDoubleValue>(proc, Origin(), std::bit_cast<double>(encode(i))));
            CHECK_EQ(std::bit_cast<uint64_t>(compileAndRun<double>(proc)), encode(i));
        }
    }
}

void testConstFloatMove()
{
    // FMOV
    auto encode = [](uint64_t value) -> float {
        constexpr unsigned E = 8;
        constexpr unsigned F = 32 - E - 1;
        uint32_t sign = (value & 0b10000000U) ? 1 : 0;
        uint32_t upper = (value & 0b01000000U) ? 0b01111100U : 0b10000000U;
        uint32_t exp = upper | ((value & 0b00110000U) >> 4);
        uint32_t frac = (value & 0b1111U) << (F - 4);
        return std::bit_cast<float>((sign << 31) | (exp << F) | frac);
    };

    for (uint8_t i = 0; i < UINT8_MAX; ++i) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        root->appendNewControlValue(proc, Return, Origin(), root->appendNew<ConstFloatValue>(proc, Origin(), encode(i)));
        CHECK_EQ(compileAndRun<float>(proc), encode(i));
    }
}

void testSShrCompare32(int32_t constantValue)
{
    auto compile = [&](B3::Opcode opcode, uint32_t shiftAmount, uint32_t constantValue) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t>(proc, root);
        auto* shifted = root->appendNew<Value>(proc, SShr, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), shiftAmount));
        auto* constant = root->appendNew<Const32Value>(proc, Origin(), constantValue);
        auto* comparison = root->appendNew<Value>(proc, opcode, Origin(), shifted, constant);
        root->appendNewControlValue(proc, Return, Origin(), comparison);
        return compileProc(proc);
    };

    auto testWithOpcode = [&](B3::Opcode opcode, auto compare) {
        for (uint32_t shiftAmount = 0; shiftAmount < 32; ++shiftAmount) {
            auto code = compile(opcode, shiftAmount, constantValue);
            for (auto input : int32OperandsMore()) {
                for (uint32_t step = 0; step < 1000; ++step) {
                    int32_t before = static_cast<uint32_t>(input.value) - step;
                    int32_t middle = static_cast<uint32_t>(input.value);
                    int32_t after = static_cast<uint32_t>(input.value) + step;
                    CHECK_EQ(invoke<bool>(*code, before), compare(shiftAmount, constantValue, before));
                    CHECK_EQ(invoke<bool>(*code, middle), compare(shiftAmount, constantValue, middle));
                    CHECK_EQ(invoke<bool>(*code, after), compare(shiftAmount, constantValue, after));
                }
            }
        }
    };

    testWithOpcode(Above, [](uint32_t shiftAmount, uint32_t constantValue, int32_t value) { return static_cast<uint32_t>(value >> shiftAmount) > constantValue; });
    testWithOpcode(AboveEqual, [](uint32_t shiftAmount, uint32_t constantValue, int32_t value) { return static_cast<uint32_t>(value >> shiftAmount) >= constantValue; });
    testWithOpcode(Below, [](uint32_t shiftAmount, uint32_t constantValue, int32_t value) { return static_cast<uint32_t>(value >> shiftAmount) < constantValue; });
    testWithOpcode(BelowEqual, [](uint32_t shiftAmount, uint32_t constantValue, int32_t value) { return static_cast<uint32_t>(value >> shiftAmount) <= constantValue; });
}

void testSShrCompare64(int64_t constantValue)
{
    auto compile = [&](B3::Opcode opcode, uint64_t shiftAmount, uint64_t constantValue) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<uint64_t>(proc, root);
        auto* shifted = root->appendNew<Value>(proc, SShr, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), shiftAmount));
        auto* constant = root->appendNew<Const64Value>(proc, Origin(), constantValue);
        auto* comparison = root->appendNew<Value>(proc, opcode, Origin(), shifted, constant);
        root->appendNewControlValue(proc, Return, Origin(), comparison);
        return compileProc(proc);
    };

    auto testWithOpcode = [&](B3::Opcode opcode, auto compare) {
        for (uint64_t shiftAmount = 0; shiftAmount < 64; ++shiftAmount) {
            auto code = compile(opcode, shiftAmount, constantValue);
            for (auto input : int64OperandsMore()) {
                for (uint64_t step = 0; step < 1000; ++step) {
                    int64_t before = static_cast<uint64_t>(input.value) - step;
                    int64_t middle = static_cast<uint64_t>(input.value);
                    int64_t after = static_cast<uint64_t>(input.value) + step;
                    CHECK_EQ(invoke<bool>(*code, before), compare(shiftAmount, constantValue, before));
                    CHECK_EQ(invoke<bool>(*code, middle), compare(shiftAmount, constantValue, middle));
                    CHECK_EQ(invoke<bool>(*code, after), compare(shiftAmount, constantValue, after));
                }
            }
        }
    };

    testWithOpcode(Above, [](uint64_t shiftAmount, uint64_t constantValue, int64_t value) { return static_cast<uint64_t>(value >> shiftAmount) > constantValue; });
    testWithOpcode(AboveEqual, [](uint64_t shiftAmount, uint64_t constantValue, int64_t value) { return static_cast<uint64_t>(value >> shiftAmount) >= constantValue; });
    testWithOpcode(Below, [](uint64_t shiftAmount, uint64_t constantValue, int64_t value) { return static_cast<uint64_t>(value >> shiftAmount) < constantValue; });
    testWithOpcode(BelowEqual, [](uint64_t shiftAmount, uint64_t constantValue, int64_t value) { return static_cast<uint64_t>(value >> shiftAmount) <= constantValue; });
}

void testMulHigh64()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);

    Value* argumentA = arguments[0];
    Value* argumentB = arguments[1];

    root->appendNewControlValue(
        proc, Return, Origin(),
        root->appendNew<Value>(
            proc, MulHigh, Origin(),
            argumentA,
            argumentB));

    auto code = compileProc(proc);
    for (auto a : int64Operands()) {
        for (auto b : int64Operands())
            CHECK_EQ(invoke<int64_t>(*code, a.value, b.value), static_cast<int64_t>((static_cast<Int128>(a.value) * static_cast<Int128>(b.value)) >> 64));
    }
}

void testMulHigh32()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    root->appendNewControlValue(
        proc, Return, Origin(),
        root->appendNew<Value>(
            proc, MulHigh, Origin(),
            arguments[0],
            arguments[1]));

    auto code = compileProc(proc);
    for (auto a : int32Operands()) {
        for (auto b : int32Operands())
            CHECK_EQ(invoke<int32_t>(*code, a.value, b.value), static_cast<int32_t>((static_cast<int64_t>(a.value) * static_cast<int64_t>(b.value)) >> 32));
    }
}

void testUMulHigh64()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<uint64_t, uint64_t>(proc, root);

    Value* argumentA = arguments[0];
    Value* argumentB = arguments[1];

    root->appendNewControlValue(
        proc, Return, Origin(),
        root->appendNew<Value>(
            proc, UMulHigh, Origin(),
            argumentA,
            argumentB));

    auto code = compileProc(proc);
    for (auto a : int64Operands()) {
        for (auto b : int64Operands())
            CHECK_EQ(invoke<uint64_t>(*code, a.value, b.value), static_cast<uint64_t>((static_cast<UInt128>(static_cast<uint64_t>(a.value)) * static_cast<UInt128>(static_cast<uint64_t>(b.value))) >> 64));
    }
}

void testUMulHigh32()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<uint32_t, uint32_t>(proc, root);

    root->appendNewControlValue(
        proc, Return, Origin(),
        root->appendNew<Value>(
            proc, UMulHigh, Origin(),
            arguments[0],
            arguments[1]));

    auto code = compileProc(proc);
    for (auto a : int32Operands()) {
        for (auto b : int32Operands())
            CHECK_EQ(invoke<uint32_t>(*code, a.value, b.value), static_cast<uint32_t>((static_cast<uint64_t>(static_cast<uint32_t>(a.value)) * static_cast<uint64_t>(static_cast<uint32_t>(b.value))) >> 32));
    }
}

void testMemoryCopy()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<void*, void*, void*>(proc, root);
    root->appendNew<BulkMemoryValue>(proc, MemoryCopy, Origin(), arguments[0], arguments[1], arguments[2]);
    root->appendNewControlValue(proc, Return, Origin());

    auto code = compileProc(proc);
    Vector<uint8_t> src(4096 + 1024);
    Vector<uint8_t> dst(4096 + 1024);

    for (unsigned base = 1; base < 4096; base <<= 1) {
        unsigned offset = 0;
        for (auto a : int32Operands()) {
            dst.fill(0);
            src.fill(static_cast<uint8_t>(a.value));
            invoke<void>(*code, dst.mutableSpan().data(), src.span().data(), static_cast<uintptr_t>(base + offset));
            for (unsigned i = 0; i < (base + offset); ++i)
                CHECK_EQ(dst[i], static_cast<uint8_t>(a.value));
            CHECK_EQ(dst[(base + offset)], 0);
            ++offset;
        }
    }

    for (unsigned base = 1; base < 4096; base <<= 1) {
        for (unsigned i = 0; i < src.size(); ++i)
            src[i] = i;
        invoke<void>(*code, src.mutableSpan().data(), src.span().data() + 1, static_cast<uintptr_t>(base));
        for (unsigned i = 0; i < base; ++i)
            CHECK_EQ(src[i], static_cast<uint8_t>(i + 1));
        CHECK_EQ(src[base], static_cast<uint8_t>(base));
    }

    for (unsigned base = 1; base < 4096; base <<= 1) {
        for (unsigned i = 0; i < src.size(); ++i)
            src[i] = i;
        invoke<void>(*code, src.mutableSpan().data() + 1, src.span().data(), static_cast<uintptr_t>(base));
        for (unsigned i = 0; i < base; ++i)
            CHECK_EQ(src[i + 1], static_cast<uint8_t>(i));
        CHECK_EQ(src[0], 0);
    }
}

void testMemoryCopyConstant()
{
    Vector<uint8_t> src(4096 + 1024);
    Vector<uint8_t> dst(4096 + 1024);

    for (unsigned width = 0; width < 128; ++width) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<void*, void*>(proc, root);
        root->appendNew<BulkMemoryValue>(proc, MemoryCopy, Origin(), arguments[0], arguments[1], root->appendIntConstant(proc, Origin(), pointerType(), width));
        root->appendNewControlValue(proc, Return, Origin());
        auto code = compileProc(proc);

        for (auto a : int32Operands()) {
            dst.fill(0);
            src.fill(static_cast<uint8_t>(a.value));
            invoke<void>(*code, dst.mutableSpan().data(), src.span().data());
            for (unsigned i = 0; i < width; ++i)
                CHECK_EQ(dst[i], static_cast<uint8_t>(a.value));
            CHECK_EQ(dst[width], 0);
        }

        for (unsigned i = 0; i < src.size(); ++i)
            src[i] = i;
        invoke<void>(*code, src.mutableSpan().data(), src.span().data() + 1);
        for (unsigned i = 0; i < width; ++i)
            CHECK_EQ(src[i], static_cast<uint8_t>(i + 1));
        CHECK_EQ(src[width], static_cast<uint8_t>(width));

        for (unsigned i = 0; i < src.size(); ++i)
            src[i] = i;
        invoke<void>(*code, src.mutableSpan().data() + 1, src.span().data());
        for (unsigned i = 0; i < width; ++i)
            CHECK_EQ(src[i + 1], static_cast<uint8_t>(i));
        CHECK_EQ(src[0], 0);
    }
}

void testMemoryFill()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<void*, uint32_t, void*>(proc, root);
    root->appendNew<BulkMemoryValue>(proc, MemoryFill, Origin(), arguments[0], arguments[1], arguments[2]);
    root->appendNewControlValue(proc, Return, Origin());

    auto code = compileProc(proc);
    Vector<uint8_t> src(4096 + 1024);

    for (unsigned base = 1; base < 4096; base <<= 1) {
        unsigned offset = 0;
        for (auto a : int32Operands()) {
            src.fill(0);
            invoke<void>(*code, src.mutableSpan().data(), static_cast<uint8_t>(a.value), static_cast<uintptr_t>(base + offset));
            for (unsigned i = 0; i < (base + offset); ++i)
                CHECK_EQ(src[i], static_cast<uint8_t>(a.value));
            CHECK_EQ(src[(base + offset)], 0);
            ++offset;
        }
    }
}

void testMemoryFillConstant()
{
    Vector<uint8_t> src(4096 + 1024);

    for (unsigned width = 0; width < 128; ++width) {
        for (auto a : int32Operands()) {
            Procedure proc;
            BasicBlock* root = proc.addBlock();
            auto arguments = cCallArgumentValues<void*>(proc, root);
            root->appendNew<BulkMemoryValue>(proc, MemoryFill, Origin(), arguments[0], root->appendIntConstant(proc, Origin(), Int32, a.value), root->appendIntConstant(proc, Origin(), pointerType(), width));
            root->appendNewControlValue(proc, Return, Origin());
            auto code = compileProc(proc);

            src.fill(0);
            invoke<void>(*code, src.mutableSpan().data(), static_cast<uint8_t>(a.value));
            for (unsigned i = 0; i < width; ++i)
                CHECK_EQ(src[i], static_cast<uint8_t>(a.value));
            CHECK_EQ(src[width], 0);
        }
    }
}

void testLoadImmutable()
{
    Vector<uint64_t> memory(4);
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<void*, void*>(proc, root);

    auto* value1 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), arguments[0]);
    value1->setReadsMutability(B3::Mutability::Immutable);
    root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const32Value>(proc, Origin(), 0), arguments[1]);
    auto* value2 = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), arguments[0]);
    value2->setReadsMutability(B3::Mutability::Immutable);
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Origin(), value1, value2));
    auto code = compileProc(proc);

    memory.fill(42);
    CHECK_EQ(invoke<uint64_t>(*code, memory.mutableSpan().data(), memory.mutableSpan().data() + 1), 84U);
}

// ARM64 conditional compare (ccmp) tests
// These tests verify that BitAnd/BitOr of comparisons are optimized using ccmp instruction

void testCCmpAnd32(int32_t a, int32_t b, int32_t c, int32_t d)
{
    // Test: (a == b) && (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testCCmpAnd64(int64_t a, int64_t b, int64_t c, int64_t d)
{
    // Test: (a == b) && (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t, int64_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testCCmpOr32(int32_t a, int32_t b, int32_t c, int32_t d)
{
    // Test: (a == b) || (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b || c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testCCmpOr64(int64_t a, int64_t b, int64_t c, int64_t d)
{
    // Test: (a == b) || (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t, int64_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b || c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

// 3-comparison chain tests (nested patterns)
void testCCmpAndAnd32(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f)
{
    // Test: ((a == b) && (c == d)) && (e == f)
    // This should emit: cmp a,b; ccmp c,d; ccmp e,f; branch
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* and1 = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);
    Value* cmp3 = root->appendNew<Value>(proc, Equal, Origin(), arguments[4], arguments[5]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), and1, cmp3);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d && e == f) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d, e, f), expected);
}

void testCCmpOrOr32(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f)
{
    // Test: ((a == b) || (c == d)) || (e == f)
    // This should emit: cmp a,b; ccmp c,d; ccmp e,f; branch
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* or1 = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, cmp2);
    Value* cmp3 = root->appendNew<Value>(proc, Equal, Origin(), arguments[4], arguments[5]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), or1, cmp3);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b || c == d || e == f) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d, e, f), expected);
}

void testCCmpAndOr32(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f)
{
    // Test: ((a == b) && (c == d)) || (e == f)
    // Mixed pattern: AND then OR
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* and1 = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);
    Value* cmp3 = root->appendNew<Value>(proc, Equal, Origin(), arguments[4], arguments[5]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), and1, cmp3);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = ((a == b && c == d) || e == f) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d, e, f), expected);
}

// Tests for ccmn (conditional compare with negative immediates)
void testCCmnAnd32WithNegativeImm(int32_t a, int32_t b)
{
    // Test: (a > 10) && (b == -5)
    // The second comparison should use ccmn with immediate 5
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 10));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), -5));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a > 10 && b == -5) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmnAnd64WithNegativeImm(int64_t a, int64_t b)
{
    // Test: (a > 10) && (b == -31)
    // The second comparison should use ccmn with immediate 31
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[0], root->appendNew<Const64Value>(proc, Origin(), 10));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const64Value>(proc, Origin(), -31));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a > 10 && b == -31) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmpWithLargePositiveImm(int32_t a, int32_t b)
{
    // Test: (a > 10) && (b == 100)
    // The second comparison should use a register (100 > 31)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 10));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 100));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a > 10 && b == 100) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmpWithLargeNegativeImm(int32_t a, int32_t b)
{
    // Test: (a > 10) && (b == -100)
    // The second comparison should use a register (-100 < -31)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 10));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), -100));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a > 10 && b == -100) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Tests for ccmp optimization: smart operand ordering
// This test ensures that when the first comparison has a small immediate (5)
// and the second has a large immediate (1000), we swap them so that the
// large immediate goes into cmp (which has wider immediate range) and the
// small immediate goes into ccmp.
void testCCmpSmartOperandOrdering32(int32_t a, int32_t b)
{
    // Test: (a == 5) && (b == 1000)
    // Should be optimized to: cmp b, 1000; ccmp a, 5, ...
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 5));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 1000));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == 5 && b == 1000) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmpSmartOperandOrdering64(int64_t a, int64_t b)
{
    // Test: (a == 10) && (b == 5000)
    // Should be optimized to: cmp b, 5000; ccmp a, 10, ...
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], root->appendNew<Const64Value>(proc, Origin(), 10));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const64Value>(proc, Origin(), 5000));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == 10 && b == 5000) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Tests for ccmp optimization: operand commutation within ccmp
// This test ensures that if the left operand of a comparison is a small immediate,
// we swap the operands to put the immediate on the right where it can be encoded.
void testCCmpOperandCommutation32(int32_t a, int32_t b)
{
    // Test: (15 == a) && (b > 100)
    // The first comparison should commute to (a == 15)
    // and optimize to: cmp a, 15; ccmp b, 100, ...
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), root->appendNew<Const32Value>(proc, Origin(), 15), arguments[0]);
    Value* cmp2 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 100));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (15 == a && b > 100) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmpOperandCommutation64(int64_t a, int64_t b)
{
    // Test: (a < 50) && (20 == b)
    // The second comparison should commute in the ccmp to (b == 20)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[0], root->appendNew<Const64Value>(proc, Origin(), 50));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), root->appendNew<Const64Value>(proc, Origin(), 20), arguments[1]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a < 50 && 20 == b) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Combined test: both smart ordering and operand commutation
void testCCmpCombinedOptimizations(int32_t a, int32_t b)
{
    // Test: (10 == a) && (b == 2000)
    // First comparison has commutable immediate on left
    // Second comparison has large immediate
    // Should optimize to: cmp b, 2000; ccmp a, 10, ...
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), root->appendNew<Const32Value>(proc, Origin(), 10), arguments[0]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 2000));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (10 == a && b == 2000) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Test for zero register optimization
void testCCmpZeroRegisterOptimization32(int32_t a, int32_t b)
{
    // Test: (a == 0) && (b > 5)
    // The first comparison should use the zero register for 0
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 0));
    Value* cmp2 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 5));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == 0 && b > 5) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmpZeroRegisterOptimization64(int64_t a, int64_t b)
{
    // Test: (0 == a) && (b < 100)
    // The first comparison should use the zero register, and also test commutation
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), root->appendNew<Const64Value>(proc, Origin(), 0), arguments[0]);
    Value* cmp2 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[1], root->appendNew<Const64Value>(proc, Origin(), 100));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (0 == a && b < 100) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Mixed AND/OR tests - these now work with tree-based processing
void testCCmpMixedAndOr32(int32_t a, int32_t b, int32_t c)
{
    // Test: (a == b && b == c) || (a > 100)
    // Left child is AND (logic op), right child is comparison
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], arguments[2]);
    Value* andVal = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);
    Value* cmp3 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 100));
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), andVal, cmp3);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = ((a == b && b == c) || a > 100) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c), expected);
}

void testCCmpMixedOrAnd32(int32_t a, int32_t b, int32_t c)
{
    // Test: (a < 0) || (b == c && c > 50)
    // Left child is comparison, right child is AND (logic op)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[0], root->appendNew<Const32Value>(proc, Origin(), 0));
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[1], arguments[2]);
    Value* cmp3 = root->appendNew<Value>(proc, GreaterThan, Origin(), arguments[2], root->appendNew<Const32Value>(proc, Origin(), 50));
    Value* andVal = root->appendNew<Value>(proc, BitAnd, Origin(), cmp2, cmp3);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, andVal);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a < 0 || (b == c && c > 50)) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c), expected);
}

void testCCmpNegatedAnd32(int32_t a, int32_t b)
{
    // Test: !(a > 10 && b == 20)
    // This becomes: (a > 10 && b == 20) == 0
    // Should be optimized with ccmp and final condition negation
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();

    Value* arg1 = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    Value* arg2 = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);

    Value* greaterThan10 = root->appendNew<Value>(
        proc, GreaterThan, Origin(),
        root->appendNew<Value>(proc, Trunc, Origin(), arg1),
        root->appendNew<Const32Value>(proc, Origin(), 10));

    Value* equal20 = root->appendNew<Value>(
        proc, Equal, Origin(),
        root->appendNew<Value>(proc, Trunc, Origin(), arg2),
        root->appendNew<Const32Value>(proc, Origin(), 20));

    Value* andResult = root->appendNew<Value>(
        proc, BitAnd, Origin(),
        greaterThan10,
        equal20);

    // Negation: andResult == 0
    Value* negated = root->appendNew<Value>(
        proc, Equal, Origin(),
        andResult,
        root->appendNew<Const32Value>(proc, Origin(), 0));

    root->appendNewControlValue(
        proc, Branch, Origin(),
        negated,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = !(a > 10 && b == 20) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

void testCCmpNegatedOr32(int32_t a, int32_t b)
{
    // Test: !(a < 5 || b >= 100)
    // This becomes: (a < 5 || b >= 100) == 0
    // Should be optimized with ccmp and final condition negation
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();

    Value* arg1 = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    Value* arg2 = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);

    Value* lessThan5 = root->appendNew<Value>(
        proc, LessThan, Origin(),
        root->appendNew<Value>(proc, Trunc, Origin(), arg1),
        root->appendNew<Const32Value>(proc, Origin(), 5));

    Value* greaterOrEqual100 = root->appendNew<Value>(
        proc, GreaterEqual, Origin(),
        root->appendNew<Value>(proc, Trunc, Origin(), arg2),
        root->appendNew<Const32Value>(proc, Origin(), 100));

    Value* orResult = root->appendNew<Value>(
        proc, BitOr, Origin(),
        lessThan5,
        greaterOrEqual100);

    // Negation: orResult == 0
    Value* negated = root->appendNew<Value>(
        proc, Equal, Origin(),
        orResult,
        root->appendNew<Const32Value>(proc, Origin(), 0));

    root->appendNewControlValue(
        proc, Branch, Origin(),
        negated,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = !(a < 5 || b >= 100) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Test for mixed-width compare chains (32-bit and 64-bit comparisons in same chain)
// This tests the per-ccmp width handling fix
void testCCmpMixedWidth32And64(int32_t a, int64_t b, int32_t c)
{
    // Test: (a == 5) && (b == 1000) && (c == 10)
    // First is 32-bit, second is 64-bit, third is 32-bit
    // Each ccmp must use its own width for the opcode
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int64_t, int32_t>(proc, root);

    // arguments[0] is Int32, arguments[1] is Int64, arguments[2] is Int32
    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(),
        arguments[0],
        root->appendNew<Const32Value>(proc, Origin(), 5));

    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(),
        arguments[1],
        root->appendNew<Const64Value>(proc, Origin(), 1000));

    Value* and1 = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    Value* cmp3 = root->appendNew<Value>(proc, Equal, Origin(),
        arguments[2],
        root->appendNew<Const32Value>(proc, Origin(), 10));

    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), and1, cmp3);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == 5 && b == 1000 && c == 10) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c), expected);
}

void testCCmpMixedWidth64And32(int64_t a, int32_t b)
{
    // Test: (a == 5000) && (b == 10)
    // First is 64-bit, second is 32-bit
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(),
        arguments[0],
        root->appendNew<Const64Value>(proc, Origin(), 5000));

    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(),
        arguments[1],
        root->appendNew<Const32Value>(proc, Origin(), 10));

    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == 5000 && b == 10) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b), expected);
}

// Regression for findCompareChain rollback: pre-fix, BitOr's BitXor-child
// failure leaked an inner logic node, dropping LessThan from the chain.
void testCCmpChainRollback(int32_t i, int32_t len, int32_t a, int32_t b, int32_t c, int32_t d)
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>(proc, root);

    Value* iLtLen = root->appendNew<Value>(proc, LessThan, Origin(), arguments[0], arguments[1]);
    Value* eqAB = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* eqCD = root->appendNew<Value>(proc, Equal, Origin(), arguments[4], arguments[5]);
    Value* innerAnd = root->appendNew<Value>(proc, BitAnd, Origin(), eqAB, eqCD);
    Value* xorAC = root->appendNew<Value>(proc, BitXor, Origin(), arguments[2], arguments[4]);
    Value* outerOr = root->appendNew<Value>(proc, BitOr, Origin(), innerAnd, xorAC);
    Value* eqZero = root->appendNew<Value>(proc, Equal, Origin(),
        outerOr, root->appendNew<Const32Value>(proc, Origin(), 0));
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), iLtLen, eqZero);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int zero = 0; // style checker complains about == 0
    int32_t expected = (i < len && ((((a == b) & (c == d)) | (a ^ c)) == zero)) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, i, len, a, b, c, d), expected);
}

void testConstDoubleZero()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    root->appendNewControlValue(proc, Return, Origin(),
        root->appendNew<ConstDoubleValue>(proc, Origin(), 0.0));
    CHECK_EQ(compileAndRun<double>(proc), 0.0);
}

void testConstDoubleNegativeZero()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    root->appendNewControlValue(proc, Return, Origin(),
        root->appendNew<ConstDoubleValue>(proc, Origin(), -0.0));
    double result = compileAndRun<double>(proc);
    CHECK_EQ(std::bit_cast<uint64_t>(result), 0x8000000000000000ULL);
}

void testConstFloatZero()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    root->appendNewControlValue(proc, Return, Origin(),
        root->appendNew<ConstFloatValue>(proc, Origin(), 0.0f));
    CHECK_EQ(compileAndRun<float>(proc), 0.0f);
}

void testConstFloatNegativeZero()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    root->appendNewControlValue(proc, Return, Origin(),
        root->appendNew<ConstFloatValue>(proc, Origin(), -0.0f));
    float result = compileAndRun<float>(proc);
    CHECK_EQ(std::bit_cast<uint32_t>(result), 0x80000000U);
}

void testConstDoubleAddZero()
{
    auto test = [&] (double input, double expected) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<double>(proc, root);
        Value* zero = root->appendNew<ConstDoubleValue>(proc, Origin(), 0.0);
        Value* result = root->appendNew<Value>(proc, Add, Origin(), arguments[0], zero);
        root->appendNewControlValue(proc, Return, Origin(), result);
        CHECK_EQ(compileAndRun<double>(proc, input), expected);
    };

    test(2.5, 2.5);
    test(-3.14, -3.14);
    test(0.0, 0.0);
}

void testConstFloatAddZero()
{
    auto test = [&] (float input, float expected) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<float>(proc, root);
        Value* zero = root->appendNew<ConstFloatValue>(proc, Origin(), 0.0f);
        Value* result = root->appendNew<Value>(proc, Add, Origin(), arguments[0], zero);
        root->appendNewControlValue(proc, Return, Origin(), result);
        CHECK_EQ(compileAndRun<float>(proc, input), expected);
    };

    test(2.5f, 2.5f);
    test(-3.14f, -3.14f);
    test(0.0f, 0.0f);
}

void testConstDoubleCompareZero()
{
    auto test = [&] (double input, int32_t expected) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<double>(proc, root);
        Value* zero = root->appendNew<ConstDoubleValue>(proc, Origin(), 0.0);
        Value* result = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], zero);
        root->appendNewControlValue(proc, Return, Origin(), result);
        CHECK_EQ(compileAndRun<int32_t>(proc, input), expected);
    };

    test(0.0, 1);
    test(-0.0, 1); // -0.0 == 0.0
    test(1.0, 0);
    test(-1.0, 0);
}

void testConstFloatCompareZero()
{
    auto test = [&] (float input, int32_t expected) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<float>(proc, root);
        Value* zero = root->appendNew<ConstFloatValue>(proc, Origin(), 0.0f);
        Value* result = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], zero);
        root->appendNewControlValue(proc, Return, Origin(), result);
        CHECK_EQ(compileAndRun<int32_t>(proc, input), expected);
    };

    test(0.0f, 1);
    test(-0.0f, 1); // -0.0f == 0.0f
    test(1.0f, 0);
    test(-1.0f, 0);
}

void testConstDoubleSelectZero()
{
    auto test = [&] (int32_t selector, double input, double expected) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t, double>(proc, root);
        Value* zero = root->appendNew<ConstDoubleValue>(proc, Origin(), 0.0);
        Value* result = root->appendNew<Value>(proc, Select, Origin(), arguments[0], arguments[1], zero);
        root->appendNewControlValue(proc, Return, Origin(), result);
        CHECK_EQ(compileAndRun<double>(proc, selector, input), expected);
    };

    test(1, 2.5, 2.5);
    test(0, 2.5, 0.0);
}

void testConstFloatSelectZero()
{
    auto test = [&] (int32_t selector, float input, float expected) {
        Procedure proc;
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t, float>(proc, root);
        Value* zero = root->appendNew<ConstFloatValue>(proc, Origin(), 0.0f);
        Value* result = root->appendNew<Value>(proc, Select, Origin(), arguments[0], arguments[1], zero);
        root->appendNewControlValue(proc, Return, Origin(), result);
        CHECK_EQ(compileAndRun<float>(proc, selector, input), expected);
    };

    test(1, 2.5f, 2.5f);
    test(0, 2.5f, 0.0f);
}

void testConstDoubleMultipleZeroUses()
{
    // Test that multiple uses of zero constant work correctly
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double>(proc, root);
    Value* zero = root->appendNew<ConstDoubleValue>(proc, Origin(), 0.0);

    // Use zero multiple times: (a + 0) + (b + 0)
    Value* aPlusZero = root->appendNew<Value>(proc, Add, Origin(), arguments[0], zero);
    Value* bPlusZero = root->appendNew<Value>(proc, Add, Origin(), arguments[1], zero);
    Value* result = root->appendNew<Value>(proc, Add, Origin(), aPlusZero, bPlusZero);

    root->appendNewControlValue(proc, Return, Origin(), result);

    CHECK_EQ(compileAndRun<double>(proc, 2.5, 3.5), 6.0);
}

void testConstFloatMultipleZeroUses()
{
    // Test that multiple uses of zero constant work correctly
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<float, float>(proc, root);
    Value* zero = root->appendNew<ConstFloatValue>(proc, Origin(), 0.0f);

    // Use zero multiple times: (a + 0) + (b + 0)
    Value* aPlusZero = root->appendNew<Value>(proc, Add, Origin(), arguments[0], zero);
    Value* bPlusZero = root->appendNew<Value>(proc, Add, Origin(), arguments[1], zero);
    Value* result = root->appendNew<Value>(proc, Add, Origin(), aPlusZero, bPlusZero);

    root->appendNewControlValue(proc, Return, Origin(), result);

    CHECK_EQ(compileAndRun<float>(proc, 2.5f, 3.5f), 6.0f);
}

void testFCCmpAndDouble(double a, double b, double c, double d)
{
    // Test: (a == b) && (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpOrDouble(double a, double b, double c, double d)
{
    // Test: (a == b) || (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b || c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpAndFloat(float a, float b, float c, float d)
{
    // Test: (a == b) && (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<float, float, float, float>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpOrFloat(float a, float b, float c, float d)
{
    // Test: (a == b) || (c == d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<float, float, float, float>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b || c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpAndAndDouble(double a, double b, double c, double d, double e, double f)
{
    // Test: ((a == b) && (c == d)) && (e == f)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* and1 = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);
    Value* cmp3 = root->appendNew<Value>(proc, Equal, Origin(), arguments[4], arguments[5]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), and1, cmp3);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d && e == f) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d, e, f), expected);
}

void testFCCmpMixedIntDouble(int32_t a, int32_t b, double c, double d)
{
    // Test: (a == b) && (c < d) — int compare AND float compare
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c < d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpMixedDoubleInt(double a, double b, int32_t c, int32_t d)
{
    // Test: (a < b) && (c == d) — float compare AND int compare
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, int32_t, int32_t>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a < b && c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpLessThanAndDouble(double a, double b, double c, double d)
{
    // Test: (a < b) && (c < d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a < b && c < d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpGreaterEqualOrDouble(double a, double b, double c, double d)
{
    // Test: (a >= b) || (c >= d)
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, GreaterEqual, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, GreaterEqual, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitOr, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a >= b || c >= d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpNaN(double a, double b, double c, double d)
{
    // Test: (a == b) && (c == d) with NaN inputs
    // NaN comparisons should always be false
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, Equal, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, Equal, Origin(), arguments[2], arguments[3]);
    Value* condition = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    root->appendNewControlValue(
        proc, Branch, Origin(), condition,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = (a == b && c == d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

void testFCCmpNegatedAndDouble(double a, double b, double c, double d)
{
    // Test: !(a < b && c < d)
    // This becomes: (a < b && c < d) == 0
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* thenCase = proc.addBlock();
    BasicBlock* elseCase = proc.addBlock();
    auto arguments = cCallArgumentValues<double, double, double, double>(proc, root);

    Value* cmp1 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[0], arguments[1]);
    Value* cmp2 = root->appendNew<Value>(proc, LessThan, Origin(), arguments[2], arguments[3]);
    Value* andResult = root->appendNew<Value>(proc, BitAnd, Origin(), cmp1, cmp2);

    Value* negated = root->appendNew<Value>(
        proc, Equal, Origin(),
        andResult,
        root->appendNew<Const32Value>(proc, Origin(), 0));

    root->appendNewControlValue(
        proc, Branch, Origin(), negated,
        FrequentedBlock(thenCase), FrequentedBlock(elseCase));

    thenCase->appendNewControlValue(
        proc, Return, Origin(),
        thenCase->appendNew<Const32Value>(proc, Origin(), 1));

    elseCase->appendNewControlValue(
        proc, Return, Origin(),
        elseCase->appendNew<Const32Value>(proc, Origin(), 0));

    int32_t expected = !(a < b && c < d) ? 1 : 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, a, b, c, d), expected);
}

#if USE(BUN_JSC_ADDITIONS)

namespace {

unsigned countValues(Procedure& proc, B3::Opcode opcode)
{
    unsigned count = 0;
    for (Value* value : proc.values())
        count += value->opcode() == opcode;
    return count;
}

// value = Load(pointer + firstOffset); <between>; return value + Load(pointer + firstOffset)
// Returns how many loads are left after load elimination.
unsigned loadsLeftAround(bool hasCodeFromC, const Function<void(Procedure&, BasicBlock*, Value* pointer)>& between)
{
    Procedure proc;
    if (hasCodeFromC)
        proc.setHasCodeFromC();
    BasicBlock* root = proc.addBlock();
    Value* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    Value* first = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), pointer, 8);
    between(proc, root, pointer);
    Value* second = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), pointer, 8);
    root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Origin(), first, second));
    proc.resetReachability();
    eliminateCommonSubexpressions(proc);
    validate(proc);
    return countValues(proc, Load);
}

} // anonymous namespace

void testLoadEliminationByAddress()
{
    auto storeAt = [](int32_t offset, B3::Opcode opcode = Store) {
        return [=](Procedure& proc, BasicBlock* block, Value* pointer) {
            block->appendNew<MemoryValue>(proc, opcode, Origin(), block->appendNew<Const32Value>(proc, Origin(), 1), pointer, offset);
        };
    };
    // The loaded bytes are [8, 12). Code lowered from C: a store through the same pointer that misses them
    // leaves the first load good.
    for (int32_t offset : { 0, 4, 12, 16, -4 })
        CHECK_EQ(loadsLeftAround(true, storeAt(offset)), 1u);
    CHECK_EQ(loadsLeftAround(true, storeAt(7, Store8)), 1u);
    CHECK_EQ(loadsLeftAround(true, storeAt(12, Store8)), 1u);
    CHECK_EQ(loadsLeftAround(true, storeAt(6, Store16)), 1u);
    // One that touches any of them does not.
    for (int32_t offset : { 5, 7, 9, 11 })
        CHECK_EQ(loadsLeftAround(true, storeAt(offset)), 2u);
    for (int32_t offset : { 8, 11 })
        CHECK_EQ(loadsLeftAround(true, storeAt(offset, Store8)), 2u);
    CHECK_EQ(loadsLeftAround(true, storeAt(7, Store16)), 2u);
    CHECK_EQ(loadsLeftAround(true, storeAt(11, Store16)), 2u);
    // Nor does a store through another pointer, a fenced store, an atomic, a fence, a call or a patchpoint.
    CHECK_EQ(loadsLeftAround(true, [](Procedure& proc, BasicBlock* block, Value*) {
        Value* other = block->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
        block->appendNew<MemoryValue>(proc, Store, Origin(), block->appendNew<Const32Value>(proc, Origin(), 1), other, 0);
    }), 2u);
    CHECK_EQ(loadsLeftAround(true, [](Procedure& proc, BasicBlock* block, Value* pointer) {
        block->appendNew<MemoryValue>(proc, Store, Origin(), block->appendNew<Const32Value>(proc, Origin(), 1), pointer, 0, HeapRange::top(), HeapRange::top());
    }), 2u);
    CHECK_EQ(loadsLeftAround(true, [](Procedure& proc, BasicBlock* block, Value* pointer) {
        block->appendNew<AtomicValue>(proc, AtomicXchgAdd, Origin(), Width32, block->appendNew<Const32Value>(proc, Origin(), 1), pointer, 0);
    }), 2u);
    CHECK_EQ(loadsLeftAround(true, [](Procedure& proc, BasicBlock* block, Value*) {
        block->appendNew<FenceValue>(proc, Origin());
    }), 2u);
    CHECK_EQ(loadsLeftAround(true, [](Procedure& proc, BasicBlock* block, Value*) {
        PatchpointValue* patchpoint = block->appendNew<PatchpointValue>(proc, Void, Origin());
        patchpoint->effects = Effects::forCall();
        patchpoint->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
    }), 2u);
    // Everywhere else, only the abstract heaps say what a store leaves alone: these two have the same one.
    for (int32_t offset : { 0, 4, 12, 16 })
        CHECK_EQ(loadsLeftAround(false, storeAt(offset)), 2u);
}

void testLoadEliminationAcrossStackSlots()
{
    // store to slot A; load from slot B; store to slot A; load from slot B
    auto loadsLeft = [](bool hasCodeFromC, bool sameSlot) {
        Procedure proc;
        if (hasCodeFromC)
            proc.setHasCodeFromC();
        BasicBlock* root = proc.addBlock();
        Value* a = root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(16));
        Value* b = sameSlot ? a : root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(16));
        Value* argument = root->appendNew<Value>(proc, Trunc, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
        root->appendNew<MemoryValue>(proc, Store, Origin(), argument, b, 0);
        Value* first = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), b, 0);
        root->appendNew<MemoryValue>(proc, Store, Origin(), first, a, 0);
        Value* second = root->appendNew<MemoryValue>(proc, Load, Int32, Origin(), b, 0);
        root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Origin(), first, second));
        proc.resetReachability();
        eliminateCommonSubexpressions(proc);
        validate(proc);
        unsigned loads = countValues(proc, Load);
        CHECK_EQ(compileAndRun<int32_t>(proc, 21), 42);
        return loads;
    };
    // The first load is the stored value either way; the second is too when the store between them is to
    // another slot, or is of the value the slot already holds.
    CHECK_EQ(loadsLeft(true, false), 0u);
    CHECK_EQ(loadsLeft(true, true), 0u);
    CHECK_EQ(loadsLeft(false, false), 1u);
}

void testRematerializeStackAddresses()
{
    // long f(long x) { long a[24]; for each i: a[i] = x + i; escape(a); return sum of a[i]; }
    auto build = [](Procedure& proc) {
        BasicBlock* root = proc.addBlock();
        Value* base = root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(24 * 8));
        Value* argument = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
        for (int i = 0; i < 24; ++i) {
            Value* element = root->appendNew<Value>(proc, Add, Origin(), argument, root->appendNew<Const64Value>(proc, Origin(), i));
            root->appendNew<MemoryValue>(proc, Store, Origin(), element, base, i * 8);
        }
        // Something opaque that is handed the address, so the stores and loads stay.
        PatchpointValue* escape = root->appendNew<PatchpointValue>(proc, Void, Origin());
        escape->effects = Effects::forCall();
        escape->append(base, ValueRep::SomeRegister);
        escape->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
        Value* sum = root->appendNew<Const64Value>(proc, Origin(), 0);
        for (int i = 0; i < 24; ++i)
            sum = root->appendNew<Value>(proc, Add, Origin(), sum, root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), base, i * 8));
        root->appendNewControlValue(proc, Return, Origin(), sum);
    };
    int64_t expected = 24 * 1000 + 23 * 24 / 2;
    {
        Procedure proc;
        proc.setHasCodeFromC();
        build(proc);
        auto compilation = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*compilation, static_cast<int64_t>(1000)), expected);
        // Every access addresses the slot itself; only the patchpoint needs the address in a register.
        if (isX86() && Options::defaultB3OptLevel() == 2) {
            unsigned addressComputations = 0;
            checkDisassembly(*compilation, [&](const char* disassembly) {
                // `lea -0xc0(%rbp), %rax`, as against `lea 0x3(%rdi), %rax`, which is an addition.
                for (const char* cursor = disassembly; (cursor = strstr(cursor, "lea ")); ++cursor) {
                    const char* endOfLine = strchr(cursor, '\n');
                    const char* frameRelative = strstr(cursor, "(%rbp)");
                    addressComputations += frameRelative && (!endOfLine || frameRelative < endOfLine);
                }
                return true;
            }, "could not read the disassembly");
            CHECK_EQ(addressComputations, 1u);
        }
    }
    {
        // The phase itself: every user gets a SlotBase of its own, right before it.
        Procedure proc;
        proc.setHasCodeFromC();
        build(proc);
        proc.resetReachability();
        rematerializeStackAddresses(proc);
        validate(proc);
        CHECK_EQ(countValues(proc, SlotBase), 49u);
        for (BasicBlock* block : proc) {
            for (unsigned i = 0; i < block->size(); ++i) {
                for (Value* child : block->at(i)->children()) {
                    if (child->opcode() == SlotBase)
                        CHECK(i && block->at(i - 1) == child);
                }
            }
        }
    }
    {
        // It leaves every other procedure as it is.
        Procedure proc;
        build(proc);
        proc.resetReachability();
        rematerializeStackAddresses(proc);
        CHECK_EQ(countValues(proc, SlotBase), 1u);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(1000)), expected);
    }
}

void testStackAddressInAUserThatLowersToALoop()
{
    // A compare-and-swap whose operands are a stack slot's address: on a target where it becomes a loop of
    // its own blocks, the address has to be computed ahead of the loop.
    for (bool useBranch : { false, true }) {
        Procedure proc;
        proc.setHasCodeFromC();
        BasicBlock* root = proc.addBlock();
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
        Value* cell = root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(8));
        Value* local = root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(8));
        Value* zero = root->appendNew<Const64Value>(proc, Origin(), 0);
        root->appendNew<MemoryValue>(proc, Store, Origin(), zero, cell, 0);
        // cell was 0: it becomes &local, and the old value (0) comes back.
        Value* old = root->appendNew<AtomicValue>(proc, AtomicStrongCAS, Origin(), Width64, zero, local, cell);
        Value* stored = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), cell, 0);
        Value* isLocal = root->appendNew<Value>(proc, Equal, Origin(), stored, local);
        if (useBranch) {
            root->appendNewControlValue(proc, Branch, Origin(), root->appendNew<Value>(proc, Equal, Origin(), old, zero), FrequentedBlock(taken), FrequentedBlock(notTaken));
            taken->appendNewControlValue(proc, Return, Origin(), isLocal);
            notTaken->appendNewControlValue(proc, Return, Origin(), notTaken->appendNew<Const32Value>(proc, Origin(), -1));
        } else {
            root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, BitAnd, Origin(), isLocal, root->appendNew<Value>(proc, Equal, Origin(), old, zero)));
            taken->appendNewControlValue(proc, Oops, Origin());
            notTaken->appendNewControlValue(proc, Oops, Origin());
        }
        CHECK_EQ(compileAndRun<int32_t>(proc), 1);
    }
}

void testAccessBelowAStackSlot()
{
    // C may form any address from a local's; what it finds there is its business, and B3 compiles it.
    // void f(long offset, long* out) { long low = 7, high = 7; out[0] = *(long*)((char*)&high + offset); out[1] = (&high)[-1]; out[2] = (&low)[-1]; }
    for (unsigned optLevel : { 0, 1, 2 }) {
        Procedure proc;
        proc.setHasCodeFromC();
        proc.setOptLevel(optLevel);
        BasicBlock* root = proc.addBlock();
        Value* offset = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
        Value* out = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
        Value* low = root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(8));
        Value* high = root->appendNew<SlotBaseValue>(proc, Origin(), proc.addStackSlot(8));
        root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const64Value>(proc, Origin(), 7), low, 0);
        root->appendNew<MemoryValue>(proc, Store, Origin(), root->appendNew<Const64Value>(proc, Origin(), 7), high, 0);
        // Where the offset is not known until the code runs, and where it is: in the instruction, and added first.
        Value* belowByRegister = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), root->appendNew<Value>(proc, Add, Origin(), high, offset), 0);
        Value* belowByAddition = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), root->appendNew<Value>(proc, Add, Origin(), high, root->appendNew<Const64Value>(proc, Origin(), -8)), 0);
        Value* belowTheOther = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), low, -8);
        root->appendNew<MemoryValue>(proc, Store, Origin(), belowByRegister, out, 0);
        root->appendNew<MemoryValue>(proc, Store, Origin(), belowByAddition, out, 8);
        root->appendNew<MemoryValue>(proc, Store, Origin(), belowTheOther, out, 16);
        root->appendNewControlValue(proc, Return, Origin());

        generateToAir(proc);
        // The loads are there, the offset in them: below the slot itself.
        unsigned accessesBelowASlot = 0;
        for (Air::BasicBlock* block : proc.code()) {
            for (Air::Inst& inst : *block) {
                for (Air::Arg& arg : inst.args())
                    accessesBelowASlot += arg.isStack() && arg.offset() == -8;
            }
        }
        CHECK_EQ(accessesBelowASlot, optLevel ? 2u : 1u);

        Air::prepareForGeneration(proc.code());
        CCallHelpers jit;
        generate(proc, jit);
        LinkBuffer linkBuffer(jit, nullptr);
        auto code = FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "testb3 compilation");
        // Whatever is there, the two that name the same place found the same thing.
        int64_t found[3] = { 1, 2, 3 };
        invoke<void>(code.code(), static_cast<int64_t>(-8), found);
        CHECK_EQ(found[0], found[1]);
    }
}

void testRegistersACallerInAnotherConventionExpectsKept()
{
    // Code whose callers follow a convention that keeps more registers than the JIT's own does (C on Windows:
    // rsi, rdi and xmm6 to xmm15): the general-purpose ones are given to Air as more callee saves, which it saves
    // like any other it uses, and the vector ones are pinned, which keeps every one of their 128 bits out of use.
#if CPU(X86_64)
    const GPRReg kept[] = { X86Registers::esi, X86Registers::edi };
    const FPRReg pinned[] = { X86Registers::xmm6, X86Registers::xmm7, X86Registers::xmm8, X86Registers::xmm9, X86Registers::xmm10,
        X86Registers::xmm11, X86Registers::xmm12, X86Registers::xmm13, X86Registers::xmm14, X86Registers::xmm15 };
#elif CPU(ARM64)
    const GPRReg kept[] = { ARM64Registers::x10, ARM64Registers::x11 };
    const FPRReg pinned[] = { ARM64Registers::q16, ARM64Registers::q17, ARM64Registers::q18, ARM64Registers::q19, ARM64Registers::q20,
        ARM64Registers::q21, ARM64Registers::q22, ARM64Registers::q23, ARM64Registers::q24, ARM64Registers::q25 };
#else
    return;
#endif
#if CPU(X86_64) || CPU(ARM64)
    // double inner(<two arguments its caller keeps in the registers under test>, const double* in) { thirty doubles loaded, all of them live at once; the two registers overwritten; their sum }
    constexpr unsigned count = 30;
    Procedure inner;
    {
        RegisterSet additional;
        for (GPRReg reg : kept)
            additional.add(reg, IgnoreVectors);
        inner.code().setAdditionalCalleeSaveRegisters(additional);
        for (FPRReg reg : pinned)
            inner.pinRegister(reg);
        BasicBlock* root = inner.addBlock();
        Value* in = root->appendNew<ArgumentRegValue>(inner, Origin(), GPRInfo::argumentGPR2);
        Vector<Value*> loaded;
        for (unsigned i = 0; i < count; ++i)
            loaded.append(root->appendNew<MemoryValue>(inner, Load, Double, Origin(), in, static_cast<int32_t>(i * sizeof(double))));
        PatchpointValue* overwrite = root->appendNew<PatchpointValue>(inner, Void, Origin());
        overwrite->effects = Effects::forCall();
        RegisterSet overwritten;
        for (GPRReg reg : kept)
            overwritten.add(reg, IgnoreVectors);
        overwrite->clobberLate(overwritten);
        // Every one of them is needed after it, in a register or a spill slot.
        for (Value* value : loaded)
            overwrite->append(value, ValueRep::ColdAny);
        overwrite->setGenerator([kept](CCallHelpers& jit, const StackmapGenerationParams&) {
            for (GPRReg reg : kept)
                jit.move(CCallHelpers::TrustedImm64(0x0123456789abcdefll), reg);
        });
        Value* sum = loaded[0];
        for (unsigned i = 1; i < count; ++i)
            sum = root->appendNew<Value>(inner, Add, Origin(), sum, loaded[i]);
        root->appendNewControlValue(inner, Return, Origin(), sum);
    }
    auto innerCode = compileProc(inner);

    // long outer(const double* in, double* sum, uint64_t* found): the registers given values, inner called, what is in them after.
    Procedure outer;
    {
        BasicBlock* root = outer.addBlock();
        Value* in = root->appendNew<ArgumentRegValue>(outer, Origin(), GPRInfo::argumentGPR0);
        Value* sum = root->appendNew<ArgumentRegValue>(outer, Origin(), GPRInfo::argumentGPR1);
        Value* found = root->appendNew<ArgumentRegValue>(outer, Origin(), GPRInfo::argumentGPR2);
        PatchpointValue* call = root->appendNew<PatchpointValue>(outer, Void, Origin());
        call->effects = Effects::forCall();
        call->append(in, ValueRep::reg(GPRInfo::argumentGPR2));
        call->append(sum, ValueRep::reg(GPRInfo::nonArgGPR0));
        call->append(found, ValueRep::reg(GPRInfo::nonArgGPR1));
        RegisterSet everything = RegisterSet::allRegisters();
        everything.exclude(RegisterSet::stackRegisters());
        everything.exclude(RegisterSet::reservedHardwareRegisters());
        call->clobberLate(everything);
        void* target = innerCode->code().taggedPtr();
        call->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams&) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            // This code's own callers expect callee saves kept too: nonArgGPR0 and nonArgGPR1 are not among them on
            // either target, and they are what is needed after the call.
            jit.pushPair(GPRInfo::nonArgGPR0, GPRInfo::nonArgGPR1);
            for (unsigned i = 0; i < std::size(kept); ++i)
                jit.move(CCallHelpers::TrustedImm64(0x1111111111111111ll * (i + 1)), kept[i]);
            for (unsigned i = 0; i < std::size(pinned); ++i) {
                jit.move(CCallHelpers::TrustedImm64(0x0101010101010101ll * (i + 1)), GPRInfo::nonArgGPR0);
                jit.vectorSplatInt64(GPRInfo::nonArgGPR0, pinned[i]);
            }
            jit.move(CCallHelpers::TrustedImmPtr(target), GPRInfo::nonArgGPR0);
            jit.call(GPRInfo::nonArgGPR0, JITCompilationPtrTag);
            jit.popPair(GPRInfo::nonArgGPR0, GPRInfo::nonArgGPR1);
            jit.storeDouble(FPRInfo::returnValueFPR, CCallHelpers::Address(GPRInfo::nonArgGPR0));
            unsigned offset = 0;
            for (GPRReg reg : kept) {
                jit.store64(reg, CCallHelpers::Address(GPRInfo::nonArgGPR1, offset));
                offset += 8;
            }
            for (FPRReg reg : pinned) {
                jit.storeVector(reg, CCallHelpers::Address(GPRInfo::nonArgGPR1, offset));
                offset += 16;
            }
        });
        root->appendNewControlValue(outer, Return, Origin());
    }
    outer.setUsesSIMD();
    auto outerCode = compileProc(outer);

    double in[count];
    double expected = 0;
    for (unsigned i = 0; i < count; ++i) {
        in[i] = i + 0.5;
        expected += in[i];
    }
    double sum = 0;
    uint64_t found[std::size(kept) + 2 * std::size(pinned)] = { };
    invoke<void>(*outerCode, in, &sum, found);
    CHECK_EQ(sum, expected);
    for (unsigned i = 0; i < std::size(kept); ++i)
        CHECK_EQ(found[i], 0x1111111111111111ull * (i + 1));
    for (unsigned i = 0; i < std::size(pinned); ++i) {
        CHECK_EQ(found[std::size(kept) + 2 * i], 0x0101010101010101ull * (i + 1));
        CHECK_EQ(found[std::size(kept) + 2 * i + 1], 0x0101010101010101ull * (i + 1));
    }
#endif
}

#endif // USE(BUN_JSC_ADDITIONS)

void testCompareAndSwapIsNotMovedPastALoad()
{
    // old = CAS(cell: 0 -> 5); seen = *cell (or: *cell = 7); then something that tests whether it swapped.
    // The compare-and-swap can be emitted where its result is tested, but not if that is past the access between
    // them. Each of these is a shape the lowering emits as one compare-and-swap that sets the flags.
    enum class Shape { EqualStrong, BranchEqualStrong, BranchStrongExpectingZero, XorWeak, BranchWeak };
    for (Shape shape : { Shape::EqualStrong, Shape::BranchEqualStrong, Shape::BranchStrongExpectingZero, Shape::XorWeak, Shape::BranchWeak }) {
        for (bool storeBetween : { false, true }) {
            Procedure proc;
            BasicBlock* root = proc.addBlock();
            BasicBlock* swappedCase = proc.addBlock();
            BasicBlock* notSwappedCase = proc.addBlock();
            Value* cell = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
            Value* zero = root->appendNew<Const64Value>(proc, Origin(), 0);
            Value* five = root->appendNew<Const64Value>(proc, Origin(), 5);
            bool isWeak = shape == Shape::XorWeak || shape == Shape::BranchWeak;
            // A strong one gives back the old value, a weak one whether it swapped.
            Value* result = root->appendNew<AtomicValue>(proc, isWeak ? AtomicWeakCAS : AtomicStrongCAS, Origin(), Width64, zero, five, cell);
            Value* seen;
            if (storeBetween) {
                seen = root->appendNew<Const64Value>(proc, Origin(), 7);
                root->appendNew<MemoryValue>(proc, Store, Origin(), seen, cell, 0);
            } else
                seen = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), cell, 0);
            Value* notSwapped = notSwappedCase->appendNew<Const64Value>(proc, Origin(), -1);
            // The test's result as a value of its own: not part of a branch or a select.
            auto returnByArithmetic = [&](Value* scaled, int64_t scale) {
                root->appendNewControlValue(proc, Return, Origin(), root->appendNew<Value>(proc, Add, Origin(), seen, root->appendNew<Value>(proc, Mul, Origin(), root->appendNew<Value>(proc, ZExt32, Origin(), scaled), root->appendNew<Const64Value>(proc, Origin(), scale))));
                swappedCase->appendNewControlValue(proc, Oops, Origin());
                notSwappedCase->appendNewControlValue(proc, Oops, Origin());
            };
            auto returnByBranch = [&](Value* predicate, bool takenMeansSwapped) {
                root->appendNewControlValue(proc, Branch, Origin(), predicate, FrequentedBlock(takenMeansSwapped ? swappedCase : notSwappedCase), FrequentedBlock(takenMeansSwapped ? notSwappedCase : swappedCase));
                swappedCase->appendNewControlValue(proc, Return, Origin(), seen);
                notSwappedCase->appendNewControlValue(proc, Return, Origin(), notSwapped);
            };
            switch (shape) {
            case Shape::EqualStrong:
                // seen + 100 when it swapped.
                returnByArithmetic(root->appendNew<Value>(proc, Equal, Origin(), result, zero), 100);
                break;
            case Shape::BranchEqualStrong:
                returnByBranch(root->appendNew<Value>(proc, Equal, Origin(), result, zero), true);
                break;
            case Shape::BranchStrongExpectingZero:
                // The old value is zero exactly when it swapped.
                returnByBranch(result, false);
                break;
            case Shape::XorWeak:
                // seen + 200 when it swapped, seen + 100 when it did not.
                seen = root->appendNew<Value>(proc, Add, Origin(), seen, root->appendNew<Const64Value>(proc, Origin(), 200));
                returnByArithmetic(root->appendNew<Value>(proc, BitXor, Origin(), result, root->appendNew<Const32Value>(proc, Origin(), 1)), -100);
                break;
            case Shape::BranchWeak:
                returnByBranch(result, true);
                break;
            }
            auto code = compileProc(proc);
            int64_t seenWhenSwapped = storeBetween ? 7 : 5;
            int64_t seenWhenNotSwapped = storeBetween ? 7 : 0;
            int64_t whenSwapped = seenWhenSwapped;
            int64_t whenNotSwapped = -1;
            if (shape == Shape::EqualStrong) {
                whenSwapped = seenWhenSwapped + 100;
                whenNotSwapped = seenWhenNotSwapped;
            } else if (shape == Shape::XorWeak) {
                whenSwapped = seenWhenSwapped + 200;
                whenNotSwapped = seenWhenNotSwapped + 100;
            }
            // A weak one may fail for no reason, and then the cell is as it was; it does not fail every time.
            bool swapped = false;
            for (unsigned attempt = 0; attempt < 100 && !swapped; ++attempt) {
                int64_t value = 0;
                int64_t returned = invoke<int64_t>(*code, &value);
                if (isWeak && returned == whenNotSwapped) {
                    CHECK_EQ(value, seenWhenNotSwapped);
                    continue;
                }
                CHECK_EQ(returned, whenSwapped);
                CHECK_EQ(value, seenWhenSwapped);
                swapped = true;
            }
            CHECK(swapped);
        }
    }
}

void testPureValueAfterForwardedLoadInLoop()
{
    // In a loop: store a byte, load it back sign-extended, and sign-extend the stored value as well. Load
    // elimination turns the load into a new SExt8 ahead of the one already there, and then sweeps the loop's
    // blocks again: each of the two has to end up defined before it is used.
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock();
    BasicBlock* done = proc.addBlock();
    Value* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    Value* limit = root->appendNew<Value>(proc, Trunc, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1));
    Value* indexPhi = loop->appendNew<Value>(proc, Phi, Int32, Origin());
    Value* sumPhi = loop->appendNew<Value>(proc, Phi, Int32, Origin());
    Value* zero = root->appendNew<Const32Value>(proc, Origin(), 0);
    root->appendNew<UpsilonValue>(proc, Origin(), zero, indexPhi);
    root->appendNew<UpsilonValue>(proc, Origin(), zero, sumPhi);
    root->appendNewControlValue(proc, Jump, Origin(), FrequentedBlock(loop));

    loop->appendNew<MemoryValue>(proc, Store8, Origin(), indexPhi, pointer, 0);
    Value* loaded = loop->appendNew<MemoryValue>(proc, Load8S, Origin(), pointer, 0);
    Value* extended = loop->appendNew<Value>(proc, SExt8, Origin(), indexPhi);
    Value* same = loop->appendNew<Value>(proc, Equal, Origin(), loaded, extended);
    Value* nextSum = loop->appendNew<Value>(proc, Add, Origin(), sumPhi, same);
    Value* nextIndex = loop->appendNew<Value>(proc, Add, Origin(), indexPhi, loop->appendNew<Const32Value>(proc, Origin(), 1));
    loop->appendNew<UpsilonValue>(proc, Origin(), nextIndex, indexPhi);
    loop->appendNew<UpsilonValue>(proc, Origin(), nextSum, sumPhi);
    loop->appendNewControlValue(proc, Branch, Origin(), loop->appendNew<Value>(proc, LessThan, Origin(), nextIndex, limit), FrequentedBlock(loop), FrequentedBlock(done));
    done->appendNewControlValue(proc, Return, Origin(), nextSum);

    proc.resetReachability();
    eliminateCommonSubexpressions(proc);
    validate(proc);
    uint8_t byte = 0;
    CHECK_EQ(compileAndRun<int32_t>(proc, &byte, 300), 300);
}

#endif // ENABLE(B3_JIT)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
