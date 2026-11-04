/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
#include <wtf/BitVector.h>
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
    // Born with room for the ProgramTables, which joinTableParts() writes there. No group puts anything into it, and no offset leads into it.
    Header,
    Types,
    Data,
    Layouts,
    FieldTable,
    Statics,
    Functions,
    FunctionTexts,
    Modules,
    Requests,
    Constants,
    // The last: the image builder takes it off the tables and puts it in front of the constants of the code.
    ShapeSets,
};
inline constexpr unsigned numberOfTableParts = 12;
static_assert(static_cast<unsigned>(TablePartID::ShapeSets) + 1 == numberOfTableParts);

// Offsets count from the start of the part while it is written. The run time's offsets count from the start of the tables.
// So every word that holds an offset is linked: joinTableParts() adds the start of the part that the offset leads into.
// A word that is 0 stays 0, and no part has anything at its offset 0.
class TablePart {
    WTF_MAKE_NONCOPYABLE(TablePart);
public:
    TablePart(TablePartID, BuildContext&);
    TablePart(TablePart&&) = default;

    TablePartID id() const { return m_id; }
    size_t size() const { return m_bytes.size(); }

    // Zero-filled. A part begins at a multiple of eight, so no alignment, here or below, is above eight.
    uint32_t allocate(size_t size, size_t alignment = 8);
    // Whoever asks writes every byte, through bytesAt(), on as many threads as it likes.
    uint32_t allocateUnfilled(size_t size, size_t alignment = 8);
    // They stay where they are until the part is asked for room again.
    std::span<uint8_t> bytesAt(uint32_t offset, size_t size) { return m_bytes.mutableSpan().subspan(offset, size); }
    // A part that is told how much is still to come is not moved while it grows.
    void reserveRoomFor(uint64_t furtherBytes);

    template<typename T>
    T& at(uint32_t offset)
    {
        ASSERT(!(offset % alignof(T)));
        return *reinterpret_cast<T*>(m_bytes.mutableSpan().subspan(offset, sizeof(T)).data());
    }

    // Equal bytes with the same alignment are written once. Link a word of them only where every owner of equal bytes in the part links it too.
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

    // Words that each hold an offset into the target, or 0. Links them. Equal words for one target are written once, and never share bytes with what append() was handed.
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

    // The 32-bit word at the offset holds an offset into the target, or 0.
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

    // A table of the header. The range counts from the start of the part until the parts are joined.
    template<typename T>
    uint32_t setTable(ProgramTableIndex index, std::span<const T> items)
    {
        uint32_t offset = append(items);
        setTableRange(index, offset, static_cast<uint32_t>(items.size()));
        return offset;
    }
    template<typename T, size_t inlineCapacity>
    uint32_t setTable(ProgramTableIndex index, const Vector<T, inlineCapacity>& items) { return setTable(index, items.span()); }
    // As setTable() of the items that `write` leaves in the room it is handed: the table lies where setTable() would have put it, and equal bytes are still held once.
    // `write` fills the room to the last byte, on as many threads as it likes, and asks the part for nothing.
    template<typename T>
    uint32_t setTableWrittenInPlace(ProgramTableIndex index, size_t count, const Invocable<void(std::span<T>)> auto& write)
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
    // 0: what append() was handed. 1 + target: offsets into the target. Bytes of two kinds never share a copy.
    uint32_t appendSharedAmong(unsigned kindOfBytes, std::span<const uint8_t>, size_t alignment);
    // The bytes are the last of the part, which ended at `endInFront` before it made room for them. Answers what append() would have answered had it been handed them there, and leaves the part as append() would have.
    uint32_t shareBytesWrittenInPlace(size_t endInFront, uint32_t offset, size_t size, size_t alignment);

    TablePartID m_id;
    BuildContext& m_build;
    Vector<uint8_t> m_bytes;
    std::array<Vector<uint32_t>, numberOfTableParts> m_links;
    Vector<WrittenTable> m_tables;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_appended;
    bool m_didOverflow { false };
};

// In the order of TablePartID. Each part begins at a multiple of eight, and nothing follows the last. Links that are listed twice count once. Applies the links in parallel.
// The ranges of the header are filled from setTableRange() of all parts, and the header is copied to the start of the tables last. The parts keep their sizes.
// Fails with InternalErrorKind::ProgramTableCheckFailed where a table of the header was written by no part or by two, or where a word of bytes that append() handed out several times is linked by fewer owners than have them.
// The answer is empty after an overflow. After a broken rule it is the joined bytes, and the diagnostics have the error.
Vector<uint8_t> joinTableParts(std::span<TablePart>, ProgramTables& header, BuildContext&);

