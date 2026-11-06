/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyBytecode.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyIO.h"
#include "PyIterators.h"
#include "PyMonitoring.h"
#include "PyNative.h"
#include "PreciseJumpTargetsInlines.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// Objects that only a temporary register refers to.
//
// With reference counts, the view in bytes(memoryview(b)[:n]) is gone when bytes() returns, the file in open(p, "w").write(s) is closed
// when write() returns, and the generator in `for x in g(): break` is closed by the `break`. Programs count on it: the next statement
// resizes b, reads the file, or expects what a `finally` in g has done. Here nothing is known about who refers to an object until the collector has looked. But of some objects it can
// be known without looking: those that were made for an instruction whose result goes in a temporary register with a
// single use, if that use is known to keep nothing. When it is over, they are finalized. (Finalized, not freed: an error
// here would be a view released too early, not memory used after it is freed.)
//
// What is known, and from where:
//  - That the register is a temporary with one use, and which: from the compiler, which leaves marks beside the code
//    (PythonMark::TemporaryMade, TemporaryUsed). No instruction is added, and code that makes no such object never looks at them.
//  - That the object is new and nothing else refers to it: from the function that made it, which says so (didMakeResult()), and
//    then only if that function was called by the marked instruction itself: its frame is on top and the frame of the
//    instruction is the one below.
//  - That the use keeps nothing: this is settled WHEN THE OBJECT IS MADE, or the object is not taken on. It cannot wait until the
//    use is over, for a use that does keep the object (a function written in Python, a store in a list) runs no code of
//    ours that could say so. A callee is evaluated before its arguments and waits in a temporary; a method of a built-in type is
//    known by its name. See ownTemporary().
//  So from its making to the end of its use the object is out of the program's reach, and a note of it that is never
//  taken up (an exception came between) is of an object that can never turn up again.
//  - Except by the few ways in which a program gets at objects it has no reference to: the module gc and frame.f_generator, which
//    call temporariesMayHaveEscaped(), and the arguments that sys.monitoring shows: forgetOwnedTemporaries(). So does what
//    assigns to a variable of a frame from outside (frame.f_locals), for one kind of use names a variable.
//
// The notes are weak: the collector does not visit them, and forgets those of objects that it has found dead. (It will not do to
// forget them all: what sets a collection off is allocation, and in bytes(memoryview(b)) it is the use that allocates.)

void Runtime::forgetDeadTemporaries(SlotVisitor& visitor)
{
    unsigned kept = 0;
    for (unsigned i = 0; i < m_ownedTemporaryCount; ++i) {
        if (visitor.isMarked(m_ownedTemporaries[i].object))
            m_ownedTemporaries[kept++] = m_ownedTemporaries[i];
    }
    m_ownedTemporaryCount = kept;
    for (unsigned i = 0; i < m_newGenerators.size() && m_newGenerators[i].generator;) {
        if (NewGenerator& note = m_newGenerators[i]; visitor.isMarked(note.generator) && (!note.iterator || visitor.isMarked(note.iterator)))
            ++i;
        else
            forget(m_newGenerators[i]);
    }
    m_usesOfCalls.fill({ });
}

static ALWAYS_INLINE bool isFrameOfPythonCode(CallFrame* callFrame)
{
    if (callFrame->isNativeCalleeFrame() || callFrame->isZombieFrame())
        return false;
    CodeBlock* codeBlock = callFrame->codeBlock();
    return codeBlock && codeBlock->unlinkedCodeBlock()->isPython();
}

// The frame of the Python code that made the call, if it was Python code.
static CallFrame* pythonCallerOf(VM& vm, CallFrame* nativeFrame)
{
    void* caller = nativeFrame->callerFrameOrEntryFrame();
    if (!caller || caller == vm.topEntryFrame)
        return nullptr;
    auto* callFrame = static_cast<CallFrame*>(caller);
    return isFrameOfPythonCode(callFrame) ? callFrame : nullptr;
}

static const PythonMark* markAt(std::span<const PythonMark> marks, unsigned offset, PythonMark::Kind kind)
{
    auto [first, last] = std::ranges::equal_range(marks, offset, { }, &PythonMark::offset);
    for (auto it = first; it != last; ++it) {
        if (it->kind() == kind)
            return &*it;
    }
    return nullptr;
}

static bool keepsNoArguments(Runtime& rt, JSValue callee)
{
    if (auto* native = PyNativeFunction::tryFrom(callee))
        return native->def().flags & KeepsNothing;
    // They copy what a view shows, or share what it is a view of. See didCallType().
    return callee == JSValue(rt.bytesType()) || callee == JSValue(rt.bytearrayType()) || callee == JSValue(rt.memoryviewType());
}

