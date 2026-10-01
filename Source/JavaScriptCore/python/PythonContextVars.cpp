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

#include "InternalFieldTuple.h"
#include "JSCellButterfly.h"
#include "PythonContextVars.h"
#include <wtf/MathExtras.h>

// _contextvars (PEP 567): Python/context.c of CPython.
//
// What the variables have is where JavaScript keeps the like: JSGlobalObject::m_asyncContextData, which whatever puts something off takes note of, a promise's reaction or a timer, and puts
// back for as long as that runs. See AsyncContextSwapScope. So what a variable of Python's has follows what is being done from one language to the other and across `await`, as what an
// AsyncLocalStorage has does, and by the same means.
//
// What is kept there is a chain of frames, each of which binds something to a value: an object with the properties `storage`, `value`, `prev` and `masked`. The embedder's AsyncLocalStorage makes
// them, and so does whatever else has something to keep there, with no prototype. What goes through them looks for its own `storage` and passes over the rest, and copies what it has to as it is.
// Python has one frame, whose `storage` is the class ContextVar and whose `value` is a map from variables to what they have. The map is never changed: setting a variable makes another, and
// another frame. So taking note of how things are is taking note of one pointer, JavaScript has at most one frame more to pass over however many variables there are, and entering a Context and
// leaving it changes Python's frame and no other, as AsyncLocalStorage.run() changes only its own.

