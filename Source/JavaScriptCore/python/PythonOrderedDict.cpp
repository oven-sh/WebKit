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
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonText.h"

// collections.OrderedDict: Objects/odictobject.c of CPython, function for function.
//
// It is a dict, and beside what a dict has it keeps its keys in a list that is linked both ways, which is the order that it is gone through in. That is not the order of the dict once something has been moved to an end. To find
// where a key is in the list without going through it there is a row of nodes beside the entries of the dict, one for one: the dict is asked which entry has the key, and the node is at the same place.
//
// What is done to one by a method of dict, dict.__setitem__(od, key, value), goes behind the back of all this, here as in CPython, and what comes of it is much the same.

namespace JSC { namespace Python {

namespace {

constexpr uint32_t noNode = UINT32_MAX;

// What PyODictObject has after od_dict, but for the __dict__ and the weak references. It is in a cell of its own, which the dict has under a name that no program can name, and has none until there is something to be in it.
struct OrderedDictOrder final : NativeState {
    PYTHON_NATIVE_STATE(OrderedDictOrder);
    // _ODictNode. A node goes by a number, and what there is to a node is in these at that number. The keys are in a tuple that no program comes by, with nothing at all where there is no node, and get another to grow. The rest is
    // nothing to the collector.
    WriteBarrier<PyTuple> keys;
    Vector<uint32_t> hashes; // As the dict keeps them
    Vector<uint32_t> next; // Of one that is not in use, the next that is not
    Vector<uint32_t> previous;
    uint32_t firstUnused { noNode };

    uint32_t first { noNode }; // od_first
    uint32_t last { noNode }; // od_last
    Vector<uint32_t> fastNodes; // od_fast_nodes: the node for each entry of the dict, and for the one that would come next
    WriteBarrier<PyHashStorage> resizeSentinel; // od_resize_sentinel: what the dict kept its entries in when that was made. They stay where they are for as long as it is the same.
    size_t state { 0 }; // od_state, which goes up whenever the list changes

