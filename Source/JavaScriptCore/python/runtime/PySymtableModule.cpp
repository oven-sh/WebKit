/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PySymbolTable.h"

namespace JSC {
namespace Python {

// The module _symtable, which Lib/symtable.py wraps: the compiler's symbol table, as objects.
// (Modules/symtablemodule.c and the end of Python/symtable.c in CPython are the reference.)

namespace SymtableModule {

// What programs see is CPython's encoding of a symbol: these flags, and the scope in the bits above them.
enum : int {
    FlagGlobal = 1,
    FlagLocal = 2,
    FlagParam = 2 << 1,
    FlagNonlocal = 2 << 2,
    FlagUse = 2 << 3,
    FlagFreeClass = 2 << 5,
    FlagImport = 2 << 6,
    FlagAnnot = 2 << 7,
    FlagCompIter = 2 << 8,
    FlagTypeParam = 2 << 9,
    FlagCompCell = 2 << 10,
    FlagBound = FlagLocal | FlagParam | FlagImport,

    ScopeOffset = 12,
    ScopeMask = FlagGlobal | FlagLocal | FlagParam | FlagNonlocal,

    ScopeLocal = 1,
    ScopeGlobalExplicit = 2,
    ScopeGlobalImplicit = 3,
    ScopeFree = 4,
    ScopeCell = 5,
};

enum EntryField : unsigned {
    EntryID,
    EntryName,
    EntrySymbols,
    EntryVarnames,
    EntryChildren,
    EntryNested,
    EntryType,
    EntryLineno,
    EntryFieldCount,
};

static int flagsOf(const Symbol& symbol)
{
    static_assert(+DefGlobal == +FlagGlobal && +DefLocal == +FlagLocal && +DefParam == +FlagParam && +DefNonlocal == +FlagNonlocal && +Use == +FlagUse);
    int flags = symbol.flags & (DefGlobal | DefLocal | DefParam | DefNonlocal | Use);
    if (symbol.flags & DefFreeClass)
        flags |= FlagFreeClass;
    if (symbol.flags & DefImport)
        flags |= FlagImport;
    if (symbol.flags & DefAnnot)
        flags |= FlagAnnot;
    if (symbol.flags & DefCompIter)
        flags |= FlagCompIter;
    if (symbol.flags & DefTypeParam)
        flags |= FlagTypeParam;
    return flags;
}

static int scopeOf(Binding binding)
{
    switch (binding) {
    case Binding::Unknown:
        return 0;
    case Binding::Local:
        return ScopeLocal;
    case Binding::GlobalExplicit:
        return ScopeGlobalExplicit;
    case Binding::GlobalImplicit:
        return ScopeGlobalImplicit;
    case Binding::Free:
        return ScopeFree;
    case Binding::Cell:
        return ScopeCell;
    }
    return 0;
}

static int blockTypeOf(ScopeKind kind)
{
    switch (kind) {
    case ScopeKind::Function:
        return 0;
    case ScopeKind::Class:
        return 1;
    case ScopeKind::Module:
        return 2;
    case ScopeKind::Annotation:
        return 3;
    case ScopeKind::TypeAlias:
        return 4;
    case ScopeKind::TypeParameters:
        return 5;
    case ScopeKind::TypeVariable:
        return 6;
    }
    return 0;
}

template<EntryField field>
static JSValue entryGet(JSGlobalObject*, JSValue self)
{
    return uncheckedDowncast<PyRecord>(self.asCell())->get(field);
}

static void entrySet(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raise(globalObject, scope, runtime(globalObject).AttributeError(), "readonly attribute"_s);
}

static JSValue entryRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* entry = uncheckedDowncast<PyRecord>(self.asCell());
    String name = asString(entry->get(EntryName))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String id = intToString(globalObject, entry->get(EntryID));
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<symtable entry "_s, name, '(', id, "), line "_s, entry->get(EntryLineno).asInt32(), '>'));
}

static constexpr NativeDef entryMethods[] = {
    PY_SLOT("__repr__", entryRepr, "($self, /)"),
};

static constexpr NativeGetSet entryGetSets[] = {
    { "id"_s, entryGet<EntryID>, entrySet },
    { "name"_s, entryGet<EntryName>, entrySet },
    { "symbols"_s, entryGet<EntrySymbols>, entrySet },
    { "varnames"_s, entryGet<EntryVarnames>, entrySet },
    { "children"_s, entryGet<EntryChildren>, entrySet },
    { "nested"_s, entryGet<EntryNested>, entrySet },
    { "type"_s, entryGet<EntryType>, entrySet },
    { "lineno"_s, entryGet<EntryLineno>, entrySet },
};

static constexpr TypeDef entryTypeDef { "symtable entry"_s, { TypeFlag::DisallowInstantiation }, entryMethods, entryGetSets, { } };

// A comprehension that is compiled as part of the scope around it is a scope of its own to the compiler here. CPython has
// none for it once the analysis is over: its names are among those of the scope around it, and so are its children.
// So that is how it is shown.
class Exporter {
public:
    Exporter(JSGlobalObject* globalObject, const SymbolTable& table)
        : m_globalObject(globalObject)
        , m_vm(getVM(globalObject))
        , m_dotDefaults(table.names().dotDefaults)
    {
    }