namespace JSC { namespace Python {

// ---- The map
//
// A hash array mapped trie, as in CPython's Python/hamt.c, so that to make one that differs in one place from another is to make what is on the way to that place, and share the rest. Keys are
// variables, which are the same only if they are the one object, so nothing here runs a program's code.
//
// A node is an array that is never changed. The first thing in it is a bitmap, and after it come pairs, one for each bit that is set: five bits of the hash choose the bit. A pair is a variable and what
// it has, or null and the node for what has those five bits alike, which goes by the next five. If the first thing is null, what comes after is pairs of variables that have the whole hash alike.
//
// A map is an InternalFieldTuple of how many there are and the first node. It is an object because it is where JavaScript can come by it, though it can make nothing of it. There is none with nothing in it.

namespace {

using TrieNode = JSCellButterfly;
using VariableMap = InternalFieldTuple;

constexpr unsigned bitsPerLevel = 5;

enum VariableField : unsigned { VariableName, VariableDefault, VariableHash };

uint32_t hashOf(JSValue variable) { return static_cast<uint32_t>(asNativeObject(variable)->field(VariableHash).asInt32()); }
uint32_t bitFor(uint32_t hash, unsigned shift) { return 1u << ((hash >> shift) & 31); }
bool isCollision(TrieNode* node) { return node->get(0).isNull(); }
uint32_t bitmapOf(TrieNode* node) { return static_cast<uint32_t>(node->get(0).asInt32()); }
unsigned pairCount(TrieNode* node) { return (node->length() - 1) / 2; }
JSValue keyAt(TrieNode* node, unsigned pair) { return node->get(1 + 2 * pair); }
JSValue valueAt(TrieNode* node, unsigned pair) { return node->get(2 + 2 * pair); }
TrieNode* childAt(TrieNode* node, unsigned pair) { return uncheckedDowncast<TrieNode>(valueAt(node, pair).asCell()); }
unsigned pairFor(uint32_t bitmap, uint32_t bit) { return std::popcount(bitmap & (bit - 1)); }

TrieNode* newNode(VM& vm, JSValue head, unsigned pairs)
{
    TrieNode* node = TrieNode::create(vm, CopyOnWriteArrayWithContiguous, 1 + 2 * pairs);
    node->setIndex(vm, 0, head);
    return node;
}

JSValue bitmapValue(uint32_t bitmap) { return jsNumber(static_cast<int32_t>(bitmap)); }

void setPair(VM& vm, TrieNode* node, unsigned pair, JSValue key, JSValue value)
{
    node->setIndex(vm, 1 + 2 * pair, key);
    node->setIndex(vm, 2 + 2 * pair, value);
}

// Another like it, with a pair put in its place, put in before it, or left out.
enum class Change : uint8_t { Replace, Insert, Remove };

TrieNode* copyWith(VM& vm, TrieNode* node, JSValue head, Change change, unsigned at, JSValue key = { }, JSValue value = { })
{
    unsigned count = pairCount(node);
    TrieNode* result = newNode(vm, head, change == Change::Insert ? count + 1 : change == Change::Remove ? count - 1 : count);
    unsigned to = 0;
    for (unsigned from = 0; from < count; ++from) {
        if (from == at) {
            if (change == Change::Remove)
                continue;
            setPair(vm, result, to++, key, value);
            if (change == Change::Replace)
                continue;
        }
        setPair(vm, result, to++, keyAt(node, from), valueAt(node, from));
    }
    if (change == Change::Insert && at == count)
        setPair(vm, result, to, key, value);
    return result;
}

JSValue find(TrieNode* node, uint32_t hash, JSValue key)
{
    for (unsigned shift = 0;; shift += bitsPerLevel) {
        if (isCollision(node)) {
            for (unsigned pair = 0; pair < pairCount(node); ++pair) {
                if (keyAt(node, pair) == key)
                    return valueAt(node, pair);
            }
            return { };
        }
        uint32_t bit = bitFor(hash, shift);
        if (!(bitmapOf(node) & bit))
            return { };
        unsigned pair = pairFor(bitmapOf(node), bit);
        if (!keyAt(node, pair).isNull())
            return keyAt(node, pair) == key ? valueAt(node, pair) : JSValue();
        node = childAt(node, pair);
    }
}

TrieNode* assoc(VM&, TrieNode*, unsigned shift, uint32_t hash, JSValue key, JSValue value, bool& wasAdded);

// A node for two that have been alike so far.
TrieNode* nodeForTwo(VM& vm, unsigned shift, JSValue firstKey, JSValue firstValue, uint32_t hash, JSValue key, JSValue value)
{
    if (hashOf(firstKey) == hash) {
        TrieNode* node = newNode(vm, jsNull(), 2);
        setPair(vm, node, 0, firstKey, firstValue);
        setPair(vm, node, 1, key, value);
        return node;
    }
    TrieNode* node = newNode(vm, bitmapValue(bitFor(hashOf(firstKey), shift)), 1);
    setPair(vm, node, 0, firstKey, firstValue);
    bool wasAdded;
    return assoc(vm, node, shift, hash, key, value, wasAdded);
}

TrieNode* assoc(VM& vm, TrieNode* node, unsigned shift, uint32_t hash, JSValue key, JSValue value, bool& wasAdded)
{
    wasAdded = false;
    if (isCollision(node)) {
        uint32_t theirHash = hashOf(keyAt(node, 0));
        if (theirHash != hash) {
            // They go one further down, to make room.
            TrieNode* parent = newNode(vm, bitmapValue(bitFor(theirHash, shift)), 1);
            setPair(vm, parent, 0, jsNull(), node);
            return assoc(vm, parent, shift, hash, key, value, wasAdded);
        }
        for (unsigned pair = 0; pair < pairCount(node); ++pair) {
            if (keyAt(node, pair) == key)
                return copyWith(vm, node, jsNull(), Change::Replace, pair, key, value);
        }
        wasAdded = true;
        return copyWith(vm, node, jsNull(), Change::Insert, pairCount(node), key, value);
    }

    uint32_t bitmap = bitmapOf(node);
    uint32_t bit = bitFor(hash, shift);
    unsigned pair = pairFor(bitmap, bit);
    if (!(bitmap & bit)) {
        wasAdded = true;
        return copyWith(vm, node, bitmapValue(bitmap | bit), Change::Insert, pair, key, value);
    }
    if (keyAt(node, pair).isNull())
        return copyWith(vm, node, node->get(0), Change::Replace, pair, jsNull(), assoc(vm, childAt(node, pair), shift + bitsPerLevel, hash, key, value, wasAdded));
    if (keyAt(node, pair) == key)
        return copyWith(vm, node, node->get(0), Change::Replace, pair, key, value);
    wasAdded = true;
    return copyWith(vm, node, node->get(0), Change::Replace, pair, jsNull(), nodeForTwo(vm, shift + bitsPerLevel, keyAt(node, pair), valueAt(node, pair), hash, key, value));
}

// Null if there is nothing left. The node itself if the key is not in it.
TrieNode* without(VM& vm, TrieNode* node, unsigned shift, uint32_t hash, JSValue key)
{
    if (isCollision(node)) {
        for (unsigned pair = 0; pair < pairCount(node); ++pair) {
            if (keyAt(node, pair) != key)
                continue;
            if (pairCount(node) > 2)
                return copyWith(vm, node, jsNull(), Change::Remove, pair);
            // One is left, and collides with nothing.
            TrieNode* result = newNode(vm, bitmapValue(bitFor(hash, shift)), 1);
            setPair(vm, result, 0, keyAt(node, !pair), valueAt(node, !pair));
            return result;
        }
        return node;
    }

    uint32_t bitmap = bitmapOf(node);
    uint32_t bit = bitFor(hash, shift);
    if (!(bitmap & bit))
        return node;
    unsigned pair = pairFor(bitmap, bit);
    auto remove = [&] () -> TrieNode* {
        return pairCount(node) == 1 ? nullptr : copyWith(vm, node, bitmapValue(bitmap & ~bit), Change::Remove, pair);
    };
    if (!keyAt(node, pair).isNull())
        return keyAt(node, pair) == key ? remove() : node;

    TrieNode* child = childAt(node, pair);
    TrieNode* result = without(vm, child, shift + bitsPerLevel, hash, key);
    if (result == child)
        return node;
    if (!result)
        return remove();
    // A node with one variable in it and nothing else need not be there.
    if (!isCollision(result) && pairCount(result) == 1 && !keyAt(result, 0).isNull())
        return copyWith(vm, node, node->get(0), Change::Replace, pair, keyAt(result, 0), valueAt(result, 0));
    return copyWith(vm, node, node->get(0), Change::Replace, pair, jsNull(), result);
}

template<typename Function>
bool forEachIn(TrieNode* node, const Function& function)
{
    for (unsigned pair = 0; pair < pairCount(node); ++pair) {
        if (!(keyAt(node, pair).isNull() ? forEachIn(childAt(node, pair), function) : function(keyAt(node, pair), valueAt(node, pair))))
            return false;
    }
    return true;
}

unsigned countOf(VariableMap* map) { return map ? map->getInternalField(0).asInt32() : 0; }
TrieNode* rootOf(VariableMap* map) { return uncheckedDowncast<TrieNode>(map->getInternalField(1).asCell()); }
JSValue find(VariableMap* map, JSValue variable) { return map ? find(rootOf(map), hashOf(variable), variable) : JSValue(); }

VariableMap* newMap(JSGlobalObject* globalObject, unsigned count, TrieNode* root)
{
    return VariableMap::create(globalObject->vm(), globalObject->internalFieldTupleStructure(), JSC::jsNumber(count), root);
}

VariableMap* mapWith(JSGlobalObject* globalObject, VariableMap* map, JSValue variable, JSValue value)
{
    VM& vm = globalObject->vm();
    if (!map) {
        TrieNode* root = newNode(vm, bitmapValue(bitFor(hashOf(variable), 0)), 1);
        setPair(vm, root, 0, variable, value);
        return newMap(globalObject, 1, root);
    }
    bool wasAdded;
    TrieNode* root = assoc(vm, rootOf(map), 0, hashOf(variable), variable, value, wasAdded);
    return newMap(globalObject, countOf(map) + wasAdded, root);
}

VariableMap* mapWithout(JSGlobalObject* globalObject, VariableMap* map, JSValue variable)
{
    if (!map)
        return nullptr;
    TrieNode* root = without(globalObject->vm(), rootOf(map), 0, hashOf(variable), variable);
    if (root == rootOf(map))
        return map;
    return root ? newMap(globalObject, countOf(map) - 1, root) : nullptr;
}

template<typename Function>
bool forEachIn(VariableMap* map, const Function& function) { return !map || forEachIn(rootOf(map), function); }

// ---- The frames
//
// find(), push(), without() and copyUntil() of the embedder's AsyncLocalStorage, for Python's own frame. `masked` is what it has been told to make nothing of from a frame on, which is never Python's,
// and goes with the frame that is first.

enum FrameOffset : PropertyOffset { FrameStorage, FrameValue, FramePrevious, FrameMasked, numberOfFrameProperties };

struct FrameNames {
    explicit FrameNames(VM& vm)
        : storage(Identifier::fromString(vm, "storage"_s))
        , value(vm.propertyNames->value)
        , previous(Identifier::fromString(vm, "prev"_s))
        , masked(Identifier::fromString(vm, "masked"_s))
    {
    }

