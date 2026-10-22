/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlock.h"
#include "AOTChangeLog.h"
#include "AOTSchemaRecords.h"
#include "AOTTypeTable.h"
#include "AOTWire.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionRemap;
class StringRemap;
class TypeRemap;

enum class ConstructorForm : uint8_t { NotConstructor, Base, Derived };

struct Function;

// The blocks or the values of a function, for reading. Whoever writes goes through Function::edit() and its neighbors.
template<typename T, size_t minCapacity>
class BodyTable {
public:
    using Storage = Vector<T, 0, CrashOnOverflow, minCapacity>;

    BodyTable() = default;
    BodyTable(const BodyTable&) = default;

    size_t size() const { return m_storage.size(); }
    size_t capacity() const { return m_storage.capacity(); }
    bool isEmpty() const { return m_storage.isEmpty(); }
    const T& operator[](size_t index) const LIFETIME_BOUND { return m_storage[index]; }
    const T& at(size_t index) const LIFETIME_BOUND { return m_storage[index]; }
    const T& first() const LIFETIME_BOUND { return m_storage.first(); }
    const T& last() const LIFETIME_BOUND { return m_storage.last(); }
    auto begin() const LIFETIME_BOUND { return m_storage.begin(); }
    auto end() const LIFETIME_BOUND { return m_storage.end(); }
    std::span<const T> span() const LIFETIME_BOUND { return m_storage.span(); }
    std::span<const T> subspan(size_t offset) const LIFETIME_BOUND { return m_storage.span().subspan(offset); }
    template<typename Functor> bool containsIf(NOESCAPE const Functor& functor) const { return m_storage.containsIf(functor); }
    template<typename Functor> size_t findIf(NOESCAPE const Functor& functor) const { return m_storage.findIf(functor); }

private:
    friend struct Function;

    BodyTable(BodyTable&&) = default;
    BodyTable& operator=(const BodyTable&) = default;
    BodyTable& operator=(BodyTable&&) = default;

    Storage m_storage;
};

// A copy of a function has no log. A function that is given another body keeps its log, which forgets.
class ChangeLogOfFunction {
public:
    ChangeLogOfFunction() = default;
    ChangeLogOfFunction(const ChangeLogOfFunction&) { }
    ChangeLogOfFunction(ChangeLogOfFunction&& other)
        : valuesAreTypedAsDefined(other.valuesAreTypedAsDefined)
    {
    }
    ChangeLogOfFunction& operator=(const ChangeLogOfFunction&)
    {
        didEditEverything();
        return *this;
    }
    ChangeLogOfFunction& operator=(ChangeLogOfFunction&& other)
    {
        didEditEverything();
        valuesAreTypedAsDefined = other.valuesAreTypedAsDefined;
        return *this;
    }

    ALWAYS_INLINE void didEdit()
    {
        ++numberOfEdits;
        valuesAreTypedAsDefined = false;
    }

    void didEditEverything()
    {
        didEdit();
        if (log)
            log->forget();
    }

    ChangeLog* log { nullptr };
    uint32_t numberOfEdits { 0 };
    // Set by whoever has typed the values, cleared by every edit, with a log or without. A copy does not have it; a body that is moved takes it along.
    bool valuesAreTypedAsDefined { false };
};

struct Function : FunctionRecord {
    using Blocks = BodyTable<Block, 2>;
    using Values = BodyTable<Value, 4>;

    Blocks blocks;
    Values values;
    bool representationsAreChosen { false };
    // The values have their forms, and the function holds no conversions but those that cannot wait: the code generator converts an operand where it fetches it.
    bool operandsAreConvertedAtFetch { false };
    bool bodyIsReleased { false };

    bool has(FunctionFlags flag) const { return flags.contains(flag); }
    bool hasAny(OptionSet<FunctionFlags> set) const { return flags.containsAny(set); }
    bool has(FunctionCapabilities capability) const { return capabilities.contains(capability); }
    bool hasBody() const { return bodyIsReleased || !blocks.isEmpty(); }
    void releaseBody();

    const Value& at(ValueID id) const { ASSERT(id.index() < values.size()); return values.span().data()[id.index()]; }
    const Block& at(BlockID id) const { ASSERT(id.index() < blocks.size()); return blocks.span().data()[id.index()]; }

