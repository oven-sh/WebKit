/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonSequences.h"

// The module _collections: Modules/_collectionsmodule.c of CPython.

namespace JSC { namespace Python {

namespace {

struct CollectionsState final : NativeState {
    PYTHON_NATIVE_STATE(CollectionsState);
    WriteBarrier<PyType> deque;
    WriteBarrier<PyType> defaultDict;
    WriteBarrier<PyType> dequeIterator;
    WriteBarrier<PyType> dequeReverseIterator;
    WriteBarrier<PyType> tupleGetter;
    WriteBarrier<Structure> blockStructure;
};

template<typename Visitor>
void CollectionsState::visit(Visitor& visitor)
{
    visitor.append(deque);
    visitor.append(defaultDict);
    visitor.append(dequeIterator);
    visitor.append(dequeReverseIterator);
    visitor.append(tupleGetter);
    visitor.append(blockStructure);
}

CollectionsState& initializeCollections(JSGlobalObject*);

CollectionsState& collectionsState(JSGlobalObject* globalObject)
{
    auto& state = globalObject->pyRealm()->moduleState<CollectionsState>();
    return state.deque ? state : initializeCollections(globalObject);
}

#define STOP() return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()))

// ---- deque

// What is in a deque is in a chain of blocks, each of so many places, so that adding to either end and taking from it moves nothing else. There is always at least one block. The first element is at
// leftBlock[leftIndex] and the last at rightBlock[rightIndex], both of which are places that are in use. In an empty one, leftIndex is center + 1 and rightIndex is center.
//
// A block is a cell, since the collector is to go through what is in it, and what it goes through is not to be in anything that grows. A place that is no longer in use is emptied, so that it keeps nothing alive.
static constexpr int64_t blockLength = 64;
static constexpr int64_t center = (blockLength - 1) / 2;
// sizeof(block) in CPython, for __sizeof__().
static constexpr int64_t sizeOfBlock = (blockLength + 2) * 8;

struct Block final : NativeState {
    PYTHON_NATIVE_STATE(Block);
    WriteBarrier<PyStateObject> leftLink;
    WriteBarrier<PyStateObject> rightLink;
    std::array<WriteBarrier<Unknown>, blockLength> data;
};

template<typename Visitor>
void Block::visit(Visitor& visitor)
{
    visitor.append(leftLink);
    visitor.append(rightLink);
    for (auto& item : data)
        visitor.append(item);
}

Block& fieldsOf(PyStateObject* block) { return block->state<Block>(); }

struct DequeState final : NativeState {
    PYTHON_NATIVE_STATE(DequeState);
    WriteBarrier<PyStateObject> leftBlock;
    WriteBarrier<PyStateObject> rightBlock;
    int64_t leftIndex { center + 1 };
    int64_t rightIndex { center };
    int64_t size { 0 };
    uint64_t state { 0 }; // It goes up whenever the indices move.
    int64_t maxLength { -1 }; // Less than nothing if there is no limit.
};

template<typename Visitor>
void DequeState::visit(Visitor& visitor)
{
    visitor.append(leftBlock);
    visitor.append(rightBlock);
}

DequeState& dequeOf(JSValue value) { return stateOf<DequeState>(value); }

PyStateObject* newBlock(JSGlobalObject* globalObject)
{
    return PyStateObject::create(globalObject->vm(), collectionsState(globalObject).blockStructure.get(), makeUnique<Block>());
}

// deque_new()
PyStateObject* newDeque(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<DequeState>());
    auto& deque = object->state<DequeState>();
    PyStateObject* block = newBlock(globalObject);
    deque.leftBlock.set(vm, object, block);
    deque.rightBlock.set(vm, object, block);
    return object;
}

// deque_pop_impl(), of one that is not empty.
JSValue popRight(VM& vm, JSCell* self, DequeState& deque)
{
    auto& slot = fieldsOf(deque.rightBlock.get()).data[deque.rightIndex];
    JSValue item = slot.get();
    slot.clear();
    --deque.rightIndex;
    --deque.size;
    ++deque.state;
    if (deque.rightIndex < 0) {
        if (deque.size) {
            PyStateObject* previous = fieldsOf(deque.rightBlock.get()).leftLink.get();
            fieldsOf(previous).rightLink.clear();
            deque.rightBlock.set(vm, self, previous);
            deque.rightIndex = blockLength - 1;
        } else {
            deque.leftIndex = center + 1;
            deque.rightIndex = center;
        }
    }
    return item;
}

// deque_popleft_impl(), likewise.
JSValue popLeft(VM& vm, JSCell* self, DequeState& deque)
{
    auto& slot = fieldsOf(deque.leftBlock.get()).data[deque.leftIndex];
    JSValue item = slot.get();
    slot.clear();
    ++deque.leftIndex;
    --deque.size;
    ++deque.state;
    if (deque.leftIndex == blockLength) {
        if (deque.size) {
            PyStateObject* next = fieldsOf(deque.leftBlock.get()).rightLink.get();
            fieldsOf(next).leftLink.clear();
            deque.leftBlock.set(vm, self, next);
            deque.leftIndex = 0;
        } else {
            deque.leftIndex = center + 1;
            deque.rightIndex = center;
        }
    }
    return item;
}

// True whenever 0 <= maxLength < size.
bool needsTrim(const DequeState& deque, int64_t maxLength) { return static_cast<uint64_t>(maxLength) < static_cast<uint64_t>(deque.size); }

void addBlockOnRight(JSGlobalObject* globalObject, JSCell* self, DequeState& deque)
{
    VM& vm = globalObject->vm();
    PyStateObject* block = newBlock(globalObject);
    fieldsOf(block).leftLink.set(vm, block, deque.rightBlock.get());
    fieldsOf(deque.rightBlock.get()).rightLink.set(vm, deque.rightBlock.get(), block);
    deque.rightBlock.set(vm, self, block);
    deque.rightIndex = -1;
}

// deque_append_lock_held()
void appendRight(JSGlobalObject* globalObject, JSCell* self, DequeState& deque, JSValue item, int64_t maxLength)
{
    VM& vm = globalObject->vm();
    if (deque.rightIndex == blockLength - 1)
        addBlockOnRight(globalObject, self, deque);
    ++deque.size;
    ++deque.rightIndex;
    fieldsOf(deque.rightBlock.get()).data[deque.rightIndex].set(vm, deque.rightBlock.get(), item);
    if (needsTrim(deque, maxLength))
        popLeft(vm, self, deque);
    else
        ++deque.state;
}

// deque_appendleft_lock_held()
void appendLeft(JSGlobalObject* globalObject, JSCell* self, DequeState& deque, JSValue item, int64_t maxLength)
{
    VM& vm = globalObject->vm();
    if (!deque.leftIndex) {
        PyStateObject* block = newBlock(globalObject);
        fieldsOf(block).rightLink.set(vm, block, deque.leftBlock.get());
        fieldsOf(deque.leftBlock.get()).leftLink.set(vm, deque.leftBlock.get(), block);
        deque.leftBlock.set(vm, self, block);
        deque.leftIndex = blockLength;
    }
    ++deque.size;
    --deque.leftIndex;
    fieldsOf(deque.leftBlock.get()).data[deque.leftIndex].set(vm, deque.leftBlock.get(), item);
    if (needsTrim(deque, maxLength))
        popRight(vm, self, deque);
    else
        ++deque.state;
}

// deque_clear()
void clearDeque(JSGlobalObject* globalObject, JSCell* self, DequeState& deque)
{
    VM& vm = globalObject->vm();
    if (!deque.size)
        return;
    PyStateObject* block = newBlock(globalObject);
    deque.size = 0;
    deque.leftBlock.set(vm, self, block);
    deque.rightBlock.set(vm, self, block);
    deque.leftIndex = center + 1;
    deque.rightIndex = center;
    ++deque.state;
}

// deque_extend_impl() and deque_extendleft_impl(). False if it raised.
bool extendDeque(JSGlobalObject* globalObject, JSCell* self, JSValue iterable, bool isOnLeft)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& deque = dequeOf(self);
    int64_t maxLength = deque.maxLength;
    // With itself, it is with what is in it now.
    if (iterable == JSValue(self)) {
        JSArray* list = listFromIterable(globalObject, iterable);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, extendDeque(globalObject, self, list, isOnLeft));
    }
    JSValue iterator = getIterator(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, false);
    // So as to leave the most room on the side that is being added to.
    if (maxLength && !deque.size) {
        deque.leftIndex = isOnLeft ? blockLength - 1 : 1;
        deque.rightIndex = isOnLeft ? blockLength - 2 : 0;
    }
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        // If there is room for nothing, it is gone through all the same.
        if (!maxLength)
            continue;
        if (isOnLeft)
            appendLeft(globalObject, self, deque, item, maxLength);
        else
            appendRight(globalObject, self, deque, item, maxLength);
    }
}