    Identifier storage;
    Identifier value;
    Identifier previous;
    Identifier masked;
};

JSValue property(VM& vm, JSObject* frame, const Identifier& name)
{
    JSValue value = frame->getDirect(vm, name);
    return value ? value : jsUndefined();
}

JSValue currentFrame(JSGlobalObject* globalObject) { return globalObject->m_asyncContextData->getInternalField(0); }

JSObject* newFrame(JSGlobalObject* globalObject, JSValue storage, JSValue value, JSValue previous, JSValue masked)
{
    VM& vm = globalObject->vm();
    JSObject* frame = constructEmptyObject(vm, globalObject->pyRealm()->asyncContextFrameStructure());
    frame->putDirectOffset(vm, FrameStorage, storage);
    frame->putDirectOffset(vm, FrameValue, value);
    frame->putDirectOffset(vm, FramePrevious, previous);
    frame->putDirectOffset(vm, FrameMasked, masked);
    return frame;
}

JSObject* findFrame(JSGlobalObject* globalObject, const FrameNames& names, JSValue head)
{
    VM& vm = globalObject->vm();
    JSValue storage = globalObject->pyRealm()->typeContextVar();
    for (JSValue frame = head; frame.isObject(); frame = property(vm, asObject(frame), names.previous)) {
        if (property(vm, asObject(frame), names.storage) == storage)
            return asObject(frame);
    }
    return nullptr;
}

// The chain without Python's frame.
JSValue withoutFrame(JSGlobalObject* globalObject, const FrameNames& names, JSValue head)
{
    VM& vm = globalObject->vm();
    JSObject* found = findFrame(globalObject, names, head);
    if (!found)
        return head;
    JSValue tail = property(vm, found, names.previous);
    if (found == head.asCell()) {
        // What was to be made nothing of from the first frame on still is.
        JSValue masked = property(vm, found, names.masked);
        if (!tail.isObject() || property(vm, asObject(tail), names.masked) == masked)
            return tail;
        JSObject* next = asObject(tail);
        return newFrame(globalObject, property(vm, next, names.storage), property(vm, next, names.value), property(vm, next, names.previous), masked);
    }
    Vector<JSObject*, 8> above;
    for (JSObject* frame = asObject(head); frame != found; frame = asObject(property(vm, frame, names.previous)))
        above.append(frame);
    for (JSObject* frame : above | std::views::reverse)
        tail = newFrame(globalObject, property(vm, frame, names.storage), property(vm, frame, names.value), tail, property(vm, frame, names.masked));
    return tail;
}

VariableMap* currentMap(JSGlobalObject* globalObject)
{
    JSValue head = currentFrame(globalObject);
    if (!head.isObject())
        return nullptr;
    VM& vm = globalObject->vm();
    // It is nearly always the first, having been put there last.
    if (asObject(head)->structure() == globalObject->pyRealm()->asyncContextFrameStructure() && asObject(head)->getDirect(FrameStorage) == JSValue(globalObject->pyRealm()->typeContextVar()))
        return uncheckedDowncast<VariableMap>(asObject(head)->getDirect(FrameValue).asCell());
    FrameNames names(vm);
    JSObject* frame = findFrame(globalObject, names, head);
    return frame ? uncheckedDowncast<VariableMap>(property(vm, frame, names.value).asCell()) : nullptr;
}

void setCurrentMap(JSGlobalObject* globalObject, VariableMap* map)
{
    VM& vm = globalObject->vm();
    FrameNames names(vm);
    JSValue rest = withoutFrame(globalObject, names, currentFrame(globalObject));
    if (map) {
        // From now on there is something for what is put off to take note of.
        vm.setAsyncContextTrackingEnabled();
        rest = newFrame(globalObject, globalObject->pyRealm()->typeContextVar(), map, rest, rest.isObject() ? property(vm, asObject(rest), names.masked) : jsUndefined());
    }
    globalObject->m_asyncContextData->putInternalField(vm, 0, rest);
}

// ---- Context

enum ContextField : unsigned {
    ContextMap, // What the variables have in it, or None.
    ContextPrevious, // While it is entered, the one that was entered before it, or None.
    ContextIsEntered,
    ContextOuterMap, // While it is entered, what the variables had outside, or None.
};

bool isContext(JSGlobalObject* globalObject, JSValue value) { return typeOf(globalObject, value) == globalObject->pyRealm()->typeContext(); }
bool isVariable(JSGlobalObject* globalObject, JSValue value) { return typeOf(globalObject, value) == globalObject->pyRealm()->typeContextVar(); }
JSValue orNone(VariableMap* map) { return map ? JSValue(map) : jsUndefined(); }
VariableMap* asMap(JSValue value) { return isNone(value) ? nullptr : uncheckedDowncast<VariableMap>(value.asCell()); }
VariableMap* mapOf(JSValue context) { return asMap(asNativeObject(context)->field(ContextMap)); }

PyNativeObject* newContext(JSGlobalObject* globalObject, VariableMap* map)
{
    return PyNativeObject::create(globalObject, BuiltinType::Context, orNone(map), jsUndefined(), jsBoolean(false), jsUndefined());
}

// What the variables have from now on, in whatever is being done. It is what they have in the context that has been entered, too, unless what is being done is something that was put off and has
// come round while that is entered, which has nothing to do with it.
void changeCurrentMap(JSGlobalObject* globalObject, VariableMap* from, VariableMap* to)
{
    VM& vm = globalObject->vm();
    setCurrentMap(globalObject, to);
    if (JSObject* context = globalObject->pyRealm()->currentContext(); context && mapOf(context) == from)
        asNativeObject(context)->setField(vm, ContextMap, orNone(to));
}

} // anonymous namespace

Structure* createAsyncContextFrameStructure(VM& vm, JSGlobalObject* globalObject)
{
    FrameNames names(vm);
    Structure* structure = JSFinalObject::createStructure(vm, globalObject, jsNull(), numberOfFrameProperties);
    PropertyOffset offset;
    for (const Identifier* name : { &names.storage, &names.value, &names.previous, &names.masked })
        structure = Structure::addPropertyTransition(vm, structure, *name, 0, offset);
    RELEASE_ASSERT(offset == FrameMasked);
    return structure;
}

// _PyContext_Enter()
bool enterContext(JSGlobalObject* globalObject, PyNativeObject* context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (context->field(ContextIsEntered).isTrue()) {
        String shown = repr(globalObject, context);
        RETURN_IF_EXCEPTION(scope, false);
        raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("cannot enter context: "_s, shown, " is already entered"_s));
        return false;
    }
    context->setField(vm, ContextPrevious, realm->currentContext() ? JSValue(realm->currentContext()) : jsUndefined());
    context->setField(vm, ContextIsEntered, jsBoolean(true));
    context->setField(vm, ContextOuterMap, orNone(currentMap(globalObject)));
    realm->setCurrentContext(vm, context);
    setCurrentMap(globalObject, mapOf(context));
    return true;
}

// _PyContext_Exit(). It is for what entered it, when whatever it entered it for is over.
void exitContext(JSGlobalObject* globalObject, PyNativeObject* context)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    ASSERT(context->field(ContextIsEntered).isTrue() && realm->currentContext() == context);
    JSValue previous = context->field(ContextPrevious);
    realm->setCurrentContext(vm, isNone(previous) ? nullptr : asObject(previous));
    setCurrentMap(globalObject, asMap(context->field(ContextOuterMap)));
    context->setField(vm, ContextPrevious, jsUndefined());
    context->setField(vm, ContextIsEntered, jsBoolean(false));
    context->setField(vm, ContextOuterMap, jsUndefined());
}

