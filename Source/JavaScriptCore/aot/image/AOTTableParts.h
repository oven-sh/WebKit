/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBodyFactsForTables.h"
#include "AOTBuildContext.h"
#include "AOTID.h"
#include "AOTIdentifierNumbering.h"
#include "AOTLayoutData.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTProgramTables.h"
#include "AOTSectionWriter.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

// ImageSection::ProgramTables is written as parts, each by one group of encoders on a thread of its own, and joined in this order.
enum class TablePartID : uint8_t {
    Header,
    Types,
    Data,
    Layouts,
    FieldTable,
    Statics,
    Functions,
    FunctionSources,
    Modules,
    Requests,
    Constants,
    ShapeSets,
};
inline constexpr unsigned numberOfTableParts = 12;
static_assert(static_cast<unsigned>(TablePartID::ShapeSets) + 1 == numberOfTableParts);

class TablePart {
    WTF_MAKE_NONCOPYABLE(TablePart);
public:
    TablePart(TablePartID, BuildContext&);
    TablePart(TablePart&&) = default;

    TablePartID id() const { return m_id; }
    size_t size() const { return m_bytes.size(); }

    uint32_t allocate(size_t size, size_t alignment = 8);
    uint32_t allocateUnfilled(size_t size, size_t alignment = 8);
    std::span<uint8_t> bytesAt(uint32_t offset, size_t size) { return m_bytes.mutableSpan().subspan(offset, size); }
    void reserveRoomFor(uint64_t furtherBytes);

    template<typename T>
    T& at(uint32_t offset)
    {
        ASSERT(!(offset % alignof(T)));
        return *reinterpret_cast<T*>(m_bytes.mutableSpan().subspan(offset, sizeof(T)).data());
    }

    uint32_t append(std::span<const uint8_t>, size_t alignment = 8);

    template<typename T>
    uint32_t append(std::span<const T> items)
    {
        static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
        return append(asBytes(items), sizeof(T) == 1 ? 8 : alignof(T));
    }

    template<typename T, size_t inlineCapacity>
    uint32_t append(const Vector<T, inlineCapacity>& items) { return append(items.span()); }

    uint32_t appendUnshared(std::span<const uint8_t>, size_t alignment = 1);

    uint32_t appendOffsets(std::span<const uint32_t>, TablePartID target);
    uint32_t appendOffsets(std::span<const uint32_t> offsets) { return appendOffsets(offsets, m_id); }

    template<typename T>
    uint32_t write(uint32_t offset, std::span<const T> items)
    {
        static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
        ASSERT(!(offset % alignof(T)));
        memcpySpan(m_bytes.mutableSpan().subspan(offset), asBytes(items));
        return offset + static_cast<uint32_t>(items.size_bytes());
    }

    void link(uint32_t offsetOfWord, TablePartID target) { m_links[static_cast<unsigned>(target)].append(offsetOfWord); }
    void link(uint32_t offsetOfWord) { link(offsetOfWord, m_id); }
    void linkWords(uint32_t offsetOfFirstWord, size_t count, TablePartID target)
    {
        for (size_t index = 0; index < count; ++index)
            link(offsetOfFirstWord + static_cast<uint32_t>(index * sizeof(uint32_t)), target);
    }
    void linkWords(uint32_t offsetOfFirstWord, size_t count) { linkWords(offsetOfFirstWord, count, m_id); }

    template<typename Record>
    void link(uint32_t offsetOfRecord, uint32_t Record::* field, TablePartID target)
    {
        const Record* none = nullptr;
        link(offsetOfRecord + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&(none->*field))), target);
    }
    template<typename Record>
    void link(uint32_t offsetOfRecord, uint32_t Record::* field) { link(offsetOfRecord, field, m_id); }

    template<typename Record>
    void linkInEach(uint32_t offsetOfFirstRecord, size_t count, uint32_t Record::* field, TablePartID target)
    {
        for (size_t index = 0; index < count; ++index)
            link(offsetOfFirstRecord + static_cast<uint32_t>(index * sizeof(Record)), field, target);
    }
    template<typename Record>
    void linkInEach(uint32_t offsetOfFirstRecord, size_t count, uint32_t Record::* field) { linkInEach(offsetOfFirstRecord, count, field, m_id); }

    template<typename T>
    uint32_t setTable(ProgramTableIndex index, std::span<const T> items)
    {
        uint32_t offset = append(items);
        setTableRange(index, offset, static_cast<uint32_t>(items.size()));
        return offset;
    }
    template<typename T, size_t inlineCapacity>
    uint32_t setTable(ProgramTableIndex index, const Vector<T, inlineCapacity>& items) { return setTable(index, items.span()); }
    template<typename T>
    uint32_t setTableInPlace(ProgramTableIndex index, size_t count, const Invocable<void(std::span<T>)> auto& write)
    {
        static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
        const size_t alignment = sizeof(T) == 1 ? 8 : alignof(T);
        const size_t size = count * sizeof(T);
        const size_t endInFront = m_bytes.size();
        uint32_t offset = allocateUnfilled(size, alignment);
        write(spanReinterpretCast<T>(bytesAt(offset, size)));
        offset = shareBytesWrittenInPlace(endInFront, offset, size, alignment);
        setTableRange(index, offset, static_cast<uint32_t>(count));
        return offset;
    }
    void setTableRange(ProgramTableIndex, uint32_t offset, uint32_t count);