// deque_copy_impl(). Empty if it raised.
JSValue copyDeque(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& old = dequeOf(self);
    PyType* dequeType = collectionsState(globalObject).deque.get();
    PyType* type = typeOf(globalObject, self);
    if (type == dequeType) {
        PyStateObject* copy = newDeque(globalObject, dequeType);
        auto& deque = copy->state<DequeState>();
        deque.maxLength = old.maxLength;
        if (old.size > 0) {
            PyStateObject* block = old.leftBlock.get();
            int64_t index = old.leftIndex;
            deque.leftIndex = 1;
            deque.rightIndex = 0;
            for (int64_t i = 0; i < old.size; ++i) {
                appendRight(globalObject, copy, deque, fieldsOf(block).data[index].get(), deque.maxLength);
                if (++index == blockLength) {
                    block = fieldsOf(block).rightLink.get();
                    index = 0;
                }
            }
        }
        return copy;
    }
    JSValue result = old.maxLength < 0 ? call(globalObject, type->object(), self) : call(globalObject, type->object(), self, intFromInt64(globalObject, old.maxLength));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance(globalObject, result, dequeType))
        return raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), "() must return a deque, not "_s, typeName(globalObject, result)));
    return result;
}

// _deque_rotate()
void rotateDeque(JSGlobalObject* globalObject, JSCell* self, DequeState& deque, int64_t n)
{
    VM& vm = globalObject->vm();
    PyStateObject* spare = nullptr;
    PyStateObject* leftBlock = deque.leftBlock.get();
    PyStateObject* rightBlock = deque.rightBlock.get();
    int64_t leftIndex = deque.leftIndex;
    int64_t rightIndex = deque.rightIndex;
    int64_t length = deque.size;
    int64_t halfLength = length >> 1;
    if (length <= 1)
        return;
    if (n > halfLength || n < -halfLength) {
        n %= length;
        if (n > halfLength)
            n -= length;
        else if (n < -halfLength)
            n += length;
    }
    ++deque.state;
    auto move = [&] (PyStateObject* from, int64_t source, PyStateObject* to, int64_t destination, int64_t count) {
        auto& sourceData = fieldsOf(from).data;
        auto& destinationData = fieldsOf(to).data;
        do {
            destinationData[destination++].set(vm, to, sourceData[source].get());
            sourceData[source++].clear();
        } while (--count);
    };
    // The deque is kept as it should be all the way through, since making a block can bring on a collection.
    auto store = [&] {
        deque.leftBlock.set(vm, self, leftBlock);
        deque.rightBlock.set(vm, self, rightBlock);
        deque.leftIndex = leftIndex;
        deque.rightIndex = rightIndex;
    };
    while (n > 0) {
        if (!leftIndex) {
            if (!spare)
                spare = newBlock(globalObject);
            fieldsOf(spare).rightLink.set(vm, spare, leftBlock);
            fieldsOf(leftBlock).leftLink.set(vm, leftBlock, spare);
            leftBlock = spare;
            leftIndex = blockLength;
            spare = nullptr;
        }
        int64_t m = std::min({ n, rightIndex + 1, leftIndex });
        rightIndex -= m;
        leftIndex -= m;
        n -= m;
        move(rightBlock, rightIndex + 1, leftBlock, leftIndex, m);
        if (rightIndex < 0) {
            spare = rightBlock;
            rightBlock = fieldsOf(rightBlock).leftLink.get();
            fieldsOf(rightBlock).rightLink.clear();
            fieldsOf(spare).leftLink.clear();
            rightIndex = blockLength - 1;
        }
        store();
    }
    while (n < 0) {
        if (rightIndex == blockLength - 1) {
            if (!spare)
                spare = newBlock(globalObject);
            fieldsOf(spare).leftLink.set(vm, spare, rightBlock);
            fieldsOf(rightBlock).rightLink.set(vm, rightBlock, spare);
            rightBlock = spare;
            rightIndex = -1;
            spare = nullptr;
        }
        int64_t m = std::min({ -n, blockLength - leftIndex, blockLength - 1 - rightIndex });
        move(leftBlock, leftIndex, rightBlock, rightIndex + 1, m);
        leftIndex += m;
        rightIndex += m;
        n += m;
        if (leftIndex == blockLength) {
            spare = leftBlock;
            leftBlock = fieldsOf(leftBlock).rightLink.get();
            fieldsOf(leftBlock).leftLink.clear();
            fieldsOf(spare).rightLink.clear();
            leftIndex = 0;
        }
        store();
    }
    ensureStillAliveHere(spare);
}