    bool isEmpty() const { return first == noNode; }
    JSValue key(uint32_t node) const { return keys->at(node); }
};

template<typename Visitor>
void OrderedDictOrder::visit(Visitor& visitor)
{
    visitor.append(keys);
    visitor.append(resizeSentinel);
}

enum IteratorKind : uint8_t {
    IsReversed = 1,
    GivesKeys = 2,
    GivesValues = 4,
    GivesItems = GivesKeys | GivesValues,
};

// odictiterobject
struct OrderedDictIterator final : NativeState {
    PYTHON_NATIVE_STATE(OrderedDictIterator);
    uint8_t gives { 0 }; // kind
    WriteBarrier<PyDict> dict; // di_odict
    int64_t size { 0 };
    size_t state { 0 };
    WriteBarrier<Unknown> current; // The key that comes next, and not the node, in case of what is done meanwhile
};

template<typename Visitor>
void OrderedDictIterator::visit(Visitor& visitor)
{
    visitor.append(dict);
    visitor.append(current);
}

struct OrderedDictState final : NativeState {
    PYTHON_NATIVE_STATE(OrderedDictState);
    WriteBarrier<PyType> type;
    WriteBarrier<PyType> iteratorType;
    WriteBarrier<PyType> keysType;
    WriteBarrier<PyType> valuesType;
    WriteBarrier<PyType> itemsType;
    WriteBarrier<Structure> orderStructure;
};

template<typename Visitor>
void OrderedDictState::visit(Visitor& visitor)
{
    visitor.append(type);
    visitor.append(iteratorType);
    visitor.append(keysType);
    visitor.append(valuesType);
    visitor.append(itemsType);
    visitor.append(orderStructure);
}

OrderedDictState& orderedDictState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<OrderedDictState>(); }

bool isOrderedDict(JSGlobalObject* globalObject, JSValue value) { return isInstance(globalObject, value, orderedDictState(globalObject).type.get()); } // PyODict_Check()
bool isExactlyOrderedDict(JSGlobalObject* globalObject, JSValue value) { return isExactly(globalObject, value, orderedDictState(globalObject).type.get()); } // PyODict_CheckExact()

// Null if nothing has been put in it yet.
OrderedDictOrder* tryOrderOf(VM& vm, PyDict* dict)
{
    JSValue cell = dict->getDirect(vm, vm.pythonNames().private_order);
    return cell ? &stateOf<OrderedDictOrder>(cell) : nullptr;
}

OrderedDictOrder& orderOf(JSGlobalObject* globalObject, PyDict* dict)
{
    VM& vm = globalObject->vm();
    if (auto* order = tryOrderOf(vm, dict))
        return *order;
    auto* cell = PyStateObject::create(vm, orderedDictState(globalObject).orderStructure.get(), makeUnique<OrderedDictOrder>());
    dict->putDirect(vm, vm.pythonNames().private_order, cell);
    return cell->state<OrderedDictOrder>();
}

JSCell* cellOfOrder(VM& vm, PyDict* dict) { return dict->getDirect(vm, vm.pythonNames().private_order).asCell(); }

// PyErr_SetObject(PyExc_KeyError, key), which is what is done here. A dict takes care that a key that is a tuple is not taken for all that the exception is made of, and this does not.
JSValue raiseKeyError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue key) { return raiseMadeOf(globalObject, scope, BuiltinType::KeyError, key); }

JSValue raiseMutated(JSGlobalObject* globalObject, ThrowScope& scope) { return raise(globalObject, scope, BuiltinType::RuntimeError, "OrderedDict mutated during iteration"_s); }

// PyObject_Hash(), as the dict keeps it. It may raise.
uint32_t hashForDict(JSGlobalObject* globalObject, JSValue key) { return PyHashTable::foldHash(hash(globalObject, key)); }

// _PyType_Name(): what comes after the last dot in what the class is called
String shortNameOfType(JSGlobalObject* globalObject, JSValue value)
{
    String name = typeOf(globalObject, value)->nameWithoutModule(globalObject);
    size_t dot = name.reverseFind('.');
    return dot == notFound ? name : name.substring(dot + 1);
}

// ---- The list of keys

// _odict_get_index_raw(): which entry of the dict has the key, or which the next to be added would be. Less than 0 if it raised.
int64_t indexInDictRaw(JSGlobalObject* globalObject, PyDict* dict, JSValue key, uint32_t hash)
{
    PyHashTable& table = dict->ownTable();
    int entry = table.find(globalObject, key, hash);
    if (entry == PyHashTable::notFound)
        return table.entryCount();
    return entry;
}

// _odict_resize(): makes the row of nodes again, to go with the entries of the dict as they now are. It may raise.
void resizeFastNodes(JSGlobalObject* globalObject, PyDict* dict, OrderedDictOrder& order)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyHashStorage* storage = dict->ownTable().storage();
    Vector<uint32_t> fastNodes;
    if (!fastNodes.tryGrow((storage ? storage->capacity() : 0) + 1)) {
        raiseMemoryError(globalObject, scope);
        return;
    }
    fastNodes.fill(noNode);
    size_t state = order.state;
    for (uint32_t node = order.first; node != noNode; node = order.next[node]) {
        int64_t index = indexInDictRaw(globalObject, dict, order.key(node), order.hashes[node]);
        RETURN_IF_EXCEPTION(scope, void());
        // Looking for a key runs nothing if it is the very key that is there, as it is unless something has gone behind the back of this. If it has, anything may have been run, and the nodes may not be what they were.
        if (order.state != state) [[unlikely]] {
            raiseMutated(globalObject, scope);
            return;
        }
        if (static_cast<uint64_t>(index) < fastNodes.size())
            fastNodes[index] = node;
    }
    order.fastNodes = WTF::move(fastNodes);
    order.resizeSentinel.setMayBeNull(vm, cellOfOrder(vm, dict), storage);
}

// _odict_get_index(). Less than 0 if it raised.
int64_t indexInDict(JSGlobalObject* globalObject, PyDict* dict, OrderedDictOrder& order, JSValue key, uint32_t hash)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyHashStorage* storage = dict->ownTable().storage();
    if (order.resizeSentinel.get() != storage || order.fastNodes.size() != (storage ? storage->capacity() : 0) + 1) {
        resizeFastNodes(globalObject, dict, order);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    RELEASE_AND_RETURN(scope, indexInDictRaw(globalObject, dict, key, hash));
}

// What is at a place in the row. Finding the place may have run something that made the row shorter.
uint32_t fastNodeAt(const OrderedDictOrder& order, int64_t index) { return static_cast<uint64_t>(index) < order.fastNodes.size() ? order.fastNodes[index] : noNode; }

// _odict_find_node_hash(): noNode if there is none, or if it raised.
uint32_t findNode(JSGlobalObject* globalObject, PyDict* dict, OrderedDictOrder& order, JSValue key, uint32_t hash)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (order.isEmpty())
        return noNode;
    int64_t index = indexInDict(globalObject, dict, order, key, hash);
    RETURN_IF_EXCEPTION(scope, noNode);
    return fastNodeAt(order, index);
}

// _odict_find_node()
uint32_t findNode(JSGlobalObject* globalObject, PyDict* dict, OrderedDictOrder& order, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (order.isEmpty())
        return noNode;
    uint32_t hash = hashForDict(globalObject, key);
    RETURN_IF_EXCEPTION(scope, noNode);
    RELEASE_AND_RETURN(scope, findNode(globalObject, dict, order, key, hash));
}