// Where joinTableParts() puts the part.
inline uint64_t startInJoinedTables(std::span<const TablePart> parts, TablePartID id)
{
    uint64_t end = 0;
    for (const TablePart& part : parts.first(static_cast<unsigned>(id)))
        end = roundUpToMultipleOf<8>(end) + part.size();
    return roundUpToMultipleOf<8>(end);
}

// What nameWhatModuleTablesName() and encodeModuleTables() both ask of the program, Facts, the plan and the code, and of no number of the image. askAboutModules() of AOTModuleLists.cpp asks once, on all threads.
struct AnswersAboutModules {
    enum class Answer : uint8_t {
        // The body, its start or the entry that the run time calls has code.
        HasCompiledBody = 1 << 0,
        // The plan has the body entered by whoever evaluates the module, and nothing that has code or that the host calls names it.
        IsNeverEvaluated = 1 << 1,
        IsBodyEnteredByRunTime = 1 << 2,
        // Neither the module nor anything that its requests lead to runs anything.
        IsNotWorthVisiting = 1 << 3,
        IsUncompiledCode = 1 << 4,
    };

    struct Directory {
        // In `directories`. Number24::max: none.
        uint32_t parent;
        StringID name;
    };

    bool has(ModuleID id, Answer answer) const { return ofModules[id.index()].contains(answer); }

    // [ModuleID]
    Vector<OptionSet<Answer>> ofModules;
    // [ModuleID]. Null: none.
    Vector<VariableID> variablesThatHoldMocks;
    // A test run, or a program with a module that may be mocked.
    bool mayMocksStandInPlaceOfModules { false };
    // A directory stands behind the one above it, and the directories stand in the order in which the paths of the modules come to them.
    Vector<Directory> directories;
    // [ModuleID]. In `directories`. Number24::max: the path is one string.
    Vector<uint32_t> directoryOf;
    // [ModuleID]. The path. Of a module that has a directory: what stands behind the last slash of it.
    Vector<StringID> pathOrNameOf;
    // Ascending.
    Vector<uint32_t> modulesOfUncompiledCode;
    // Places in Program::runtimeFunctions, ascending: the host calls the function, and what it enters has code.
    Vector<uint32_t> compiledRuntimeFunctions;
};

struct TableInputs {
    const Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const LinkedFunctions& functions;
    const BodyFactsForTables& bodyFacts;
    const CompileOptions& options;
    // A nameWhat...() asks it for the loops of the lane that it runs in.
    const BuildContext* build { nullptr };
    // nameWhatModuleTablesName() leaves them for encodeModuleTables(), which takes them. No other group touches them.
    mutable std::optional<AnswersAboutModules> answersAboutModules { };
};

// The record that a row of a table is written for, for the message of a failure.
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

// What the record of an object layout says of its fields, in the order of the record. FieldData::type is an offset in TablePartID::Types.
struct FieldsOfLayoutRecord {
    Vector<FieldData> fields;
    Vector<uint32_t> operands;
    // Only the members that the fields decide.
    LayoutData flags { };
};

// What the groups that run second read of those that ran first.
struct FirstTableParts {
    // [place in ImageNumbers::typesNeededAtRunTime()]: the offset from the start of TablePartID::Types.
    Vector<uint32_t> typeOffsets;
    // [DataID]: the index of the node's first word in ProgramTables::data(), which is no offset and is never linked. 0 where the data has no node.
    Vector<uint32_t> dataNodes;
    // [place in ImageNumbers::requests()]: the offset of the TableInImage of a RequestKind::CollectionData from the start of TablePartID::Data. 0 where the request has none.
    Vector<uint32_t> tablesInImage;
    // The table shapeSetBits, whole. ShapeTestData::bitSetOffset of every row of layoutSets is a location in it: PagedBitSet::locationOf().
    Vector<uint64_t> shapeSetWords;
    // [LayoutID]. Nothing where the layout has no record of an object. collectFieldsOfLayoutRecords().
    Vector<FieldsOfLayoutRecord> fieldsOfLayoutRecords;
};

// One for each group, on its thread.
struct TableEncoder {
    TableEncoder(const TableInputs&, const ImageNumbers&, const FirstTableParts*, TablePartID, BuildContext&);

