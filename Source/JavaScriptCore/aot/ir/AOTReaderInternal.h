/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTExternalFunctionBodies.h"
#include "AOTIR.h"
#include "AOTLayoutUses.h"
#include "AOTLongExpectations.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include "AOTWireSections.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyDecoder;
class RecordChecker;

using Failure = std::optional<InternalError>;

template<typename External> bool isValidExternal(uint32_t word, const BuiltinTable&);

// A field whose type lists nothing but integers, or nothing but strings, besides undefined and null.
class TellingFields {
public:
    TellingFields(const ParallelLoops&, const Wire::Sections& checkedSections);

    bool tellApart(uint32_t layout, uint32_t otherLayout) const;
    bool excludes(uint32_t layout, uint32_t key, const LayoutTag&) const;

    // Whether the layout declares nothing for the key but literals, no more of them than given, of each of which the functor holds.
    bool declaresOnlyLiterals(uint32_t layout, uint32_t key, size_t maxLiterals, NOESCAPE const Invocable<bool(const LayoutTag&)> auto& isAdmitted) const
    {
        for (const Field& field : fieldsOf(layout)) {
            if (field.key != key)
                continue;
            if (field.nullish)
                return false;
            if (field.holdsStrings) {
                return field.strings.size() <= maxLiterals && std::ranges::all_of(field.strings, [&](uint32_t string) {
                    return isAdmitted(LayoutTag { StringID(string) });
                });
            }
            uint64_t numberAboveMinimum = static_cast<uint64_t>(field.maximum) - static_cast<uint64_t>(field.minimum);
            if (numberAboveMinimum >= maxLiterals)
                return false;
            for (uint64_t offset = 0; offset <= numberAboveMinimum; ++offset) {
                if (!isAdmitted(LayoutTag { static_cast<int64_t>(static_cast<uint64_t>(field.minimum) + offset) }))
                    return false;
            }
            return true;
        }
        return false;
    }

private:
    struct Field {
        uint32_t key;
        uint32_t nullish;
        bool holdsStrings;
        int64_t minimum;
        int64_t maximum;
        std::span<const uint32_t> strings;
    };

    std::span<const Field> fieldsOf(uint32_t layout) const { return m_fields.subspan(m_firstField[layout], m_firstField[layout + 1] - m_firstField[layout]); }
    static bool haveNothingInCommon(const Field&, const Field&);

    Vector<Field> m_fields;
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_bases;
};

class Reader {
    WTF_MAKE_NONCOPYABLE(Reader);
public:
    using TypeCompletion = WTF::Function<void(uint32_t wireIndex, Type&)>;

    Reader(Program&, IRForm, BuildContext&);

    void setTypeCompletion(TypeCompletion&& completion) { m_completeType = WTF::move(completion); }
    void setVerifierInputs(const VerifierInputs& inputs) { m_verifierInputs = inputs; }
    void setExternalFunctionBodies(const ExternalFunctionBodies* bodies) { m_externalBodies = bodies; }
    // For a program that goes on into the pipeline: what the first steps of the read stage do to each function is done where its body is decoded.
    void setPreparesFunctionsForTyping() { m_preparesFunctionsForTyping = true; }
    // For a tool that prints what arrived: a type lists the layouts and the functions that it names, not the set of each.
    void keepLayoutsAsWritten() { m_keepsLayoutsAsWritten = true; }
    bool run();

    IRForm form() const { return m_form; }
    const Program& program() const { return m_program; }
    BuildContext& build() const { return m_build; }
    const BuiltinTable& builtins() const { return m_builtins; }
    const Wire::Sections& sections() const { return m_sections; }

    template<typename IDType> size_t numberOfRecords() const;
    std::span<const TypeID> typesAsRead() const { return m_types.span(); }

private:
    // What the threads make of a type. One thread interns the rest, in the order of the types: the contents of the classes, a set of layouts that is the family of no layout, the type.
    struct TypeAsRead {
        Type type;
        std::optional<LayoutSet> layoutsToIntern;
        uint32_t hashOfLayouts { 0 };
        std::optional<uint32_t> hashOfType;
    };

