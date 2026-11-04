/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "JSFunction.h"
#include "PyCodeInfo.h"
#include "PyDict.h"
#include "PyTuple.h"

namespace JSC {
namespace Python {

// What holds a variable that an enclosed function also uses.
class PyCell final : public PyObject {
public:
    PY_DECLARE_CELL(PyCell, PyObject, PyCellType);
    DECLARE_VISIT_CHILDREN;

    static PyCell* create(JSGlobalObject*, JSValue value);

    JSValue value() const { return m_value.get(); } // Empty: the variable has no value.
    void setValue(VM& vm, JSValue value) { m_value.set(vm, this, value); }

    static constexpr ptrdiff_t offsetOfValue() { return OBJECT_OFFSETOF(PyCell, m_value); }

private:
    PyCell(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_value;
};

// A function bound to its first argument.
class PyMethod final : public PyObject {
public:
    PY_DECLARE_CELL(PyMethod, PyObject, PyMethodType);
    DECLARE_VISIT_CHILDREN;

    static PyMethod* create(JSGlobalObject*, JSValue function, JSValue self);
    static CallData getCallData(JSCell*);

    JSValue function() const { return m_function.get(); }
    JSValue self() const { return m_self.get(); }

private:
    PyMethod(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_function;
    WriteBarrier<Unknown> m_self;
};

// A code object.
class PyCode final : public PyObject {
public:
    PY_DECLARE_CELL(PyCode, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    // The one code object of the code.
    static PyCode* of(JSGlobalObject*, FunctionExecutable*);

    // code.replace(co_flags=flags)
    static PyCode* createWithFlags(JSGlobalObject*, FunctionExecutable*, OptionSet<CodeFlag>);
    // The same for code that nobody has seen yet: it is the one code object of the code.
    static PyCode* ofWithFlags(JSGlobalObject*, FunctionExecutable*, OptionSet<CodeFlag>);

    FunctionExecutable* executable() const { return m_executable.get(); }
    CodeInfo& codeInfo() const { return *m_executable->unlinkedExecutable()->pythonCodeInfo(); }
    OptionSet<CodeFlag> flags() const { return m_codeFlags; }

private:
    PyCode(VM&, Structure*, PyType*);

    WriteBarrier<FunctionExecutable> m_executable;
    OptionSet<CodeFlag> m_codeFlags;
};

// A function written in Python.
class PyFunction final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | jsViewStructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    PY_DECLARE_JS_VIEW;

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    // __builtins__ is found in the globals.
    static PyFunction* create(JSGlobalObject*, FunctionExecutable*, PyDict* globals);

    static bool is(JSCell* cell) { return cell->type() == JSFunctionType && cell->classInfo() == info(); }
    static bool is(JSValue value) { return value && value.isCell() && is(value.asCell()); }
    static PyFunction* from(JSValue value)
    {
        ASSERT(is(value));
        return uncheckedDowncast<PyFunction>(value.asCell());
    }
    static PyFunction* tryFrom(JSValue value) { return is(value) ? uncheckedDowncast<PyFunction>(value.asCell()) : nullptr; }

    // Most functions are defined and never called, so a `def` leaves this where the code came from: in the bytecode cache, as a rule.
    // Only the mutator can take it out of there. Code that has a CodeBlock has it (didCreateCodeBlock()), so whoever has found the function
    // in a frame may ask.
    CodeInfo& codeInfo() const
    {
        if (!m_info) [[unlikely]]
            m_info = functionExecutable()->unlinkedExecutable()->pythonCodeInfo();
        return *m_info;
    }
    FunctionExecutable* functionExecutable() const { return static_cast<FunctionExecutable*>(executable()); }

    PyDict* globals() const { return m_globals.get(); }
    // If __builtins__ is not a dict, this is an empty one.
    PyDict* builtins() const { return m_builtins.get(); }
    // __builtins__: nearly always builtins(), but any mapping will do.
    JSValue builtinsObject() const { return m_builtinsObject ? m_builtinsObject.get() : JSValue(m_builtins.get()); }
    // Whether the globals or the builtins are something else than a dict or a frozendict: an instance of a class derived from
    // dict, say, whose __getitem__ and __missing__ are then what looks a global name up.
    bool hasUnusualNamespaces() const { return m_hasUnusualNamespaces; }
    // builtinsObject()[name]. Empty: there is no such key (and no exception), or an exception.
    JSValue lookupBuiltin(JSGlobalObject*, JSString* name);
    PyTuple* closure() const { return m_closure.get(); } // May be null, as those that follow.
    PyTuple* defaults() const { return m_defaults.get(); }
    PyDict* kwDefaults() const { return m_kwDefaults.get(); }
    PyDict* dict() const { return m_dict.get(); }
    // These three are those of the code at the `def`, until they are set. The strs are made when one of them is first asked for.
    JSValue name() { return m_name ? m_name.get() : (takeNamesFromCode(), m_name.get()); }
    JSValue qualname() { return m_qualname ? m_qualname.get() : (takeNamesFromCode(), m_qualname.get()); }
    JSValue doc() { return m_doc ? m_doc.get() : (takeNamesFromCode(), m_doc.get()); }
    JSValue module() const { return m_module.get(); }
    JSValue annotations() const { return m_annotations.get(); } // Empty: not computed.
    JSValue annotate() const { return m_annotate.get(); } // Empty: none.
    JSValue typeParams() const { return m_typeParams.get(); } // Empty: ().
    // What __code__ was set to. Null: it was not. For the body of generators: what the generator function had before it was set.
    PyCode* assignedCode() const { return m_code.get(); }
    bool isIterableCoroutine() const { return m_code && m_code->flags().contains(CodeFlag::IterableCoroutine); }

