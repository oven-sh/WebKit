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
#include "AOTVector.h"
#include "AOTWire.h"
#include <optional>
#include <utility>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionRemap;
class ReasonsForOperationsThatWent;
class StringRemap;
class TypeRemap;

enum class ConstructorForm : uint8_t { NotConstructor, Base, Derived };

struct Function;

void didReplaceBody(ReasonsForOperationsThatWent&);

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

class ChangeLogOfFunction {
public:
    ChangeLogOfFunction() = default;
    ChangeLogOfFunction(const ChangeLogOfFunction&) { }
    ChangeLogOfFunction(ChangeLogOfFunction&& other)
        : reasonsForOperationsThatWent(std::exchange(other.reasonsForOperationsThatWent, nullptr))
        , valuesAreTypedAsDefined(other.valuesAreTypedAsDefined)
    {
    }
    ChangeLogOfFunction& operator=(const ChangeLogOfFunction&)
    {
        didEditEverything();
        if (reasonsForOperationsThatWent) [[unlikely]]
            didReplaceBody(*reasonsForOperationsThatWent);
        return *this;
    }
    ChangeLogOfFunction& operator=(ChangeLogOfFunction&& other)
    {
        didEditEverything();
        if (reasonsForOperationsThatWent) [[unlikely]]
            didReplaceBody(*reasonsForOperationsThatWent);
        else
            reasonsForOperationsThatWent = std::exchange(other.reasonsForOperationsThatWent, nullptr);
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
    mutable ReasonsForOperationsThatWent* reasonsForOperationsThatWent { nullptr };
    uint32_t numberOfEdits { 0 };
    bool valuesAreTypedAsDefined { false };
};

struct WrittenPosition {
    ValueID value;
    uint32_t position { 0 };
};

struct ConstructionStart {
    uint32_t reported { 0 };
    uint32_t start { 0 };
    // Reflect.construct(..), which calls the host function where it cannot construct.
    bool isWrittenAsCall { false };
};

struct Function : FunctionRecord {
    using Blocks = BodyTable<Block, 2>;
    using Values = BodyTable<Value, 4>;

    Blocks blocks;
    Values values;
    // Sorted by value. A value's position is where an error is reported; only a value that the source text has elsewhere is listed.
    Vector<WrittenPosition> writtenPositions;
    Vector<ConstructionStart> constructionStarts;
    bool representationsAreChosen { false };
    bool bodyIsReleased { false };

    bool has(FunctionFlags flag) const { return flags.contains(flag); }
    bool hasAny(OptionSet<FunctionFlags> set) const { return flags.containsAny(set); }
    bool has(FunctionCapabilities capability) const { return capabilities.contains(capability); }
    bool hasBody() const { return bodyIsReleased || !blocks.isEmpty(); }
    void releaseBody();

    const Value& at(ValueID id) const { ASSERT(id.index() < values.size()); return values.span().data()[id.index()]; }
    const Block& at(BlockID id) const { ASSERT(id.index() < blocks.size()); return blocks.span().data()[id.index()]; }

    Value& edit(ValueID id, OptionSet<ValueEdit> edits = anyValueEdit)
    {
        ASSERT(id.index() < values.size());
        Value& value = values.m_storage.mutableSpan().data()[id.index()];
        m_changes.didEdit();
        if (m_changes.log)
            m_changes.log->didEdit(id, edits, value.type, value.expectedForTyping().predicted());
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
    ReasonsForOperationsThatWent* listenerForOperationsThatGo() const { return m_changes.reasonsForOperationsThatWent; }
    void setListenerForOperationsThatGo(ReasonsForOperationsThatWent* listener) const { m_changes.reasonsForOperationsThatWent = listener; }
    uint32_t numberOfEdits() const { return m_changes.numberOfEdits; }
    bool valuesAreTypedAsDefined() const { return m_changes.valuesAreTypedAsDefined; }
    void setValuesAreTypedAsDefined() { m_changes.valuesAreTypedAsDefined = true; }

    const DeclaredType& result() const;

    bool isModuleBody() const;
    bool isUnsplitCoroutine() const;
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
    std::optional<uint32_t> writtenPositionOf(ValueID) const;
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
// Callers and closures name a function by its id. What is known of them holds for whatever stands under that id.
OptionSet<FunctionCapabilities> capabilitiesInPlaceOf(const Function& replacement, OptionSet<FunctionCapabilities> ofReplaced, OptionSet<FunctionCapabilities> alsoKept = { });

struct Renumbering {
    BlockID newIDOf(BlockID block) const { return blocks.isEmpty() ? block : blocks[block.index()]; }
    ValueID newIDOf(ValueID value) const { return values[value.index()]; }

    Vector<BlockID> blocks;
    Vector<ValueID> values;
};

bool compactFunction(Function&, Renumbering* = nullptr);
bool needsCompaction(const Function&);
void remapIDs(Function&, const TypeRemap&, const StringRemap&, const FunctionRemap&);
void adoptIDs(Function& fromOtherArena, TypeArena&);
size_t numberOfForeignIDs(const Function&, const TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