bool isValidIndex(int64_t index, int64_t limit) { return static_cast<uint64_t>(index) < static_cast<uint64_t>(limit); }

// Where the element of that number is, going from whichever end is nearer.
WriteBarrier<Unknown>& slotAt(DequeState& deque, int64_t index, bool isNearLeft, PyStateObject*& block)
{
    int64_t i = index + deque.leftIndex;
    int64_t n = i / blockLength;
    i %= blockLength;
    if (isNearLeft) {
        block = deque.leftBlock.get();
        while (--n >= 0)
            block = fieldsOf(block).rightLink.get();
    } else {
        n = (deque.leftIndex + deque.size - 1) / blockLength - n;
        block = deque.rightBlock.get();
        while (--n >= 0)
            block = fieldsOf(block).leftLink.get();
    }
    return fieldsOf(block).data[i];
}

// deque_del_item()
void deleteAt(JSGlobalObject* globalObject, JSCell* self, DequeState& deque, int64_t index)
{
    rotateDeque(globalObject, self, deque, -index);
    popLeft(globalObject->vm(), self, deque);
    rotateDeque(globalObject, self, deque, index);
}

// getindex() of CPython's Objects/typeobject.c: what the wrapper for sq_item makes of its argument. Nothing if it raised.
std::optional<int64_t> indexArgument(JSGlobalObject* globalObject, DequeState& deque, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto index = toIndexOrOverflow(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return *index < 0 ? *index + deque.size : *index;
}

JSValue raiseMutated(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type = BuiltinType::RuntimeError)
{
    return raise(globalObject, scope, type, "deque mutated during iteration"_s);
}

// One place after another from the left.
struct Cursor {
    PyStateObject* block;
    int64_t index;
    JSValue get() const { return fieldsOf(block).data[index].get(); }
    void advance()
    {
        if (++index == blockLength) {
            block = fieldsOf(block).rightLink.get();
            index = 0;
        }
    }
};

} // anonymous namespace

PYTHON_NATIVE(dequeNew)
{
    return JSValue::encode(newDeque(globalObject, asType(callFrame->uncheckedArgument(0))));
}

