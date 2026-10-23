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
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTProgramTables.h"
#include "AOTStringConstantNumbering.h"
#include "AOTTableWriter.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

// The number by which the run time knows the bit of a guard. The offset is of the word that holds the bit, and the bit counts within its side of the class, which may fill several words.
inline uint32_t numberInStaticsOf(const PlacedGuardBit& placed)
{
    return MemberGuardAggregates::bitNumber(placed.wordOffset, placed.bit % MemberGuardData::bitsPerWord);
}

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
    uint32_t shownNameNumber(StringID);
    uint32_t stringConstantNumber(StringID);
    uint32_t identifierNumber(PropertyKeyID) const;
    uint32_t identifierNumberOfText(StringID) const;
    uint32_t typeOffset(TypeID) const;
    uint32_t dataNodeOf(DataID) const;
    FunctionID findEntrypoint(FunctionID) const;
    uint32_t entrypointOf(FunctionID) const;
    uint32_t checkedOffset(uint32_t offset) const;
    bool obfuscates() const { return build.options().obfuscates(); }

    template<typename T>
    void setTable(ProgramTableIndex index, std::span<const T> items)
    {
        RELEASE_ASSERT(sizeof(T) == elementSizeOf(index));
        uint32_t offset = writer.append(items);
        setTableRange(index, offset, static_cast<uint32_t>(items.size()));
    }

    template<typename T, size_t inlineCapacity>
    void setTable(ProgramTableIndex index, const Vector<T, inlineCapacity>& items) { setTable(index, items.span()); }

    void setTableRange(ProgramTableIndex index, uint32_t offset, uint32_t count)
    {
        header.ranges[static_cast<unsigned>(index)] = { offset, count };
        writtenTables.set(static_cast<unsigned>(index));
    }

    void fail(ProgramTableRule, LayoutID = { }, FunctionID = { }) const;

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
    BitVector layoutsWithSharedName;
    // By the index of a class: the classes whose assumed base it is, ascending.
    Vector<Vector<uint32_t>> subclasses;
};

FastBitVector findUsedTypeSites(const Program&, const ParallelLoops&);
BitVector findModuleBodiesThatOnlyReturn(const Program&);
Vector<ModuleImportSite> findImportSites(const Program&, const ParallelLoops&);
void findModulesNamedByCode(const Program&, const BitVector& bodiesWithoutCode, const ParallelLoops&, BodyFactsForTables&);

void encodeTypeTables(TableEncoder&);
void encodeLayoutTables(TableEncoder&);
void encodePropertyRunTable(TableEncoder&);
void encodeFieldTable(TableEncoder&);
void encodeMethodTable(TableEncoder&);
void encodeStaticsTables(TableEncoder&);
void encodeFunctionTables(TableEncoder&);
void encodeModuleTables(TableEncoder&);
void encodeDataTable(TableEncoder&);
void encodeRequestTable(TableEncoder&);
void encodeConstantTables(TableEncoder&);
void encodeShapeSetBits(TableEncoder&);

// What ProgramTables::recordOfFunction() reads.
struct NumberedFunctionRecords {
    Vector<FunctionRecordData> records;
    Vector<uint8_t> numbers;
    Vector<FunctionRecordByFunctionData> recordsByFunction;
};
NumberedFunctionRecords numberFunctionRecords(std::span<const FunctionRecordData> recordsOfFunctions);

struct ClassRecordParts {
    Vector<MethodData> methods;
    uint32_t numberOfStaticMethods { 0 };
    Vector<FieldData> staticFields;
    Vector<MemberGuardData> memberGuards;
    Vector<MemberGuardAggregates> memberGuardAggregates;
    ClassData data { };
};

// What the row of a class in the field table has an entry for, in ascending order of names. A method has no getter and no setter. The numbers are cells of the closure table.
struct DeclaredClassMember {
    uint32_t name;
    uint32_t method { 0 };
    uint32_t getter { 0 };
    uint32_t setter { 0 };
};
// No answer where the record of the class does not say all that its prototype object has, in a way that the table can hold.
std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder&, const LayoutData& classLayout);

ClassRecordParts collectClassRecordParts(TableEncoder&, LayoutID);
void encodeConstructors(TableEncoder&);
void encodeBoundFunctions(TableEncoder&);

struct DeclarationLocation {
    uint32_t path { noDeclaredPath };
    uint32_t line { 0 };
};

SlotData placedVariableSlot(const TableEncoder&, VariableID);
bool retainsDeclarationLocation(TableEncoder&, LayoutID);
DeclarationLocation declarationLocationOf(TableEncoder&, LayoutID);
uint64_t encodeDataWord(TableEncoder&, const DataNode&);

void checkProgramTables(const TableEncoder&, std::span<const uint8_t> tables);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
