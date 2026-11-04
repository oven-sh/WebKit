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
#include "PyFunction.h"
#include "PyGenerator.h"
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
// With reference counts, the view in bytes(memoryview(b)[:n]) is gone when bytes() returns, and the generator in
// `for x in g(): break` is closed by the `break`. Programs count on it: the next statement resizes b, or expects what a `finally` in g
// has done. Here nothing is known about who refers to an object until the collector has looked. But of some objects it can
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
    // The code block is only compared with. While it lives, there is no other at its address.
    if (m_newGenerator && (!visitor.isMarked(m_newGenerator) || !visitor.isMarked(m_codeBlockOfCallerOfNewGenerator)))
        m_newGenerator = nullptr;
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
        // The type is built in and cannot be changed, and its instances have no attributes of their own.
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

// So far the only objects taken on are memoryviews.
static bool canBeOwned(JSCell* object)
{
    return PyMemoryView::is(object);
}

void Runtime::didMakeResult(CallFrame* nativeFrame, JSCell* object)
{
    if (!canBeOwned(object))
        return;
    if (CallFrame* callFrame = pythonCallerOf(m_vm, nativeFrame))
        ownTemporary(callFrame, callFrame->bytecodeIndex().offset(), object);
}

void Runtime::didMakeResultOfInstruction(CallFrame* callFrame, JSCell* object)
{
    if (canBeOwned(object) && isFrameOfPythonCode(callFrame))
        ownTemporary(callFrame, callFrame->bytecodeIndex().offset(), object);
}

template<typename IsOperand>
void Runtime::releaseTemporariesUsedAt(CallFrame* callFrame, unsigned offset, JSValue result, const IsOperand& isOperand)
{
    // On the stack, which keeps them: to release one may run a __release_buffer__() of the program.
    std::array<PyMemoryView*, maxOwnedTemporaries> views;
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
            views[count++] = uncheckedDowncast<PyMemoryView>(temporary.object);
    }
    // As when the reference count falls to zero. There may be an exception pending, which stays.
    for (unsigned i = 0; i < count; ++i) {
        if (!views[i]->exports)
            views[i]->release(m_globalObject);
        ensureStillAliveHere(views[i]);
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

void Runtime::didCallMethod(JSValue self, JSValue result, bool resultIsFresh)
{
    CallFrame* nativeFrame = m_vm.topCallFrame;
    if (!nativeFrame || !nativeFrame->callee().isCell())
        return;
    auto* method = PyNativeFunction::tryFrom(nativeFrame->jsCallee());
    if (!method || !(method->def().flags & KeepsNothing) || !nativeFrame->argumentCount() || nativeFrame->uncheckedArgument(0) != self)
        return;
    if (ownsTemporaries())
        didCallWithTemporaries(nativeFrame, result);
    if (resultIsFresh && result && result.isCell())
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

// ---- The iterator of a `for` loop ----
//
// for x in g(): the generator is in the register where the loop keeps its iterator, and nowhere else. Generators are made too
// often to look at marks each time. So a generator function only notes the last one made and for whom (didMakeGenerator()), and
// op_py_get_iter, which is the very next instruction to run in the caller, looks: whether it is at the head of a `for` loop,
// over a call, and that call is the one that made the generator. Every generator made by such a call is looked at there,
// so a note that is left over was made by some other instruction, and does not pass.
// A loop that is left before its end closes the generator it owns: ReleaseIterator on the way of `break`, `return` and a `continue`
// of an outer loop; for an exception, the unwinder asks releaseIteratorsOfLoopsLeft().

void Runtime::claimIteratorOfLoop(CallFrame* callFrame, unsigned offset)
{
    PyGenerator* generator = std::exchange(m_newGenerator, nullptr);
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (m_codeBlockOfCallerOfNewGenerator != codeBlock)
        return;
    auto marks = codeBlock->unlinkedCodeBlock()->pythonMarks();
    const PythonMark* use = markAt(marks, offset, PythonMark::TemporaryUsed);
    if (!use || TemporaryUse::kindOf(use->bits) != TemporaryUse::ForHeader)
        return;
    // The call comes right before.
    unsigned callSite = m_callSiteOfNewGenerator.bits();
    for (const PythonMark* mark = use; mark != marks.data() && (--mark)->offset >= callSite;) {
        if (mark->offset == callSite && mark->kind() == PythonMark::TemporaryMade && mark->value() == use->value()) {
            generator->loopOwnership = m_ownershipEpoch;
            return;
        }
    }
}

std::span<const BlockPiece> blockPiecesOf(CodeBlock* codeBlock, bool onlyIfALoopOwnsItsIterator)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable());
    CodeInfo* info = executable ? executable->unlinkedExecutable()->pythonCodeInfo() : nullptr;
    if (!info)
        return { };
    // A generator function has it in common with the body of the generator. It only binds its arguments.
    if (info->isGeneratorLike() && codeBlock->unlinkedCodeBlock()->parseMode() != SourceParseMode::GeneratorBodyMode)
        return { };
    if (!info->knowsBlockPieces) [[unlikely]] {
        info->blockPieces = FixedVector<BlockPiece>(blockPiecesIn(codeBlock->unlinkedCodeBlock()->pythonMarks()));
        info->hasLoopThatOwnsItsIterator = std::ranges::any_of(info->blockPieces, &BlockPiece::ownsIterator);
        info->knowsBlockPieces = true;
    }
    if (onlyIfALoopOwnsItsIterator && !info->hasLoopThatOwnsItsIterator)
        return { };
    return info->blockPieces.span();
}

// The collector leaves such a generator alone: there is no handler in it that a GeneratorExit would get to. It must not hold anything
// either that is to be closed with it: whom it delegates to, the iterator of a loop of its own.
static bool hasNothingToClose(PyGenerator* generator)
{
    if (generator->finalization() != PyGenerator::Finalization::None || generator->delegate())
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

void Runtime::releaseIteratorsOfLoopsLeft(CallFrame* callFrame, unsigned offset, bool isCaught)
{
    if (callFrame->isZombieFrame())
        return;
    CodeBlock* codeBlock = callFrame->codeBlock();
    auto loops = blockPiecesOf(codeBlock, true);
    if (loops.empty()) [[likely]]
        return;
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
        // The op_catch is at the end of the code, out of the way, and jumps to where the compiler began the handler: that is the place
        // which is in a loop or not.
        auto catchInstruction = codeBlock->instructions().at(handler->target);
        auto jump = codeBlock->instructions().at(handler->target + catchInstruction->size());
        if (catchInstruction->opcodeID() != op_catch || jump->opcodeID() != op_jmp)
            return;
        landing = jump.offset() + jumpTargetForInstruction<OpJmp>(codeBlock, jump);
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
