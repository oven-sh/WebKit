/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"

namespace JSC {
namespace Python {

// The module _queue. (Modules/_queuemodule.c in CPython is the reference.)

static constexpr TypeDef queueEmptyTypeDef { "_queue.Empty"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, "Exception raised by Queue.get(block=0)/get_nowait()."_s };

static PyType* queueEmptyType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(queueEmptyTypeDef, rt.Exception());
}

// ---- SimpleQueue ---- (field 0: a list of the items; number: the index of the first one that has not been taken)

static JSValue simpleQueueNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    // A class derived from it may have an __init__ that takes some.
    auto* init = PyNativeFunction::tryFrom(type->lookup(Special::Init));
    if (init && init->ownerType() == rt.objectType()) {
        if (args.size())
            return raiseTypeError(globalObject, scope, "SimpleQueue() takes no positional arguments"_s);
        if (args.keywordCount())
            return raiseTypeError(globalObject, scope, "SimpleQueue() takes no keyword arguments"_s);
    }
    JSArray* items = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* queue = PyRecord::create(globalObject, type, 1);
    queue->set(vm, 0, items);
    return queue;
}

static unsigned simpleQueueSize(PyRecord* queue) { return listLength(asList(queue->get(0))) - static_cast<unsigned>(queue->number); }

// The queue is never full: `block` and `timeout` are there for the sake of queue.Queue.
static JSValue simpleQueuePut(JSGlobalObject* globalObject, JSValue self, JSValue item, JSValue block, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (block) {
        isTrue(globalObject, block);
        RETURN_IF_EXCEPTION(scope, { });
    }
    listAppend(globalObject, asList(PyRecord::from(self)->get(0)), item);
    RETURN_IF_EXCEPTION(scope, { });
    runtime(globalObject).threads().wakeOne(PyRecord::from(self));
    return jsUndefined();
}

static JSValue simpleQueuePutNoWait(JSGlobalObject* globalObject, JSValue self, JSValue item) { return simpleQueuePut(globalObject, self, item, { }, { }); }

static JSValue simpleQueueGet(JSGlobalObject* globalObject, JSValue self, JSValue blockValue, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* queue = PyRecord::from(self);
    bool blocks = !blockValue || isTrue(globalObject, blockValue);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t timeout = 0;
    if (blocks) {
        timeout = -1;
        if (timeoutValue && !isNone(timeoutValue)) {
            if (failed(scope, nanosecondsFromSeconds(globalObject, timeoutValue, TimeRounding::Ceiling, timeout)))
                return { };
            if (timeout < 0)
                return raiseValueError(globalObject, scope, "'timeout' must be a non-negative number"_s);
        }
    }
    bool hasItem = runtime(globalObject).threads().waitUntil(globalObject, queue, timeout, "the queue is empty"_s, "wait for an item at interpreter finalization"_s, [&] { return !!simpleQueueSize(queue); });
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasItem)
        return raiseWithArgs(globalObject, scope, queueEmptyType(globalObject), { });

    JSArray* items = asList(queue->get(0));
    unsigned first = static_cast<unsigned>(queue->number);
    JSValue item = listGet(items, first);
    listSet(globalObject, items, first, jsUndefined());
    ++first;
    // What has been taken is cut off when it has come to be half of the list.
    if (first * 2 >= listLength(items)) {
        listRemove(globalObject, items, 0, first);
        first = 0;
    }
    queue->number = first;
    return item;
}

static JSValue simpleQueueGetNoWait(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount())
        return raiseTypeError(globalObject, scope, "get_nowait() takes no arguments"_s);
    RELEASE_AND_RETURN(scope, simpleQueueGet(globalObject, self, jsBoolean(false), { }));
}

static JSValue simpleQueueEmpty(JSGlobalObject*, JSValue self) { return jsBoolean(!simpleQueueSize(PyRecord::from(self))); }
static JSValue simpleQueueQSize(JSGlobalObject*, JSValue self) { return jsNumber(simpleQueueSize(PyRecord::from(self))); }

static constexpr NativeDef simpleQueueMethods[] = {
    PY_NEW(simpleQueueNew, "($type, *args, **kwargs)"),
    PY_METHOD("empty", simpleQueueEmpty, "($self, /)"),
    PY_WAITING_METHOD("get", simpleQueueGet, "($self, /, block=True, timeout=None)"),
    PY_METHOD("get_nowait", simpleQueueGetNoWait, "!($self, /)"),
    PY_METHOD("put", simpleQueuePut, "($self, /, item, block=True, timeout=None)"),
    PY_METHOD("put_nowait", simpleQueuePutNoWait, "($self, /, item)"),
    PY_METHOD("qsize", simpleQueueQSize, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr TypeDef simpleQueueTypeDef { "_queue.SimpleQueue"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasWeakref, TypeFlag::OwnsContainers }, simpleQueueMethods, { }, { } };

void initializeQueueModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "SimpleQueue"_s, runtime(globalObject).typeFor(simpleQueueTypeDef));
    module->add(globalObject, "Empty"_s, queueEmptyType(globalObject));
}

} // namespace Python
} // namespace JSC
