/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGeneratedFunction.h"

#include "BuiltinExecutables.h"
#include "BytecodeGenerator.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "NodeConstructors.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SegmentedVector.h>

namespace JSC {

struct GeneratedFunctionAssembler::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);
    SegmentedVector<RegisterID, 4> arguments;
    SegmentedVector<Identifier, 16> identifiers;
    Vector<Ref<JSC::Label>, 64> labels;
    Vector<RefPtr<RegisterID>, 32> keptRegisters;
    Vector<RefPtr<RegisterID>, 32> registers;
};

GeneratedFunctionAssembler::GeneratedFunctionAssembler(BytecodeGenerator& generator)
    : m_generator(generator)
    , m_state(makeUnique<State>())
{
}

GeneratedFunctionAssembler::~GeneratedFunctionAssembler()
{
    releaseRegistersFrom(0);
}

VM& GeneratedFunctionAssembler::vm() { return m_generator.vm(); }

auto GeneratedFunctionAssembler::argument(unsigned index) -> Register
{
    while (m_state->arguments.size() <= index)
        m_state->arguments.alloc(CallFrame::argumentOffset(m_state->arguments.size()));
    return &m_state->arguments[index];
}

void GeneratedFunctionAssembler::readArgumentCount(Register destination) { m_generator.emitArgumentCount(destination); }

auto GeneratedFunctionAssembler::constant(JSValue value) -> Register { return m_generator.emitLoad(nullptr, value); }

auto GeneratedFunctionAssembler::newRegister() -> Register
{
    m_state->registers.append(m_generator.newTemporary());
    return m_state->registers.last().get();
}

unsigned GeneratedFunctionAssembler::registersInUse() const { return m_state->registers.size(); }

void GeneratedFunctionAssembler::releaseRegistersFrom(unsigned count)
{
    while (m_state->registers.size() > count)
        m_state->registers.removeLast();
}

void GeneratedFunctionAssembler::keepUntilTheEnd(Register value) { m_state->keptRegisters.append(value); }

void GeneratedFunctionAssembler::keepRegistersInUse()
{
    for (auto& one : m_state->registers)
        m_state->keptRegisters.append(one);
}

auto GeneratedFunctionAssembler::newLabel() -> Label
{
    m_state->labels.append(m_generator.newLabel());
    return m_state->labels.size() - 1;
}

void GeneratedFunctionAssembler::bind(Label label) { m_generator.emitLabel(m_state->labels[label].get()); }
void GeneratedFunctionAssembler::jump(Label label) { m_generator.emitJump(m_state->labels[label].get()); }
void GeneratedFunctionAssembler::loopHint() { m_generator.emitLoopHint(); }

void GeneratedFunctionAssembler::jumpIfTruthy(Register condition, bool whenItIs, Label label)
{
    if (whenItIs)
        m_generator.emitJumpIfTrue(condition, m_state->labels[label].get());
    else
        m_generator.emitJumpIfFalse(condition, m_state->labels[label].get());
}

void GeneratedFunctionAssembler::jumpIfCellType(Register value, JSType type, bool whenItIs, Label label)
{
    jumpIfTruthy(m_generator.emitIsCellWithType(m_generator.newTemporary(), value, type), whenItIs, label);
}

void GeneratedFunctionAssembler::jumpIfStructureFlags(Register value, unsigned flags, bool whenItHasAny, Label label)
{
    jumpIfTruthy(m_generator.emitHasStructureWithFlags(m_generator.newTemporary(), value, flags), whenItHasAny, label);
}

void GeneratedFunctionAssembler::jumpIfNumber(Register value, bool whenItIs, Label label)
{
    jumpIfTruthy(m_generator.emitIsNumber(m_generator.newTemporary(), value), whenItIs, label);
}

void GeneratedFunctionAssembler::jumpIfBoolean(Register value, bool whenItIs, Label label)
{
    jumpIfTruthy(m_generator.emitUnaryOp<OpIsBoolean>(m_generator.newTemporary(), value), whenItIs, label);
}