    JSValue run(Scope* top)
    {
        auto throwScope = DECLARE_THROW_SCOPE(m_vm);
        Block block;
        if (failed(throwScope, collect(top, block)))
            return { };
        RELEASE_AND_RETURN(throwScope, makeEntry(top, block));
    }

private:
    struct Item {
        AST::Name name;
        int flags;
        int scope;
    };

    struct Block {
        Vector<Item, 16> items;
        UncheckedKeyHashMap<UniquedStringImpl*, unsigned> indices;
        JSArray* children { nullptr };
        bool takesDefaults { false };

        Item* find(AST::Name name)
        {
            auto it = indices.find(name->impl());
            return it == indices.end() ? nullptr : &items[it->value];
        }
        void add(const Item& item)
        {
            indices.add(item.name->impl(), items.size());
            items.append(item);
        }
    };

    bool collect(Scope* scope, Block& block)
    {
        auto throwScope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_vm.isSafeToRecurseSoft()) {
            raiseRecursionError(m_globalObject, throwScope, "maximum recursion depth exceeded during compilation"_s);
            return false;
        }
        block.children = newList(m_globalObject);
        RETURN_IF_EXCEPTION(throwScope, false);

        // CPython has the scope of the type parameters of a function take the defaults of the function whether there are any or not.
        block.takesDefaults = false;
        if (scope->kind == ScopeKind::TypeParameters && !scope->find(m_dotDefaults)) {
            for (Scope* child : scope->children) {
                if (child->kind == ScopeKind::Function && child->name->impl() == scope->name->impl())
                    block.takesDefaults = true;
            }
            if (block.takesDefaults)
                block.add({ m_dotDefaults, FlagParam, ScopeLocal });
        }

        // A name without flags is there only because it passes through, from a scope further out to one further in.
        for (auto& symbol : scope->symbols) {
            if (symbol.flags)
                block.add({ symbol.name, flagsOf(symbol), scopeOf(symbol.binding) });
        }

        UncheckedKeyHashSet<UniquedStringImpl*> inlinedCells;
        for (Scope* child : scope->children) {
            Block childBlock;
            if (failed(throwScope, collect(child, childBlock)))
                return false;
            if (!child->isInlinedComprehension) {
                JSValue entry = makeEntry(child, childBlock);
                RETURN_IF_EXCEPTION(throwScope, false);
                listAppend(m_globalObject, block.children, entry);
                RETURN_IF_EXCEPTION(throwScope, false);
                continue;
            }
            for (auto& item : childBlock.items) {
                if (item.flags & FlagParam)
                    continue;
                if (item.scope == ScopeCell || (item.flags & FlagCompCell))
                    inlinedCells.add(item.name->impl());
                if (Item* existing = block.find(item.name)) {
                    // The names of a class are not those a comprehension in it sees.
                    if (item.scope == ScopeFree && scope->kind != ScopeKind::Class)
                        existing->flags |= item.flags & FlagUse;
                } else
                    block.add(item);
            }
            listExtend(m_globalObject, block.children, childBlock.children);
            RETURN_IF_EXCEPTION(throwScope, false);
        }

        for (auto& symbol : scope->symbols) {
            if (!symbol.flags && !block.find(symbol.name))
                block.add({ symbol.name, 0, scopeOf(symbol.binding) });
        }

        for (auto& item : block.items) {
            if (!inlinedCells.contains(item.name->impl()))
                continue;
            item.flags |= FlagCompCell;
            if (item.scope == ScopeLocal && scope->isFunctionLike())
                item.scope = ScopeCell;
        }
        return true;
    }