// deque([iterable[, maxlen]])
PYTHON_NATIVE(dequeInit)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& deque = dequeOf(self);
    int64_t maxLength = -1;
    if (JSValue value = args.at(2); value && !isNone(value)) {
        auto given = toSsizeOfInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (*given < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "maxlen must be non-negative"_s));
        maxLength = *given;
    }
    deque.maxLength = maxLength;
    clearDeque(globalObject, self, deque);
    if (JSValue iterable = args.at(1)) {
        extendDeque(globalObject, self, iterable, false);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(dequePop)
{
    bool isOnLeft = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& deque = dequeOf(self);
    if (!deque.size)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop from an empty deque"_s));
    return JSValue::encode(isOnLeft ? popLeft(vm, self, deque) : popRight(vm, self, deque));
}

PYTHON_NATIVE(dequeAppend)
{
    bool isOnLeft = unpack<bool>(callFrame, 0);
    JSCell* self = callFrame->uncheckedArgument(0).asCell();
    auto& deque = dequeOf(self);
    if (isOnLeft)
        appendLeft(globalObject, self, deque, callFrame->uncheckedArgument(1), deque.maxLength);
    else
        appendRight(globalObject, self, deque, callFrame->uncheckedArgument(1), deque.maxLength);
    RETURN_NONE();
}

PYTHON_NATIVE(dequeExtend)
{
    bool isOnLeft = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    extendDeque(globalObject, args[0].asCell(), args[1], isOnLeft);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(dequeInPlaceConcat)
{
    NATIVE_PROLOGUE();
    extendDeque(globalObject, args[0].asCell(), args[1], false);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(dequeCopy)
{
    return JSValue::encode(copyDeque(globalObject, callFrame->uncheckedArgument(0)));
}

PYTHON_NATIVE(dequeConcat)
{
    NATIVE_PROLOGUE();
    bool isDeque = isInstanceOf(globalObject, args[1], collectionsState(globalObject).deque->object());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDeque)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("can only concatenate deque (not \""_s, typeName(globalObject, args[1]), "\") to deque"_s)));
    JSValue result = copyDeque(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    extendDeque(globalObject, result.asCell(), args[1], false);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(dequeClear)
{
    JSCell* self = callFrame->uncheckedArgument(0).asCell();
    clearDeque(globalObject, self, dequeOf(self));
    RETURN_NONE();
}

// deque_inplace_repeat_lock_held(). False if it raised.
static bool repeatInPlace(JSGlobalObject* globalObject, JSCell* self, int64_t n)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& deque = dequeOf(self);
    int64_t size = deque.size;
    if (!size || n == 1)
        return true;
    if (n <= 0) {
        clearDeque(globalObject, self, deque);
        return true;
    }
    if (size == 1) {
        JSValue item = fieldsOf(deque.leftBlock.get()).data[deque.leftIndex].get();
        if (deque.maxLength >= 0 && n > deque.maxLength)
            n = deque.maxLength;
        // There is no room for more than a list has.
        if (n > std::numeric_limits<int32_t>::max()) {
            raiseMemoryError(globalObject, scope);
            return false;
        }
        ++deque.state;
        for (int64_t i = 0; i < n - 1; ++i) {
            if (deque.rightIndex == blockLength - 1)
                addBlockOnRight(globalObject, self, deque);
            ++deque.rightIndex;
            ++deque.size;
            fieldsOf(deque.rightBlock.get()).data[deque.rightIndex].set(vm, deque.rightBlock.get(), item);
        }
        return true;
    }
    if (static_cast<uint64_t>(size) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / static_cast<uint64_t>(n)) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    JSArray* sequence = listFromIterable(globalObject, self);
    RETURN_IF_EXCEPTION(scope, false);
    // No more times than it takes to fill it.
    if (deque.maxLength >= 0 && n * size > deque.maxLength)
        n = (deque.maxLength + size - 1) / size;
    if (deque.maxLength < 0 && n * size > std::numeric_limits<int32_t>::max()) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    for (int64_t i = 0; i < n - 1; ++i) {
        extendDeque(globalObject, self, sequence, false);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

PYTHON_NATIVE(dequeRepeat)
{
    bool isInPlace = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto n = toIndexOrOverflow(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue target = args[0];
    if (!isInPlace) {
        target = copyDeque(globalObject, target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    repeatInPlace(globalObject, target.asCell(), *n);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(target);
}

// rotate(n=1, /)
PYTHON_NATIVE(dequeRotate)
{
    NATIVE_PROLOGUE();
    int64_t n = 1;
    if (JSValue value = args.at(1)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        n = *given;
    }
    JSCell* self = args[0].asCell();
    rotateDeque(globalObject, self, dequeOf(self), n);
    RETURN_NONE();
}

PYTHON_NATIVE(dequeReverse)
{
    VM& vm = globalObject->vm();
    auto& deque = dequeOf(callFrame->uncheckedArgument(0));
    PyStateObject* leftBlock = deque.leftBlock.get();
    PyStateObject* rightBlock = deque.rightBlock.get();
    int64_t leftIndex = deque.leftIndex;
    int64_t rightIndex = deque.rightIndex;
    for (int64_t n = deque.size >> 1; --n >= 0;) {
        auto& left = fieldsOf(leftBlock).data[leftIndex];
        auto& right = fieldsOf(rightBlock).data[rightIndex];
        JSValue kept = left.get();
        left.set(vm, leftBlock, right.get());
        right.set(vm, rightBlock, kept);
        if (++leftIndex == blockLength) {
            leftBlock = fieldsOf(leftBlock).rightLink.get();
            leftIndex = 0;
        }
        if (--rightIndex < 0) {
            rightBlock = fieldsOf(rightBlock).leftLink.get();
            rightIndex = blockLength - 1;
        }
    }
    RETURN_NONE();
}

PYTHON_NATIVE(dequeCount)
{
    NATIVE_PROLOGUE();
    auto& deque = dequeOf(args[0]);
    Cursor cursor { deque.leftBlock.get(), deque.leftIndex };
    uint64_t startState = deque.state;
    int64_t count = 0;
    for (int64_t n = deque.size; --n >= 0;) {
        bool isSame = isEqual(globalObject, cursor.get(), args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        count += isSame;
        if (startState != deque.state)
            return JSValue::encode(raiseMutated(globalObject, scope));
        cursor.advance();
    }
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(dequeContains)
{
    NATIVE_PROLOGUE();
    auto& deque = dequeOf(args[0]);
    Cursor cursor { deque.leftBlock.get(), deque.leftIndex };
    uint64_t startState = deque.state;
    for (int64_t n = deque.size; --n >= 0;) {
        bool isSame = isEqual(globalObject, cursor.get(), args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            return JSValue::encode(jsBoolean(true));
        if (startState != deque.state)
            return JSValue::encode(raiseMutated(globalObject, scope));
        cursor.advance();
    }
    return JSValue::encode(jsBoolean(false));
}

PYTHON_NATIVE(dequeLength)
{
    return JSValue::encode(intFromInt64(globalObject, dequeOf(callFrame->uncheckedArgument(0)).size));
}

// index(value, [start, [stop]])
PYTHON_NATIVE(dequeIndex)
{
    NATIVE_PROLOGUE();
    // _PyArg_CheckPositional()
    if (unsigned count = args.size() - 1; count < 1 || count > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("index expected at "_s, count < 1 ? "least 1 argument"_s : "most 3 arguments"_s, ", got "_s, count)));
    auto& deque = dequeOf(args[0]);
    int64_t start = 0;
    int64_t stop = std::numeric_limits<int64_t>::max();
    if (JSValue value = args.at(2)) {
        auto given = toSliceIndex(globalObject, value, false);
        RETURN_IF_EXCEPTION(scope, { });
        start = *given;
    }
    if (JSValue value = args.at(3)) {
        auto given = toSliceIndex(globalObject, value, false);
        RETURN_IF_EXCEPTION(scope, { });
        stop = *given;
    }
    Cursor cursor { deque.leftBlock.get(), deque.leftIndex };
    uint64_t startState = deque.state;
    int64_t size = deque.size;
    if (start < 0)
        start = std::max<int64_t>(start + size, 0);
    if (stop < 0)
        stop = std::max<int64_t>(stop + size, 0);
    stop = std::min(stop, size);
    start = std::min(start, stop);
    int64_t i = 0;
    for (; i < start - blockLength; i += blockLength)
        cursor.block = fieldsOf(cursor.block).rightLink.get();
    for (; i < start; ++i)
        cursor.advance();
    for (int64_t n = stop - i; --n >= 0;) {
        bool isSame = isEqual(globalObject, cursor.get(), args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            return JSValue::encode(intFromInt64(globalObject, stop - n - 1));
        if (startState != deque.state)
            return JSValue::encode(raiseMutated(globalObject, scope));
        cursor.advance();
    }
    return JSValue::encode(raiseValueError(globalObject, scope, "deque.index(x): x not in deque"_s));
}

// insert(index, value, /)
PYTHON_NATIVE(dequeInsert)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& deque = dequeOf(self);
    auto given = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t index = *given;
    JSValue value = args[2];
    int64_t n = deque.size;
    if (deque.maxLength == n)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "deque already at its maximum size"_s));
    if (index >= n) {
        appendRight(globalObject, self, deque, value, deque.maxLength);
        RETURN_NONE();
    }
    if (index <= -n || !index) {
        appendLeft(globalObject, self, deque, value, deque.maxLength);
        RETURN_NONE();
    }
    rotateDeque(globalObject, self, deque, -index);
    if (index < 0)
        appendRight(globalObject, self, deque, value, deque.maxLength);
    else
        appendLeft(globalObject, self, deque, value, deque.maxLength);
    rotateDeque(globalObject, self, deque, index);
    RETURN_NONE();
}

PYTHON_NATIVE(dequeGetItem)
{
    NATIVE_PROLOGUE();
    auto& deque = dequeOf(args[0]);
    auto index = indexArgument(globalObject, deque, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isValidIndex(*index, deque.size))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "deque index out of range"_s));
    PyStateObject* block;
    return JSValue::encode(slotAt(deque, *index, *index < (deque.size >> 1), block).get());
}

// __setitem__(index, value) and __delitem__(index)
PYTHON_NATIVE(dequeSetItem)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& deque = dequeOf(self);
    auto index = indexArgument(globalObject, deque, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isValidIndex(*index, deque.size))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "deque index out of range"_s));
    if (args.size() < 3) {
        deleteAt(globalObject, self, deque, *index);
        RETURN_NONE();
    }
    PyStateObject* block;
    auto& slot = slotAt(deque, *index, *index <= ((deque.size + 1) >> 1), block);
    slot.set(vm, block, args[2]);
    RETURN_NONE();
}

PYTHON_NATIVE(dequeRemove)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& deque = dequeOf(self);
    Cursor cursor { deque.leftBlock.get(), deque.leftIndex };
    int64_t n = deque.size;
    uint64_t startState = deque.state;
    int64_t i = 0;
    for (; i < n; ++i) {
        bool isSame = isEqual(globalObject, cursor.get(), args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (startState != deque.state)
            return JSValue::encode(raiseMutated(globalObject, scope, BuiltinType::IndexError));
        if (isSame)
            break;
        cursor.advance();
    }
    if (i == n)
        return JSValue::encode(raiseValueError(globalObject, scope, "deque.remove(x): x not in deque"_s));
    deleteAt(globalObject, self, deque, i);
    RETURN_NONE();
}

PYTHON_NATIVE(dequeReduce)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& deque = dequeOf(self);
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue arguments = deque.maxLength < 0 ? JSValue(realm->emptyTuple()) : JSValue(PyTuple::create(globalObject, { realm->emptyTuple(), intFromInt64(globalObject, deque.maxLength) }));
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, self)->object(), arguments, state, iterator }));
}