// PyContext_CopyCurrent()
PyNativeObject* copyCurrentContext(JSGlobalObject* globalObject)
{
    return newContext(globalObject, currentMap(globalObject));
}

PYTHON_NATIVE(contextNew)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1 || args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "Context() does not accept any arguments"_s));
    return JSValue::encode(newContext(globalObject, nullptr));
}

// context_check_key_type()
static bool checkKey(JSGlobalObject* globalObject, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isVariable(globalObject, key))
        return true;
    String shown = repr(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    raiseTypeError(globalObject, scope, concatenate("a ContextVar key was expected, got "_s, shown));
    return false;
}

PYTHON_NATIVE(contextLength)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(intFromUInt64(globalObject, countOf(mapOf(args[0]))));
}

PYTHON_NATIVE(contextGetItem)
{
    NATIVE_PROLOGUE();
    if (!checkKey(globalObject, args[1]))
        return { };
    if (JSValue value = find(mapOf(args[0]), args[1]))
        return JSValue::encode(value);
    raiseObject(globalObject, scope, createException(globalObject, realm->typeKeyError(), args[1]));
    return { };
}

PYTHON_NATIVE(contextContains)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (!checkKey(globalObject, args[1]))
        return { };
    return JSValue::encode(jsBoolean(!!find(mapOf(args[0]), args[1])));
}