    bool readHeader();
    bool readNamesAndFamilies();
    bool readStrings();
    bool readPropertyKeys();
    bool computeFamilies();
    bool computeDeclaredMembers();
    void planTaggedLayouts(Vector<Vector<LayoutID>>& members, const TellingFields&);
    Failure computeFunctionFamilies();
    void makeRoomForFunctions();
    bool readTypes();
    bool readTypesOneByOne();
    void internLayoutSetsInOrder(std::span<TypeAsRead>);
    void hashTypeWithoutContents(uint32_t index, TypeAsRead&) const;
    bool readProgramLists();
    bool readLayouts();
    Vector<Failure> readFunctions();
    bool readListsLayoutsAndFunctions();
    bool finishProgram();

    Failure readType(uint32_t index, TypeAsRead&) const;
    TypeID internType(uint32_t index, TypeAsRead&&);
    bool readsAsAnything(std::span<const LayoutID> named) const;
    void addLayouts(TypeAsRead&, std::span<const LayoutID> named, bool hasMembersWithoutLayout) const;
    void addFunctions(Type&, std::span<const FunctionID> named) const;
    Failure readClassPart(RecordChecker&, uint32_t typeIndex, const Wire::ClassPart&, ClassPart&) const;
    struct SeenInBody {
        bool namesObjectCreate { false };
        Vector<FunctionID, 4> directCallees;
    };
    Failure readFunction(uint32_t index, Function&, SeenInBody&) const;
    bool checkTypesAgainstLayouts();
    bool setConstructedClasses();
    void setParameterSiteFunctions();

    template<typename WireRecord, typename Record, size_t inlineCapacity>
    bool readList(std::span<const WireRecord>, Vector<Record, inlineCapacity>&);
    template<typename WireRecord, typename Record, size_t inlineCapacity>
    bool readListInParallel(std::span<const WireRecord>, Vector<Record, inlineCapacity>&);
    static Vector<Failure> readInChunks(const ParallelLoops&, size_t recordCount, size_t recordsPerChunk, NOESCAPE const Invocable<Failure(uint32_t)> auto& readRecord);
    // One thread constructs the records. All threads take the page faults first.
    template<typename Record, size_t inlineCapacity>
    static void growFromEmpty(const ParallelLoops&, Vector<Record, inlineCapacity>&, size_t size);

    bool report(const InternalError&);
    bool reportFirst(std::span<const Failure>);
    static InternalError inRecord(uint32_t index) { return { .kind = InternalErrorKind::Unknown, .count = index }; }

    Program& m_program;
    IRForm m_form;
    BuildContext& m_build;
    const BuiltinTable& m_builtins;
    std::span<const uint8_t> m_buffer;
    const Wire::Header* m_header { nullptr };
    Wire::Sections m_sections;
    Vector<TypeID> m_types;
    Vector<LayoutSetID> m_declaredMembers;
    BitVector m_typeParametersWithoutFamily;
    BitVector m_viewsWithoutLayouts;
    Vector<CalleeSetID> m_functionFamilies;
    uint32_t m_numberOfDerivedLayouts { 0 };
    TypeCompletion m_completeType;
    bool m_keepsLayoutsAsWritten { false };
    VerifierInputs m_verifierInputs;
    const ExternalFunctionBodies* m_externalBodies { nullptr };
    std::optional<LayoutUseCollector> m_layoutUses;
    bool m_preparesFunctionsForTyping { false };
    std::optional<LongExpectations> m_longExpectations;
    // AllocateTaggedLayouts looks at them as they were read.
    BitVector m_constructorsOfTaggedFamilies;
};

class RecordChecker {
public:
    RecordChecker(const Reader& reader, InternalError subject)
        : m_reader(reader)
        , m_subject(subject)
    {
    }

    const Reader& reader() const { return m_reader; }
    bool acceptsEngineMembers() const { return m_reader.form() != IRForm::AsRead; }
    void setIDRemap(const UnitIDRemap* remap) { m_remap = remap; }

