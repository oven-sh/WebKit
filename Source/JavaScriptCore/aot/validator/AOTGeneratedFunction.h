/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSType.h>
#include <span>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class BytecodeGenerator;
class JSFunction;
class JSGlobalObject;
class RegisterID;
class SourceProvider;
class VM;

// Writes the bytecode of a function from native code. Every tier runs the result like any other function.
class GeneratedFunctionAssembler {
    WTF_MAKE_NONCOPYABLE(GeneratedFunctionAssembler);
public:
    using Register = RegisterID*;
    using Label = unsigned;
    enum class Comparison : uint8_t { Less, LessOrEqual, Greater, GreaterOrEqual };
    enum class Arithmetic : uint8_t { Add, Subtract, Divide, Remainder, BitOr };
    struct StringCase {
        StringImpl* string;
        Label label;
    };

    explicit GeneratedFunctionAssembler(BytecodeGenerator&);
    ~GeneratedFunctionAssembler();

    JS_EXPORT_PRIVATE VM& vm();

    JS_EXPORT_PRIVATE Register argument(unsigned);
    JS_EXPORT_PRIVATE void readArgumentCount(Register destination);
    JS_EXPORT_PRIVATE Register constant(JSValue);
    JS_EXPORT_PRIVATE Register newRegister();
    JS_EXPORT_PRIVATE unsigned liveRegisterCount() const;
    JS_EXPORT_PRIVATE void releaseRegistersAbove(unsigned);
    JS_EXPORT_PRIVATE void pinRegister(Register);
    // Call before deferring code that jumps back here, so the deferred code gets registers of its own.
    JS_EXPORT_PRIVATE void pinLiveRegisters();

    JS_EXPORT_PRIVATE Label newLabel();
    JS_EXPORT_PRIVATE void bind(Label);
    JS_EXPORT_PRIVATE void jump(Label);
    JS_EXPORT_PRIVATE void loopHint();

    JS_EXPORT_PRIVATE void jumpIfCellType(Register, JSType, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfStructureFlags(Register, unsigned flags, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfNumber(Register, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfBoolean(Register, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfObject(Register, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfCallable(Register, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfStrictlyEqual(Register, Register, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfCompares(Comparison, Register, Register, bool expected, Label);
    JS_EXPORT_PRIVATE void jumpIfTruthy(Register, bool expected, Label);

    JS_EXPORT_PRIVATE void move(Register destination, Register source);
    JS_EXPORT_PRIVATE void compute(Arithmetic, Register destination, Register, Register);
    JS_EXPORT_PRIVATE void increment(Register);
    JS_EXPORT_PRIVATE void readOwnProperty(Register destination, Register object, UniquedStringImpl*);
    JS_EXPORT_PRIVATE void readLength(Register destination, Register);
    JS_EXPORT_PRIVATE void readElement(Register destination, Register array, Register index);
    JS_EXPORT_PRIVATE void writeOwnProperty(Register object, UniquedStringImpl*, Register value);
    JS_EXPORT_PRIVATE void writeOwnElement(Register object, Register key, Register value);
    JS_EXPORT_PRIVATE void call(Register destination, JSValue function, std::span<const Register> arguments);

    // Bind every case label and the default between these two calls.
    JS_EXPORT_PRIVATE void beginStringSwitch(Register);
    JS_EXPORT_PRIVATE void endStringSwitch(std::span<const StringCase>, Label defaultLabel);

    JS_EXPORT_PRIVATE void returnValue(Register);

private:
    struct State;
    BytecodeGenerator& m_generator;
    std::unique_ptr<State> m_state;
};

class GeneratedFunctionBody : public ThreadSafeRefCounted<GeneratedFunctionBody> {
public:
    JS_EXPORT_PRIVATE virtual ~GeneratedFunctionBody();
    virtual void emit(GeneratedFunctionAssembler&) = 0;
};

JS_EXPORT_PRIVATE JSFunction* createGeneratedFunction(VM&, JSGlobalObject*, const String& name, unsigned parameterCount, Ref<GeneratedFunctionBody>&&);
JS_EXPORT_PRIVATE void compileGeneratedFunction(VM&, JSFunction*);

bool emitGeneratedFunctionBody(BytecodeGenerator&, SourceProvider*);

} // namespace JSC
