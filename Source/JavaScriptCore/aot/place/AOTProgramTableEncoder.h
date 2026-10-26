/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTBuildContext.h"
#include "AOTIdentifierNumbering.h"
#include "AOTInstanceLayout.h"
#include "AOTLimits.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTProgramTables.h"
#include "AOTSectionWriter.h"
#include "AOTStringUses.h"
#include "AOTVector.h"
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

class TableWriter {
    WTF_MAKE_NONCOPYABLE(TableWriter);
public:
    explicit TableWriter(BuildContext& build)
        : m_build(build)
    {
    }

    uint32_t allocate(size_t size, size_t alignment = 8)
    {
        if (std::optional<uint32_t> offset = tryAllocate(size, alignment))
            return *offset;
        RELEASE_ASSERT(size <= defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));
        if (size > m_bytes.size())
            growSectionTo(m_bytes, size);
        return 0;
    }

    template<typename T>
    T& at(uint32_t offset)
    {
        ASSERT(!(offset % alignof(T)));
        return *reinterpret_cast<T*>(m_bytes.mutableSpan().subspan(offset, sizeof(T)).data());
    }

    uint32_t append(std::span<const uint8_t> bytes, size_t alignment = 8)
    {
        auto& candidates = m_appended.add(hashOf(bytes), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
        for (auto [offset, size] : candidates) {
            if (size == bytes.size() && !(offset % alignment) && equalSpans(m_bytes.span().subspan(offset, size), bytes))
                return offset;
        }
        std::optional<uint32_t> offset = tryAllocate(bytes.size(), alignment);
        if (!offset)
            return 0;
        memcpySpan(m_bytes.mutableSpan().subspan(*offset), bytes);
        candidates.append({ *offset, static_cast<uint32_t>(bytes.size()) });
        return *offset;
    }

    template<typename T>
    uint32_t append(std::span<const T> items)
    {
        static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
        return append(asBytes(items), sizeof(T) == 1 ? 8 : alignof(T));
    }

    template<typename T, size_t inlineCapacity>
    uint32_t append(const Vector<T, inlineCapacity>& items) { return append(items.span()); }

    uint32_t appendUnshared(std::span<const uint8_t> bytes, size_t alignment = 1)
    {
        std::optional<uint32_t> offset = tryAllocate(bytes.size(), alignment);
        if (!offset)
            return 0;
        memcpySpan(m_bytes.mutableSpan().subspan(*offset), bytes);
        return *offset;
    }

    template<typename T>
    uint32_t write(uint32_t offset, std::span<const T> items)
    {
        static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
        ASSERT(!(offset % alignof(T)));
        memcpySpan(m_bytes.mutableSpan().subspan(offset), asBytes(items));
        return offset + static_cast<uint32_t>(items.size_bytes());
    }

    size_t size() const { return m_bytes.size(); }

    Vector<uint8_t> take()
    {
        m_appended.clear();
        if (m_didOverflow)
            return { };
        return std::exchange(m_bytes, { });
    }

private:
    std::optional<uint32_t> tryAllocate(size_t size, size_t alignment)
    {
        size_t end = m_bytes.size();
        size_t offset = WTF::roundUpToMultipleOf(alignment, end);
        if (!m_didOverflow)
            m_didOverflow = !checkSectionSize(offset + size, m_build);
        if (m_didOverflow)
            return std::nullopt;
        growSectionTo(m_bytes, offset + size);
        zeroSpan(m_bytes.mutableSpan().subspan(end));
        return static_cast<uint32_t>(offset);
    }

    static uint64_t hashOf(std::span<const uint8_t> bytes)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : bytes)
            hash = (hash ^ byte) * 1099511628211ull;
        return hash >> 1 | 1;
    }

    BuildContext& m_build;
    Vector<uint8_t> m_bytes;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_appended;
    bool m_didOverflow { false };
};