PYTHON_NATIVE(dequeRepr)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    ReprGuard guard(globalObject, self.asCell());
    if (guard.isRecursive())
        return JSValue::encode(jsNontrivialString(vm, "[...]"_s));
    JSArray* list = listFromIterable(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    String shown = repr(globalObject, list);
    RETURN_IF_EXCEPTION(scope, { });
    String name = typeOf(globalObject, self)->nameWithoutModule(globalObject);
    int64_t maxLength = dequeOf(self).maxLength;
    if (maxLength >= 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', shown, ", maxlen="_s, maxLength, ')'))));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', shown, ')'))));
}

PYTHON_NATIVE(dequeCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue v = args[0];
    JSValue w = args[1];
    PyType* dequeType = collectionsState(globalObject).deque.get();
    if (!isInstance(globalObject, v, dequeType) || !isInstance(globalObject, w, dequeType))
        RETURN_NOT_IMPLEMENTED();
    if (isEquality(op)) {
        bool isEq = op == ComparisonOperator::Eq;
        if (v == w)
            return JSValue::encode(jsBoolean(isEq));
        if (dequeOf(v).size != dequeOf(w).size)
            return JSValue::encode(jsBoolean(!isEq));
    }
    // The first place at which they differ.
    JSValue first = getIterator(globalObject, v);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue second = getIterator(globalObject, w);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue x;
    JSValue y;
    while (true) {
        x = iteratorNext(globalObject, first);
        RETURN_IF_EXCEPTION(scope, { });
        y = iteratorNext(globalObject, second);
        if (!x || !y)
            break;
        bool isSame = isEqual(globalObject, x, y);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isSame) {
            // PyObject_RichCompareBool()
            JSValue result = compare(globalObject, op, x, y);
            RETURN_IF_EXCEPTION(scope, { });
            bool answer = isTrue(globalObject, result);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(jsBoolean(answer));
        }
    }
    RETURN_IF_EXCEPTION(scope, { });
    // One has come to its end, or both have.
    bool result = false;
    switch (op) {
    case ComparisonOperator::Lt:
        result = !!y;
        break;
    case ComparisonOperator::LtE:
        result = !x;
        break;
    case ComparisonOperator::Eq:
        result = !x && !y;
        break;
    case ComparisonOperator::NotEq:
        result = !!x || !!y;
        break;
    case ComparisonOperator::Gt:
        result = !!x;
        break;
    case ComparisonOperator::GtE:
        result = !y;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return JSValue::encode(jsBoolean(result));
}

PYTHON_NATIVE(dequeSizeOf)
{
    JSValue self = callFrame->uncheckedArgument(0);
    auto& deque = dequeOf(self);
    int64_t blocks = (deque.leftIndex + deque.size + blockLength - 1) / blockLength;
    return JSValue::encode(intFromInt64(globalObject, typeOf(globalObject, self)->basicSize() + blocks * sizeOfBlock));
}

// ---- What goes through a deque, either way