    void setClosure(VM& vm, PyTuple* value) { m_closure.setMayBeNull(vm, this, value); }
    void setDefaults(VM& vm, PyTuple* value) { m_defaults.setMayBeNull(vm, this, value); }
    void setKwDefaults(VM& vm, PyDict* value) { m_kwDefaults.setMayBeNull(vm, this, value); }
    void setDict(VM& vm, PyDict* value) { m_dict.setMayBeNull(vm, this, value); }
    void setName(VM& vm, JSValue value) { m_name.set(vm, this, value); }
    void setQualname(VM& vm, JSValue value) { m_qualname.set(vm, this, value); }
    void setDoc(VM& vm, JSValue value) { m_doc.set(vm, this, value); }
    void setModule(VM& vm, JSValue value) { m_module.set(vm, this, value); }
    void setAnnotations(VM& vm, JSValue value) { m_annotations.set(vm, this, value); }
    void setAnnotate(VM& vm, JSValue value) { m_annotate.set(vm, this, value); }
    void setTypeParams(VM& vm, JSValue value) { m_typeParams.set(vm, this, value); }
    // function.__code__ = code
    void setCode(VM&, PyCode*);

    // For a generator function: the function that runs the body of its generators.
    PyFunction* generatorBody(JSGlobalObject*, CodeBlock* ownCodeBlock);
    PyFunction* generatorBodyIfMade() const { return m_generatorBody.get(); }
    // For such a body, the generator function; for any other function, itself.
    PyFunction* codeOwner() { return m_generatorOwner ? m_generatorOwner.get() : this; }

    static constexpr ptrdiff_t offsetOfGlobals() { return OBJECT_OFFSETOF(PyFunction, m_globals); }
    static constexpr ptrdiff_t offsetOfBuiltins() { return OBJECT_OFFSETOF(PyFunction, m_builtins); }
    static constexpr ptrdiff_t offsetOfClosure() { return OBJECT_OFFSETOF(PyFunction, m_closure); }

private:
    PyFunction(VM&, FunctionExecutable*, JSGlobalObject*, Structure*);
    void takeNamesFromCode();

    mutable CodeInfo* m_info { nullptr };
    WriteBarrier<PyDict> m_globals;
    WriteBarrier<PyDict> m_builtins;
    WriteBarrier<Unknown> m_builtinsObject; // Empty: m_builtins.
    WriteBarrier<PyTuple> m_closure;
    WriteBarrier<PyTuple> m_defaults;
    WriteBarrier<PyDict> m_kwDefaults;
    WriteBarrier<PyDict> m_dict;
    WriteBarrier<Unknown> m_name; // Empty, like the next two: not asked for yet.
    WriteBarrier<Unknown> m_qualname;
    WriteBarrier<Unknown> m_doc;
    WriteBarrier<Unknown> m_module;
    WriteBarrier<Unknown> m_annotations;
    WriteBarrier<Unknown> m_annotate;
    WriteBarrier<Unknown> m_typeParams;
    WriteBarrier<PyCode> m_code;
    WriteBarrier<PyFunction> m_generatorBody;
    WriteBarrier<PyFunction> m_generatorOwner;
    bool m_hasUnusualNamespaces { false };
};

// function.__code__
PyCode* codeOf(JSGlobalObject*, PyFunction*);

// The code that a call runs. Its function may have been given other code since it began (function.__code__ = code), so whatever is about
// the registers of a call, or about where it has got to, asks these and not the function.
ALWAYS_INLINE FunctionExecutable* executableOf(CallFrame* callFrame) { return uncheckedDowncast<FunctionExecutable>(callFrame->codeBlock()->ownerExecutable()); }
ALWAYS_INLINE CodeInfo& codeInfoOf(CallFrame* callFrame) { return *executableOf(callFrame)->unlinkedExecutable()->pythonCodeInfo(); }
PyCode* codeOf(JSGlobalObject*, CallFrame*);
// The same for a call that ran this code, or will.
PyCode* codeOf(JSGlobalObject*, PyFunction*, FunctionExecutable*);

// Puts the arguments of a call in the parameters of the function that is starting in the frame. False: an exception.
bool bindArguments(JSGlobalObject*, CallFrame*, PyFunction*);

// Whether an instruction can call it by itself, in a frame with room for so many parameters ("Calls that instructions make" in
// LowLevelInterpreter64.asm). Not before it has been called once, in the way that makes its code.
bool canBeCalledByInstruction(PyFunction*, unsigned maxParameterCount);
// With the operands of an operator.
bool canBeCalledByOperator(PyFunction*);

} // namespace Python
} // namespace JSC