struct ProgramTableInputs {
    const PipelineOutput& pipeline;
    InstanceSizes instanceSizes;
    const StringConstantNumbering& stringConstants;
    const BodyFactsForTables& bodyFacts;
};

// stringsOnlyShown are the named strings that only C++ reads out of the tables, and seldom: for a message, a stack trace, the name of a function, the path of a module.
struct EncodedProgramTables {
    Vector<uint8_t> bytes;
    BitVector namedStrings;
    BitVector stringsOnlyShown;
};

JS_EXPORT_PRIVATE EncodedProgramTables encodeProgramTables(const ProgramTableInputs&, BuildContext&);

#define FOR_EACH_AOT_PROGRAM_TABLE_RULE(v) \
    v(TableOutOfBounds) \
    v(TableNotWritten) \
    v(TableNotSorted) \
    v(NameWithoutIdentifier) \
    v(StringWithoutConstantNumber) \
    v(TypeNotEncoded) \
    v(DataNotEncoded) \
    v(FunctionWithoutEntrypoint) \
    v(FunctionNotListed) \
    v(ClassCapturesMismatch) \
    v(UnknownFieldFlag) \
    v(ShapeWithoutLayout) \
    v(NameLeftInObfuscatedBuild) \
    v(KeyWithoutPromisedColumn) \
    v(FieldMissingFromFieldTable) \
    v(FieldTableEntryWithoutField) \
    v(MemberMissingFromClassMemberTable) \
    v(ClassMemberEntryWithoutMember) \
    v(GuardMissingFromSet)

enum class ProgramTableRule : uint32_t {
    None,
#define AOT_PROGRAM_TABLE_RULE(name) name,
    FOR_EACH_AOT_PROGRAM_TABLE_RULE(AOT_PROGRAM_TABLE_RULE)
#undef AOT_PROGRAM_TABLE_RULE
};

JS_EXPORT_PRIVATE ASCIILiteral nameOf(ProgramTableRule);

// The record that a row of a table is written for.
struct RowOwner {
    RowOwner() = default;
    RowOwner(FunctionID id)
        : function(id)
    {
    }
    RowOwner(LayoutID id)
        : layout(id)
    {
    }
    RowOwner(VariableID id)
        : variable(id)
    {
    }
    RowOwner(ModuleID id)
        : module(id)
    {
    }

    FunctionID function;
    LayoutID layout;
    VariableID variable;
    ModuleID module;
};