    bool hasFailed() const { return m_failure.has_value(); }
    Failure takeFailure() { return std::exchange(m_failure, std::nullopt); }
    void fail(InternalErrorKind);
    void require(bool condition, InternalErrorKind);
    void requireAscending(std::strong_ordering neighborOrder);
    template<typename T> void requireAscending(const T& previous, const T& next) { requireAscending(previous <=> next); }
    void requireAscendingStrings(StringID previous, StringID next);

    template<typename IDType> IDType id(uint32_t word);
    template<typename IDType> IDType optionalID(uint32_t word);
    // What id() answers for a valid word. It reports nothing.
    template<typename IDType> uint32_t checkedID(uint32_t word, bool& isValid) const;
    template<typename External> External external(uint32_t word);
    template<typename External> External optionalExternalID(uint32_t word);
    template<typename External> std::optional<External> optionalExternalEnumerator(uint32_t word);
    template<typename E> E enumerator(uint32_t word);
    template<typename E> std::optional<E> optionalEnumerator(uint32_t word);
    template<typename F> OptionSet<F> flags(uint32_t word);
    bool boolean(uint32_t word);
    std::optional<uint32_t> optionalNumber(uint32_t word);

    template<typename WireRecord> std::span<const WireRecord> slice(Wire::Span);
    std::span<const uint32_t> words(Wire::Span span) { return sliceOf(m_reader.sections().words, span); }
    std::span<const uint32_t> code(Wire::Span span) { return sliceOf(m_reader.sections().code, span); }
    std::span<const uint8_t> bytes(Wire::Span span) { return sliceOf(m_reader.sections().bytes, span); }

    template<typename Record> void didRead(Record&) { }
    void didRead(DeclaredType&);
    void didRead(PropertyKey&);
    void didRead(FunctionRecord&);
    void didRead(LayoutRecord&);

private:
    template<typename T> std::span<const T> sliceOf(std::span<const T> all, Wire::Span);
    template<typename IDType> bool remap(uint32_t& word);

    const Reader& m_reader;
    const UnitIDRemap* m_remap { nullptr };
    InternalError m_subject;
    Failure m_failure;
};

class BodyDecoder {
    WTF_MAKE_NONCOPYABLE(BodyDecoder);
public:
    BodyDecoder(const Reader&, FunctionID, const Wire::Body&, std::span<const uint32_t> code, Function&, const UnitIDRemap* = nullptr);

    // A use of one of these entry parameters is decoded as a use of the value that follows the last one on the wire, plus the position in the list.
    void setParametersCheckedAtEntry(std::span<const unsigned>);
    // giveLayoutsToCreatedObjects() adds values to such a function, in front of the checks at its entry.
    bool namesObjectCreate() const { return m_namesObjectCreate; }
    Vector<FunctionID, 4> takeDirectCallees() { return WTF::move(m_directCallees); }

    Failure run();

    RecordChecker& checker() { return m_checker; }
    IRForm form() const { return m_checker.reader().form(); }

    uint32_t next();
    uint64_t nextPair();
    uint32_t nextCount(uint32_t minWordsPerElement = 1) { return checkedCount(next(), minWordsPerElement); }
    ValueID nextOperand();
    ValueID nextOptionalOperand();
    void decodeVariadicOperands(Instruction&);

    // The next words of a program as read, if there are that many. After a failure no word is left.
    const uint32_t* peekWords(uint32_t count) const { return form() == IRForm::AsRead && hasWords(count) ? m_code.subspan(m_offset).data() : nullptr; }
    void skipWords(uint32_t count) { m_offset += count; }
    ValueID checkedOperand(uint32_t word, bool& isValid) const
    {
        isValid &= word < m_wire.valueCount;
        return usedValue(word);
    }
    ValueID checkedOptionalOperand(uint32_t word, bool& isValid) const
    {
        static_assert(!ValueID(Wire::none));
        isValid &= word < m_wire.valueCount || word == Wire::none;
        return usedValue(word);
    }
    template<typename IDType> uint32_t checkedID(uint32_t word, bool& isValid) const { return m_checker.checkedID<IDType>(word, isValid); }
    template<typename IDType> uint32_t checkedOptionalID(uint32_t word, bool& isValid) const
    {
        static_assert(!IDType(Wire::none));
        return word == Wire::none ? word : m_checker.checkedID<IDType>(word, isValid);
    }