void GeneratedFunctionAssembler::jumpIfObject(Register value, bool whenItIs, Label label)
{
    jumpIfTruthy(m_generator.emitIsObject(m_generator.newTemporary(), value), whenItIs, label);
}

void GeneratedFunctionAssembler::jumpIfCallable(Register value, bool whenItIs, Label label)
{
    jumpIfTruthy(m_generator.emitIsCallable(m_generator.newTemporary(), value), whenItIs, label);
}

void GeneratedFunctionAssembler::jumpIfStrictlyEqual(Register left, Register right, bool whenTheyAre, Label label)
{
    jumpIfTruthy(m_generator.emitEqualityOp<OpStricteq>(m_generator.newTemporary(), left, right), whenTheyAre, label);
}

void GeneratedFunctionAssembler::jumpIfCompares(Comparison comparison, Register left, Register right, bool whenItHolds, Label label)
{
    Register result = m_generator.newTemporary();
    switch (comparison) {
    case Comparison::Less:
        m_generator.emitBinaryOp<OpLess>(result, left, right);
        break;
    case Comparison::LessOrEqual:
        m_generator.emitBinaryOp<OpLesseq>(result, left, right);
        break;
    case Comparison::Greater:
        m_generator.emitBinaryOp<OpGreater>(result, left, right);
        break;
    case Comparison::GreaterOrEqual:
        m_generator.emitBinaryOp<OpGreatereq>(result, left, right);
        break;
    }
    jumpIfTruthy(result, whenItHolds, label);
}

void GeneratedFunctionAssembler::move(Register destination, Register source) { m_generator.move(destination, source); }

void GeneratedFunctionAssembler::compute(Arithmetic operation, Register destination, Register left, Register right)
{
    OperandTypes types(ResultType::unknownType(), ResultType::unknownType());
    switch (operation) {
    case Arithmetic::Add:
        m_generator.emitBinaryOp<OpAdd>(destination, left, right, types);
        break;
    case Arithmetic::Subtract:
        m_generator.emitBinaryOp<OpSub>(destination, left, right, types);
        break;
    case Arithmetic::Divide:
        m_generator.emitBinaryOp<OpDiv>(destination, left, right, types);
        break;
    case Arithmetic::Remainder:
        m_generator.emitBinaryOp<OpMod>(destination, left, right, types);
        break;
    case Arithmetic::BitOr:
        m_generator.emitBinaryOp<OpBitor>(destination, left, right, types);
        break;
    }
}

void GeneratedFunctionAssembler::increment(Register value) { m_generator.emitInc(value); }

void GeneratedFunctionAssembler::readOwnProperty(Register destination, Register object, UniquedStringImpl* name)
{
    m_generator.emitDirectGetById(destination, object, Identifier::fromUid(vm(), name));
}

void GeneratedFunctionAssembler::readLength(Register destination, Register value) { m_generator.emitGetLength(destination, value); }
void GeneratedFunctionAssembler::readElement(Register destination, Register array, Register index) { m_generator.emitGetByVal(destination, array, index); }

void GeneratedFunctionAssembler::writeOwnProperty(Register object, UniquedStringImpl* name, Register value)
{
    m_generator.emitDirectPutById(object, Identifier::fromUid(vm(), name), value);
}

void GeneratedFunctionAssembler::writeOwnElement(Register object, Register key, Register value) { m_generator.emitDirectPutByVal(object, key, value); }

void GeneratedFunctionAssembler::call(Register destination, JSValue function, std::span<const Register> arguments)
{
    CallArguments frame(m_generator, nullptr, arguments.size());
    m_generator.emitLoad(frame.thisRegister(), jsUndefined());
    for (unsigned i = 0; i < arguments.size(); ++i)
        m_generator.move(frame.argumentRegister(i), arguments[i]);
    JSTextPosition position(0);
    m_generator.emitCall(destination, constant(function), NoExpectedFunction, frame, position, position, position, DebuggableCall::No);
}