constexpr size_t elementSizeOf(ProgramTableIndex index)
{
    switch (index) {
#define AOT_PROGRAM_TABLE_ELEMENT_SIZE(accessor, ElementType) \
    case ProgramTableIndex::accessor: \
        return sizeof(ElementType);
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_PROGRAM_TABLE_ELEMENT_SIZE)
#undef AOT_PROGRAM_TABLE_ELEMENT_SIZE
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct TableEncoder {
    TableEncoder(const ProgramTableInputs&, BuildContext&);

    uint32_t stringNumber(StringID);
    uint32_t shownStringNumber(StringID);
    // Of a function, a class, a type: an obfuscated build has the empty string in its place.
    uint32_t shownNameNumber(StringID id) { return shownStringNumber(obfuscates() ? StringID() : id); }
    uint32_t stringConstantNumber(StringID);
    // The message of a failure reads: the name is <use> <owner>.
    uint32_t identifierNumber(PropertyKeyID, ASCIILiteral use, RowOwner = { }) const;
    uint32_t identifierNumberOfText(StringID, ASCIILiteral use, RowOwner = { }) const;
    uint32_t typeOffset(TypeID) const;
    uint32_t dataNodeOf(DataID) const;
    FunctionID findEntrypoint(FunctionID) const;
    // The message of a failure reads: the function is <use> <owner>.
    uint32_t entrypointOf(FunctionID, ASCIILiteral use, RowOwner = { }) const;
    uint32_t checkedOffset(uint32_t offset) const;
    bool obfuscates() const { return build.options().obfuscates(); }

    uint32_t numberOrFail(std::optional<uint32_t> number, ProgramTableRule rule) const
    {
        if (!number)
            fail(rule);
        return number.value_or(0);
    }

    template<typename T>
    uint32_t setTable(ProgramTableIndex index, std::span<const T> items)
    {
        RELEASE_ASSERT(sizeof(T) == elementSizeOf(index));
        uint32_t offset = writer.append(items);
        setTableRange(index, offset, static_cast<uint32_t>(items.size()));
        return offset;
    }

    template<typename T, size_t inlineCapacity>
    uint32_t setTable(ProgramTableIndex index, const Vector<T, inlineCapacity>& items) { return setTable(index, items.span()); }

    template<typename T>
    void setTableFrom(ProgramTableIndex index, const auto& source, NOESCAPE const auto& elementOf)
    {
        Vector<T> items;
        items.reserveInitialCapacity(std::size(source));
        for (const auto& element : source)
            items.append(std::invoke(elementOf, element));
        setTable(index, items);
    }

    void setTableRange(ProgramTableIndex index, uint32_t offset, uint32_t count)
    {
        header.ranges[static_cast<unsigned>(index)] = { offset, count };
        writtenTables.set(static_cast<unsigned>(index));
    }

    void fail(ProgramTableRule, LayoutID = { }, FunctionID = { }, const String& circumstances = { }) const;

    const ProgramTableInputs& inputs;
    const Program& program;
    const FlowResults& flow;
    const SlotPlan& slots;
    const Placement& placement;
    const IdentifierNumbering& identifiers;
    BuildContext& build;
    TableWriter writer;
    ProgramTables header { };
    BitVector writtenTables;
    BitVector namedStrings;
    BitVector stringsWithOtherUses;
    Vector<uint32_t> typeOffsets;
    Vector<uint32_t> dataNodes;
    Vector<std::optional<uint32_t>> tablesInImage;
    // By the index of a class: the classes whose assumed base it is, ascending.
    Vector<Vector<uint32_t>> subclasses;
};

// Every table but the header, in the order in which they lie in the image. Ends behind the first group that reports an error.
void encodeTables(TableEncoder&);

void encodeTypeTables(TableEncoder&);
void encodeLayoutTables(TableEncoder&);
void encodePropertyRunTable(TableEncoder&);
void encodeFieldTable(TableEncoder&);
void encodeSiteNames(TableEncoder&);
void encodeMethodTable(TableEncoder&);
void encodeStaticsTables(TableEncoder&);
void encodeFunctionTables(TableEncoder&);
void encodeModuleTables(TableEncoder&);
void encodeDataTable(TableEncoder&);
void encodeRequestTable(TableEncoder&);
void encodeConstantTables(TableEncoder&);

SlotData placedVariableSlot(const TableEncoder&, VariableID);

inline uint32_t numberInStaticsOf(const PlacedGuardBit& placed)
{
    return MemberGuardAggregates::bitNumber(placed.wordOffset, placed.bit % MemberGuardData::bitsPerWord);
}

struct NumberedFunctionRecords {
    Vector<FunctionRecordData> records;
    Vector<uint8_t> numbers;
    Vector<FunctionRecordByFunctionData> recordsByFunction;
};
NumberedFunctionRecords numberFunctionRecords(std::span<const FunctionRecordData> recordsOfFunctions);

// What the row of a class in the field table has an entry for, in ascending order of names. A method has no getter and no setter. The numbers are cells of the closure table.
struct DeclaredClassMember {
    uint32_t name;
    uint32_t method { 0 };
    uint32_t getter { 0 };
    uint32_t setter { 0 };
};
// No answer where the record of the class does not say all that its prototype object has, in a way that the table can hold.
std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder&, const LayoutData& classLayout);

void checkProgramTables(const TableEncoder&, std::span<const uint8_t> tables);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