// _odict_add_head()
void addAtHead(OrderedDictOrder& order, uint32_t node)
{
    order.previous[node] = noNode;
    order.next[node] = order.first;
    if (order.first == noNode)
        order.last = node;
    else
        order.previous[order.first] = node;
    order.first = node;
    ++order.state;
}

// _odict_add_tail()
void addAtTail(OrderedDictOrder& order, uint32_t node)
{
    order.previous[node] = order.last;
    order.next[node] = noNode;
    if (order.last == noNode)
        order.first = node;
    else
        order.next[order.last] = node;
    order.last = node;
    ++order.state;
}

// A node that is not in use, which there is made room for if there is none. noNode, with MemoryError raised, if there is none to be had.
uint32_t takeUnusedNode(JSGlobalObject* globalObject, PyDict* dict, OrderedDictOrder& order)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (order.firstUnused == noNode) {
        size_t oldCount = order.hashes.size();
        size_t newCount = std::max<size_t>(8, oldCount * 2);
        if (newCount >= noNode) {
            raiseMemoryError(globalObject, scope);
            return noNode;
        }
        PyTuple* keys = PyTuple::tryCreate(globalObject, static_cast<unsigned>(newCount));
        RETURN_IF_EXCEPTION(scope, noNode);
        for (auto& place : keys->span())
            place.clear();
        if (!order.hashes.tryGrow(newCount) || !order.next.tryGrow(newCount) || !order.previous.tryGrow(newCount)) {
            raiseMemoryError(globalObject, scope);
            return noNode;
        }
        for (size_t i = 0; i < oldCount; ++i) {
            if (JSValue key = order.keys->at(i))
                keys->initializeAt(vm, i, key);
        }
        order.keys.set(vm, cellOfOrder(vm, dict), keys);
        for (size_t i = newCount; i-- > oldCount;) {
            order.next[i] = order.firstUnused;
            order.firstUnused = static_cast<uint32_t>(i);
        }
    }
    uint32_t node = order.firstUnused;
    order.firstUnused = order.next[node];
    return node;
}

// _odictnode_DEALLOC()
void releaseNode(OrderedDictOrder& order, uint32_t node)
{
    order.keys->span()[node].clear();
    order.next[node] = order.firstUnused;
    order.firstUnused = node;
}

// _odict_add_new_node(): at the end of the list, unless the key has one already. It may raise.
void addNewNode(JSGlobalObject* globalObject, PyDict* dict, JSValue key, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& order = orderOf(globalObject, dict);
    int64_t index = indexInDict(globalObject, dict, order, key, hash);
    RETURN_IF_EXCEPTION(scope, void());
    if (static_cast<uint64_t>(index) >= order.fastNodes.size()) [[unlikely]] {
        raiseKeyError(globalObject, scope, key);
        return;
    }
    if (order.fastNodes[index] != noNode)
        return;
    uint32_t node = takeUnusedNode(globalObject, dict, order);
    RETURN_IF_EXCEPTION(scope, void());
    order.keys->initializeAt(vm, node, key);
    order.hashes[node] = hash;
    addAtTail(order, node);
    order.fastNodes[index] = node;
}

// _odict_remove_node(). To do it again to the same node does nothing.
void removeNode(OrderedDictOrder& order, uint32_t node)
{
    if (order.first == node)
        order.first = order.next[node];
    else if (order.previous[node] != noNode)
        order.next[order.previous[node]] = order.next[node];

    if (order.last == node)
        order.last = order.previous[node];
    else if (order.next[node] != noNode)
        order.previous[order.next[node]] = order.previous[node];

    order.previous[node] = noNode;
    order.next[node] = noNode;
    ++order.state;
}

// _odict_clear_node(): does away with the node of a key, if it has one. It comes before the key is taken out of the dict, after which there is no asking the dict where it was. It may raise.
void clearNode(JSGlobalObject* globalObject, PyDict* dict, OrderedDictOrder& order, JSValue key, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Whether it is a KeyError is for what comes after to say.
    if (order.isEmpty())
        return;
    int64_t index = indexInDict(globalObject, dict, order, key, hash);
    RETURN_IF_EXCEPTION(scope, void());
    uint32_t node = fastNodeAt(order, index);
    if (node == noNode)
        return;
    order.fastNodes[index] = noNode;
    removeNode(order, node);
    releaseNode(order, node);
}

// _odict_clear_nodes()
void clearNodes(OrderedDictOrder& order)
{
    order.fastNodes.clear();
    order.resizeSentinel.clear();
    order.keys.clear();
    order.hashes.clear();
    order.next.clear();
    order.previous.clear();
    order.firstUnused = noNode;
    order.first = noNode;
    order.last = noNode;
    ++order.state;
}