    uint32_t stringNumber(StringID) const;
    // For a table that lists literal strings. One of ImageNumbers::literalStringsWithoutText() has a number and no record, so whoever reads such a table asks ImageText::hasText().
    uint32_t numberOfLiteralString(StringID id) const { return numbers.literalStringsWithoutText().get(id.index()) ? id.index() : stringNumber(id); }
    // Of a function, a class, a type: an obfuscated build has the empty string in its place.
    uint32_t shownNameNumber(StringID id) const { return stringNumber(obfuscates() ? StringID() : id); }
    uint32_t stringConstantNumber(StringID) const;
    // The message of a failure reads: the name is <use> <owner>.
    uint32_t identifierNumber(PropertyKeyID, ASCIILiteral use, RowOwner = { }) const;
    uint32_t identifierNumberOfText(StringID, ASCIILiteral use, RowOwner = { }) const;
    // In TablePartID::Types: whoever stores the answer links the word to that part. Through ImageNumbers::placeOfType(). A type without a place stops the build: its nameWhat...() left it out.
    uint32_t typeOffset(TypeID) const;
    // The index of the node's first word in ProgramTables::data(), as DataRequest::root, CollectionDataRequest::elements and DataWord::encodeNode() hold it. No offset: it is never linked. Data without a node stops the build.
    uint32_t dataNodeOf(DataID) const;
    // The FunctionID in the image of the entry that a closure word of the function names. The message of a failure reads: the function is <use> <owner>.
    uint32_t entrypointOf(FunctionID, ASCIILiteral use, RowOwner = { }) const;
    std::optional<FunctionID> findEntrypoint(FunctionID) const;
    bool hasCode(CodeUnit) const;
    bool obfuscates() const { return build.options().obfuscates(); }
    void fail(ASCIILiteral rule, RowOwner = { }, const String& circumstances = { }) const;

    const TableInputs& inputs;
    const Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const ImageNumbers& numbers;
    const FirstTableParts* firstParts;
    BuildContext& build;
    TablePart part;
    // groupClassesByParentInClassRecords() fills both. The classes that have a rank, grouped by the class that their record names as parent (ClassData::base or expectedBase), each group ascending.
    Vector<uint32_t> classesByParent;
    // [LayoutID], and one more: where the group of a parent begins in classesByParent.
    Vector<uint32_t> firstClassBelow;
};

// A heap snapshot names the variables of the files of the program: those of environments (AOTLayoutTables.cpp), of modules (AOTStaticsTables.cpp) and what closures hold of them (AOTFunctionProgramTables.cpp).
bool showsVariablesOf(const TableInputs&, ModuleID);

// Each group is two functions in one file. The first says what its tables are going to name, so that Image can number it. It reads the same records in the same way as the second and writes nothing.
// A name or a string that the second asks for and the first left out stops the build: TableEncoder::identifierNumber(), stringNumber().