void Runtime::ownTemporary(CallFrame* callFrame, unsigned offset, JSCell* object)
{
    CodeBlock* codeBlock = callFrame->codeBlock();
    auto marks = codeBlock->unlinkedCodeBlock()->pythonMarks();
    const PythonMark* made = markAt(marks, offset, PythonMark::TemporaryMade);
    if (!made)
        return;

    const PythonMark* firstUse = nullptr;
    const PythonMark* lastUse = nullptr;
    for (const PythonMark* mark = made + 1; mark != marks.data() + marks.size(); ++mark) {
        if (mark->kind() != PythonMark::TemporaryUsed || mark->value() != made->value())
            continue;
        if (!firstUse)
            firstUse = mark;
        lastUse = mark;
        if (TemporaryUse::isLast(mark->bits))
            break;
    }
    if (!firstUse)
        return;

    VirtualRegister reg(TemporaryUse::registerOffsetOf(firstUse->bits));
    switch (TemporaryUse::kindOf(firstUse->bits)) {
    case TemporaryUse::Receiver: {
        // The type is built in and cannot be changed, and the instance has no attributes but what its maker gave it.
        auto* method = PyNativeFunction::tryFrom(typeOf(object)->lookup(asString(codeBlock->getConstant(reg))));
        if (!method || !(method->def().flags & KeepsNothing))
            return;
        break;
    }
    case TemporaryUse::SubscriptBase:
        break;
    case TemporaryUse::Argument:
    case TemporaryUse::KeywordArgument:
        if (!keepsNoArguments(*this, callFrame->r(reg).jsValue()))
            return;
        break;
    case TemporaryUse::StoredValue:
        if (JSValue base = callFrame->r(reg).jsValue(); !PyByteArray::is(base) || typeOf(base.asCell()) != bytearrayType())
            return;
        break;
    case TemporaryUse::ForHeader:
        return;
    }

    if (m_ownedTemporaryCount == maxOwnedTemporaries) {
        // The oldest is the least likely to be taken up.
        std::copy(m_ownedTemporaries.begin() + 1, m_ownedTemporaries.end(), m_ownedTemporaries.begin());
        --m_ownedTemporaryCount;
    }
    m_ownedTemporaries[m_ownedTemporaryCount++] = { object, callFrame, firstUse->offset, lastUse->offset };
}

// Memoryviews, and what open() returns.
static bool canBeOwned(JSGlobalObject* globalObject, JSCell* object)
{
    return PyMemoryView::is(object) || ioIsOfTypeThatOpenMakes(globalObject, object);
}

void Runtime::didMakeResult(CallFrame* nativeFrame, JSCell* object)
{
    if (!canBeOwned(m_globalObject, object))
        return;
    if (CallFrame* callFrame = pythonCallerOf(m_vm, nativeFrame))
        ownTemporary(callFrame, callFrame->bytecodeIndex().offset(), object);
}

void Runtime::didMakeResultOfInstruction(CallFrame* callFrame, JSCell* object)
{
    if (canBeOwned(m_globalObject, object) && isFrameOfPythonCode(callFrame))
        ownTemporary(callFrame, callFrame->bytecodeIndex().offset(), object);
}

template<typename IsOperand>
void Runtime::releaseTemporariesUsedAt(CallFrame* callFrame, unsigned offset, JSValue result, const IsOperand& isOperand)
{
    // On the stack, which keeps them: to release one may run a __release_buffer__() of the program.
    std::array<JSCell*, maxOwnedTemporaries> objects;
    unsigned count = 0;
    for (unsigned i = 0; i < m_ownedTemporaryCount;) {
        OwnedTemporary temporary = m_ownedTemporaries[i];
        if (temporary.frame != callFrame || offset < temporary.firstUse || offset > temporary.lastUse) {
            ++i;
            continue;
        }
        std::copy(m_ownedTemporaries.begin() + i + 1, m_ownedTemporaries.begin() + m_ownedTemporaryCount, m_ownedTemporaries.begin() + i);
        --m_ownedTemporaryCount;
        if (isOperand(temporary.object) && result != JSValue(temporary.object))
            objects[count++] = temporary.object;
    }
    // As when the reference count falls to zero: CPython lets go of the operands of a call from the last to the first, and of the
    // receiver after the arguments. There may be an exception pending, which stays.
    for (unsigned i = count; i--;) {
        if (auto* view = PyMemoryView::tryFrom(objects[i])) {
            if (!view->exports)
                view->release(m_globalObject);
        } else
            ioFinalizeNow(m_globalObject, uncheckedDowncast<PyRecord>(objects[i]));
        ensureStillAliveHere(objects[i]);
    }
}