// _odict_keys_equal(). It may raise.
bool keysAreEqual(JSGlobalObject* globalObject, PyDict* a, PyDict* b)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& orderA = orderOf(globalObject, a);
    auto& orderB = orderOf(globalObject, b);
    // So as to tell if either is changed meanwhile
    size_t stateA = orderA.state;
    size_t stateB = orderB.state;
    uint32_t nodeA = orderA.first;
    uint32_t nodeB = orderB.first;
    while (true) {
        if (nodeA == noNode && nodeB == noNode)
            return true;
        if (nodeA == noNode || nodeB == noNode)
            return false;
        bool areEqual = isEqual(globalObject, orderA.key(nodeA), orderB.key(nodeB));
        RETURN_IF_EXCEPTION(scope, false);
        if (orderA.state != stateA || orderB.state != stateB) {
            raiseMutated(globalObject, scope);
            return false;
        }
        if (!areEqual)
            return false;
        nodeA = orderA.next[nodeA];
        nodeB = orderB.next[nodeB];
    }
}

// ---- Setting and deleting

// _PyDict_DelItem_KnownHash(). It may raise.
void deleteFromDict(JSGlobalObject* globalObject, PyDict* dict, JSValue key, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (dict->backing()) [[unlikely]] {
        JSValue removed = dict->remove(globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
        if (!removed)
            raise(globalObject, scope, BuiltinType::KeyError, key);
        return;
    }
    PyHashTable& table = dict->ownTable();
    int entry = table.find(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, void());
    if (entry == PyHashTable::notFound) {
        raise(globalObject, scope, BuiltinType::KeyError, key);
        return;
    }
    table.removeEntry(vm, entry);
}

// _PyODict_SetItem_KnownHash_LockHeld(). It may raise.
void setItemWithHash(JSGlobalObject* globalObject, PyDict* dict, JSValue key, JSValue value, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (dict->backing()) [[unlikely]]
        dict->set(globalObject, key, value);
    else
        dict->ownTable().addWithHash(globalObject, key, hash, value);
    RETURN_IF_EXCEPTION(scope, void());
    addNewNode(globalObject, dict, key, hash);
    if (scope.exception()) [[unlikely]] {
        // It is taken out of the dict again.
        Exception* raised = takeRaisedException(vm);
        if (!raised)
            return;
        deleteFromDict(globalObject, dict, key, hash);
        scope.release();
        chainRaisedExceptions(globalObject, raised);
    }
}

// PyODict_SetItem()
void orderedDictSetItem(JSGlobalObject* globalObject, PyDict* dict, JSValue key, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    uint32_t hash = hashForDict(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, setItemWithHash(globalObject, dict, key, value, hash));
}

// PyODict_DelItem()
void orderedDictDeleteItem(JSGlobalObject* globalObject, PyDict* dict, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    uint32_t hash = hashForDict(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    clearNode(globalObject, dict, orderOf(globalObject, dict), key, hash);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, deleteFromDict(globalObject, dict, key, hash));
}

// odict_mp_ass_sub()
PYTHON_NATIVE(orderedDictSetItemMethod)
{
    NATIVE_PROLOGUE();
    orderedDictSetItem(globalObject, asDict(args[0]), args[1], args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(orderedDictDeleteItemMethod)
{
    NATIVE_PROLOGUE();
    orderedDictDeleteItem(globalObject, asDict(args[0]), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// ---- update(), which is MutableMapping's

// mutablemapping_add_pairs(). It may raise.
void addPairs(JSGlobalObject* globalObject, JSValue self, JSValue pairs)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue iterator = getIterator(globalObject, pairs);
    RETURN_IF_EXCEPTION(scope, void());
    while (true) {
        JSValue pair = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!pair)
            return;
        JSValue pairIterator = getIterator(globalObject, pair);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue key = iteratorNext(globalObject, pairIterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!key) {
            raiseValueError(globalObject, scope, "need more than 0 values to unpack"_s);
            return;
        }
        JSValue value = iteratorNext(globalObject, pairIterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!value) {
            raiseValueError(globalObject, scope, "need more than 1 value to unpack"_s);
            return;
        }
        JSValue unexpected = iteratorNext(globalObject, pairIterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (unexpected) {
            raiseValueError(globalObject, scope, "too many values to unpack (expected 2)"_s);
            return;
        }
        setItem(globalObject, self, key, value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// PyDict_Items(): a list of what is in it now
JSValue itemsOfDictNow(JSGlobalObject* globalObject, PyDict* dict)
{
    MarkedArgumentBuffer items;
    dict->forEach(globalObject, [&] (JSValue key, JSValue value) {
        items.append(PyTuple::create(globalObject, { key, value }));
        return true;
    });
    return constructArray(globalObject, static_cast<ArrayAllocationProfile*>(nullptr), items);
}

// mutablemapping_update_arg(). It may raise.
void updateFromArgument(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isExactly(globalObject, argument, BuiltinType::Dict)) {
        JSValue items = itemsOfDictNow(globalObject, asDict(argument));
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, addPairs(globalObject, self, items));
    }
    JSValue function = getAttributeIfPresent(globalObject, argument, Identifier::fromString(vm, "keys"_s));
    RETURN_IF_EXCEPTION(scope, void());
    if (function) {
        JSValue keys = call(globalObject, function);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue iterator = getIterator(globalObject, keys);
        RETURN_IF_EXCEPTION(scope, void());
        while (true) {
            JSValue key = iteratorNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, void());
            if (!key)
                return;
            JSValue value = getItem(globalObject, argument, key);
            RETURN_IF_EXCEPTION(scope, void());
            setItem(globalObject, self, key, value);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    function = getAttributeIfPresent(globalObject, argument, Identifier::fromString(vm, "items"_s));
    RETURN_IF_EXCEPTION(scope, void());
    if (function) {
        JSValue items = call(globalObject, function);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, addPairs(globalObject, self, items));
    }
    RELEASE_AND_RETURN(scope, addPairs(globalObject, self, argument));
}

// mutablemapping_update(), of what comes after self. It may raise.
void updateFromArguments(JSGlobalObject* globalObject, const NativeArguments& args)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue self = args[0];
    unsigned given = args.size() - 1;
    if (given > 1) {
        raiseTypeError(globalObject, scope, concatenate("update() takes at most 1 positional argument ("_s, given, " given)"_s));
        return;
    }
    if (given) {
        updateFromArgument(globalObject, self, args[1]);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        setItem(globalObject, self, args.keywordNameAsGiven(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

PYTHON_NATIVE(orderedDictUpdate)
{
    NATIVE_PROLOGUE();
    updateFromArguments(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// odict_init()
PYTHON_NATIVE(orderedDictInit)
{
    NATIVE_PROLOGUE();
    if (args.size() - 1 > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected at most 1 argument, got "_s, args.size() - 1)));
    updateFromArguments(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// odict_or()
PYTHON_NATIVE(orderedDictOr)
{
    NATIVE_PROLOGUE();
    bool isReflected = unpack<bool>(callFrame, 0);
    JSValue left = args[isReflected];
    JSValue right = args[!isReflected];
    bool leftIsOne = isOrderedDict(globalObject, left);
    PyType* type = typeOf(globalObject, leftIsOne ? left : right);
    if (!isDict(leftIsOne ? right : left))
        RETURN_NOT_IMPLEMENTED();
    JSValue result = call(globalObject, type->object(), left);
    RETURN_IF_EXCEPTION(scope, { });
    updateFromArgument(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

// odict_inplace_or()
PYTHON_NATIVE(orderedDictInPlaceOr)
{
    NATIVE_PROLOGUE();
    updateFromArgument(globalObject, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(args[0]);
}

// ---- The methods

// OrderedDict_fromkeys_impl(), which is _PyDict_FromKeys(), of a class that is not dict
PYTHON_NATIVE(orderedDictFromKeys)
{
    NATIVE_PROLOGUE();
    JSValue result = call(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = args.at(2) ? args.at(2) : jsUndefined();
    forEach(globalObject, args[1], [&] (JSValue key) {
        setItem(globalObject, result, key, value);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

// OrderedDict___sizeof___impl()
PYTHON_NATIVE(orderedDictSizeOf)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    JSValue ofDict = call(globalObject, realm->typeDict()->lookup(vm, Identifier::fromString(vm, "__sizeof__"_s)), self);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t size = tryInt64(ofDict).value_or(0);
    if (auto* order = tryOrderOf(vm, self)) {
        // A pointer for each place in the row, which in CPython is as long as the index of the dict, and four words for each node
        if (order->resizeSentinel)
            size += sizeof(void*) * order->resizeSentinel->indexSize();
        if (!order->isEmpty())
            size += 4 * sizeof(void*) * self->size();
    }
    return JSValue::encode(intFromInt64(globalObject, size));
}

// OrderedDict___reduce___impl()
PYTHON_NATIVE(orderedDictReduce)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue items = callMethodNamed(globalObject, self, Identifier::fromString(vm, "items"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIterator(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, self)->object(), realm->emptyTuple(), state, jsUndefined(), iterator }));
}

// OrderedDict_setdefault_impl(), which does not call __missing__()
PYTHON_NATIVE(orderedDictSetDefault)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue key = args[1];
    JSValue defaultValue = args.at(2) ? args.at(2) : jsUndefined();
    if (isExactlyOrderedDict(globalObject, self)) {
        JSValue result = asDict(self)->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return JSValue::encode(result);
        orderedDictSetItem(globalObject, asDict(self), key, defaultValue);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(defaultValue);
    }
    bool exists = contains(globalObject, self, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (exists)
        RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, self, key)));
    setItem(globalObject, self, key, defaultValue);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(defaultValue);
}

// _odict_popkey_hash(). `fallback` may be empty.
JSValue popKeyWithHash(JSGlobalObject* globalObject, PyDict* dict, JSValue key, JSValue fallback, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& order = orderOf(globalObject, dict);
    uint32_t node = findNode(globalObject, dict, order, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    if (node == noNode) {
        if (fallback)
            return fallback;
        return raiseKeyError(globalObject, scope, key);
    }
    // The node goes first, in case of what may be run and of what the dict makes of keys whose hashes collide.
    clearNode(globalObject, dict, order, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    // _PyDict_Pop_KnownHash()
    JSValue value;
    if (dict->backing()) [[unlikely]] {
        value = dict->remove(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        PyHashTable& table = dict->ownTable();
        int entry = table.find(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, { });
        if (entry != PyHashTable::notFound) {
            value = table.valueAt(entry);
            table.removeEntry(vm, entry);
        }
    }
    // In CPython, with no fallback, this is to return nothing with nothing raised.
    if (!value)
        return fallback ? fallback : raiseKeyError(globalObject, scope, key);
    return value;
}

// OrderedDict_pop_impl(), which does not call __missing__()
PYTHON_NATIVE(orderedDictPop)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    uint32_t hash = hashForDict(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(popKeyWithHash(globalObject, self, args[1], args.at(2), hash)));
}

// OrderedDict_popitem_impl()
PYTHON_NATIVE(orderedDictPopItem)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    bool last = true;
    if (JSValue given = args.at(1)) {
        last = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto* order = tryOrderOf(vm, self);
    if (!order || order->isEmpty())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, "dictionary is empty"_s));
    uint32_t node = last ? order->last : order->first;
    JSValue key = order->key(node);
    JSValue value = popKeyWithHash(globalObject, self, key, JSValue(), order->hashes[node]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { key, value }));
}

// OrderedDict_clear_impl()
PYTHON_NATIVE(orderedDictClear)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    self->clear(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* order = tryOrderOf(vm, self))
        clearNodes(*order);
    RETURN_NONE();
}

// OrderedDict_copy_impl()
PYTHON_NATIVE(orderedDictCopy)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    bool isExact = isExactlyOrderedDict(globalObject, self);
    JSValue copy = isExact ? JSValue(PyDict::create(vm, orderedDictState(globalObject).type->instanceStructure())) : call(globalObject, typeOf(globalObject, self)->object());
    RETURN_IF_EXCEPTION(scope, { });
    auto& order = orderOf(globalObject, self);
    // What is run meanwhile may change this one, which is told as __eq__() tells it.
    size_t state = order.state;
    for (uint32_t node = order.first; node != noNode; node = order.next[node]) {
        JSValue key = order.key(node);
        if (isExact) {
            uint32_t hash = order.hashes[node];
            JSValue value = self->get(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            if (!value)
                return JSValue::encode(raiseKeyError(globalObject, scope, key));
            setItemWithHash(globalObject, asDict(copy), key, value, hash);
            RETURN_IF_EXCEPTION(scope, { });
        } else {
            JSValue value = getItem(globalObject, self, key);
            RETURN_IF_EXCEPTION(scope, { });
            setItem(globalObject, copy, key, value);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (order.state != state)
            return JSValue::encode(raiseMutated(globalObject, scope));
    }
    return JSValue::encode(copy);
}

// OrderedDict_move_to_end_impl()
PYTHON_NATIVE(orderedDictMoveToEnd)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    JSValue key = args[1];
    bool last = true;
    if (JSValue given = args.at(2)) {
        last = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto* order = tryOrderOf(vm, self);
    if (!order || order->isEmpty())
        return JSValue::encode(raiseKeyError(globalObject, scope, key));
    uint32_t node = last ? order->last : order->first;
    if (!isIdentical(key, order->key(node))) {
        node = findNode(globalObject, self, *order, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (node == noNode)
            return JSValue::encode(raiseKeyError(globalObject, scope, key));
        // Only if it is not there already
        if (last) {
            if (node != order->last) {
                removeNode(*order, node);
                addAtTail(*order, node);
            }
        } else if (node != order->first) {
            removeNode(*order, node);
            addAtHead(*order, node);
        }
    }
    RETURN_NONE();
}

// odict_repr()
PYTHON_NATIVE(orderedDictRepr)
{
    NATIVE_PROLOGUE();
    PyDict* self = asDict(args[0]);
    String name = shortNameOfType(globalObject, self);
    if (!self->size())
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, "()"_s))));
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return JSValue::encode(jsNontrivialString(vm, "..."_s));
    // PyDict_Copy(), which asks it for its keys and then for what goes with each
    PyDict* copy = PyDict::create(globalObject);
    updateDictFrom(globalObject, copy, self);
    RETURN_IF_EXCEPTION(scope, { });
    String shown = repr(globalObject, copy);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', shown, ')'))));
}

// odict_richcompare()
PYTHON_NATIVE(orderedDictCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    JSValue self = args[0];
    JSValue other = args[1];
    if (!isDict(other) || !isEquality(op))
        RETURN_NOT_IMPLEMENTED();
    // PyDict_Type.tp_richcompare()
    JSValue asDicts = call(globalObject, realm->typeDict()->lookup(vm, op == ComparisonOperator::Eq ? names.dunder_eq : names.dunder_ne), self, other);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isOrderedDict(globalObject, other))
        return JSValue::encode(asDicts);
    if (asDicts == jsBoolean(op != ComparisonOperator::Eq))
        return JSValue::encode(asDicts);
    bool areEqual = keysAreEqual(globalObject, asDict(self), asDict(other));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(areEqual == (op == ComparisonOperator::Eq)));
}

// ---- What goes through one

// odictiter_new()
JSValue newIterator(JSGlobalObject* globalObject, PyDict* dict, uint8_t kind)
{
    VM& vm = globalObject->vm();
    auto* object = PyStateObject::create(vm, orderedDictState(globalObject).iteratorType->instanceStructure(), makeUnique<OrderedDictIterator>());
    auto& self = object->state<OrderedDictIterator>();
    self.gives = kind;
    if (auto* order = tryOrderOf(vm, dict)) {
        uint32_t node = kind & IsReversed ? order->last : order->first;
        if (node != noNode)
            self.current.set(vm, object, order->key(node));
        self.state = order->state;
    }
    self.size = dict->size();
    self.dict.set(vm, object, dict);
    return object;
}

// odictiter_nextkey(). Empty if there are no more, or if it raised.
JSValue nextKey(JSGlobalObject* globalObject, JSCell* owner, OrderedDictIterator& self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = self.dict.get();
    if (!dict)
        return { };
    if (!self.current) {
        self.dict.clear();
        return { };
    }
    auto* order = tryOrderOf(vm, dict);
    if ((order ? order->state : 0) != self.state) {
        self.dict.clear();
        return raiseMutated(globalObject, scope);
    }
    if (self.size != static_cast<int64_t>(dict->size())) {
        self.size = -1; // So that it goes on saying so
        return raise(globalObject, scope, BuiltinType::RuntimeError, "OrderedDict changed size during iteration"_s);
    }

    JSValue key = self.current.get();
    uint32_t node = order ? findNode(globalObject, dict, *order, key) : noNode;
    if (node == noNode) {
        // It must have been deleted.
        self.current.clear();
        RETURN_IF_EXCEPTION(scope, { });
        return raiseKeyError(globalObject, scope, key);
    }
    node = self.gives & IsReversed ? order->previous[node] : order->next[node];
    if (node == noNode)
        self.current.clear();
    else
        self.current.set(vm, owner, order->key(node));
    return key;
}

// odictiter_iternext()
PYTHON_NATIVE(orderedDictIteratorNext)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<OrderedDictIterator>(args[0]);
    JSValue key = nextKey(globalObject, args[0].asCell(), self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!key)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    if (!(self.gives & GivesValues))
        return JSValue::encode(key);

    JSValue value = self.dict->get(globalObject, key);
    if (!value) {
        self.current.clear();
        self.dict.clear();
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseKeyError(globalObject, scope, key));
    }
    if (!(self.gives & GivesKeys))
        return JSValue::encode(value);
    return JSValue::encode(PyTuple::create(globalObject, { key, value }));
}

// odictiter_reduce()
PYTHON_NATIVE(orderedDictIteratorReduce)
{
    NATIVE_PROLOGUE();
    // What is left is got from a copy, so that this one is where it was.
    auto& self = stateOf<OrderedDictIterator>(args[0]);
    auto* object = PyStateObject::create(vm, orderedDictState(globalObject).iteratorType->instanceStructure(), makeUnique<OrderedDictIterator>());
    auto& copy = object->state<OrderedDictIterator>();
    copy.gives = self.gives;
    copy.dict.setMayBeNull(vm, object, self.dict.get());
    copy.size = self.size;
    copy.state = self.state;
    copy.current.set(vm, object, self.current.get());
    JSArray* list = listFromIterable(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iter = getBuiltin(globalObject, "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { list }) }));
}

// odict_iter() and odict_reversed()
PYTHON_NATIVE(orderedDictIter)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newIterator(globalObject, asDict(args[0]), unpack<uint8_t>(callFrame, 0)));
}

// ---- The views

// odictkeys_new(), odictvalues_new() and odictitems_new(): _PyDictView_New()
PYTHON_NATIVE(orderedDictView)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& state = orderedDictState(globalObject);
    PyType* types[] = { state.keysType.get(), state.valuesType.get(), state.itemsType.get() };
    auto* view = PyNativeObject::create(vm, types[unpack<unsigned>(callFrame, 0)]->instanceStructure());
    view->setField(vm, 0, args[0]);
    return JSValue::encode(view);
}

// odictkeys_iter(), odictkeys_reversed() and the like
PYTHON_NATIVE(orderedDictViewIter)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newIterator(globalObject, asDict(uncheckedDowncast<PyNativeObject>(args[0].asCell())->field(0)), unpack<uint8_t>(callFrame, 0)));
}

} // namespace

PyType* initializeOrderedDict(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = orderedDictState(globalObject);
    if (state.type)
        return state.type.get();

    state.orderStructure.set(vm, realm, PyStateObject::createStructure(vm, globalObject, jsNull()));

    PyType* type = createBuiltinType(globalObject, "collections.OrderedDict"_s, realm->typeDict(), realm->typeDict()->layout(), PyType::IsBaseType | PyType::IsMapping | PyType::IsDerivedFromBuiltin);
    state.type.set(vm, realm, type);
    addMethods(globalObject, type, {
        { "__init__"_s, orderedDictInit, Kind::Wrapper, 0, { }, Arguments::AreNotChecked },
        { "__setitem__"_s, orderedDictSetItemMethod },
        { "__delitem__"_s, orderedDictDeleteItemMethod },
        { "__repr__"_s, orderedDictRepr },
        { "__iter__"_s, orderedDictIter, Kind::Wrapper, pack(GivesKeys) },
        { "__reversed__"_s, orderedDictIter, Kind::Method, pack(GivesKeys | IsReversed) },
        { "__or__"_s, orderedDictOr, Kind::Wrapper, pack(false) },
        { "__ror__"_s, orderedDictOr, Kind::Wrapper, pack(true) },
        { "__ior__"_s, orderedDictInPlaceOr },
        { "fromkeys"_s, orderedDictFromKeys, Kind::ClassMethod },
        { "__sizeof__"_s, orderedDictSizeOf },
        { "__reduce__"_s, orderedDictReduce },
        { "setdefault"_s, orderedDictSetDefault },
        { "pop"_s, orderedDictPop },
        { "popitem"_s, orderedDictPopItem },
        { "keys"_s, orderedDictView, Kind::Method, pack(0u) },
        { "values"_s, orderedDictView, Kind::Method, pack(1u) },
        { "items"_s, orderedDictView, Kind::Method, pack(2u) },
        { "update"_s, orderedDictUpdate, Kind::Method, 0, "($self, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
        { "clear"_s, orderedDictClear },
        { "copy"_s, orderedDictCopy },
        { "move_to_end"_s, orderedDictMoveToEnd },
    });
    addComparisons(globalObject, type, orderedDictCompare);
    type->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    addGetSet(globalObject, type, "__dict__"_s, getInstanceDict, setInstanceDictOfBuiltin);

    PyType* iterator = createBuiltinType(globalObject, "odict_iterator"_s, realm->typeObject(), PyType::Layout::Native, 0);
    iterator->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, iterator));
    state.iteratorType.set(vm, realm, iterator);
    addMethods(globalObject, iterator, {
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, orderedDictIteratorNext },
        { "__reduce__"_s, orderedDictIteratorReduce },
    });

    struct View {
        WriteBarrier<PyType>& slot;
        ASCIILiteral name;
        PyType* base;
        uint8_t kind;
    };
    for (auto& view : { View { state.keysType, "odict_keys"_s, realm->typeDictKeys(), GivesKeys }, View { state.valuesType, "odict_values"_s, realm->typeDictValues(), GivesValues }, View { state.itemsType, "odict_items"_s, realm->typeDictItems(), GivesItems } }) {
        PyType* viewType = createBuiltinType(globalObject, view.name, view.base, PyType::Layout::Native, 0);
        viewType->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, viewType));
        view.slot.set(vm, realm, viewType);
        addMethods(globalObject, viewType, {
            { "__iter__"_s, orderedDictViewIter, Kind::Wrapper, pack(view.kind) },
            { "__reversed__"_s, orderedDictViewIter, Kind::Method, pack(view.kind | IsReversed) },
        });
    }
    return type;
}

} } // namespace JSC::Python
