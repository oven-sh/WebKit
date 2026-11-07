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
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWireSections.h"
#include "AOTWireTypes.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class RecordChecker;

class Reader {
    WTF_MAKE_NONCOPYABLE(Reader);
public:
    Reader(Program&, BuildContext&);

    void setExternalFunctionBodies(const ExternalFunctionBodies* bodies) { m_externalBodies = bodies; }
    void setSchemaHashCheck(SchemaHashCheck check) { m_schemaHashCheck = check; }

    bool run();
    bool tablesHaveRoomFor(const Wire::Sections&);

    const Program& program() const { return m_program; }
    const BuiltinTable& builtins() const { return m_builtins; }
    const Wire::Sections& sections() const { return m_sections; }

private:
    bool readHeader();
    void readStrings();
    void readPropertyKeys();
    NumbersOfProgramRecords numbersOfRecords() const;
    void forEachListReadIntoTableOfItsName(const Wire::Sections&, const auto& functor);
    void makeRoomForTables();
    void readProgramLists();
    void readLayouts();
    void setConstructedClasses();
    void setParameterSiteFunctions();
    bool report(const InternalError&);

    void readFunctions();
    void readFunction(uint32_t index, Function&) const;

    template<typename WireRecord, typename Record, size_t inlineCapacity>
    void readList(std::span<const WireRecord>, Vector<Record, inlineCapacity>&);
    static void readInChunks(const ParallelLoops&, size_t recordCount, size_t recordsPerChunk, NOESCAPE const Invocable<void(uint32_t)> auto& readRecord);
    template<typename Record, size_t inlineCapacity>
    static void makeRoomFor(const ParallelLoops&, Vector<Record, inlineCapacity>&, size_t capacity);
    template<typename Record, size_t inlineCapacity>
    static void growFromEmpty(const ParallelLoops&, Vector<Record, inlineCapacity>&, size_t size);

    Program& m_program;
    BuildContext& m_build;
    const BuiltinTable& m_builtins;
    std::span<const uint8_t> m_buffer;
    const Wire::Header* m_header { nullptr };
    Wire::Sections m_sections;
    std::optional<WireTypeReader> m_types;
    const ExternalFunctionBodies* m_externalBodies { nullptr };
    SchemaHashCheck m_schemaHashCheck { SchemaHashCheck::Required };
};

class RecordChecker {
public:
    explicit RecordChecker(const Reader& reader)
        : m_reader(reader)
    {
    }

    const Reader& reader() const { return m_reader; }
    void setUnit(const UnitIDRemap* unit) { m_unit = unit; }

    template<typename IDType> IDType id(uint32_t word);
    template<typename IDType> IDType optionalID(uint32_t word);
    template<typename External> External external(uint32_t word);
    template<typename External> External optionalExternalID(uint32_t word);
    template<typename External> std::optional<External> optionalExternalEnumerator(uint32_t word);
    template<typename E> E enumerator(uint32_t word);
    template<typename E> std::optional<E> optionalEnumerator(uint32_t word);
    template<typename F> OptionSet<F> flags(uint32_t word);
    bool boolean(uint32_t word) { return word; }
    std::optional<uint32_t> optionalNumber(uint32_t word);

    template<typename WireRecord> std::span<const WireRecord> slice(Wire::Span);
    std::span<const uint32_t> words(Wire::Span span) { return sliceOf(m_reader.sections().words, span); }
    std::span<const uint32_t> code(Wire::Span span) { return sliceOf(m_reader.sections().code, span); }
    std::span<const uint8_t> bytes(Wire::Span span) { return sliceOf(m_reader.sections().bytes, span); }

    template<typename Record> void didRead(Record&) { }
    void didRead(LayoutRecord&);

private:
    template<typename T> std::span<const T> sliceOf(std::span<const T> all, Wire::Span span) { return all.subspan(span.start, span.count); }

    const Reader& m_reader;
    const UnitIDRemap* m_unit { nullptr };
};

void Reader::readInChunks(const ParallelLoops& loops, size_t recordCount, size_t recordsPerChunk, NOESCAPE const Invocable<void(uint32_t)> auto& readRecord)
{
    forEachIndexInParallel(loops, static_cast<unsigned>((recordCount + recordsPerChunk - 1) / recordsPerChunk), [&](unsigned chunk) {
        size_t begin = static_cast<size_t>(chunk) * recordsPerChunk;
        size_t end = std::min(recordCount, begin + recordsPerChunk);
        for (size_t index = begin; index < end; ++index)
            readRecord(static_cast<uint32_t>(index));
    });
}

template<typename Record, size_t inlineCapacity>
void Reader::makeRoomFor(const ParallelLoops& loops, Vector<Record, inlineCapacity>& records, size_t capacity)
{
    constexpr size_t bytesPerTask = 256 * KB;
    RELEASE_ASSERT(records.isEmpty());
    records.reserveInitialCapacity(capacity);
#if !ASAN_ENABLED
    std::span<uint8_t> bytes = unsafeMakeSpan(reinterpret_cast<uint8_t*>(records.mutableSpan().data()), records.capacity() * sizeof(Record));
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + bytesPerTask - 1) / bytesPerTask), [&](unsigned task) {
        zeroSpan(bytes.subspan(task * bytesPerTask, std::min(bytesPerTask, bytes.size() - task * bytesPerTask)));
    });
#else
    UNUSED_PARAM(loops);
    UNUSED_VARIABLE(bytesPerTask);
#endif
}

template<typename Record, size_t inlineCapacity>
void Reader::growFromEmpty(const ParallelLoops& loops, Vector<Record, inlineCapacity>& records, size_t size)
{
    RELEASE_ASSERT(records.isEmpty());
    if (records.capacity() < size)
        makeRoomFor(loops, records, size);
    records.grow(size);
}

template<typename IDType>
IDType RecordChecker::id(uint32_t word)
{
    if (m_unit)
        return programIDOf<IDType>(*m_unit, word);
    if constexpr (std::is_same_v<IDType, TypeID>)
        return m_reader.program().types.typeOfWireIndex(word);
    else
        return IDType(word);
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
    return OptionSet<F>::fromRaw(static_cast<typename OptionSet<F>::StorageType>(word));
}

inline std::optional<uint32_t> RecordChecker::optionalNumber(uint32_t word)
{
    if (word == Wire::none)
        return std::nullopt;
    return word;
}

template<typename WireRecord>
std::span<const WireRecord> RecordChecker::slice(Wire::Span span)
{
    return sliceOf(m_reader.sections().template listOf<WireRecord>(), span);
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaReader.h"

#endif // ENABLE(AOT)