void Runtime::didCallWithTemporaries(CallFrame* nativeFrame, JSValue result)
{
    CallFrame* callFrame = pythonCallerOf(m_vm, nativeFrame);
    if (!callFrame)
        return;
    releaseTemporariesUsedAt(callFrame, callFrame->bytecodeIndex().offset(), result, [&](JSCell* object) {
        for (unsigned i = 0; i < nativeFrame->argumentCount(); ++i) {
            if (nativeFrame->uncheckedArgument(i) == JSValue(object))
                return true;
        }
        return false;
    });
}

void Runtime::didRunInstructionWithTemporaries(CallFrame* callFrame, std::span<const JSValue> operands, JSValue result)
{
    if (!isFrameOfPythonCode(callFrame))
        return;
    releaseTemporariesUsedAt(callFrame, callFrame->bytecodeIndex().offset(), result, [&](JSCell* object) {
        return std::ranges::find(operands, JSValue(object)) != operands.end();
    });
}

void Runtime::didCallType(PyType* type, JSValue result, bool resultIsFresh)
{
    CallFrame* nativeFrame = m_vm.topCallFrame;
    if (!nativeFrame || !nativeFrame->callee().isCell() || nativeFrame->jsCallee() != type)
        return;
    if (ownsTemporaries())
        didCallWithTemporaries(nativeFrame, result);
    if (resultIsFresh && result && result.isCell())
        didMakeResult(nativeFrame, result.asCell());
}

void Runtime::didCallMethod(JSValue self, JSValue result, bool resultIsFresh, void* function)
{
    CallFrame* nativeFrame = m_vm.topCallFrame;
    if (!nativeFrame || !nativeFrame->callee().isCell())
        return;
    auto* method = PyNativeFunction::tryFrom(nativeFrame->jsCallee());
    if (!method || !(method->def().flags & KeepsNothing) || !nativeFrame->argumentCount() || nativeFrame->uncheckedArgument(0) != self)
        return;
    if (function && std::bit_cast<void*>(method->def().pointer) != function)
        return;
    if (ownsTemporaries())
        didCallWithTemporaries(nativeFrame, result);
    if (resultIsFresh && result && result.isCell())
        didMakeResult(nativeFrame, result.asCell());
}

void Runtime::didCallFunction(NativeDef::F8 function, JSValue result)
{
    CallFrame* nativeFrame = m_vm.topCallFrame;
    if (!nativeFrame || !nativeFrame->callee().isCell() || !result || !result.isCell())
        return;
    auto* callee = PyNativeFunction::tryFrom(nativeFrame->jsCallee());
    if (callee && callee->def().signature.parameterCount == 8 && callee->def().pointer.f8 == function)
        didMakeResult(nativeFrame, result.asCell());
}

void Runtime::didSubscript(JSValue base, JSValue result)
{
    CallFrame* callFrame = m_vm.topCallFrame;
    if (!callFrame || !isFrameOfPythonCode(callFrame))
        return;
    auto* instruction = callFrame->codeBlock()->instructions().at(callFrame->bytecodeIndex()).ptr();
    if (instruction->opcodeID() != op_py_get_item)
        return;
    auto bytecode = instruction->as<OpPyGetItem>();
    if (bytecode.m_base.isConstant() || callFrame->r(bytecode.m_base).jsValue() != base)
        return;
    if (ownsTemporaries())
        didRunInstructionWithTemporaries(callFrame, { &base, 1 }, result);
    if (result && result.isCell() && result != base)
        didMakeResultOfInstruction(callFrame, result.asCell());
}

void Runtime::didStoreInItem(JSValue value)
{
    CallFrame* callFrame = m_vm.topCallFrame;
    if (!callFrame || !isFrameOfPythonCode(callFrame))
        return;
    auto* instruction = callFrame->codeBlock()->instructions().at(callFrame->bytecodeIndex()).ptr();
    if (instruction->opcodeID() != op_py_set_item)
        return;
    auto bytecode = instruction->as<OpPySetItem>();
    if (bytecode.m_value.isConstant() || callFrame->r(bytecode.m_value).jsValue() != value)
        return;
    didRunInstructionWithTemporaries(callFrame, { &value, 1 }, { });
}

