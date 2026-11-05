/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyBytecode.h"
#include "PyBytes.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyIO.h"
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
    if (m_newGenerator && !visitor.isMarked(m_newGenerator))
        m_newGenerator = nullptr;
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
// of the argument. A generator function notes the last generator made, if it was made for such a use: at the head of a `for`
// loop, or as an argument of a built-in function that is already waiting in its register and says that it closes such an
// argument (NativeFlag::ClosesGeneratorArgument).
// The use takes the note up: op_py_get_iter, which is the very next instruction to run in the caller (for a generator expression, the call
// that makes it, with what it goes through for its argument), or the function, which does so
// whether or not the generator is the argument that it goes through (next(it, g()) may return it). So no note outlives the use. One that
// the use never came for, because another argument raised, is of a generator that can never turn up again. Other arguments are
// evaluated between the making and the use; they cannot get at the generator, and if they make another for such a use,
// the note is of that one and this one is left to the collector.
// A loop that is left before its end closes the generator it owns: ReleaseIterator on the way of `break`, `return` and a `continue`
// of an outer loop; for an exception, the unwinder asks releaseIteratorsOfLoopsLeft(). A function does when it is done: GeneratorArgumentScope.

void Runtime::findUseOfCall(UseOfCall& found, CallFrame* callFrame)
{
    found = { callFrame->codeBlock(), callFrame->callSiteIndex().bits() };
    if (!isFrameOfPythonCode(callFrame))
        return;
    auto marks = found.codeBlock->unlinkedCodeBlock()->pythonMarks();
    const PythonMark* made = markAt(marks, found.call, PythonMark::TemporaryMade);
    if (!made)
        return;
    const PythonMark* use = made + 1;
    const PythonMark* end = marks.data() + marks.size();
    while (use != end && (use->kind() != PythonMark::TemporaryUsed || use->value() != made->value()))
        ++use;
    if (use == end)
        return;
    found.use = use->offset;
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

void Runtime::didMakeGenerator(PyGenerator* generator, CallFrame* frameOfFunction)
{
    PyGenerator* previous = std::exchange(m_newGenerator, nullptr);
    void* caller = frameOfFunction->callerFrameOrEntryFrame();
    if (caller == m_vm.topEntryFrame) [[unlikely]]
        return;
    auto* callFrame = static_cast<CallFrame*>(caller);
    CodeBlock* codeBlock = callFrame->codeBlock();
    unsigned call = callFrame->callSiteIndex().bits();
    // (x for x in g()): this call is the use. See GeneratorsOwnedByFrame.
    if (previous && m_callerOfNewGenerator == callFrame && m_useOfNewGenerator == call) [[unlikely]]
        previous->loopOwnership = m_ownershipEpoch;
    UseOfCall& use = m_usesOfCalls[(call ^ std::bit_cast<uintptr_t>(codeBlock) >> 5) % m_usesOfCalls.size()];
    if (use.codeBlock != codeBlock || use.call != call) [[unlikely]]
        findUseOfCall(use, callFrame);
    if (use.kind == UseOfCall::Other)
        return;
    if (use.kind == UseOfCall::Argument) {
        auto* native = PyNativeFunction::tryFrom(callFrame->r(VirtualRegister(use.callee)).jsValue());
        if (!native || !(native->def().flags & ClosesGeneratorArgument))
            return;
    }
    m_newGenerator = generator;
    m_callerOfNewGenerator = callFrame;
    m_useOfNewGenerator = use.use;
}

void Runtime::claimIteratorOfLoop(unsigned offset)
{
    PyGenerator* generator = std::exchange(m_newGenerator, nullptr);
    if (m_useOfNewGenerator == offset)
        generator->loopOwnership = m_ownershipEpoch;
}

// Nothing but the call that the note is about can have passed the generator on.
PyGenerator* Runtime::claimGeneratorArgument(JSValue argument, PyGenerator**& whereToLeaveIt)
{
    PyGenerator* generator = std::exchange(m_newGenerator, nullptr);
    whereToLeaveIt = std::exchange(m_whereToLeaveGeneratorArgument, nullptr);
    if (argument != JSValue(generator))
        return nullptr;
    generator->loopOwnership = m_ownershipEpoch;
    return generator;
}

void GeneratorArgumentScope::release()
{
    if (m_whereToLeaveIt) {
        *m_whereToLeaveIt = m_generator;
        return;
    }
    // Most have been gone through to their end.
    if (m_generator->isCompleted()) [[likely]] {
        m_generator->loopOwnership = 0;
        return;
    }
    // An exception that the function has raised waits.
    SuspendExceptionScope suspended(m_runtime.vm());
    m_runtime.releaseIteratorOfLoop(m_generator);
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
    for (unsigned i = 0; i < generator->slotCount(); ++i) {
        if (auto* held = PyGenerator::tryFrom(generator->slot(i)); held && held->loopOwnership)
            return false;
    }
    return true;
}

void Runtime::releaseIteratorOfLoop(JSValue iterator)
{
    auto* generator = PyGenerator::tryFrom(iterator);
    if (!generator || !generator->loopOwnership)
        return;
    if (std::exchange(generator->loopOwnership, 0) == m_ownershipEpoch && !hasNothingToClose(generator))
        finalizeGeneratorNow(m_globalObject, generator);
}

GeneratorsOwnedByFrame::GeneratorsOwnedByFrame(Runtime& runtime, PyGenerator* generator)
    : m_runtime(runtime)
{
    if (!generator->body()->codeInfo().hasLoopThatOwnsItsIterator) [[likely]]
        return;
    for (unsigned i = 0; i < generator->slotCount() && m_count < m_generators.size(); ++i) {
        if (auto* held = PyGenerator::tryFrom(generator->slot(i)); held && held->loopOwnership == runtime.ownershipEpoch())
            m_generators[m_count++] = held;
    }
}

void GeneratorsOwnedByFrame::frameIsGone()
{
    // That of the innermost loop first, as when they are taken off the stack of the frame.
    while (m_count)
        m_runtime.releaseIteratorOfLoop(m_generators[--m_count]);
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
    if (callFrame->isZombieFrame()) [[unlikely]]
        return;
    CodeInfo& info = codeInfoOf(callFrame);
    if (!info.hasLoopThatOwnsItsIterator) [[likely]]
        return;
    CodeBlock* codeBlock = callFrame->codeBlock();
    auto loops = blockPiecesOf(codeBlock, info);
    auto generatorOf = [&](const BlockPiece& loop) -> PyGenerator* {
        if (!loop.ownsIterator || !loop.contains(offset))
            return nullptr;
        auto* generator = PyGenerator::tryFrom(callFrame->r(VirtualRegister(loop.held)).jsValue());
        return generator && generator->loopOwnership ? generator : nullptr;
    };
    if (std::ranges::none_of(loops, generatorOf))
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
        PyGenerator* generator = generatorOf(loop);
        if (!generator)
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
        {
            SetForScope topCallFrame(m_vm.topCallFrame, callFrame);
            framesToldOfException.append(callFrame);
            auto isDone = makeScopeExit([&] {
                framesToldOfException.removeLast(callFrame);
            });
            releaseIteratorOfLoop(generator);
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