    // The only ways to write to a body.
    Value& edit(ValueID id, OptionSet<ValueEdit> edits = anyValueEdit)
    {
        ASSERT(id.index() < values.size());
        Value& value = values.m_storage.mutableSpan().data()[id.index()];
        m_changes.didEdit();
        if (m_changes.log)
            m_changes.log->didEdit(id, edits, value.type, value.expectedForTyping());
        return value;
    }
    Block& edit(BlockID id, OptionSet<BlockEdit> edits = anyBlockEdit)
    {
        ASSERT(id.index() < blocks.size());
        Block& block = blocks.m_storage.mutableSpan().data()[id.index()];
        m_changes.didEdit();
        if (m_changes.log) {
            m_changes.log->didEdit(id, edits);
            if (edits.contains(BlockEdit::Edges) && !m_changes.log->wasAddedAndShownToNobody(id))
                willEditEdgesOf(block);
        }
        return block;
    }
    // For whoever writes a value many times over and knows in the end whether it has changed: it says so with didEdit(), before anybody reads the log or asks an analysis.
    Value& editAndReportLater(ValueID id)
    {
        ASSERT(id.index() < values.size());
        return values.m_storage.mutableSpan().data()[id.index()];
    }
    void didEdit(ValueID id, OptionSet<ValueEdit> edits, TypeID typeBefore, TypeID expectedBefore)
    {
        m_changes.didEdit();
        if (m_changes.log)
            m_changes.log->didEdit(id, edits, typeBefore, expectedBefore);
    }
    ValueID add(Value&& value, OptionSet<ValueEdit> edits = ValueEdit::Added)
    {
        ValueID id(values.size());
        m_changes.didEdit();
        if (m_changes.log)
            m_changes.log->didEdit(id, edits, { }, { });
        values.m_storage.append(WTF::move(value));
        return id;
    }
    ValueID add(const Value& value) { return add(Value(value)); }
    BlockID addBlock(Block&& block = { })
    {
        BlockID id(blocks.size());
        m_changes.didEdit();
        if (m_changes.log)
            m_changes.log->didEdit(id, BlockEdit::Added);
        blocks.m_storage.append(WTF::move(block));
        return id;
    }
    // All of them may change, and their number. The log forgets.
    Blocks::Storage& editAllBlocks() LIFETIME_BOUND
    {
        m_changes.didEditEverything();
        return blocks.m_storage;
    }
    Values::Storage& editAllValues() LIFETIME_BOUND
    {
        m_changes.didEditEverything();
        return values.m_storage;
    }
    Blocks::Storage takeBlocks()
    {
        m_changes.didEditEverything();
        return WTF::move(blocks.m_storage);
    }
    Values::Storage takeValues()
    {
        m_changes.didEditEverything();
        return WTF::move(values.m_storage);
    }
    void replaceBlocks(Blocks&& others)
    {
        m_changes.didEditEverything();
        blocks = WTF::move(others);
    }
    void replaceValues(Values&& others)
    {
        m_changes.didEditEverything();
        values = WTF::move(others);
    }
    // For whoever changes nothing that a reader of the body can see: the room that a list takes.
    Blocks::Storage& storageOfBlocks() LIFETIME_BOUND { return blocks.m_storage; }
    Values::Storage& storageOfValues() LIFETIME_BOUND { return values.m_storage; }

    ChangeLog* changeLog() const { return m_changes.log; }
    void attach(ChangeLog& log)
    {
        RELEASE_ASSERT(!m_changes.log);
        m_changes.log = &log;
    }
    void detach(ChangeLog& log)
    {
        RELEASE_ASSERT(m_changes.log == &log);
        m_changes.log = nullptr;
    }
    // Rises with every edit, with a log or without.
    uint32_t numberOfEdits() const { return m_changes.numberOfEdits; }
    bool valuesAreTypedAsDefined() const { return m_changes.valuesAreTypedAsDefined; }
    void setValuesAreTypedAsDefined() { m_changes.valuesAreTypedAsDefined = true; }

    const DeclaredType& result() const;

    bool isModuleBody() const;
    bool isUnsplitCoroutine() const;
    // A call goes on in the generic version of a function once a test at the entrance has failed. A coroutine's ramp and body have roles of their own.
    bool runsOnlyBehindFailedTest() const { return role == FunctionRole::GenericVersion || has(FunctionFlags::IsSplitOffGenericVersion); }
    ConstructorForm constructorForm() const;

    bool mayChangeSignature() const { return has(FunctionCapabilities::MayChangeSignature); }
    bool mayBeCloned() const { return has(FunctionCapabilities::MayBeCloned); }
    bool mayBeInlined() const { return has(FunctionCapabilities::MayBeInlined); }
    bool hasOrdinaryFrame() const { return has(FunctionCapabilities::HasOrdinaryFrame); }
    bool hasOnlyDirectCallers() const { return has(FunctionCapabilities::HasOnlyDirectCallers); }
    bool mayBeCalledByHost() const { return !has(FunctionCapabilities::IsNeverCalledByHost); }
    bool isHiddenInStackTraces() const { return has(FunctionCapabilities::IsHiddenInStackTraces) || has(FunctionFlags::IsHiddenInStackTraces); }

    Wire::SourceLocation locationOf(ValueID) const;
    Wire::SourceLocation locationOf(Origin) const;
    Wire::SourceLocation declarationLocation() const;

    unsigned numberOfEntryParameters() const;
    std::optional<unsigned> thisParameterIndex() const;
    std::optional<unsigned> calleeParameterIndex() const;
    std::optional<unsigned> newTargetParameterIndex() const;
    std::optional<unsigned> argumentCountParameterIndex() const;
    unsigned firstParameterIndex() const;
    std::optional<unsigned> hiddenRestParameterIndex() const;
    unsigned numberOfPositionalParameters() const;

    EntryParameter entryParameterKind(unsigned entryParameterIndex) const;
    const DeclaredType* declaredEntryParameter(unsigned entryParameterIndex) const LIFETIME_BOUND;
    DeclaredType* declaredEntryParameter(unsigned entryParameterIndex) LIFETIME_BOUND;

private:
    void willEditEdgesOf(const Block&);

    ChangeLogOfFunction m_changes;
};

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function&);

// Where compactFunction() has put what it kept, by the former index. What it removed has no new number.
struct Renumbering {
    BlockID newIDOf(BlockID block) const { return blocks.isEmpty() ? block : blocks[block.index()]; }
    ValueID newIDOf(ValueID value) const { return values[value.index()]; }

    // Empty where no block moved.
    Vector<BlockID> blocks;
    Vector<ValueID> values;
};

// Answers whether a block or a value moved.
bool compactFunction(Function&, Renumbering* = nullptr);
bool needsCompaction(const Function&);
void remapIDs(Function&, const TypeRemap&, const StringRemap&, const FunctionRemap&);
void adoptIDs(Function& fromOtherArena, TypeArena&);
size_t numberOfForeignIDs(const Function&, const TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