// ---- Generators ----
//
// for x in g(): the generator is in the register where the loop keeps its iterator, and nowhere else. next(g()): it is in the register
// of the argument. A generator function makes a note of the generator it has made, if it was made for a use that goes through it and keeps nothing: at the head of a `for`
// loop, as what `in` searches or an assignment takes apart, or as a positional argument of a built-in function or type that is already waiting in its register and says that it
// closes such an argument (NativeFlag::ClosesGeneratorArgument, TypeFlag::GoesThroughArgument).
// The use takes the note up, and owns the generator from then on: op_py_get_iter, op_py_compare or op_py_unpack, which is the very next instruction to run in the caller (for a generator
// expression, the call that makes it, with what it goes through for its argument), or the function.
//
// for i, x in enumerate(g()): the generator is in the iterator, and nowhere else, and the iterator is where the generator is above. A type whose instances let nobody have what they were
// made from says so (TypeFlag::HasIteratorsToItself). To be passed to it is a use that notes are made for as well, and when the iterator is made, the notes that were made for
// the call go on to be about the iterator and its use, if that is such a use again; if not, that is the end of them. The iterator remembers that there may be something in it
// (hasGeneratorsToItself): whoever closes what it owns looks for the generators there (forEachGeneratorBehind()), and in no other iterator.
//
// No note outlives the use. What is called at the use forgets those that were made for the call and are not of what it goes through: next(it, g()) may return the generator.
// That it is called, and nothing else, was settled when the note was made. One that the use never came for, because another argument raised, is of a
// generator that can never turn up again: the unwinder forgets the notes of each frame it comes through. Other arguments are evaluated between the making and the use, and they cannot get at the generator. They may make notes of their
// own: there is room for a few, and the oldest goes first.
// A frame that is suspended between the making and the use, zip(g(), (yield)), goes on elsewhere on the stack and would not know its note: the compiler marks no use then, and no note is made.
// A thread that gives way in between takes its notes along (ThreadLocalState), so that no other thread finds notes to look through; it gets them back unless the program has been
// handed objects it had no reference to meanwhile.
//
// A loop that is left before its end closes the generators it owns: ReleaseIterator on the way of `break`, `return` and a `continue`
// of an outer loop; for an exception, the unwinder asks releaseIteratorsOfLoopsLeft(). So does a loop that ends, for zip(g(), h()) ends with the first that does.
// A function does when it is done (GeneratorArgumentScope), and so do `in` and the assignment. An iterator that lets go of what it went through before it goes itself, as islice() does,
// closes what was owned with it then.

void Runtime::findUseOfCall(UseOfCall& found, CodeBlock* codeBlock, unsigned call)
{
    found = { codeBlock, call };
    auto marks = codeBlock->unlinkedCodeBlock()->pythonMarks();
    const PythonMark* made = markAt(marks, call, PythonMark::TemporaryMade);
    if (!made)
        return;
    const PythonMark* end = marks.data() + marks.size();
    auto nextUse = [&](const PythonMark* mark) {
        while (mark != end && (mark->kind() != PythonMark::TemporaryUsed || mark->value() != made->value()))
            ++mark;
        return mark;
    };
    const PythonMark* use = nextUse(made + 1);
    if (use == end)
        return;
    found.use = found.otherUse = use->offset;
    if (!TemporaryUse::isLast(use->bits)) {
        if (const PythonMark* other = nextUse(use + 1); other != end)
            found.otherUse = other->offset;
    }
    switch (TemporaryUse::kindOf(use->bits)) {
    case TemporaryUse::ForHeader:
        found.kind = UseOfCall::ForHeader;
        break;
    case TemporaryUse::Argument:
        found.kind = UseOfCall::Argument;
        found.callee = TemporaryUse::registerOffsetOf(use->bits);
        break;
    default:
        break;
    }
}

ALWAYS_INLINE const Runtime::UseOfCall* Runtime::owningUseOfCall(CallFrame* callFrame, unsigned call)
{
    CodeBlock* codeBlock = callFrame->codeBlock();
    UseOfCall& use = m_usesOfCalls[(call ^ std::bit_cast<uintptr_t>(codeBlock) >> 5) % m_usesOfCalls.size()];
    if (use.codeBlock != codeBlock || use.call != call) [[unlikely]] {
        if (!isFrameOfPythonCode(callFrame))
            return nullptr;
        findUseOfCall(use, codeBlock, call);
    }
    if (use.kind == UseOfCall::Other)
        return nullptr;
    if (use.kind == UseOfCall::Argument) {
        JSValue callee = callFrame->r(VirtualRegister(use.callee)).jsValue();
        if (auto* native = PyNativeFunction::tryFrom(callee)) {
            if (!(native->def().flags & ClosesGeneratorArgument))
                return nullptr;
        } else if (auto* type = PyType::tryFrom(callee); !type || !(type->has(TypeFlag::HasIteratorsToItself) || type->has(TypeFlag::GoesThroughArgument)))
            return nullptr;
    }
    return &use;
}

