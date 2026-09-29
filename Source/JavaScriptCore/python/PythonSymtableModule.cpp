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
#include "PythonCompiler.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonSymbolTable.h"

// The module _symtable: Modules/symtablemodule.c of CPython, and the class of what it gives, which is in Python/symtable.c. What every name refers to is found by what finds it for the compiler: PythonSymbolTable.cpp.

namespace JSC { namespace Python {

namespace {

constexpr unsigned scopeOffset = 12; // SCOPE_OFFSET
constexpr unsigned scopeMask = DefGlobal | DefLocal | DefParameter | DefNonlocal; // SCOPE_MASK

struct SymtableModuleState final : NativeState {
    PYTHON_NATIVE_STATE(SymtableModuleState);
    WriteBarrier<PyType> entryType;
};

template<typename Visitor> void SymtableModuleState::visit(Visitor& visitor) { visitor.append(entryType); }

// PySTEntryObject, or as much of one as a program can see
struct SymtableEntry final : NativeState {
    PYTHON_NATIVE_STATE(SymtableEntry);
    WriteBarrier<Unknown> id;
    WriteBarrier<Unknown> name;
    WriteBarrier<Unknown> symbols;
    WriteBarrier<Unknown> parameters;
    WriteBarrier<Unknown> children;
    bool isNested { false };
    BlockType type { BlockType::Module };
    int line { 0 };
};

template<typename Visitor>
void SymtableEntry::visit(Visitor& visitor)
{
    visitor.append(id);
    visitor.append(name);
    visitor.append(symbols);
    visitor.append(parameters);
    visitor.append(children);
}

PyType* entryType(JSGlobalObject*);

} // anonymous namespace

JSValue newSymbolTableEntry(JSGlobalObject* globalObject, Block& block)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* symbols = PyDict::create(globalObject);
    for (Symbol& symbol : block.symbols) {
        symbols->set(globalObject, jsString(vm, symbol.name->string()), jsNumber(symbol.flags | static_cast<unsigned>(symbol.scope) << scopeOffset));
        RETURN_IF_EXCEPTION(scope, { });
    }
    MarkedArgumentBuffer parameters;
    for (const Identifier* name : block.parameters)
        parameters.append(jsString(vm, name->string()));
    MarkedArgumentBuffer children;
    for (Block* child : block.children) {
        children.append(newSymbolTableEntry(globalObject, *child));
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto* object = PyStateObject::create(vm, entryType(globalObject)->instanceStructure(), makeUnique<SymtableEntry>());
    auto& state = object->state<SymtableEntry>();
    // In CPython it is where in memory the part of the syntax tree was that the block is for. All that is asked of it is that no two in one table have the same.
    state.id.set(vm, object, intFromUInt64(globalObject, std::bit_cast<uintptr_t>(&block)));
    state.name.set(vm, object, jsString(vm, block.name->string()));
    state.symbols.set(vm, object, symbols);
    state.parameters.set(vm, object, newList(globalObject, parameters));
    state.children.set(vm, object, newList(globalObject, children));
    state.isNested = block.isNested;
    state.type = block.type;
    state.line = block.location.line;
    return object;
}

PYTHON_NATIVE(symtableEntryRepr)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<SymtableEntry>(args[0]);
    String name = asString(state.name.get())->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String id = repr(globalObject, state.id.get());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, concatenate("<symtable entry "_s, name, '(', id, "), line "_s, state.line, '>')));
}

namespace {

PyType* entryType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<SymtableModuleState>();
    if (state.entryType)
        return state.entryType.get();
    PyType* type = createBuiltinType(globalObject, "symtable entry"_s, realm->typeObject(), PyType::Layout::Native, 0);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    state.entryType.set(vm, realm, type);
    addMethods(globalObject, type, { { "__repr__"_s, symtableEntryRepr, PyNativeFunction::Kind::Wrapper } });
    addMember(globalObject, type, "id"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<SymtableEntry>(self).id.get(); });
    addMember(globalObject, type, "name"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<SymtableEntry>(self).name.get(); });
    addMember(globalObject, type, "symbols"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<SymtableEntry>(self).symbols.get(); });
    addMember(globalObject, type, "varnames"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<SymtableEntry>(self).parameters.get(); });
    addMember(globalObject, type, "children"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<SymtableEntry>(self).children.get(); });
    addMember(globalObject, type, "nested"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<SymtableEntry>(self).isNested); });
    addMember(globalObject, type, "type"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<unsigned>(stateOf<SymtableEntry>(self).type)); });
    addMember(globalObject, type, "lineno"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<SymtableEntry>(self).line); });
    return type;
}

} // anonymous namespace