    void fail(InternalErrorKind);
    bool hasFailed() const { return m_failure.has_value(); }

private:
    template<typename Record> Record nextRecord();
    bool hasWords(uint64_t count) const { return count <= m_code.size() - m_offset; }
    uint32_t checkedCount(uint32_t count, uint32_t minWordsPerElement = 1);
    uint16_t nextFlags(uint32_t wireMask, uint32_t declaredMask);
    ValueID usedValue(uint32_t word) const { return word < m_checksOfParameters.size() ? m_checksOfParameters[word] : ValueID(word); }
    ValueID allocateValueID();
    Value& appendValue(ValueID, Opcode);
    ValueID valueID(uint32_t word);

    void decodeBlock(BlockID);
    void decodeParameters(Block&, uint32_t count);
    void decodeValue(Block&);
    void checkConvertedAllocation(const Value& conversion);
    void decodeTerminator(Block&);
    Target decodeTarget();
    void adoptCheckerFailure();

    FunctionID m_id;
    const Wire::Body& m_wire;
    std::span<const uint32_t> m_code;
    Function& m_function;
    RecordChecker m_checker;
    size_t m_offset { 0 };
    uint32_t m_nextValue { 0 };
    Vector<ValueID, 8> m_checksOfParameters;
    uint32_t m_numberOfChecks { 0 };
    bool m_namesObjectCreate { false };
    Vector<FunctionID, 4> m_directCallees;
    BlockID m_block;
    ValueID m_value;
    Failure m_failure;
};

template<> inline size_t Reader::numberOfRecords<StringID>() const { return m_sections.strings.size(); }
template<> inline size_t Reader::numberOfRecords<PropertyKeyID>() const { return m_sections.propertyKeys.size(); }
template<> inline size_t Reader::numberOfRecords<TypeSiteID>() const { return m_sections.typeSites.size(); }
template<> inline size_t Reader::numberOfRecords<ExpectedResultID>() const { return m_sections.expectedResults.size(); }
template<> inline size_t Reader::numberOfRecords<LayoutID>() const { return m_sections.layouts.size() + m_numberOfDerivedLayouts; }
template<> inline size_t Reader::numberOfRecords<VariableID>() const { return m_sections.variables.size(); }
template<> inline size_t Reader::numberOfRecords<ModuleID>() const { return m_sections.modules.size(); }
template<> inline size_t Reader::numberOfRecords<ModuleFailureID>() const { return m_sections.moduleFailures.size(); }
template<> inline size_t Reader::numberOfRecords<EvalScopeID>() const { return m_sections.evalScopes.size(); }
template<> inline size_t Reader::numberOfRecords<FunctionID>() const { return m_sections.functions.size(); }
template<> inline size_t Reader::numberOfRecords<DataID>() const { return m_sections.data.size(); }

Vector<Failure> Reader::readInChunks(const ParallelLoops& loops, size_t recordCount, size_t recordsPerChunk, NOESCAPE const Invocable<Failure(uint32_t)> auto& readRecord)
{
    Vector<Failure> failures((recordCount + recordsPerChunk - 1) / recordsPerChunk);
    forEachIndexInParallel(loops, static_cast<unsigned>(failures.size()), [&](unsigned chunk) {
        size_t begin = static_cast<size_t>(chunk) * recordsPerChunk;
        size_t end = std::min(recordCount, begin + recordsPerChunk);
        for (size_t index = begin; index < end && !failures[chunk]; ++index)
            failures[chunk] = readRecord(static_cast<uint32_t>(index));
    });
    return failures;
}

template<typename Record, size_t inlineCapacity>
void Reader::growFromEmpty(const ParallelLoops& loops, Vector<Record, inlineCapacity>& records, size_t size)
{
    constexpr size_t bytesPerTask = 256 * KB;
    RELEASE_ASSERT(records.isEmpty());
    if (records.capacity() < size)
        records.reserveInitialCapacity(size);
    // AddressSanitizer is told that nobody writes behind the end of a vector.
#if !ASAN_ENABLED
    std::span<uint8_t> bytes = unsafeMakeSpan(reinterpret_cast<uint8_t*>(records.mutableSpan().data()), size * sizeof(Record));
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + bytesPerTask - 1) / bytesPerTask), [&](unsigned task) {
        zeroSpan(bytes.subspan(task * bytesPerTask, std::min(bytesPerTask, bytes.size() - task * bytesPerTask)));
    });