void Runtime::forget(NewGenerator& note)
{
    if (&note + 1 == m_newGenerators.end() || !(&note + 1)->generator) [[likely]] {
        note.generator = nullptr;
        return;
    }
    std::move(&note + 1, m_newGenerators.end(), &note);
    m_newGenerators.back() = { };
}

// The functor is given the generators first.
template<typename Functor>
void Runtime::forgetNewGeneratorsFor(const CallOfInstruction& call, const Functor& functor)
{
    for (unsigned i = 0; i < m_newGenerators.size() && m_newGenerators[i].generator;) {
        NewGenerator& note = m_newGenerators[i];
        if (note.caller != call.frame || (note.use != call.offset && note.otherUse != call.offset)) {
            ++i;
            continue;
        }
        functor(note.generator);
        forget(note);
    }
}

void Runtime::didMakeGenerator(PyGenerator* generator, CallFrame* frameOfFunction)
{
    void* caller = frameOfFunction->callerFrameOrEntryFrame();
    if (caller == m_vm.topEntryFrame) [[unlikely]]
        return;
    auto* callFrame = static_cast<CallFrame*>(caller);
    unsigned call = callFrame->callSiteIndex().bits();
    bool hasNotes = hasNewGenerator();
    if (hasNotes) [[unlikely]] {
        // (x for x in g()): this call is the use. See GeneratorsOwnedByFrame.
        forgetNewGeneratorsFor({ callFrame, call }, [&](PyGenerator* passed) {
            passed->loopOwnership = m_ownershipEpoch;
        });
    }
    const UseOfCall* use = owningUseOfCall(callFrame, call);
    if (!use)
        return;
    if (hasNotes) [[unlikely]]
        std::move_backward(m_newGenerators.begin(), m_newGenerators.end() - 1, m_newGenerators.end());
    m_newGenerators[0] = { generator, nullptr, callFrame, use->use, use->otherUse };
}

// What an iterator goes through, if it lets nobody else have it. From the last: that is the order in which CPython lets go of the items of a tuple, and the fields of the
// records are in the order that makes it right for them.
template<typename Functor> static void forEachGeneratorIn(JSValue held, const Functor&, unsigned depth);

template<typename Functor>
static ALWAYS_INLINE void forEachGeneratorBehind(JSValue iterator, const Functor& functor, unsigned depth = 0)
{
    if (!iterator || !iterator.isCell())
        return;
    JSCell* cell = iterator.asCell();
    if (cell->type() == PyGeneratorType)
        return functor(uncheckedDowncast<PyGenerator>(cell));
    if (cell->type() != PyObjectType || !uncheckedDowncast<PyObject>(cell)->type()->has(TypeFlag::HasIteratorsToItself))
        return;
    if (cell->classInfo() == PyMultiIterator::info()) {
        if (auto* multi = uncheckedDowncast<PyMultiIterator>(cell); multi->hasGeneratorsToItself) [[unlikely]]
            forEachGeneratorIn(multi->first.get(), functor, depth + 1);
        return;
    }
    auto* record = uncheckedDowncast<PyRecord>(cell);
    if (!record->hasGeneratorsToItself) [[likely]]
        return;
    for (unsigned i = record->fieldCount(); i--;)
        forEachGeneratorIn(record->get(i), functor, depth + 1);
}

// `held`: what such an iterator has.
template<typename Functor>
static void forEachGeneratorIn(JSValue held, const Functor& functor, unsigned depth)
{
    static constexpr unsigned maxDepth = 8;
    if (!held || !held.isCell() || depth > maxDepth)
        return;
    JSCell* cell = held.asCell();
    // The iterators of a zip, the arguments of a chain.
    if (cell->type() == PyObjectType && cell->classInfo() == PyIterator::info()) {
        auto* plain = uncheckedDowncast<PyIterator>(cell);
        if (plain->kind() != PyIterator::Kind::Tuple || !plain->target())
            return;
        cell = plain->target().asCell();
    }
    if (cell->type() == PyTupleType) {
        auto* tuple = uncheckedDowncast<PyTuple>(cell);
        for (unsigned i = tuple->length(); i--;)
            forEachGeneratorBehind(tuple->at(i), functor, depth);
        return;
    }
    forEachGeneratorBehind(cell, functor, depth);
}

static NEVER_INLINE bool hasOwnedGenerator(JSCell* iterator)
{
    bool result = false;
    forEachGeneratorBehind(iterator, [&](PyGenerator* generator) {
        result |= generator->loopOwnership && !generator->isCompleted();
    });
    return result;
}

