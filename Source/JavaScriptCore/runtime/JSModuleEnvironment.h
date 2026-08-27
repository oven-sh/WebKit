/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSLexicalEnvironment.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class AbstractModuleRecord;
class ModuleGraphInstance;
class Register;

class JSModuleEnvironment final : public JSLexicalEnvironment {
    friend class JIT;
    friend class LLIntOffsetsExtractor;
public:
    using Base = JSLexicalEnvironment;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnSpecialPropertyNames | OverridesPut;

    static JSModuleEnvironment* create(VM& vm, JSGlobalObject* globalObject, JSScope* currentScope, SymbolTable* symbolTable, JSValue initialValue, AbstractModuleRecord* moduleRecord)
    {
        Structure* structure = globalObject->moduleEnvironmentStructure();
        return create(vm, structure, currentScope, symbolTable, initialValue, moduleRecord);
    }

    DECLARE_INFO;

    DECLARE_VISIT_CHILDREN;

    inline static Structure* createStructure(VM&, JSGlobalObject*);

    static size_t offsetOfModuleRecord(SymbolTable* symbolTable)
    {
        size_t offset = Base::allocationSize(symbolTable);
        ASSERT(WTF::roundUpToMultipleOf<sizeof(WriteBarrier<AbstractModuleRecord>)>(offset) == offset);
        return offset;
    }

    static size_t offsetOfGraphInstance(SymbolTable* symbolTable)
    {
        return offsetOfModuleRecord(symbolTable) + sizeof(WriteBarrier<AbstractModuleRecord>);
    }

    // Number of import slots this environment was allocated with (raw word).
    static size_t offsetOfImportSlotCount(SymbolTable* symbolTable)
    {
        return offsetOfGraphInstance(symbolTable) + sizeof(WriteBarrier<Unknown>);
    }

    static size_t offsetOfImportSlot(SymbolTable* symbolTable, unsigned index)
    {
        return offsetOfImportSlotCount(symbolTable) + sizeof(uintptr_t) + sizeof(WriteBarrier<Unknown>) * index;
    }
    // Import slot `index` addressed as a variable of this environment (past the
    // symbol table's own variables), so JITs can treat it like a closure variable.
    static ScopeOffset importSlotScopeOffset(SymbolTable* symbolTable, unsigned index)
    {
        size_t byteOffset = offsetOfImportSlot(symbolTable, index) - offsetOfVariables();
        ASSERT(!(byteOffset % sizeof(WriteBarrier<Unknown>)));
        return ScopeOffset(byteOffset / sizeof(WriteBarrier<Unknown>));
    }

    static size_t allocationSize(SymbolTable* symbolTable, unsigned importSlotCount = 0)
    {
        return offsetOfImportSlot(symbolTable, importSlotCount);
    }

    unsigned importSlotCount() { return static_cast<unsigned>(importSlotCountSlot()); }
    WriteBarrierBase<Unknown>& importSlot(unsigned index)
    {
        ASSERT(index < importSlotCount());
        return *std::bit_cast<WriteBarrierBase<Unknown>*>(std::bit_cast<char*>(this) + offsetOfImportSlot(symbolTable(), index));
    }
    // Point every import slot at the exporter's environment in this graph
    // instance (or the exporter's primary environment). Slots whose exporter has
    // no environment yet (link-time cycles) stay empty and are filled on first use.
    void fillImportSlots(JSGlobalObject*);

    AbstractModuleRecord* moduleRecord()
    {
        return moduleRecordSlot().get();
    }

    // Module graph instances (prototype): a secondary instantiation of a module
    // graph shares every record, executable and CodeBlock with the primary one
    // and differs only in its environments. Each secondary environment points at
    // its instance's map (a JSMap: AbstractModuleRecord → JSModuleEnvironment)
    // so ModuleVar resolution can find the importing instance's copy of the
    // exporting module's environment. Null on primary environments.
    // The module graph instance this environment belongs to (null: the primary instantiation).
    ModuleGraphInstance* graphInstance();
    void setGraphInstance(VM&, ModuleGraphInstance*);
    // The environment of `exporter` in the same graph instance as this one
    // (the exporter's primary environment if this is a primary environment).
    JSModuleEnvironment* importedEnvironmentFor(JSGlobalObject*, AbstractModuleRecord* exporter);
    // op_resolve_scope slow path for a ModuleVar under module graph instances:
    // walk `depth` scopes from `scope` to the importing module environment and
    // return the exporter's environment in that environment's instance
    // (`linkedExporter` — the one the CodeBlock was linked against — otherwise).
    static JSObject* resolveModuleVarScope(JSGlobalObject*, JSScope*, unsigned depth, JSModuleEnvironment* linkedExporter);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

private:
    JSModuleEnvironment(VM&, Structure*, JSScope*, SymbolTable*, JSValue initialValue, AbstractModuleRecord*);

    static JSModuleEnvironment* create(VM&, Structure*, JSScope*, SymbolTable*, JSValue initialValue, AbstractModuleRecord*);

    DECLARE_DEFAULT_FINISH_CREATION;

    WriteBarrierBase<AbstractModuleRecord>& moduleRecordSlot()
    {
        return *std::bit_cast<WriteBarrierBase<AbstractModuleRecord>*>(std::bit_cast<char*>(this) + offsetOfModuleRecord(symbolTable()));
    }
    WriteBarrierBase<Unknown>& graphInstanceSlot()
    {
        return *std::bit_cast<WriteBarrierBase<Unknown>*>(std::bit_cast<char*>(this) + offsetOfGraphInstance(symbolTable()));
    }
    uintptr_t& importSlotCountSlot()
    {
        return *std::bit_cast<uintptr_t*>(std::bit_cast<char*>(this) + offsetOfImportSlotCount(symbolTable()));
    }
};

inline JSModuleEnvironment::JSModuleEnvironment(VM& vm, Structure* structure, JSScope* currentScope, SymbolTable* symbolTable, JSValue initialValue, AbstractModuleRecord* moduleRecord)
    : Base(vm, structure, currentScope, symbolTable, initialValue)
{
    this->moduleRecordSlot().setWithoutWriteBarrier(moduleRecord);
    this->graphInstanceSlot().setWithoutWriteBarrier(JSValue());
    this->importSlotCountSlot() = 0;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