#else
    UNUSED_PARAM(loops);
    UNUSED_VARIABLE(bytesPerTask);
#endif
    records.grow(size);
}

inline void RecordChecker::fail(InternalErrorKind kind)
{
    if (m_failure)
        return;
    m_failure = m_subject;
    m_failure->kind = kind;
}

inline void RecordChecker::require(bool condition, InternalErrorKind kind)
{
    if (!condition)
        fail(kind);
}

inline void RecordChecker::requireAscending(std::strong_ordering neighborOrder)
{
    require(!std::is_eq(neighborOrder), InternalErrorKind::DuplicateListEntry);
    require(!std::is_gt(neighborOrder), InternalErrorKind::UnsortedList);
}

template<typename IDType> inline constexpr RemappedIDKind remappedIDKindOf = RemappedIDKind::String;
template<> inline constexpr RemappedIDKind remappedIDKindOf<PropertyKeyID> = RemappedIDKind::PropertyKey;
template<> inline constexpr RemappedIDKind remappedIDKindOf<TypeID> = RemappedIDKind::Type;
template<> inline constexpr RemappedIDKind remappedIDKindOf<TypeSiteID> = RemappedIDKind::TypeSite;
template<> inline constexpr RemappedIDKind remappedIDKindOf<ExpectedResultID> = RemappedIDKind::ExpectedResult;
template<> inline constexpr RemappedIDKind remappedIDKindOf<LayoutID> = RemappedIDKind::Layout;
template<> inline constexpr RemappedIDKind remappedIDKindOf<VariableID> = RemappedIDKind::Variable;
template<> inline constexpr RemappedIDKind remappedIDKindOf<ModuleID> = RemappedIDKind::Module;
template<> inline constexpr RemappedIDKind remappedIDKindOf<ModuleFailureID> = RemappedIDKind::ModuleFailure;
template<> inline constexpr RemappedIDKind remappedIDKindOf<EvalScopeID> = RemappedIDKind::EvalScope;
template<> inline constexpr RemappedIDKind remappedIDKindOf<FunctionID> = RemappedIDKind::Function;
template<> inline constexpr RemappedIDKind remappedIDKindOf<DataID> = RemappedIDKind::Data;

template<typename IDType>
bool RecordChecker::remap(uint32_t& word)
{
    static_assert(std::is_same_v<IDType, StringID> || remappedIDKindOf<IDType> != RemappedIDKind::String);
    if (!m_remap)
        return true;
    const IDRemapTable& table = m_remap->tables[static_cast<size_t>(remappedIDKindOf<IDType>)];
    if (word >= table.size || !table.entries[word]) {
        fail(InternalErrorKind::IdOutOfRange);
        return false;
    }
    word = table.entries[word] - 1;
    return true;
}

template<typename IDType>
IDType RecordChecker::id(uint32_t word)
{
    if (hasFailed() || !remap<IDType>(word))
        return { };
    if (word >= m_reader.numberOfRecords<IDType>()) {
        fail(InternalErrorKind::IdOutOfRange);
        return { };
    }
    return IDType(word);
}

template<>
inline TypeID RecordChecker::id<TypeID>(uint32_t word)
{
    if (hasFailed() || !remap<TypeID>(word))
        return { };
    if (word >= m_reader.typesAsRead().size()) {
        fail(InternalErrorKind::IdOutOfRange);
        return { };
    }
    return m_reader.typesAsRead()[word];
}