PYTHON_NATIVE(contextGet)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (!checkKey(globalObject, args[1]))
        return { };
    if (JSValue value = find(mapOf(args[0]), args[1]))
        return JSValue::encode(value);
    return JSValue::encode(args.size() > 2 ? args[2] : jsUndefined());
}

// keys(), values() and items(), and iter(). What they give goes through the map as it was, and can say how many there are.
PYTHON_NATIVE(contextView)
{
    auto kind = unpack<PyIterator::Kind>(callFrame, 0);
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MarkedArgumentBuffer items;
    forEachIn(mapOf(args[0]), [&] (JSValue key, JSValue value) {
        items.append(kind == PyIterator::Kind::ContextKeys ? key : kind == PyIterator::Kind::ContextValues ? value : JSValue(PyTuple::create(globalObject, { key, value })));
        return true;
    });
    // How many there are is kept apart, since it is still to be had when they have all been given, and what they were in has been let go of.
    return JSValue::encode(PyIterator::create(globalObject, kind, PyTuple::createFromArguments(globalObject, items), JSValue(), 0, items.size()));
}

PYTHON_NATIVE(contextViewLength)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(intFromInt64(globalObject, uncheckedDowncast<PyIterator>(args[0].asCell())->stop()));
}

PYTHON_NATIVE(contextCopy)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newContext(globalObject, mapOf(args[0])));
}