void GeneratedFunctionAssembler::beginStringSwitch(Register value) { m_generator.beginSwitch(value, SwitchInfo::SwitchType::String); }

void GeneratedFunctionAssembler::endStringSwitch(std::span<const StringCase> cases, Label defaultLabel)
{
    Vector<Ref<JSC::Label>, 8> labels;
    Vector<ExpressionNode*, 8> nodes;
    for (auto& one : cases) {
        m_state->identifiers.alloc(Identifier::fromString(vm(), String(one.string)));
        labels.append(m_state->labels[one.label].copyRef());
        nodes.append(new (m_generator.parserArena()) StringNode(JSTokenLocation(), m_state->identifiers.last()));
    }
    m_generator.endSwitch(labels, nodes.mutableSpan().data(), m_state->labels[defaultLabel].get(), 0, 0);
}

void GeneratedFunctionAssembler::returnValue(Register value) { m_generator.emitReturn(value); }

GeneratedFunctionBody::~GeneratedFunctionBody() = default;

namespace {

Lock bodiesLock;
UncheckedKeyHashMap<SourceProvider*, GeneratedFunctionBody*>& bodies() WTF_REQUIRES_LOCK(bodiesLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<SourceProvider*, GeneratedFunctionBody*>> map;
    return map;
}

class GeneratedFunctionSourceProvider final : public StringSourceProvider {
public:
    static Ref<GeneratedFunctionSourceProvider> create(const String& text, Ref<GeneratedFunctionBody>&& body)
    {
        return adoptRef(*new GeneratedFunctionSourceProvider(text, WTF::move(body)));
    }

    ~GeneratedFunctionSourceProvider() final
    {
        Locker locker { bodiesLock };
        bodies().remove(this);
    }

private:
    GeneratedFunctionSourceProvider(const String& text, Ref<GeneratedFunctionBody>&& body)
        : StringSourceProvider(text, SourceOrigin(), SourceTaintedness(), String(), TextPosition(), SourceProviderSourceType::Program)
        , m_body(WTF::move(body))
    {
        Locker locker { bodiesLock };
        bodies().add(this, m_body.ptr());
    }

    const Ref<GeneratedFunctionBody> m_body;
};

} // namespace

bool emitGeneratedFunctionBody(BytecodeGenerator& generator, SourceProvider* provider)
{
    RefPtr<GeneratedFunctionBody> body;
    {
        Locker locker { bodiesLock };
        body = bodies().get(provider);
    }
    if (!body)
        return false;
    GeneratedFunctionAssembler assembler(generator);
    body->emit(assembler);
    return true;
}

JSFunction* createGeneratedFunction(VM& vm, JSGlobalObject* globalObject, const String& name, unsigned parameterCount, Ref<GeneratedFunctionBody>&& body)
{
    static constexpr ASCIILiteral texts[] = { "(function () { \"use strict\"; })"_s, "(function (a) { \"use strict\"; })"_s, "(function (a, b) { \"use strict\"; })"_s };
    RELEASE_ASSERT(parameterCount < std::size(texts));
    SourceCode source(GeneratedFunctionSourceProvider::create(String(texts[parameterCount]), WTF::move(body)));
    auto* unlinked = BuiltinExecutables::createExecutable(vm, source, Identifier::fromString(vm, name), ImplementationVisibility::Public, ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None, NeedsClassFieldInitializer::No);
    return JSFunction::create(vm, globalObject, unlinked->link(vm, nullptr, source), globalObject);
}

void prepareGeneratedFunction(VM& vm, JSFunction* function)
{
    CodeBlock* codeBlock = nullptr;
    function->jsExecutable()->prepareForExecution<FunctionExecutable>(vm, function, function->scope(), CodeSpecializationKind::CodeForCall, codeBlock);
    if (codeBlock)
        codeBlock->jitSoon();
}

} // namespace JSC