// It is an owned generator, or has one to itself.
static ALWAYS_INLINE bool ownsGenerator(JSValue iterator)
{
    if (!iterator || !iterator.isCell())
        return false;
    JSCell* cell = iterator.asCell();
    if (cell->type() == PyGeneratorType)
        return uncheckedDowncast<PyGenerator>(cell)->loopOwnership && !uncheckedDowncast<PyGenerator>(cell)->isCompleted();
    return hasGeneratorsToItself(cell) && hasOwnedGenerator(cell);
}

void disownGenerators(JSValue iterator)
{
    forEachGeneratorBehind(iterator, [&](PyGenerator* generator) {
        generator->loopOwnership = 0;
    });
}

bool Runtime::claimNewGenerators(JSValue iterable, CallFrame* callFrame, unsigned offset)
{
    bool result = false;
    for (unsigned i = 0; i < m_newGenerators.size() && m_newGenerators[i].generator;) {
        NewGenerator& note = m_newGenerators[i];
        // (The code of a generator expression asks for the iterator of its argument before the generator is made, which is the use.)
        if (iterable != JSValue(note.iterator ? note.iterator : note.generator) || note.caller != callFrame) {
            ++i;
            continue;
        }
        if (note.use == offset) {
            note.generator->loopOwnership = m_ownershipEpoch;
            result = true;
        }
        forget(note);
    }
    return result;
}

Runtime::CallOfInstruction Runtime::callInProgress()
{
    if (m_callOnBehalf.frame) [[unlikely]]
        return std::exchange(m_callOnBehalf, { });
    CallFrame* callFrame = m_vm.topCallFrame ? pythonCallerOf(m_vm, m_vm.topCallFrame) : nullptr;
    if (!callFrame)
        return { };
    return { callFrame, callFrame->callSiteIndex().bits() };
}

void Runtime::leaveGeneratorArgumentIn(JSCell*& slot, CallFrame* callFrame, unsigned offset)
{
    // With a note, what is called is something that asks callInProgress() first of all.
    for (NewGenerator& note : m_newGenerators) {
        if (note.generator && note.caller == callFrame && (note.use == offset || note.otherUse == offset)) {
            m_callOnBehalf = { callFrame, offset, &slot };
            return;
        }
    }
}

// Nothing but the call that a note is about can have passed the generator on.
JSCell* Runtime::claimGeneratorArgument(JSValue argument, JSCell**& whereToLeaveIt)
{
    // next(g())
    if (NewGenerator& last = m_newGenerators[0]; argument == JSValue(last.generator) && !last.iterator && !m_newGenerators[1].generator) [[likely]] {
        whereToLeaveIt = m_callOnBehalf.whereToLeaveArgument;
        if (whereToLeaveIt) [[unlikely]]
            m_callOnBehalf = { };
        last.generator->loopOwnership = m_ownershipEpoch;
        last.generator = nullptr;
        return argument.asCell();
    }
    return claimGeneratorArgumentAmongNotes(argument, whereToLeaveIt);
}

NEVER_INLINE JSCell* Runtime::claimGeneratorArgumentAmongNotes(JSValue argument, JSCell**& whereToLeaveIt)
{
    CallOfInstruction call = callInProgress();
    whereToLeaveIt = call.whereToLeaveArgument;
    bool isOwned = false;
    for (unsigned i = 0; i < m_newGenerators.size() && m_newGenerators[i].generator;) {
        NewGenerator& note = m_newGenerators[i];
        if (argument != JSValue(note.iterator ? note.iterator : note.generator)) {
            ++i;
            continue;
        }
        note.generator->loopOwnership = m_ownershipEpoch;
        isOwned = true;
        forget(note);
    }
    forgetNewGeneratorsFor(call, [](PyGenerator*) { });
    return isOwned ? argument.asCell() : nullptr;
}

void Runtime::iteratorWasMade(const CallOfInstruction& call, JSCell* iterator)
{
    // (Making it may have run code of the program, the __iter__ of another argument, but the frame is at the call for as long as that lasts.)
    const UseOfCall* use = call.frame && iterator ? owningUseOfCall(call.frame, call.offset) : nullptr;
    bool* mark = nullptr;
    if (use && iterator->type() == PyObjectType && uncheckedDowncast<PyObject>(iterator)->type()->has(TypeFlag::HasIteratorsToItself))
        mark = iterator->classInfo() == PyMultiIterator::info() ? &uncheckedDowncast<PyMultiIterator>(iterator)->hasGeneratorsToItself : &uncheckedDowncast<PyRecord>(iterator)->hasGeneratorsToItself;
    for (unsigned i = 0; i < m_newGenerators.size() && m_newGenerators[i].generator;) {
        NewGenerator& note = m_newGenerators[i];
        if (note.caller != call.frame || (note.use != call.offset && note.otherUse != call.offset)) {
            ++i;
            continue;
        }
        // iter(g()) is g().
        bool isItself = iterator == note.generator;
        if (!use || (!isItself && !mark)) {
            forget(note);
            continue;
        }
        if (!isItself) {
            note.iterator = iterator;
            *mark = true;
        }
        note.use = use->use;
        note.otherUse = use->otherUse;
        ++i;
    }
}