namespace {
struct DequeIteratorState final : NativeState {
    PYTHON_NATIVE_STATE(DequeIteratorState);
    WriteBarrier<PyStateObject> block;
    int64_t index { 0 };
    WriteBarrier<Unknown> deque;
    uint64_t state { 0 }; // What the deque's was when this was made.
    int64_t counter { 0 }; // How many there are to come.
};
template<typename Visitor> void DequeIteratorState::visit(Visitor& visitor)
{
    visitor.append(block);
    visitor.append(deque);
}

// deque_iter() and deque_reviter()
PyStateObject* newDequeIterator(JSGlobalObject* globalObject, JSValue dequeObject, bool isReverse)
{
    VM& vm = globalObject->vm();
    auto& types = collectionsState(globalObject);
    auto& deque = dequeOf(dequeObject);
    auto* object = PyStateObject::create(vm, (isReverse ? types.dequeReverseIterator : types.dequeIterator)->instanceStructure(), makeUnique<DequeIteratorState>());
    auto& iterator = object->state<DequeIteratorState>();
    iterator.block.set(vm, object, isReverse ? deque.rightBlock.get() : deque.leftBlock.get());
    iterator.index = isReverse ? deque.rightIndex : deque.leftIndex;
    iterator.deque.set(vm, object, dequeObject);
    iterator.state = deque.state;
    iterator.counter = deque.size;
    return object;
}

// dequeiter_next() and dequereviter_next(). Empty at the end, and if it raised.
JSValue nextOfDequeIterator(JSGlobalObject* globalObject, JSCell* self, bool isReverse)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& iterator = stateOf<DequeIteratorState>(self);
    // The one looks first at whether it is over, and the other at whether the deque is as it was.
    if (isReverse && !iterator.counter)
        return { };
    if (dequeOf(iterator.deque.get()).state != iterator.state) {
        iterator.counter = 0;
        return raiseMutated(globalObject, scope);
    }
    if (!iterator.counter)
        return { };
    JSValue item = fieldsOf(iterator.block.get()).data[iterator.index].get();
    --iterator.counter;
    if (isReverse) {
        if (--iterator.index < 0 && iterator.counter > 0) {
            iterator.block.set(vm, self, fieldsOf(iterator.block.get()).leftLink.get());
            iterator.index = blockLength - 1;
        }
    } else if (++iterator.index == blockLength && iterator.counter > 0) {
        iterator.block.set(vm, self, fieldsOf(iterator.block.get()).rightLink.get());
        iterator.index = 0;
    }
    return item;
}
}

PYTHON_NATIVE(dequeIter)
{
    return JSValue::encode(newDequeIterator(globalObject, callFrame->uncheckedArgument(0), unpack<bool>(callFrame, 0)));
}

PYTHON_NATIVE(dequeIteratorNext)
{
    bool isReverse = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue item = nextOfDequeIterator(globalObject, args[0].asCell(), isReverse);
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        STOP();
    return JSValue::encode(item);
}

// _deque_iterator(deque[, index])
PYTHON_NATIVE(dequeIteratorNew)
{
    bool isReverse = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    // PyArg_ParseTuple() with "O!|n", which has no name to go by. What is given by name is not looked at.
    unsigned count = args.size() - 1;
    if (count < 1 || count > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("function takes at "_s, count < 1 ? "least 1"_s : "most 2"_s, " argument"_s, count < 1 ? ""_s : "s"_s, " ("_s, count, " given)"_s)));
    if (!isInstance(globalObject, args[1], collectionsState(globalObject).deque.get()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("argument 1 must be collections.deque, not "_s, typeNameOfArgument(globalObject, args[1]))));
    int64_t index = 0;
    if (count > 1) {
        auto given = toSsize(globalObject, args[2]);
        RETURN_IF_EXCEPTION(scope, { });
        index = *given;
    }
    PyStateObject* iterator = newDequeIterator(globalObject, args[1], isReverse);
    for (int64_t i = 0; i < index; ++i) {
        JSValue item = nextOfDequeIterator(globalObject, iterator, isReverse);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
    }
    return JSValue::encode(iterator);
}

PYTHON_NATIVE(dequeIteratorSelf)
{
    return JSValue::encode(callFrame->uncheckedArgument(0));
}

PYTHON_NATIVE(dequeIteratorLengthHint)
{
    return JSValue::encode(intFromInt64(globalObject, stateOf<DequeIteratorState>(callFrame->uncheckedArgument(0)).counter));
}

PYTHON_NATIVE(dequeIteratorReduce)
{
    JSValue self = callFrame->uncheckedArgument(0);
    auto& iterator = stateOf<DequeIteratorState>(self);
    JSValue arguments = PyTuple::create(globalObject, { iterator.deque.get(), intFromInt64(globalObject, dequeOf(iterator.deque.get()).size - iterator.counter) });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, self)->object(), arguments }));
}

// ---- defaultdict

// It is a dict, and what it has besides is kept where a program cannot get at it but by way of `default_factory`.
static JSValue defaultFactoryOf(VM& vm, JSValue self)
{
    return asObject(self)->getDirect(vm, vm.pythonNames().private_defaultFactory);
}

PYTHON_NATIVE(defaultDictMissing)
{
    NATIVE_PROLOGUE();
    JSValue factory = defaultFactoryOf(vm, args[0]);
    if (!factory || isNone(factory))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
    JSValue value = call(globalObject, factory);
    RETURN_IF_EXCEPTION(scope, { });
    // PyDict_SetDefaultRef(): calling it may have put something there.
    auto* dict = uncheckedDowncast<PyDict>(args[0].asCell());
    JSValue existing = dict->get(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (existing)
        return JSValue::encode(existing);
    dict->set(globalObject, args[1], value);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value);
}

// new_defdict(): the class is called, which only does for a class derived from this if it is called with the same things.
static JSValue newDefaultDict(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    JSValue factory = defaultFactoryOf(globalObject->vm(), self);
    return call(globalObject, typeOf(globalObject, self)->object(), factory ? factory : jsUndefined(), argument);
}

PYTHON_NATIVE(defaultDictCopy)
{
    JSValue self = callFrame->uncheckedArgument(0);
    return JSValue::encode(newDefaultDict(globalObject, self, self));
}

PYTHON_NATIVE(defaultDictReduce)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue factory = defaultFactoryOf(vm, self);
    JSValue arguments = !factory || isNone(factory) ? JSValue(realm->emptyTuple()) : JSValue(PyTuple::create(globalObject, { factory }));
    JSValue items = callMethodNamed(globalObject, self, Identifier::fromString(vm, "items"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIterator(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, self)->object(), arguments, jsUndefined(), jsUndefined(), iterator }));
}

PYTHON_NATIVE(defaultDictRepr)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    // PyDict_Type.tp_repr()
    JSValue base = call(globalObject, realm->typeDict()->lookup(vm, names.dunder_repr), self);
    RETURN_IF_EXCEPTION(scope, { });
    String baseText = stringIn(base)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue factory = defaultFactoryOf(vm, self);
    String factoryText = "None"_s;
    if (factory && factory.isCell()) {
        ReprGuard guard(globalObject, factory.asCell());
        if (guard.isRecursive())
            factoryText = "..."_s;
        else {
            factoryText = repr(globalObject, factory);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else if (factory) {
        factoryText = repr(globalObject, factory);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(typeOf(globalObject, self)->nameWithoutModule(globalObject), '(', factoryText, ", "_s, baseText, ')'))));
}