// context_tp_richcompare(), and _PyHamt_Eq()
PYTHON_NATIVE(contextCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!isContext(globalObject, args[1]) || !isEquality(op))
        RETURN_NOT_IMPLEMENTED();
    VariableMap* first = mapOf(args[0]);
    VariableMap* second = mapOf(args[1]);
    bool isSame = first == second;
    if (!isSame && countOf(first) == countOf(second)) {
        isSame = forEachIn(first, [&] (JSValue key, JSValue value) {
            JSValue other = find(second, key);
            return other && isEqual(globalObject, value, other) && !scope.exception();
        });
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(jsBoolean(isSame == (op == ComparisonOperator::Eq)));
}

// context_run()
PYTHON_NATIVE(contextRun)
{
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "run() missing 1 required positional argument"_s));
    PyNativeObject* context = asNativeObject(args[0]);
    MarkedArgumentBuffer arguments;
    for (unsigned i = 2; i < args.size(); ++i)
        arguments.append(args[i]);
    for (unsigned i = 0; i < args.keywordCount(); ++i)
        arguments.append(args.keywordValue(i));
    if (!enterContext(globalObject, context))
        return { };
    JSValue result = callWithKeywords(globalObject, args[1], arguments, args.keywordNames());
    exitContext(globalObject, context);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(copyContext)
{
    return JSValue::encode(copyCurrentContext(globalObject));
}

// ---- ContextVar

enum TokenField : unsigned {
    TokenContext, // The context that was entered when it was made, or None.
    TokenVariable,
    TokenOldValue, // Empty if it had none.
    TokenIsUsed,
};

// contextvar_tp_new()
PYTHON_NATIVE(variableNew)
{
    NATIVE_PROLOGUE();
    // PyArg_ParseTupleAndKeywords(args, kwds, "O|$O:ContextVar", { "", "default" }), which has words of its own for what is wrong.
    unsigned positional = args.size() - 1;
    unsigned given = positional + args.keywordCount();
    if (given > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("ContextVar() takes at most 2 "_s, positional ? ""_s : "keyword "_s, "arguments ("_s, given, " given)"_s)));
    if (positional != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("ContextVar() takes "_s, positional ? "at most"_s : "exactly"_s, " 1 positional argument ("_s, positional, " given)"_s)));
    JSValue defaultValue;
    if (args.keywordCount()) {
        String keyword = args.keywordName(0)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (keyword != "default"_s)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("ContextVar() got an unexpected keyword argument '"_s, keyword, '\'')));
        defaultValue = args.keywordValue(0);
    }
    JSValue name = args[1];
    if (!isInstance(globalObject, name, realm->typeStr()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "context variable name must be a str"_s));
    // contextvar_generate_hash(), which makes the hash of the variable out of that of its name
    hash(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::ContextVar, name, defaultValue, jsNumber(static_cast<int32_t>(globalObject->weakRandomInteger()))));
}