void GeneratorArgumentScope::release()
{
    if (m_whereToLeaveIt) {
        *m_whereToLeaveIt = m_generator;
        return;
    }
    m_runtime.releaseGeneratorGoneThrough(m_generator);
}

static std::span<const BlockPiece> blockPiecesOf(CodeBlock* codeBlock, CodeInfo& info)
{
    // A generator function has it in common with the body of the generator. It only binds its arguments.
    if (info.isGeneratorLike() && codeBlock->unlinkedCodeBlock()->parseMode() != SourceParseMode::GeneratorBodyMode)
        return { };
    if (!info.knowsBlockPieces) [[unlikely]] {
        info.blockPieces = FixedVector<BlockPiece>(blockPiecesIn(codeBlock->unlinkedCodeBlock()->pythonMarks()));
        ASSERT(info.hasLoopThatOwnsItsIterator || std::ranges::none_of(info.blockPieces, &BlockPiece::ownsIterator));
        info.knowsBlockPieces = true;
    }
    return info.blockPieces.span();
}

std::span<const BlockPiece> blockPiecesOf(CodeBlock* codeBlock)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable());
    CodeInfo* info = executable ? executable->unlinkedExecutable()->pythonCodeInfo() : nullptr;
    if (!info)
        return { };
    return blockPiecesOf(codeBlock, *info);
}

// The collector leaves such a generator alone: there is no handler in it that a GeneratorExit would get to. It must not hold anything
// either that is to be closed with it: whom it delegates to, the iterator of a loop of its own.
static bool hasNothingToClose(PyGenerator* generator)
{
    if (generator->finalization() != PyGenerator::Finalization::None || generator->delegate())
        return false;
    // A tool is told of the GeneratorExit all the same.
    if (CodeMonitor* monitor = generator->body()->codeInfo().monitor.get(); monitor && monitor->isAwake())
        return false;
    if (!generator->body()->codeInfo().hasLoopThatOwnsItsIterator) [[likely]]
        return true;
    for (unsigned i = 0; i < generator->slotCount(); ++i) {
        if (ownsGenerator(generator->slot(i)))
            return false;
    }
    return true;
}

// It is owned no longer. Whether there is something to do about that.
ALWAYS_INLINE bool Runtime::isToBeClosed(PyGenerator* generator)
{
    return generator->loopOwnership && std::exchange(generator->loopOwnership, 0) == m_ownershipEpoch && !generator->isCompleted() && !hasNothingToClose(generator);
}

void Runtime::releaseIteratorOfLoop(JSValue iterator)
{
    // for i, x in enumerate(g()): nothing that runs meanwhile can get at what has them.
    forEachGeneratorBehind(iterator, [&](PyGenerator* generator) ALWAYS_INLINE_LAMBDA {
        if (isToBeClosed(generator))
            finalizeGeneratorNow(m_globalObject, generator);
    });
}

void Runtime::releaseGeneratorGoneThrough(JSValue iterator)
{
    // Most have been gone through to their end, and most of the others have no clean-up to do: next(g()).
    if (auto* generator = PyGenerator::tryFrom(iterator); generator && (generator->isCompleted() || hasNothingToClose(generator))) [[likely]] {
        generator->loopOwnership = 0;
        return;
    }
    releaseGeneratorLeftHalfWay(iterator);
}

NEVER_INLINE void Runtime::releaseGeneratorLeftHalfWay(JSValue iterator)
{
    // An exception that was raised on the way waits.
    std::optional<ExceptionSetAside> suspended;
    forEachGeneratorBehind(iterator, [&](PyGenerator* generator) ALWAYS_INLINE_LAMBDA {
        if (!isToBeClosed(generator)) [[likely]]
            return;
        if (!suspended)
            suspended.emplace(m_globalObject);
        finalizeGeneratorNow(m_globalObject, generator);
    });
}