// __or__ and __ror__
PYTHON_NATIVE(defaultDictOr)
{
    bool isReflected = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue left = isReflected ? args[1] : args[0];
    JSValue right = isReflected ? args[0] : args[1];
    bool leftIsOne = isInstance(globalObject, left, collectionsState(globalObject).defaultDict.get());
    JSValue self = leftIsOne ? left : right;
    JSValue other = leftIsOne ? right : left;
    if (!isDict(other))
        RETURN_NOT_IMPLEMENTED();
    JSValue result = newDefaultDict(globalObject, self, left);
    RETURN_IF_EXCEPTION(scope, { });
    // PyDict_Update()
    if (!isDict(result))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "bad argument to internal function"_s));
    updateDictFrom(globalObject, uncheckedDowncast<PyDict>(result.asCell()), right);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

// defaultdict(default_factory=None, /, [...])
PYTHON_NATIVE(defaultDictInit)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue factory;
    if (args.size() > 1) {
        factory = args[1];
        if (!isCallable(globalObject, factory) && !isNone(factory))
            return JSValue::encode(raiseTypeError(globalObject, scope, "first argument must be callable or None"_s));
    }
    if (factory)
        asObject(self)->putDirect(vm, names.private_defaultFactory, factory);
    else
        asObject(self)->deleteProperty(globalObject, names.private_defaultFactory);
    RETURN_IF_EXCEPTION(scope, { });
    // PyDict_Type.tp_init(), with the rest
    MarkedArgumentBuffer rest;
    rest.append(self);
    ArgList others = args.allFrom(std::min(args.size(), 2u));
    for (unsigned i = 0; i < others.size(); ++i)
        rest.append(others.at(i));
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, realm->typeDict()->lookup(vm, names.dunder_init), rest, args.keywordNames())));
}

// ---- _count_elements(mapping, iterable, /), which is for Counter

PYTHON_NATIVE(collectionsCountElements)
{
    NATIVE_PROLOGUE();
    JSValue mapping = args[0];
    JSValue iterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    Identifier get = Identifier::fromString(vm, "get"_s);
    PyType* type = typeOf(globalObject, mapping);
    PyType* dictType = realm->typeDict();
    JSValue mappingGet = type->lookup(vm, get);
    JSValue mappingSetItem = type->lookup(vm, names.dunder_setitem);
    // If get() and __setitem__() are dict's own, there is no need to call them.
    if (mappingGet && mappingGet == dictType->lookup(vm, get) && mappingSetItem && mappingSetItem == dictType->lookup(vm, names.dunder_setitem) && isDict(mapping)) {
        auto* dict = uncheckedDowncast<PyDict>(mapping.asCell());
        while (true) {
            JSValue key = iteratorNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            if (!key)
                RETURN_NONE();
            // It is asked for its hash on its own account, and not as something to look up.
            hash(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue old = dict->get(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue count = jsNumber(1);
            if (old) {
                count = binaryOperation(globalObject, BinaryOperator::Add, false, old, jsNumber(1));
                RETURN_IF_EXCEPTION(scope, { });
            }
            dict->set(globalObject, key, count);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    JSValue boundGet = getAttribute(globalObject, mapping, get);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        JSValue key = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!key)
            RETURN_NONE();
        JSValue old = call(globalObject, boundGet, key, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue count = binaryOperation(globalObject, BinaryOperator::Add, false, old, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        setItem(globalObject, mapping, key, count);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

// ---- _tuplegetter, which is for namedtuple()

namespace {
struct TupleGetterState final : NativeState {
    PYTHON_NATIVE_STATE(TupleGetterState);
    int64_t index { 0 };
    WriteBarrier<Unknown> doc;
};
template<typename Visitor> void TupleGetterState::visit(Visitor& visitor) { visitor.append(doc); }
}

// _tuplegetter(index, doc, /)
PYTHON_NATIVE(tupleGetterNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "_tuplegetter"_s))
        return { };
    if (args.size() != 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_tuplegetter expected 2 arguments, got "_s, args.size() - 1)));
    auto index = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<TupleGetterState>());
    auto& state = object->state<TupleGetterState>();
    state.index = *index;
    state.doc.set(vm, object, args[2]);
    return JSValue::encode(object);
}

// __get__(instance, owner=None, /)
PYTHON_NATIVE(tupleGetterGet)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue object = args[1];
    // wrap_descr_get()
    if (JSValue owner = args.at(2); isNone(object) && (!owner || isNone(owner)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s));
    int64_t index = stateOf<TupleGetterState>(self).index;
    if (!isTuple(object)) {
        if (isNone(object))
            return JSValue::encode(self);
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("descriptor for index '"_s, index, "' for tuple subclasses doesn't apply to '"_s, typeName(globalObject, object), "' object"_s)));
    }
    PyTuple* tuple = asTuple(object);
    if (!isValidIndex(index, tuple->length()))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "tuple index out of range"_s));
    return JSValue::encode(tuple->at(static_cast<unsigned>(index)));
}

PYTHON_NATIVE(tupleGetterSet)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, unpack<bool>(callFrame, 0) ? "can't delete attribute"_s : "can't set attribute"_s));
}

PYTHON_NATIVE(tupleGetterReduce)
{
    JSValue self = callFrame->uncheckedArgument(0);
    auto& state = stateOf<TupleGetterState>(self);
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, self)->object(), PyTuple::create(globalObject, { intFromInt64(globalObject, state.index), state.doc ? state.doc.get() : jsUndefined() }) }));
}

PYTHON_NATIVE(tupleGetterRepr)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<TupleGetterState>(args[0]);
    String doc = "<NULL>"_s;
    if (state.doc) {
        doc = repr(globalObject, state.doc.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(typeOf(globalObject, args[0])->nameWithoutModule(globalObject), '(', state.index, ", "_s, doc, ')'))));
}

// ---- The module