// symtable(source, filename, startstr, /)
PYTHON_NATIVE(symtableSymtable)
{
    NATIVE_PROLOGUE();
    // PyUnicode_FSDecoder()
    auto path = toFileSystemPath(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    String filename = String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(path->span()));
    JSString* startString = stringIn(args[2]);
    if (!startString)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("symtable() argument 3 must be str, not "_s, typeNameOfArgument(globalObject, args[2]))));
    String start = startString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (start.contains(static_cast<char16_t>(0)))
        return JSValue::encode(raiseValueError(globalObject, scope, "embedded null character"_s));
    SourceCode source = sourceOfArgument(globalObject, scope, args[0], filename, "symtable"_s);
    RETURN_IF_EXCEPTION(scope, { });
    Module::Kind kind;
    if (start == "exec"_s)
        kind = Module::Kind::Module;
    else if (start == "eval"_s)
        kind = Module::Kind::Expression;
    else if (start == "single"_s)
        kind = Module::Kind::Interactive;
    else
        return JSValue::encode(raiseValueError(globalObject, scope, "symtable() arg 3 must be 'exec' or 'eval' or 'single'"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(symbolTableOfSource(globalObject, source, kind)));
}

JSObject* createSymtableModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_symtable"_s);
    addFunction(globalObject, module, "symtable"_s, symtableSymtable);
    auto add = [&] (ASCIILiteral name, unsigned value) { module->putDirect(vm, Identifier::fromString(vm, name), jsNumber(value)); };
    add("USE"_s, Use);
    add("DEF_GLOBAL"_s, DefGlobal);
    add("DEF_NONLOCAL"_s, DefNonlocal);
    add("DEF_LOCAL"_s, DefLocal);
    add("DEF_PARAM"_s, DefParameter);
    add("DEF_TYPE_PARAM"_s, DefTypeParameter);
    add("DEF_FREE_CLASS"_s, DefFreeClass);
    add("DEF_IMPORT"_s, DefImport);
    add("DEF_BOUND"_s, DefBound);
    add("DEF_ANNOT"_s, DefAnnotation);
    add("DEF_COMP_ITER"_s, DefComprehensionIteration);
    add("DEF_COMP_CELL"_s, DefComprehensionCell);
    add("TYPE_FUNCTION"_s, static_cast<unsigned>(BlockType::Function));
    add("TYPE_CLASS"_s, static_cast<unsigned>(BlockType::Class));
    add("TYPE_MODULE"_s, static_cast<unsigned>(BlockType::Module));
    add("TYPE_ANNOTATION"_s, static_cast<unsigned>(BlockType::Annotation));
    add("TYPE_TYPE_ALIAS"_s, static_cast<unsigned>(BlockType::TypeAlias));
    add("TYPE_TYPE_PARAMETERS"_s, static_cast<unsigned>(BlockType::TypeParameters));
    add("TYPE_TYPE_VARIABLE"_s, static_cast<unsigned>(BlockType::TypeVariable));
    add("LOCAL"_s, static_cast<unsigned>(NameScope::Local));
    add("GLOBAL_EXPLICIT"_s, static_cast<unsigned>(NameScope::GlobalExplicit));
    add("GLOBAL_IMPLICIT"_s, static_cast<unsigned>(NameScope::GlobalImplicit));
    add("FREE"_s, static_cast<unsigned>(NameScope::Free));
    add("CELL"_s, static_cast<unsigned>(NameScope::Cell));
    add("SCOPE_OFF"_s, scopeOffset);
    add("SCOPE_MASK"_s, scopeMask);
    return module;
}

} } // namespace JSC::Python