// PyContextVar_New(), with no default
JSObject* newContextVariable(JSGlobalObject* globalObject, JSString* name)
{
    return PyNativeObject::create(globalObject, BuiltinType::ContextVar, name, JSValue(), jsNumber(static_cast<int32_t>(globalObject->weakRandomInteger())));
}

// PyContextVar_Get(), with no default. Empty if it has nothing.
JSValue contextVariableValue(JSGlobalObject* globalObject, JSObject* variable)
{
    if (JSValue value = find(currentMap(globalObject), variable))
        return value;
    return asNativeObject(variable)->field(VariableDefault);
}

void setContextVariableValue(JSGlobalObject* globalObject, JSObject* variable, JSValue value)
{
    VariableMap* map = currentMap(globalObject);
    changeCurrentMap(globalObject, map, mapWith(globalObject, map, variable, value));
}

PYTHON_NATIVE(variableHash)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    // -1 is how CPython says that something went wrong, and nothing has it for a hash.
    int32_t hash = asNativeObject(args[0])->field(VariableHash).asInt32();
    return JSValue::encode(jsNumber(hash == -1 ? -2 : hash));
}

PYTHON_NATIVE(variableRepr)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNativeObject(args[0]);
    String name = repr(globalObject, self->field(VariableName));
    RETURN_IF_EXCEPTION(scope, { });
    String defaultValue = emptyString();
    if (JSValue value = self->field(VariableDefault)) {
        defaultValue = concatenate(" default="_s, repr(globalObject, value));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<ContextVar name="_s, name, defaultValue, " at "_s, addressOf(self), '>'))));
}

// PyContextVar_Get()
PYTHON_NATIVE(variableGet)
{
    NATIVE_PROLOGUE();
    if (JSValue value = find(currentMap(globalObject), args[0]))
        return JSValue::encode(value);
    if (args.size() > 1)
        return JSValue::encode(args[1]);
    if (JSValue value = asNativeObject(args[0])->field(VariableDefault))
        return JSValue::encode(value);
    raiseObject(globalObject, scope, createException(globalObject, realm->typeLookupError(), args[0]));
    return { };
}

// PyContextVar_Set()
PYTHON_NATIVE(variableSet)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    VariableMap* map = currentMap(globalObject);
    JSValue context = realm->currentContext() ? JSValue(realm->currentContext()) : jsUndefined();
    PyNativeObject* token = PyNativeObject::create(globalObject, BuiltinType::Token, context, args[0], find(map, args[0]), jsBoolean(false));
    changeCurrentMap(globalObject, map, mapWith(globalObject, map, args[0], args[1]));
    return JSValue::encode(token);
}

// PyContextVar_Reset()
static bool resetVariable(JSGlobalObject* globalObject, JSValue variable, PyNativeObject* token)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto complain = [&] (BuiltinType type, ASCIILiteral what) {
        String shown = repr(globalObject, token);
        RETURN_IF_EXCEPTION(scope, false);
        raise(globalObject, scope, type, concatenate(shown, what));
        return false;
    };
    if (token->field(TokenIsUsed).isTrue())
        return complain(BuiltinType::RuntimeError, " has already been used once"_s);
    if (token->field(TokenVariable) != variable)
        return complain(BuiltinType::ValueError, " was created by a different ContextVar"_s);
    if (token->field(TokenContext) != (realm->currentContext() ? JSValue(realm->currentContext()) : jsUndefined()))
        return complain(BuiltinType::ValueError, " was created in a different Context"_s);
    token->setField(vm, TokenIsUsed, jsBoolean(true));
    VariableMap* map = currentMap(globalObject);
    JSValue oldValue = token->field(TokenOldValue);
    changeCurrentMap(globalObject, map, oldValue ? mapWith(globalObject, map, variable, oldValue) : mapWithout(globalObject, map, variable));
    return true;
}