private:
    friend Vector<uint8_t> joinTableParts(std::span<TablePart>, ProgramTables& header, BuildContext&);

    struct WrittenTable {
        ProgramTableIndex index;
        uint32_t offset;
        uint32_t count;
    };

    enum class NewBytes : bool { AreZero, AreLeftToCaller };
    std::optional<uint32_t> tryAllocate(size_t size, size_t alignment, NewBytes = NewBytes::AreZero);
    uint32_t appendSharedAmong(unsigned kindOfBytes, std::span<const uint8_t>, size_t alignment);
    uint32_t shareBytesWrittenInPlace(size_t endInFront, uint32_t offset, size_t size, size_t alignment);

    TablePartID m_id;
    BuildContext& m_build;
    Vector<uint8_t> m_bytes;
    std::array<Vector<uint32_t>, numberOfTableParts> m_links;
    Vector<WrittenTable> m_tables;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_appended;
    bool m_didOverflow { false };
};

Vector<uint8_t> joinTableParts(std::span<TablePart>, ProgramTables& header, BuildContext&);

inline uint64_t startInJoinedTables(std::span<const TablePart> parts, TablePartID id)
{
    uint64_t end = 0;
    for (const TablePart& part : parts.first(static_cast<unsigned>(id)))
        end = roundUpToMultipleOf<8>(end) + part.size();
    return roundUpToMultipleOf<8>(end);
}

struct AnswersAboutModules {
    enum class Answer : uint8_t {
        HasCompiledBody = 1 << 0,
        IsNeverEvaluated = 1 << 1,
        IsBodyEnteredByRunTime = 1 << 2,
        IsNotWorthVisiting = 1 << 3,
        IsUncompiledCode = 1 << 4,
    };

    struct Directory {
        uint32_t parent;
        StringID name;
    };

    bool has(ModuleID id, Answer answer) const { return ofModules[id.index()].contains(answer); }

    Vector<OptionSet<Answer>> ofModules;
    Vector<VariableID> variablesThatHoldMocks;
    Vector<VariableID> instanceVariables;
    Vector<VariableID> instanceGuards;
    Vector<VariableID> instanceAllocators;
    Vector<VariableID> severalInstancesVariables;
    Vector<Directory> directories;
    Vector<uint32_t> directoryOf;
    Vector<StringID> pathOrNameOf;
    Vector<uint32_t> modulesOfUncompiledCode;
    Vector<uint32_t> compiledRuntimeFunctions;
};

class PreparedFunctionSources;

struct TableInputs {
    const Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const LinkedFunctions& functions;
    const BodyFactsForTables& bodyFacts;
    const CompileOptions& options;
    const BuildContext* build { nullptr };
    const PreparedFunctionSources* functionSources { nullptr };
    mutable std::optional<AnswersAboutModules> answersAboutModules { };
};

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

struct FieldsOfLayoutRecord {
    Vector<FieldData> fields;
    Vector<uint32_t> operands;
    LayoutData flags { };
};

struct FirstTableParts {
    static constexpr uint32_t startOfTypes = roundUpToMultipleOf<8>(sizeof(ProgramTables));
    Vector<uint32_t> typeOffsets;
    static constexpr uint32_t mostShiftOfTypeNumbers = 16;
    uint32_t shiftOfTypeNumbers { ProgramTables::shiftOfNearTypeNumbers };
    Vector<uint32_t> dataNodes;
    Vector<uint32_t> tablesInImage;
    Vector<uint64_t> shapeSetWords;
    Vector<FieldsOfLayoutRecord> fieldsOfLayoutRecords;
};

struct TableEncoder {
    TableEncoder(const TableInputs&, const ImageNumbers&, const FirstTableParts*, TablePartID, BuildContext&);