void Runtime::releaseWhatIteratorHad(JSValue held)
{
    std::optional<ExceptionSetAside> suspended;
    forEachGeneratorIn(held, [&](PyGenerator* generator) {
        if (!isToBeClosed(generator)) [[likely]]
            return;
        if (!suspended)
            suspended.emplace(m_globalObject);
        finalizeGeneratorNow(m_globalObject, generator);
    }, 1);
}

GeneratorsOwnedByFrame::GeneratorsOwnedByFrame(Runtime& runtime, PyGenerator* generator)
    : m_runtime(runtime)
{
    if (!generator->body()->codeInfo().hasLoopThatOwnsItsIterator) [[likely]]
        return;
    // Whoever has the frame object has the variables for as long as he keeps it.
    if (PyFrame* frame = generator->frameObject(); frame && frame->wasAskedFor())
        return;
    // That of the innermost loop first, as when they are taken off the stack of the frame.
    for (unsigned i = generator->slotCount(); i--;) {
        forEachGeneratorBehind(generator->slot(i), [&](PyGenerator* held) {
            if (held->loopOwnership == runtime.ownershipEpoch() && m_count < m_generators.size())
                m_generators[m_count++] = held;
        });
    }
}

void GeneratorsOwnedByFrame::frameIsGone()
{
    for (unsigned i = 0; i < m_count; ++i)
        m_runtime.releaseIteratorOfLoop(m_generators[i]);
    m_count = 0;
}

unsigned whereHandlerBegins(CodeBlock* codeBlock, unsigned offsetOfCatch)
{
    auto catchInstruction = codeBlock->instructions().at(offsetOfCatch);
    ASSERT(catchInstruction->opcodeID() == op_py_catch);
    auto jump = catchInstruction.next();
    return jump.offset() + jumpTargetForInstruction<OpJmp>(codeBlock, jump);
}

void Runtime::releaseIteratorsOfLoopsLeft(CallFrame* callFrame, unsigned offset, bool isCaught)
{
    // zip(g(), h()), and h() has raised: the use will not come.
    if (hasNewGenerator()) [[unlikely]] {
        for (unsigned i = 0; i < m_newGenerators.size() && m_newGenerators[i].generator;) {
            if (m_newGenerators[i].caller == callFrame)
                forget(m_newGenerators[i]);
            else
                ++i;
        }
    }
    if (callFrame->isZombieFrame()) [[unlikely]]
        return;
    CodeInfo& info = codeInfoOf(callFrame);
    if (!info.hasLoopThatOwnsItsIterator) [[likely]]
        return;
    CodeBlock* codeBlock = callFrame->codeBlock();
    auto loops = blockPiecesOf(codeBlock, info);
    // Empty: the loop has nothing to close.
    auto iteratorOf = [&](const BlockPiece& loop) -> JSValue {
        if (!loop.ownsIterator || !loop.contains(offset))
            return { };
        JSValue iterator = callFrame->r(VirtualRegister(loop.held)).jsValue();
        return ownsGenerator(iterator) ? iterator : JSValue();
    };
    if (std::ranges::none_of(loops, [&](const BlockPiece& loop) { return !!iteratorOf(loop); }))
        return;
    std::optional<unsigned> landing;
    if (isCaught) {
        auto* handler = codeBlock->handlerForBytecodeIndex(BytecodeIndex(offset));
        if (!handler)
            return;
        landing = whereHandlerBegins(codeBlock, handler->target);
    }

    for (size_t i = loops.size(); i--;) {
        auto& loop = loops[i];
        JSValue iterator = iteratorOf(loop);
        if (!iterator)
            continue;
        bool goesOn = landing && std::ranges::any_of(loops, [&](const BlockPiece& piece) {
            return piece.number == loop.number && piece.contains(*landing);
        });
        if (goesOn)
            continue;
        // The exception waits, as it does while CPython pops the iterator off the stack of the frame. The frames that it has
        // left are still there, below this one: see tellOfException() in PyMonitoring.cpp.
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        JSC::Exception* pending = scope.exception();
        scope.clearException();
        // Code of the program may run, which can ask for all the objects there are.
        if (auto* generator = PyGenerator::tryFrom(iterator); pending && !(generator && generator->isCompleted()))
            exceptionWillBeSeen(pending->value());
        {
            SetForScope topCallFrame(m_vm.topCallFrame, callFrame);
            framesToldOfException.append(callFrame);
            auto isDone = makeScopeExit([&] {
                framesToldOfException.removeLast(callFrame);
            });
            releaseIteratorOfLoop(iterator);
        }
        if (pending && !scope.exception()) {
            {
                auto throwScope = DECLARE_THROW_SCOPE(m_vm);
                throwException(m_globalObject, throwScope, pending);
            }
            EXCEPTION_ASSERT(scope.exception());
        }
    }
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