PYTHON_NATIVE(variableReset)
{
    NATIVE_PROLOGUE();
    if (typeOf(globalObject, args[1]) != realm->typeToken()) {
        String shown = repr(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected an instance of Token, got "_s, shown)));
    }
    scope.release();
    resetVariable(globalObject, args[0], asNativeObject(args[1]));
    RETURN_NONE();
}

// ---- Token

PYTHON_NATIVE(tokenNew)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "Tokens can only be created by ContextVars"_s));
}

PYTHON_NATIVE(tokenRepr)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNativeObject(args[0]);
    String variable = repr(globalObject, self->field(TokenVariable));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<Token"_s, self->field(TokenIsUsed).isTrue() ? " used"_s : ""_s, " var="_s, variable, " at "_s, addressOf(self), '>'))));
}

PYTHON_NATIVE(tokenExit)
{
    NATIVE_PROLOGUE();
    scope.release();
    resetVariable(globalObject, asNativeObject(args[0])->field(TokenVariable), asNativeObject(args[0]));
    RETURN_NONE();
}

PYTHON_NATIVE(missingRepr)
{
    return JSValue::encode(jsString(globalObject->vm(), String("<Token.MISSING>"_s)));
}

JSObject* createContextVarsModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "_contextvars"_s);
    for (auto [name, type] : { std::pair { "Context"_s, realm->typeContext() }, std::pair { "ContextVar"_s, realm->typeContextVar() }, std::pair { "Token"_s, realm->typeToken() } })
        module->putDirect(vm, Identifier::fromString(vm, name), type);
    addFunction(globalObject, module, "copy_context"_s, copyContext);
    return module;
}

void initializeContextVarTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    for (PyType* type : { realm->typeContext(), realm->typeContextVar(), realm->typeToken(), realm->typeTokenMissing() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));

    PyType* context = realm->typeContext();
    addMethods(globalObject, context, {
        { "__new__"_s, contextNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__len__"_s, contextLength },
        { "__getitem__"_s, contextGetItem },
        { "__contains__"_s, contextContains },
        { "__iter__"_s, contextView, Kind::Method, pack(PyIterator::Kind::ContextKeys) },
        { "get"_s, contextGet },
        { "keys"_s, contextView, Kind::Method, pack(PyIterator::Kind::ContextKeys) },
        { "values"_s, contextView, Kind::Method, pack(PyIterator::Kind::ContextValues) },
        { "items"_s, contextView, Kind::Method, pack(PyIterator::Kind::ContextItems) },
        { "copy"_s, contextCopy },
        { "run"_s, contextRun, Kind::Method, 0, "($self, callable, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked },
    });
    addComparisons(globalObject, context, contextCompare);
    context->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    for (BuiltinType view : { BuiltinType::ContextKeys, BuiltinType::ContextValues, BuiltinType::ContextItems })
        addMethods(globalObject, realm->type(view), { { "__len__"_s, contextViewLength } });

    PyType* variable = realm->typeContextVar();
    addMethods(globalObject, variable, {
        { "__new__"_s, variableNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__hash__"_s, variableHash },
        { "__repr__"_s, variableRepr },
        { "get"_s, variableGet },
        { "set"_s, variableSet },
        { "reset"_s, variableReset },
    });
    addMember(globalObject, variable, "name"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asNativeObject(self)->field(VariableName); });

    PyType* token = realm->typeToken();
    addMethods(globalObject, token, {
        { "__new__"_s, tokenNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__repr__"_s, tokenRepr },
        { "__enter__"_s, nativeSelf },
        { "__exit__"_s, tokenExit },
    });
    addGetSet(globalObject, token, "var"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asNativeObject(self)->field(TokenVariable); });
    addGetSet(globalObject, token, "old_value"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue value = asNativeObject(self)->field(TokenOldValue);
        return value ? value : globalObject->pyRealm()->typeToken()->getDirect(globalObject->vm(), Identifier::fromString(globalObject->vm(), "MISSING"_s));
    });
    token->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    addMethods(globalObject, realm->typeTokenMissing(), { { "__repr__"_s, missingRepr } });
    token->putDirect(vm, Identifier::fromString(vm, "MISSING"_s), PyNativeObject::create(globalObject, BuiltinType::TokenMissing));
}

} } // namespace JSC::Python
