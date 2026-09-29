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

#include "CyclicModuleRecord.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSPromise.h"
#include "ModuleRegistryEntry.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonImport.h"
#include "PythonOperations.h"

// `import` of a module that is JavaScript's. What Python is given is the module's namespace object, as it is: see typeOf(). The importer is lib/_javascript_importer.py, which is the last
// that is asked, so that whatever is Python's by a name is what the name means. This is what it is written with.

namespace JSC { namespace Python {

// PyErr_SetImportError()
static JSValue raiseImportError(JSGlobalObject* globalObject, ThrowScope& scope, const String& key, const String& message)
{
    VM& vm = globalObject->vm();
    JSValue error = call(globalObject, globalObject->pyRealm()->typeImportError(), jsString(vm, message));
    RETURN_IF_EXCEPTION(scope, { });
    asObject(error)->putDirect(vm, vm.pythonNames().field_path, jsString(vm, key));
    throwException(globalObject, scope, error);
    return { };
}

static JSValue raiseCannotWait(JSGlobalObject* globalObject, ThrowScope& scope, const String& key)
{
    return raiseImportError(globalObject, scope, key, concatenate("cannot import '"_s, key, "', which awaits something as it is run, or imports what does. `import` does not wait."_s));
}

// What was thrown as the module was loaded or run, as it is. But JavaScript can throw anything, and a host may say what is wrong with a file with something that is no Error. `except` does not catch such a thing, and
// what tries an import expects to be able to catch what comes of it.
static JSValue raiseWhatWasThrown(JSGlobalObject* globalObject, ThrowScope& scope, const String& key, JSValue thrown)
{
    if (isInstance(globalObject, thrown, globalObject->pyRealm()->typeBaseException())) {
        throwException(globalObject, scope, thrown);
        return { };
    }
    String text = thrown.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseImportError(globalObject, scope, key, text);
}

// It has been run to its end.
static bool isEvaluated(AbstractModuleRecord* record)
{
    if (!record)
        return false;
    if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record))
        return cyclic->status() == CyclicModuleRecord::Status::Evaluated && !cyclic->evaluationError();
    // One that has no code has been once it is linked.
    return !!record->moduleEnvironmentMayBeNull();
}

// It is in the middle of being run, and it is from there that it is being asked for: it imports what imports it. What it exports is there to be looked at, so far as it has got.
static bool isBeingEvaluatedWithoutWaiting(AbstractModuleRecord* record)
{
    auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record);
    return cyclic && cyclic->status() == CyclicModuleRecord::Status::Evaluating && !cyclic->hasTLA() && !cyclic->evaluationError();
}

JSValue loadJavaScriptModule(JSGlobalObject* globalObject, const String& keyString)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
#if USE(BUN_JSC_ADDITIONS)
    auto key = Identifier::fromString(vm, keyString);
    JSModuleLoader* loader = globalObject->moduleLoader();
    bool wasKnown = false;
    if (auto* entry = loader->registryEntry(key)) {
        wasKnown = true;
        AbstractModuleRecord* record = entry->record();
        if (isEvaluated(record) || isBeingEvaluatedWithoutWaiting(record))
            RELEASE_AND_RETURN(scope, record->getModuleNamespace(globalObject, false));
        // It is waiting for something, and cannot be linked or run again meanwhile.
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record); cyclic && cyclic->status() == CyclicModuleRecord::Status::Evaluating)
            return raiseCannotWait(globalObject, scope, keyString);
    }

    // Fetching, linking and running are each done as soon as what comes before is, and not when the queue of microtasks is next gone through. So it is all done by the time that this returns,
    // if the host has the source to hand and nothing awaits.
    JSValue thrown;
    JSPromise* promise = [&] () -> JSPromise* {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSPromise* result = loader->loadModuleSync(globalObject, key, nullptr, nullptr);
        Exception* exception = catchScope.exception();
        if (!exception)
            return result;
        if (catchScope.tryClearException())
            thrown = exception->value();
        return nullptr;
    }();
    if (thrown)
        return raiseWhatWasThrown(globalObject, scope, keyString, thrown);
    RETURN_IF_EXCEPTION(scope, { });
    promise->markAsHandled();
    auto* entry = loader->registryEntry(key);
    AbstractModuleRecord* record = entry ? entry->record() : nullptr;
    switch (promise->status()) {
    case JSPromise::Status::Fulfilled:
        break;
    case JSPromise::Status::Rejected:
        return raiseWhatWasThrown(globalObject, scope, keyString, promise->result());
    case JSPromise::Status::Pending: {
        // It is in a cycle with a module that is still being run, from which this was come to. It has itself been run, and is only not said to have been until that one has.
        if (isBeingEvaluatedWithoutWaiting(record))
            break;
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record); cyclic && cyclic->status() == CyclicModuleRecord::Status::Evaluated && !cyclic->hasTLA() && !cyclic->evaluationError())
            break;
        // It goes on being loaded if something else had asked for it. If not, it is forgotten, so that `await import()` of it begins again.
        if (!wasKnown)
            loader->removeEntry(key);
        return raiseCannotWait(globalObject, scope, keyString);
    }
    }
    if (!record) [[unlikely]]
        return raise(globalObject, scope, BuiltinType::ImportError, concatenate("'"_s, keyString, "' was loaded, and is not to be found"_s));
    if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record)) {
        if (JSValue error = cyclic->evaluationError())
            return raiseWhatWasThrown(globalObject, scope, keyString, error);
    }
    RELEASE_AND_RETURN(scope, record->getModuleNamespace(globalObject, false));
#else
    return raiseCannotWait(globalObject, scope, keyString);
#endif
}

// find_module(name, path): None, or what the host has found: (what the loader knows it by, or None; the file that it is in, or None; what its __path__ is to have in it, or None).
PYTHON_NATIVE(javaScriptFindModule)
{
    NATIVE_PROLOGUE();
    auto find = realm->configuration().findJavaScriptModule;
    JSString* name = stringIn(args[0]);
    if (!find || !name)
        RETURN_NONE();
    String nameString = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    // What is not a str is passed over, as it is by PathFinder.
    Vector<String> directories;
    bool wentThrough = forEach(globalObject, args[1], [&] (JSValue item) {
        if (JSString* string = stringIn(item)) {
            directories.append(string->value(globalObject));
            return !scope.exception();
        }
        return true;
    });
    if (!wentThrough)
        return { };
    auto found = find(globalObject, nameString, directories.span());
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        RETURN_NONE();
    auto orNone = [&] (const String& string) { return string.isNull() ? jsUndefined() : JSValue(jsString(vm, string)); };
    return JSValue::encode(PyTuple::create(globalObject, { orNone(found->key), orNone(found->file), orNone(found->package) }));
}

// load_module(key)
PYTHON_NATIVE(javaScriptLoadModule)
{
    NATIVE_PROLOGUE();
    JSString* key = stringIn(args[0]);
    if (!key)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("load_module() argument must be str, not "_s, typeName(globalObject, args[0]))));
    String keyString = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(loadJavaScriptModule(globalObject, keyString)));
}

JSObject* createJavaScriptModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "_javascript"_s);
    addFunction(globalObject, module, "find_module"_s, javaScriptFindModule, 0, "($module, name, path, /)"_s);
    addFunction(globalObject, module, "load_module"_s, javaScriptLoadModule, 0, "($module, key, /)"_s);
    return module;
}

} } // namespace JSC::Python