template<typename IDType>
uint32_t RecordChecker::checkedID(uint32_t word, bool& isValid) const
{
    static_assert(std::is_same_v<IDType, StringID> || remappedIDKindOf<IDType> != RemappedIDKind::String);
    if (m_remap) {
        const IDRemapTable& table = m_remap->tables[static_cast<size_t>(remappedIDKindOf<IDType>)];
        if (word >= table.size) [[unlikely]] {
            isValid = false;
            return 0;
        }
        // Zero stands for a record that the link dropped. It becomes a number that no list reaches.
        word = table.entries[word] - 1;
    }
    if constexpr (std::is_same_v<IDType, TypeID>) {
        std::span<const TypeID> types = m_reader.typesAsRead();
        if (word >= types.size()) [[unlikely]] {
            isValid = false;
            return 0;
        }
        return types[word].index();
    } else {
        isValid &= word < m_reader.numberOfRecords<IDType>();
        return word;
    }
}

template<typename IDType>
IDType RecordChecker::optionalID(uint32_t word)
{
    if (word == Wire::none)
        return { };
    return id<IDType>(word);
}

template<typename External>
External RecordChecker::external(uint32_t word)
{
    if (hasFailed())
        return External { };
    if (!isValidExternal<External>(word, m_reader.builtins())) {
        fail(InternalErrorKind::IdOutOfRange);
        return External { };
    }
    return static_cast<External>(word);
}

template<typename External>
External RecordChecker::optionalExternalID(uint32_t word)
{
    if (word == Wire::none)
        return External { };
    return external<External>(word);
}

template<typename External>
std::optional<External> RecordChecker::optionalExternalEnumerator(uint32_t word)
{
    if (word == Wire::none)
        return std::nullopt;
    return external<External>(word);
}

template<typename E>
E RecordChecker::enumerator(uint32_t word)
{
    static_assert(numberOfEnumerators<E> && !isFlagWord<E>);
    if (hasFailed())
        return E { };
    if (word >= numberOfEnumerators<E>) {
        fail(InternalErrorKind::UnknownEnumerator);
        return E { };
    }
    if (word >= numberOfWireEnumerators<E> && !acceptsEngineMembers()) {
        fail(InternalErrorKind::EngineOnlyOnWire);
        return E { };
    }
    return static_cast<E>(word);
}

template<typename E>
std::optional<E> RecordChecker::optionalEnumerator(uint32_t word)
{
    if (word == Wire::none)
        return std::nullopt;
    return enumerator<E>(word);
}

template<typename F>
OptionSet<F> RecordChecker::flags(uint32_t word)
{
    static_assert(isFlagWord<F> && numberOfEnumerators<F> <= 32);
    if (hasFailed())
        return { };
    constexpr uint64_t declaredMask = (uint64_t { 1 } << numberOfEnumerators<F>) - 1;
    constexpr uint64_t wireMask = (uint64_t { 1 } << numberOfWireEnumerators<F>) - 1;
    if (word & ~declaredMask) {
        fail(InternalErrorKind::UnknownFlag);
        return { };
    }
    if (word & ~wireMask && !acceptsEngineMembers()) {
        fail(InternalErrorKind::EngineOnlyOnWire);
        return { };
    }
    return OptionSet<F>::fromRaw(static_cast<typename OptionSet<F>::StorageType>(word));
}

inline bool RecordChecker::boolean(uint32_t word)
{
    require(word <= 1, InternalErrorKind::UnknownEnumerator);
    return !hasFailed() && word;
}

inline std::optional<uint32_t> RecordChecker::optionalNumber(uint32_t word)
{
    if (word == Wire::none)
        return std::nullopt;
    return word;
}

template<typename T>
std::span<const T> RecordChecker::sliceOf(std::span<const T> all, Wire::Span span)
{
    if (hasFailed())
        return { };
    if (static_cast<uint64_t>(span.start) + span.count > all.size()) {
        fail(InternalErrorKind::ListOutOfBounds);
        return { };
    }
    return all.subspan(span.start, span.count);
}

template<typename WireRecord>
std::span<const WireRecord> RecordChecker::slice(Wire::Span span)
{
    return sliceOf(m_reader.sections().template listOf<WireRecord>(), span);
}

inline bool has(const Wire::Layout& layout, LayoutFlags flag)
{
    return layout.flags & static_cast<uint32_t>(flag);
}

inline bool has(const Wire::Field& field, FieldFlags flag)
{
    return field.flags & static_cast<uint32_t>(flag);
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaReader.h"

#endif // ENABLE(AOT)