    uint32_t stringNumber(StringID) const;
    uint32_t shownNameNumber(StringID id) const { return stringNumber(obfuscates() ? StringID() : id); }
    uint32_t stringConstantNumber(StringID) const;
    uint32_t identifierNumber(PropertyKeyID, ASCIILiteral use, RowOwner = { }) const;
    uint32_t identifierNumberOfText(StringID, ASCIILiteral use, RowOwner = { }) const;
    uint32_t identifierOrStringNumber(StringID) const;
    uint32_t typeOffset(TypeID) const;
    uint16_t typeNumber(TypeID) const;
    uint32_t dataNodeOf(DataID) const;
    uint32_t entrypointOf(FunctionID, ASCIILiteral use, RowOwner = { }) const;
    std::optional<FunctionID> findEntrypoint(FunctionID) const;
    bool hasCode(CodeUnit) const;
    bool obfuscates() const { return build.options().obfuscates(); }
    COLD NEVER_INLINE void fail(ASCIILiteral rule, RowOwner = { }, const String& circumstances = { }) const;

    const TableInputs& inputs;
    const Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const ImageNumbers& numbers;
    const FirstTableParts* firstParts;
    BuildContext& build;
    TablePart part;
    Vector<uint32_t> classesByParent;
    Vector<uint32_t> firstClassBelow;
    BitVector sidesWithClassGuardsHereOrBelow;
    Typed::BitVector classesWithRecordsAllTheWayUp;
    Typed::BitVector classesWhoseClassObjectsFindNoHasInstance;
    BitVector classesWhosePrivateNamesTextsKnow;
};

bool showsVariablesOf(const TableInputs&, ModuleID);

void nameWhatTypeTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeTypeTables(TableEncoder&, FirstTableParts&);
std::optional<uint32_t> findShiftOfTypeNumbers(const TablePart&, std::span<const uint32_t> firstWords);
uint32_t placeTypeRecord(TablePart&, std::span<const uint8_t> record, size_t unit);
void nameWhatDataTableNames(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeDataTable(TableEncoder&, FirstTableParts&);
void nameWhatLayoutTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeLayoutTables(TableEncoder&);
void encodeFieldTable(TableEncoder&);
void nameWhatStaticsTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void nameVariablesThatHeapSnapshotsShow(const TableInputs&, const GatheredMentions&, const ParallelLoops&, NamesOfTables&);
void encodeStaticsTables(TableEncoder&);
void encodeMethodTableDependents(TableEncoder&);
namespace Typed {
struct MethodFamily;
struct MethodTableRow;
}
std::array<uint32_t, 4> keyOfClassGuardUnderMethodTableRow(const Typed::MethodFamily&, const Typed::MethodTableRow&);
void nameWhatFunctionTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeFunctionProgramTables(TableEncoder&);
void encodeFunctionSources(TableEncoder&);
void nameWhatModuleTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeModuleTables(TableEncoder&);
void nameWhatRequestTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeRequestTables(TableEncoder&);
void nameWhatConstantTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeConstantTables(TableEncoder&);
void encodeShapeSetBits(TableEncoder&);

struct DeclaredClassMember {
    uint32_t name;
    uint32_t method { 0 };
    uint32_t getter { 0 };
    uint32_t setter { 0 };
};
std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder&, LayoutID classLayout);
struct ClassRecordParts {
    Vector<MethodData> methods;
    Vector<FieldData> staticFields;
    std::array<Vector<uint32_t>, numberOfClassSides> namedGuards;
    LayoutID parentThatListsClass;
    ClassData data { };
};
void askRowsOfClassesForClassRecords(TableEncoder&);
ClassRecordParts collectClassRecordParts(const TableEncoder&, LayoutID classLayout);
Vector<ClassBelowWithGuardOfNameData> collectClassesBelowWithGuardsOfNames(const TableEncoder&);
Vector<AssignedMemberData> collectAssignedMembers(const TableEncoder&);
void groupClassesByParentInClassRecords(TableEncoder&, std::span<const LayoutID> parentsThatListClasses);
void appendListsOfClassRecord(TableEncoder&, LayoutID classLayout, const std::array<Vector<uint32_t>, numberOfClassSides>& namedGuards, ClassData&);

Vector<FieldsOfLayoutRecord> collectFieldsOfLayoutRecords(const TableEncoder&);

NamesOfTables nameWhatTablesName(const TableInputs&, const GatheredMentions&, BuildContext&);
Vector<uint8_t> encodeProgramTables(const TableInputs&, ImageNumbers&, std::array<uint64_t, numberOfTableParts>& bytesOfParts, BuildContext&);

void checkProgramTables(const TableInputs&, const ImageNumbers&, std::span<const uint8_t> tables, BuildContext&);

void internPiecesOfModulePaths(Typed::Program&);
void hidePathsOfModules(Typed::Program&, const Typed::Facts&, const Typed::Plan&, const BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