    JSValue makeEntry(Scope* scope, Block& block)
    {
        auto throwScope = DECLARE_THROW_SCOPE(m_vm);
        PyDict* symbols = PyDict::create(m_globalObject);
        for (auto& item : block.items)
            symbols->set(m_vm, internString(m_vm, item.name->string()), jsNumber(item.flags | item.scope << ScopeOffset));
        JSArray* varnames = newList(m_globalObject, scope->parameters.size());
        RETURN_IF_EXCEPTION(throwScope, { });
        if (block.takesDefaults) {
            listAppend(m_globalObject, varnames, internString(m_vm, m_dotDefaults->string()));
            RETURN_IF_EXCEPTION(throwScope, { });
        }
        for (AST::Name parameter : scope->parameters) {
            listAppend(m_globalObject, varnames, internString(m_vm, parameter->string()));
            RETURN_IF_EXCEPTION(throwScope, { });
        }

        PyRecord* entry = PyRecord::create(m_globalObject, runtime(m_globalObject).typeFor(entryTypeDef), EntryFieldCount);
        RETURN_IF_EXCEPTION(throwScope, { });
        entry->set(m_vm, EntryID, makeInt(m_globalObject, static_cast<uint64_t>(std::bit_cast<uintptr_t>(scope))));
        entry->set(m_vm, EntryName, internString(m_vm, scope->name->string()));
        entry->set(m_vm, EntrySymbols, symbols);
        entry->set(m_vm, EntryVarnames, varnames);
        entry->set(m_vm, EntryChildren, block.children);
        entry->set(m_vm, EntryNested, jsNumber(scope->isNested ? 1 : 0));
        entry->set(m_vm, EntryType, jsNumber(blockTypeOf(scope->kind)));
        entry->set(m_vm, EntryLineno, jsNumber(scope->range.lineno));
        return entry;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    AST::Name m_dotDefaults;
};

static JSValue symtable(JSGlobalObject* globalObject, JSValue source, JSValue filenameValue, JSValue startValue, JSValue module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String filename = filenameArgument(globalObject, filenameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(startValue))
        return raiseTypeError(globalObject, scope, "symtable() argument 3 must be str, not "_s, typeName(globalObject, startValue));
    String start = strValue(startValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    int flags = 0;
    Vector<uint8_t> bytes;
    if (failed(scope, sourceBytes(globalObject, source, "symtable"_s, "string or bytes"_s, bytes, flags)))
        return { };
    ASCIILiteral mode;
    if (start == "exec"_s)
        mode = "exec"_s;
    else if (start == "eval"_s)
        mode = "eval"_s;
    else if (start == "single"_s)
        mode = "single"_s;
    else
        return raiseValueError(globalObject, scope, "symtable() arg 3 must be 'exec' or 'eval' or 'single'"_s);
    if (module && !isNone(module) && !isStr(module))
        return raiseTypeError(globalObject, scope, "symtable() argument 'module' must be str or None, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, module)));

    auto host = makeParserHost(globalObject, filename, module);
    ParseTree tree;
    bool parsed = parseSource(globalObject, tree, *host, bytes.span(), filename, mode, flags);
    RETURN_IF_EXCEPTION(scope, { });
    if (!parsed)
        return { };

    FutureFeatures future;
    SyntaxErrorInfo error;
    if (!analyzeFutureStatements(tree.root, future, error))
        RELEASE_AND_RETURN(scope, raiseSyntaxErrorInfo(globalObject, error, filename));
    SymbolTable table(vm, tree, future);
    if (!table.build())
        RELEASE_AND_RETURN(scope, raiseSyntaxErrorInfo(globalObject, table.error(), filename));
    Exporter exporter(globalObject, table);
    RELEASE_AND_RETURN(scope, exporter.run(table.top()));
}

static constexpr NativeDef functions[] = {
    PY_FUNCTION("symtable", symtable, "(source, filename, startstr, /, *, module=None)"),
};

} // namespace SymtableModule

void initializeSymtableModule(JSGlobalObject* globalObject, PyModule* module)
{
    using namespace SymtableModule;
    module->addFunctions(globalObject, functions);
    static constexpr std::pair<ASCIILiteral, int> constants[] = {
        { "USE"_s, FlagUse },
        { "DEF_GLOBAL"_s, FlagGlobal },
        { "DEF_NONLOCAL"_s, FlagNonlocal },
        { "DEF_LOCAL"_s, FlagLocal },
        { "DEF_PARAM"_s, FlagParam },
        { "DEF_TYPE_PARAM"_s, FlagTypeParam },
        { "DEF_FREE_CLASS"_s, FlagFreeClass },
        { "DEF_IMPORT"_s, FlagImport },
        { "DEF_BOUND"_s, FlagBound },
        { "DEF_ANNOT"_s, FlagAnnot },
        { "DEF_COMP_ITER"_s, FlagCompIter },
        { "DEF_COMP_CELL"_s, FlagCompCell },
        { "TYPE_FUNCTION"_s, 0 },
        { "TYPE_CLASS"_s, 1 },
        { "TYPE_MODULE"_s, 2 },
        { "TYPE_ANNOTATION"_s, 3 },
        { "TYPE_TYPE_ALIAS"_s, 4 },
        { "TYPE_TYPE_PARAMETERS"_s, 5 },
        { "TYPE_TYPE_VARIABLE"_s, 6 },
        { "LOCAL"_s, ScopeLocal },
        { "GLOBAL_EXPLICIT"_s, ScopeGlobalExplicit },
        { "GLOBAL_IMPLICIT"_s, ScopeGlobalImplicit },
        { "FREE"_s, ScopeFree },
        { "CELL"_s, ScopeCell },
        { "SCOPE_OFF"_s, ScopeOffset },
        { "SCOPE_MASK"_s, ScopeMask },
    };
    for (auto& [name, value] : constants)
        module->add(globalObject, name, jsNumber(value));
}

} // namespace Python
} // namespace JSC
