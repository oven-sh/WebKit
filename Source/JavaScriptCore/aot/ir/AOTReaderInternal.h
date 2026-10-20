/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTIR.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTVerifier.h"
#include "AOTWireSections.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyDecoder;
class RecordChecker;

using Failure = std::optional<InternalError>;

template<typename External> bool isValidExternal(uint32_t word, const BuiltinTable&);

class Reader {
    WTF_MAKE_NONCOPYABLE(Reader);
public:
    using TypeCompletion = WTF::Function<void(uint32_t wireIndex, Type&)>;

    Reader(Program&, IRForm, BuildContext&);

    void setTypeCompletion(TypeCompletion&& completion) { m_completeType = WTF::move(completion); }
    void setVerifierInputs(const VerifierInputs& inputs) { m_verifierInputs = inputs; }
    bool run();

    IRForm form() const { return m_form; }
    const Program& program() const { return m_program; }
    BuildContext& build() const { return m_build; }
    const BuiltinTable& builtins() const { return m_builtins; }
    const Wire::Sections& sections() const { return m_sections; }

    template<typename IDType> size_t numberOfRecords() const;
    std::span<const TypeID> typesAsRead() const { return m_types.span(); }
    bool promisesType(const DeclaredType&) const;
    bool isTestedAtRunTime(const DeclaredType&) const;
    TypeID promisedTypeOf(const DeclaredType&) const;
    TypeID promisedTypeOf(TypeID asWritten) const;

private:
    bool readHeader();
    bool readStrings();
    bool readPropertyKeys();
    bool computeDeclaredMembers();
    void planTaggedLayouts(Vector<Vector<LayoutID>>& members);
    bool computeFunctionFamilies();
    bool readTypes();
    bool computePromisedTypes();
    bool readProgramLists();
    bool readLayouts();
    bool readFunctions();
    bool finishProgram();

    Failure readType(uint32_t index, Type&);
    void addFunctions(Type&, std::span<const FunctionID> written) const;
    Failure readClassPart(RecordChecker&, uint32_t typeIndex, const Wire::ClassPart&, ClassPart&) const;
    Failure readFunction(uint32_t index, Function&) const;
    bool checkTypesAgainstLayouts();
    bool setConstructedClasses();
    void setParameterSiteFunctions();

    template<typename WireRecord, typename Record, size_t inlineCapacity>
    bool readList(std::span<const WireRecord>, Vector<Record, inlineCapacity>&);
    template<typename WireRecord, typename Record, size_t inlineCapacity>
    bool readListInParallel(std::span<const WireRecord>, Vector<Record, inlineCapacity>&);
    static Vector<Failure> readInChunks(const ParallelLoops&, size_t recordCount, size_t recordsPerChunk, NOESCAPE const Invocable<Failure(uint32_t)> auto& readRecord);

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
    Vector<TypeID> m_promisedTypes;
    TypeID m_anythingOrAbsent;
    Vector<LayoutSetID> m_declaredMembers;
    Vector<CalleeSetID> m_functionFamilies;
    uint32_t m_numberOfDerivedLayouts { 0 };
    Vector<TaggedFamily> m_taggedFamilies;
    TypeCompletion m_completeType;
    VerifierInputs m_verifierInputs;
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

    bool hasFailed() const { return m_failure.has_value(); }
    Failure takeFailure() { return std::exchange(m_failure, std::nullopt); }
    void fail(InternalErrorKind);
    void require(bool condition, InternalErrorKind);
    void requireAscending(std::strong_ordering neighborOrder);
    template<typename T> void requireAscending(const T& previous, const T& next) { requireAscending(previous <=> next); }
    void requireAscendingStrings(StringID previous, StringID next);

    template<typename IDType> IDType id(uint32_t word);
    template<typename IDType> IDType optionalID(uint32_t word);
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
    void didRead(ClaimSource&);
    void didRead(PropertyKey&);
    void didRead(FunctionRecord&);
    void didRead(LayoutRecord&);

private:
    template<typename T> std::span<const T> sliceOf(std::span<const T> all, Wire::Span);

    const Reader& m_reader;
    InternalError m_subject;
    Failure m_failure;
};

class BodyDecoder {
    WTF_MAKE_NONCOPYABLE(BodyDecoder);
public:
    BodyDecoder(const Reader&, FunctionID, const Wire::Body&, std::span<const uint32_t> code, Function&);

    Failure run();

    RecordChecker& checker() { return m_checker; }
    IRForm form() const { return m_checker.reader().form(); }

    uint32_t next();
    uint64_t nextPair();
    uint32_t nextCount(uint32_t minWordsPerElement = 1) { return checkedCount(next(), minWordsPerElement); }
    ValueID nextOperand();
    ValueID nextOptionalOperand();
    void decodeVariadicOperands(Instruction&);

    void fail(InternalErrorKind);
    bool hasFailed() const { return m_failure.has_value(); }

private:
    template<typename Record> Record nextRecord();
    bool hasWords(uint64_t count) const { return count <= m_code.size() - m_offset; }
    uint32_t checkedCount(uint32_t count, uint32_t minWordsPerElement = 1);
    uint16_t nextFlags(uint32_t wireMask, uint32_t declaredMask);
    ValueID allocateValueID();
    ValueID valueID(uint32_t word);

    void decodeBlock(BlockID);
    void decodeParameters(Block&, uint32_t count);
    void decodeValue(Block&);
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

template<typename IDType>
IDType RecordChecker::id(uint32_t word)
{
    if (hasFailed())
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
    if (hasFailed())
        return { };
    if (word >= m_reader.typesAsRead().size()) {
        fail(InternalErrorKind::IdOutOfRange);
        return { };
    }
    return m_reader.typesAsRead()[word];
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

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaReader.h"

#endif // ENABLE(AOT)