namespace {

CollectionsState& initializeCollections(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<CollectionsState>();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    state.blockStructure.set(vm, realm, PyStateObject::createStructure(vm, globalObject, jsNull()));
    auto create = [&] (WriteBarrier<PyType>& member, ASCIILiteral name, unsigned flags) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, flags);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        member.set(vm, realm, type);
        return type;
    };

    PyType* deque = create(state.deque, "collections.deque"_s, PyType::IsBaseType | PyType::IsSequence | PyType::HasWeakReferences | PyType::IsSubscriptedAsSequence | PyType::AddsAsSequence);
    addGenericGetAttribute(globalObject, deque);
    addClassGetItemIfGeneric(globalObject, deque);
    addMethods(globalObject, deque, {
        { "__new__"_s, dequeNew, Kind::New, 0, { }, Arguments::AreNotChecked },
        { "__init__"_s, dequeInit, Kind::Wrapper, 0, "(iterable=<unrepresentable>, maxlen=<unrepresentable>)"_s, Arguments::AreThoseOfTheClass },
        { "__repr__"_s, dequeRepr },
        { "__iter__"_s, dequeIter, Kind::Wrapper, pack(false) },
        { "__len__"_s, dequeLength },
        { "__add__"_s, dequeConcat },
        { "__mul__"_s, dequeRepeat, Kind::Wrapper, pack(false) },
        { "__rmul__"_s, dequeRepeat, Kind::Wrapper, pack(false) },
        { "__getitem__"_s, dequeGetItem },
        { "__setitem__"_s, dequeSetItem },
        { "__delitem__"_s, dequeSetItem },
        { "__contains__"_s, dequeContains },
        { "__iadd__"_s, dequeInPlaceConcat },
        { "__imul__"_s, dequeRepeat, Kind::Wrapper, pack(true) },
        { "append"_s, dequeAppend, Kind::Method, pack(false) },
        { "appendleft"_s, dequeAppend, Kind::Method, pack(true) },
        { "clear"_s, dequeClear },
        { "__copy__"_s, dequeCopy },
        { "copy"_s, dequeCopy },
        { "count"_s, dequeCount },
        { "extend"_s, dequeExtend, Kind::Method, pack(false) },
        { "extendleft"_s, dequeExtend, Kind::Method, pack(true) },
        { "index"_s, dequeIndex, Kind::Method, 0, { }, Arguments::AreNotChecked },
        { "insert"_s, dequeInsert },
        { "pop"_s, dequePop, Kind::Method, pack(false) },
        { "popleft"_s, dequePop, Kind::Method, pack(true) },
        { "__reduce__"_s, dequeReduce },
        { "remove"_s, dequeRemove },
        { "__reversed__"_s, dequeIter, Kind::Method, pack(true) },
        { "reverse"_s, dequeReverse },
        { "rotate"_s, dequeRotate },
        { "__sizeof__"_s, dequeSizeOf },
    });
    addComparisons(globalObject, deque, dequeCompare);
    deque->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    addGetSet(globalObject, deque, "maxlen"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        int64_t maxLength = dequeOf(self).maxLength;
        return maxLength < 0 ? jsUndefined() : intFromInt64(globalObject, maxLength);
    });

    PyType* defaultDict = createBuiltinType(globalObject, "collections.defaultdict"_s, realm->typeDict(), realm->typeDict()->layout(), PyType::IsBaseType | PyType::IsMapping | PyType::IsDerivedFromBuiltin);
    state.defaultDict.set(vm, realm, defaultDict);
    addGenericGetAttribute(globalObject, defaultDict);
    addClassGetItemIfGeneric(globalObject, defaultDict);
    addMethods(globalObject, defaultDict, {
        { "__init__"_s, defaultDictInit, Kind::Wrapper, 0, { }, Arguments::AreNotChecked },
        { "__repr__"_s, defaultDictRepr },
        { "__or__"_s, defaultDictOr, Kind::Wrapper, pack(false) },
        { "__ror__"_s, defaultDictOr, Kind::Wrapper, pack(true) },
        { "__missing__"_s, defaultDictMissing },
        { "copy"_s, defaultDictCopy },
        { "__copy__"_s, defaultDictCopy },
        { "__reduce__"_s, defaultDictReduce },
    });
    addMember(globalObject, defaultDict, "default_factory"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue factory = defaultFactoryOf(globalObject->vm(), self);
        return factory ? factory : jsUndefined();
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        if (value)
            asObject(self)->putDirect(vm, vm.pythonNames().private_defaultFactory, value);
        else
            asObject(self)->deleteProperty(globalObject, vm.pythonNames().private_defaultFactory);
    });

    for (bool isReverse : { false, true }) {
        PyType* iterator = create(isReverse ? state.dequeReverseIterator : state.dequeIterator, isReverse ? "collections._deque_reverse_iterator"_s : "collections._deque_iterator"_s, 0);
        addGenericGetAttribute(globalObject, iterator);
        addMethods(globalObject, iterator, {
            { "__new__"_s, dequeIteratorNew, Kind::New, pack(isReverse), { }, Arguments::AreNotChecked },
            { "__iter__"_s, dequeIteratorSelf },
            { "__next__"_s, dequeIteratorNext, Kind::Wrapper, pack(isReverse) },
            { "__length_hint__"_s, dequeIteratorLengthHint },
            { "__reduce__"_s, dequeIteratorReduce },
        });
    }

    PyType* tupleGetter = create(state.tupleGetter, "collections._tuplegetter"_s, 0);
    addMethods(globalObject, tupleGetter, {
        { "__new__"_s, tupleGetterNew, Kind::New, 0, { }, Arguments::AreNotChecked },
        { "__repr__"_s, tupleGetterRepr },
        { "__get__"_s, tupleGetterGet },
        { "__set__"_s, tupleGetterSet, Kind::Wrapper, pack(false) },
        { "__delete__"_s, tupleGetterSet, Kind::Wrapper, pack(true) },
        { "__reduce__"_s, tupleGetterReduce },
    });
    addMember(globalObject, tupleGetter, "__doc__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
        auto& doc = stateOf<TupleGetterState>(self).doc;
        return doc ? doc.get() : jsUndefined();
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        auto& doc = stateOf<TupleGetterState>(self).doc;
        if (value)
            doc.set(globalObject->vm(), self.asCell(), value);
        else
            doc.clear();
    });
    return state;
}

} // anonymous namespace

JSObject* createCollectionsModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& state = collectionsState(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "_collections"_s);
    addFunction(globalObject, module, "_count_elements"_s, collectionsCountElements);
    for (PyType* type : { state.deque.get(), state.defaultDict.get(), state.dequeIterator.get(), state.dequeReverseIterator.get(), state.tupleGetter.get() })
        module->putDirect(vm, Identifier::fromString(vm, type->nameWithoutModule(globalObject)), type);
    return module;
}

} } // namespace JSC::Python