// AOTTypeTables.cpp
void nameWhatTypeTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeTypeTables(TableEncoder&, FirstTableParts&);
// AOTDataTable.cpp
void nameWhatDataTableNames(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeDataTable(TableEncoder&, FirstTableParts&);
// AOTLayoutTables.cpp, AOTClassRecords.cpp
void nameWhatLayoutTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeLayoutTables(TableEncoder&);
// AOTFieldTableEncoder.cpp
void encodeFieldTable(TableEncoder&);
// AOTStaticsTables.cpp
void nameWhatStaticsTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
// Behind every group, over what all of them have named.
void nameVariablesThatHeapSnapshotsShow(const TableInputs&, const GatheredMentions&, const ParallelLoops&, NamesOfTables&);
void encodeStaticsTables(TableEncoder&);
// AOTMethodTable.cpp. In the part of the statics, where the bits of class guards lie: methodTableDependents.
void encodeMethodTableDependents(TableEncoder&);
// The key in NumberSpace::ClassGuard of the guard that a row with a guardedClass stands on. Code that mentions a selector of the family mentions the guard with it: gatherMentions().
namespace Typed {
struct MethodFamily;
struct MethodTableRow;
}
std::array<uint32_t, 4> keyOfClassGuardUnderMethodTableRow(const Typed::MethodFamily&, const Typed::MethodTableRow&);
// AOTFunctionProgramTables.cpp
void nameWhatFunctionTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeFunctionProgramTables(TableEncoder&);
// AOTFunctionTexts.cpp
void encodeFunctionTexts(TableEncoder&);
// AOTModuleTables.cpp, AOTModuleLists.cpp
void nameWhatModuleTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeModuleTables(TableEncoder&);
// AOTRequestTables.cpp
void nameWhatRequestTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeRequestTables(TableEncoder&);
// AOTConstantTables.cpp
void nameWhatConstantTablesName(const TableInputs&, const GatheredMentions&, NamesOfTables&);
void encodeConstantTables(TableEncoder&);
// AOTShapeSetTable.cpp
void encodeShapeSetBits(TableEncoder&);

// AOTClassRecords.cpp, for AOTLayoutTables.cpp and AOTFieldTableEncoder.cpp.
// What the row of a class in the field table has an entry for, in ascending order of names. A method has no getter and no setter. The numbers are cells of the closure table.
struct DeclaredClassMember {
    uint32_t name;
    uint32_t method { 0 };
    uint32_t getter { 0 };
    uint32_t setter { 0 };
};
// No answer where the record of the class does not say all that its prototype object has, in a way that the table can hold.
std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder&, LayoutID classLayout);
// What the record of a layout of the kind Instance has beyond that of any other object layout. AOTLayoutTables.cpp puts `data` in front of the LayoutData and the lists behind its field operands.
struct ClassRecordParts {
    // The methods of the prototype, then the static ones: ClassData::numberOfMethods, numberOfStaticMethods.
    Vector<MethodData> methods;
    // FieldData::name, and in FieldData::slot the variable where the field lies until the class object is made. Every other member is 0.
    Vector<FieldData> staticFields;
    // By ClassSide: what ClassData::namedGuardsOffset leads to. Empty where code tests no guard of a name of the side.
    std::array<Vector<uint32_t>, numberOfClassSides> namedGuards;
    // The class whose list of subclasses has this one: the parent that the record names. None where this one has no rank or the parent is no class.
    LayoutID parentThatListsClass;
    // subclassesOffset and each of namedGuardsOffset is 0 until appendListsOfClassRecord() has been there.
    ClassData data { };
};
// A group appends on one thread, so the record of a class is made in two halves. This one asks the program, Facts, the plan and the numbers and appends nothing: it runs in a loop, once a layout.
ClassRecordParts collectClassRecordParts(const TableEncoder&, LayoutID classLayout);
// [LayoutID]: ClassRecordParts::parentThatListsClass. In front of the first appendListsOfClassRecord().
void groupClassesByParentInClassRecords(TableEncoder&, std::span<const LayoutID> parentsThatListClasses);
// The other half asks nothing. It appends the lists in front of the record and puts their places into the ClassData: offsets in the part of the encoder, or 0.
void appendListsOfClassRecord(TableEncoder&, LayoutID classLayout, const std::array<Vector<uint32_t>, numberOfClassSides>& namedGuards, ClassData&);

// AOTLayoutTables.cpp. The fields as encodeLayoutTables() writes them, also for encodeFieldTable(), which runs beside it and cannot read its part.
// A group has one thread, and what is asked about a field appends nothing to a part: so it is asked in a loop of its own, behind Types and in front of both groups.
Vector<FieldsOfLayoutRecord> collectFieldsOfLayoutRecords(const TableEncoder&);

// AOTProgramTablesDriver.cpp
// Every nameWhat...() is a lane: runLanes().
NamesOfTables nameWhatTablesName(const TableInputs&, const GatheredMentions&, BuildContext&);
// Every group is a lane, so it may start loops. Beside the function texts: Types and Data beside each other, then the fields of layout records in a loop of their own, then every other group beside the others.
// Then the header and the join. Hands the offsets of the types and the nodes of the data to the numbers.
Vector<uint8_t> encodeProgramTables(const TableInputs&, ImageNumbers&, std::array<uint64_t, numberOfTableParts>& bytesOfParts, BuildContext&);

// AOTProgramTableCheck.cpp. Checking builds only. Behind ImageNumbers::setOffsetsOfTypes(): it compares the offsets that code is linked with against the records.
void checkProgramTables(const TableInputs&, const ImageNumbers&, std::span<const uint8_t> tables, BuildContext&);

// The table of modules keeps the path of a file in pieces: see ModuleDirectoryData. The pieces have to be strings of the program before its strings are numbered. AOTModuleLists.cpp.
void internPiecesOfModulePaths(Typed::Program&);
// An obfuscated executable says nothing about its sources: a module whose path the program can neither read nor find it by is named by its number. In front of internPiecesOfModulePaths().
void hidePathsOfModules(Typed::Program&, const Typed::Facts&, const Typed::Plan&, const BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
