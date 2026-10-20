/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGenerator.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTCoroutineTable.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTState.h"
#include "InternalFunction.h"
#include "IteratorOperations.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "VMInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo Generator::s_info = { "Generator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Generator) };

Structure* Generator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

Generator* Generator::create(VM& vm, Structure* structure)
{
    Generator* generator = new (NotNull, allocateCell<Generator>(vm)) Generator(vm, structure);
    generator->finishCreation(vm);
    return generator;
}

bool Generator::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    bool isSet = Base::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
    uncheckedDowncast<Generator>(object)->m_hasOtherPrototype = !object->structure()->storedPrototype().isObject();
    return isSet;
}

Generator* Generator::fromStepStubFrame(CallFrame* stubFrame)
{
    return std::bit_cast<Generator* const*>(stubFrame)[generatorSlotInStubFrame];
}

void Generator::didThrow()
{
    m_state = State::Completed;
}

template<typename Visitor>
void Generator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitor.appendUnbarriered(uncheckedDowncast<Generator>(cell)->m_savedFrame);
}

DEFINE_VISIT_CHILDREN(Generator);

void Generator::setSavedFrame(VM& vm, SavedFrame* saved)
{
    m_savedFrame = saved;
    vm.writeBarrier(this);
}

JSValue Generator::resume(JSGlobalObject* globalObject, ResumeMode mode, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isDone;
    JSValue result = resume(globalObject, mode, value, isDone);
    RETURN_IF_EXCEPTION(scope, { });
    return createIteratorResultObject(globalObject, result, isDone);
}

bool Generator::hasUnmodifiedIteratorProtocol() const
{
    if (m_hasOtherPrototype || butterfly())
        return false;
    return structure()->storedPrototypeObject()->structureID().bits() == Instance::of(realm())->coroutines.emptyGeneratorPrototypeStructureID;
}

JSValue Generator::resume(JSGlobalObject* globalObject, ResumeMode mode, JSValue value, bool& isDone)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    isDone = true;
    switch (m_state) {
    case State::Running:
        throwTypeError(globalObject, scope, "Generator is executing"_s);
        return { };
    case State::Completed:
        switch (mode) {
        case ResumeMode::Next:
            return jsUndefined();
        case ResumeMode::Throw:
            throwException(globalObject, scope, value);
            return { };
        case ResumeMode::Return:
            return value;
        }
        RELEASE_ASSERT_NOT_REACHED();
    case State::SuspendedAtStart:
    case State::SuspendedAtYield:
        break;
    }
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return { };
    }
    m_state = State::Running;
    JSValue result = enterSavedFrame(vm, *Instance::of(realm()), m_savedFrame, mode, value);
    if (scope.exception()) [[unlikely]] {
        m_state = State::Completed;
        return { };
    }
    // A body that suspended again has left through the suspension stub, which stores SuspendedAtYield.
    if (m_state == State::Running) {
        m_state = State::Completed;
        return result;
    }
    isDone = false;
    return result;
}

JSObject* createGenerator(JSGlobalObject* globalObject, Instance& instance, JSObject* generatorFunction, CoroutineKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* base = nullptr;
    switch (kind) {
    case CoroutineKind::Generator:
        base = instance.state->coroutines->generatorStructure();
        break;
    case CoroutineKind::AsyncFunction:
        RELEASE_ASSERT_NOT_REACHED();
    case CoroutineKind::AsyncGenerator:
        base = globalObject->asyncGeneratorStructure();
        break;
    }
    Structure* structure = InternalFunction::createSubclassStructure(globalObject, generatorFunction, base);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (kind == CoroutineKind::AsyncGenerator)
        return JSAsyncGenerator::create(vm, structure);
    return Generator::create(vm, structure);
}

SavedFrame* savedFrameOfGenerator(JSObject* generator)
{
    if (auto* syncGenerator = dynamicDowncast<Generator>(generator))
        return syncGenerator->savedFrame();
    JSValue frame = uncheckedDowncast<JSAsyncGenerator>(generator)->frame();
    return frame.isUndefined() ? nullptr : uncheckedDowncast<SavedFrame>(frame);
}

void setSavedFrameOfGenerator(VM& vm, JSObject* generator, SavedFrame* saved)
{
    if (auto* syncGenerator = dynamicDowncast<Generator>(generator)) {
        syncGenerator->setSavedFrame(vm, saved);
        return;
    }
    auto* asyncGenerator = uncheckedDowncast<JSAsyncGenerator>(generator);
    asyncGenerator->internalField(JSAsyncGenerator::Field::Frame).set(vm, asyncGenerator, saved);
    asyncGenerator->internalField(JSAsyncGenerator::Field::Next).set(vm, asyncGenerator, saved);
}

void completeGeneratorLeftByThrow(CallFrame* stepStubFrame)
{
    Generator::fromStepStubFrame(stepStubFrame)->didThrow();
}

bool isGeneratorObject(JSCell* cell)
{
    return cell->inherits<Generator>();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
