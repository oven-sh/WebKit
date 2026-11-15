/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOrderFile.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/BitVector.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

constexpr uint32_t absentUnit = std::numeric_limits<uint32_t>::max();
using OrderNumberMap = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
using OrderHashMap = UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

class OrderLineReader {
public:
    explicit OrderLineReader(std::span<const uint8_t> line)
        : m_rest(line)
    {
    }

    std::span<const uint8_t> rest() const { return m_rest; }

    std::span<const uint8_t> takeWord()
    {
        size_t end = std::ranges::find(m_rest, ' ') - m_rest.begin();
        std::span<const uint8_t> word = m_rest.first(end);
        m_rest = m_rest.subspan(std::min(end + 1, m_rest.size()));
        return word;
    }

private:
    std::span<const uint8_t> m_rest;
};

std::optional<uint32_t> parseOrderNumber(std::span<const uint8_t> digits)
{
    constexpr size_t maxDigits = 9;
    if (digits.empty() || digits.size() > maxDigits)
        return std::nullopt;
    uint32_t number = 0;
    for (uint8_t digit : digits) {
        if (!isASCIIDigit(digit))
            return std::nullopt;
        number = number * 10 + (digit - '0');
    }
    return number;
}

uint64_t hashOfOrderPath(std::span<const uint8_t> bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash >> 1;
}

std::optional<OrderFile::Unit> parseOrderUnit(OrderLineReader line, size_t numberOfFiles)
{
    std::span<const uint8_t> fileWord = line.takeWord();
    bool isInWholeProgram = std::ranges::equal(fileWord, byteCast<uint8_t>("="_s.span()));
    bool hasFile = !isInWholeProgram && !std::ranges::equal(fileWord, byteCast<uint8_t>("-"_s.span()));
    std::optional<uint32_t> file = hasFile ? parseOrderNumber(fileWord) : isInWholeProgram ? OrderFile::wholeProgram : OrderFile::noFile;
    std::optional<uint32_t> number = parseOrderNumber(line.takeWord());
    std::span<const uint8_t> unit = line.takeWord();
    std::span<const uint8_t> hash = line.takeWord();
    std::span<const uint8_t> position = line.takeWord();
    if (!file || !number || (hasFile && *file >= numberOfFiles) || unit.size() < 2 || (unit[0] != 'b' && unit[0] != 'e' && unit[0] != 'o') || position.empty())
        return std::nullopt;
    std::optional<uint32_t> rank = parseOrderNumber(unit.subspan(1));
    if (!rank)
        return std::nullopt;
    return OrderFile::Unit { *file, *number, *rank, unit[0], hash, line.rest() };
}

struct FunctionsOfFile {
    uint32_t first;
    uint32_t end;
};

} // anonymous namespace

Vector<OrderFile> OrderFile::parse(std::span<const uint8_t> bytes)
{
    constexpr auto head = "aot-order v1"_s;
    Vector<OrderFile> files;
    for (std::span<const uint8_t> rest = bytes; !rest.empty();) {
        auto end = std::ranges::find(rest, '\n');
        if (end == rest.end())
            break;
        size_t length = end - rest.begin();
        std::span<const uint8_t> bytesOfLine = rest.first(length);
        rest = rest.subspan(length + 1);
        if (std::ranges::equal(bytesOfLine, byteCast<uint8_t>(head.span()))) {
            files.append(OrderFile { });
            continue;
        }
        if (files.isEmpty())
            break;
        OrderFile& result = files.last();
        OrderLineReader line(bytesOfLine);
        ++result.numberOfLines;
        std::span<const uint8_t> kind = line.takeWord();
        if (kind.size() != 1)
            continue;
        if (kind[0] == 'M') {
            if (std::optional<uint32_t> number = parseOrderNumber(line.takeWord()); number && *number == result.files.size())
                result.files.append(line.rest());
        } else if (kind[0] == 'P')
            result.numberOfFunctions = parseOrderNumber(line.takeWord());
        else if (kind[0] == 'F') {
            if (std::optional<Unit> unit = parseOrderUnit(line, result.files.size()))
                result.units.append(*unit);
        }
    }
    return files;
}

static Vector<uint32_t> findListedFiles(const OrderFile& file, const Typed::Program& program)
{
    OrderHashMap numbers;
    for (uint32_t number = 0; number < file.files.size(); ++number)
        numbers.add(hashOfOrderPath(file.files[number]), number);
    return Vector<uint32_t>(program.modules.size(), [&](size_t index) {
        const Typed::Module& module = program.modules[index];
        if (!module.displayPath)
            return absentUnit;
        std::span<const uint8_t> path = program.string(module.displayPath);
        auto found = numbers.find(hashOfOrderPath(path));
        return found != numbers.end() && std::ranges::equal(file.files[found->value], path) ? found->value : absentUnit;
    });
}

namespace {

class ListedUnitFinder {
public:
    ListedUnitFinder(const OrderFile& file, const Typed::Program& program, const Typed::Plan& plan, const FunctionNumbers& numbers, const LinkedFunctions& functions)
        : m_file(file)
        , m_program(program)
        , m_conventions(plan.conventions())
        , m_numbers(numbers)
        , m_functions(functions)
    {
    }

    Vector<uint32_t> find();

private:
    const CompiledCode* bodyOf(uint32_t function) const { return m_positionOfBody[function] == absentUnit ? nullptr : &m_functions[m_positionOfBody[function]]; }
    ModuleID moduleOf(uint32_t function) const;
    std::span<const uint8_t> nameOf(uint32_t function) const;
    bool mayBe(const OrderFile::Unit&, uint32_t function) const;
    void indexProgram();
    uint32_t findFunction(const OrderFile::Unit&, int64_t shift, unsigned numberOfFilesToSkip) const;
    uint32_t findFunctionByName(const OrderFile::Unit&);
    uint32_t findFunctionWithoutFile(const OrderFile::Unit&) const;
    uint32_t findPosition(const OrderFile::Unit&, uint32_t function) const;

    const OrderFile& m_file;
    const Typed::Program& m_program;
    const Typed::CallingConventions& m_conventions;
    const FunctionNumbers& m_numbers;
    const LinkedFunctions& m_functions;
    Vector<uint32_t> m_positionOfBody;
    Vector<uint32_t> m_positionOfNumber;
    Vector<Vector<FunctionsOfFile, 1>> m_functionsOfFiles;
    OrderHashMap m_functionsWithoutFile;
    BitVector m_hasNamesOfFile;
    OrderHashMap m_functionsByName;
    Vector<std::pair<uint32_t, uint32_t>> m_otherUnits;
};

ModuleID ListedUnitFinder::moduleOf(uint32_t function) const
{
    const CompiledCode* body = bodyOf(function);
    return body && !body->sites.positions.isEmpty() ? body->sites.positions[0].source : ModuleID();
}

std::span<const uint8_t> ListedUnitFinder::nameOf(uint32_t function) const
{
    StringID name = m_program.functions[function].name;
    return name ? m_program.string(name) : std::span<const uint8_t>();
}

bool ListedUnitFinder::mayBe(const OrderFile::Unit& unit, uint32_t function) const
{
    return unit.name.empty() || std::ranges::equal(unit.name, nameOf(function));
}

uint64_t hashOfNamesake(std::span<const uint8_t> name, uint32_t number)
{
    return (hashOfOrderPath(name) ^ number * 0x9e3779b97f4a7c15ull) >> 1;
}

void ListedUnitFinder::indexProgram()
{
    const uint32_t numberOfFunctions = m_numbers.firstDerived();
    m_positionOfBody.fill(absentUnit, numberOfFunctions);
    m_positionOfNumber.fill(absentUnit, m_numbers.numberOfFunctions());
    for (uint32_t position = 0; position < m_functions.size(); ++position) {
        const CompiledCode& code = m_functions[position];
        if (code.unit.role == Typed::CodeRole::Body)
            m_positionOfBody[code.unit.function.index()] = position;
        if (FunctionID number = m_numbers.idOf(code.unitIndex))
            m_positionOfNumber[number.index()] = position;
    }
    for (uint32_t number = m_numbers.endOfClosureFunctions().index(); number < m_numbers.numberOfFunctions(); ++number)
        m_otherUnits.append({ m_conventions.unitAt(m_numbers.unitOf(FunctionID(number))).function.index(), number });
    AOT::sort(m_otherUnits);

    Vector<uint32_t> fileOfModule = findListedFiles(m_file, m_program);
    m_functionsOfFiles.grow(m_file.files.size());
    m_hasNamesOfFile.ensureSize(m_file.files.size());
    OrderNumberMap namesakes;
    ModuleID module;
    Vector<FunctionsOfFile, 1>* ofFile = nullptr;
    for (uint32_t function = 0; function < numberOfFunctions; ++function) {
        ModuleID definedIn = moduleOf(function);
        if (!definedIn) {
            if (bodyOf(function)) {
                uint32_t number = namesakes.add(m_program.functions[function].name.bits(), 0).iterator->value++;
                m_functionsWithoutFile.add(hashOfNamesake(nameOf(function), number), function);
            } else if (ofFile)
                ofFile->last().end = function + 1;
            continue;
        }
        if (definedIn != module) {
            module = definedIn;
            uint32_t file = fileOfModule[module.index()];
            ofFile = file == absentUnit ? nullptr : &m_functionsOfFiles[file];
            if (ofFile)
                ofFile->append({ function, function });
        }
        if (ofFile)
            ofFile->last().end = function + 1;
    }
}

uint32_t ListedUnitFinder::findFunction(const OrderFile::Unit& unit, int64_t shift, unsigned numberOfFilesToSkip) const
{
    for (const FunctionsOfFile& functions : m_functionsOfFiles[unit.file]) {
        int64_t function = static_cast<int64_t>(functions.first) + unit.number + shift;
        if (function >= functions.first && function < functions.end && mayBe(unit, static_cast<uint32_t>(function)) && !numberOfFilesToSkip--)
            return static_cast<uint32_t>(function);
    }
    return absentUnit;
}

uint32_t ListedUnitFinder::findFunctionByName(const OrderFile::Unit& unit)
{
    if (unit.name.empty())
        return absentUnit;
    if (!m_hasNamesOfFile.quickSet(unit.file)) {
        for (const FunctionsOfFile& functions : m_functionsOfFiles[unit.file]) {
            for (uint32_t function = functions.first; function < functions.end; ++function) {
                auto added = m_functionsByName.add(hashOfNamesake(nameOf(function), unit.file), function);
                if (!added.isNewEntry)
                    added.iterator->value = absentUnit;
            }
        }
    }
    auto found = m_functionsByName.find(hashOfNamesake(unit.name, unit.file));
    return found != m_functionsByName.end() && found->value != absentUnit && std::ranges::equal(unit.name, nameOf(found->value)) ? found->value : absentUnit;
}

uint32_t ListedUnitFinder::findFunctionWithoutFile(const OrderFile::Unit& unit) const
{
    auto found = m_functionsWithoutFile.find(hashOfNamesake(unit.name, unit.number));
    return found != m_functionsWithoutFile.end() && std::ranges::equal(unit.name, nameOf(found->value)) ? found->value : absentUnit;
}

uint32_t ListedUnitFinder::findPosition(const OrderFile::Unit& unit, uint32_t function) const
{
    switch (unit.kind) {
    case 'b':
        return m_positionOfBody[function];
    case 'e': {
        auto numbers = std::views::iota(m_numbers.firstDerived(), m_numbers.endOfClosureFunctions().index());
        auto isInFront = [&](uint32_t number) {
            return m_conventions.unitAt(m_numbers.unitOf(FunctionID(number))).function.index() < function;
        };
        size_t place = std::ranges::partition_point(numbers, isInFront) - numbers.begin() + unit.rank;
        if (place >= numbers.size() || m_conventions.unitAt(m_numbers.unitOf(FunctionID(numbers[place]))).function.index() != function)
            return absentUnit;
        return m_positionOfNumber[numbers[place]];
    }
    default: {
        size_t place = std::ranges::lower_bound(m_otherUnits, function, { }, &std::pair<uint32_t, uint32_t>::first) - m_otherUnits.begin() + unit.rank;
        if (place >= m_otherUnits.size() || m_otherUnits[place].first != function)
            return absentUnit;
        return m_positionOfNumber[m_otherUnits[place].second];
    }
    }
}

Vector<uint32_t> ListedUnitFinder::find()
{
    indexProgram();
    const size_t count = m_file.units.size();
    Vector<uint32_t> functions(FillWith { }, count, absentUnit);
    Vector<uint32_t> inOrderOfFunctions(count, [](size_t index) { return static_cast<uint32_t>(index); });
    AOT::stableSort(inOrderOfFunctions, { }, [&](uint32_t index) {
        const OrderFile::Unit& unit = m_file.units[index];
        return std::tuple(unit.file, unit.number, unit.kind, unit.rank);
    });
    uint32_t file = absentUnit;
    int64_t shift = 0;
    unsigned numberOfEqualUnits = 0;
    const OrderFile::Unit* previous = nullptr;
    for (uint32_t index : inOrderOfFunctions) {
        const OrderFile::Unit& unit = m_file.units[index];
        numberOfEqualUnits = previous && previous->file == unit.file && previous->number == unit.number && previous->kind == unit.kind && previous->rank == unit.rank ? numberOfEqualUnits + 1 : 0;
        previous = &unit;
        if (unit.file == OrderFile::noFile) {
            functions[index] = findFunctionWithoutFile(unit);
            continue;
        }
        if (unit.file == OrderFile::wholeProgram) {
            if (m_file.numberOfFunctions == m_numbers.firstDerived() && unit.number < m_numbers.firstDerived())
                functions[index] = unit.number;
            continue;
        }
        if (unit.file != file) {
            file = unit.file;
            shift = 0;
        }
        uint32_t function = findFunction(unit, shift, numberOfEqualUnits);
        if (function == absentUnit && shift)
            function = findFunction(unit, 0, numberOfEqualUnits);
        if (function == absentUnit && !numberOfEqualUnits)
            function = findFunctionByName(unit);
        functions[index] = function;
        if (function == absentUnit)
            continue;
        for (const FunctionsOfFile& candidates : m_functionsOfFiles[file]) {
            if (function >= candidates.first && function < candidates.end) {
                shift = static_cast<int64_t>(function) - candidates.first - unit.number;
                break;
            }
        }
    }

    Vector<uint32_t> positions;
    BitVector isFound(m_functions.size());
    for (size_t index = 0; index < count; ++index) {
        uint32_t position = functions[index] == absentUnit ? absentUnit : findPosition(m_file.units[index], functions[index]);
        if (position != absentUnit && !isFound.quickSet(position))
            positions.append(position);
    }
    return positions;
}

} // anonymous namespace

Vector<uint32_t> findListedUnits(const OrderFile& file, const Typed::Program& program, const Typed::Plan& plan, const FunctionNumbers& numbers, const LinkedFunctions& functions)
{
    return ListedUnitFinder(file, program, plan, numbers, functions).find();
}

OrderOfUnits orderUnits(std::span<const uint32_t> listed, std::span<const FinalJump> finalJumps, const LinkedFunctions& functions, size_t sizeOfPiece)
{
    const uint32_t count = static_cast<uint32_t>(functions.size());
    auto fallsIntoNext = [&](uint32_t position) {
        return position + 1 < count && finalJumps[position].jumpsToNextBody;
    };
    BitVector isOrdered(count);
    Vector<uint32_t> runs;
    size_t firstRunOfPiece = 0;
    uint64_t sizeOfRuns = 0;
    auto finishPiece = [&] {
        AOT::sort(runs.mutableSpan().subspan(firstRunOfPiece));
        firstRunOfPiece = runs.size();
        sizeOfRuns = 0;
    };
    for (uint32_t position : listed) {
        while (position && fallsIntoNext(position - 1))
            --position;
        if (isOrdered.quickSet(position))
            continue;
        runs.append(position);
        for (uint32_t body = position;; ++body) {
            sizeOfRuns += functions[body].body.bytes.size();
            if (!fallsIntoNext(body))
                break;
            isOrdered.quickSet(body + 1);
        }
        if (sizeOfRuns >= sizeOfPiece)
            finishPiece();
    }
    finishPiece();

    OrderOfUnits order;
    order.positions.reserveInitialCapacity(count);
    for (uint32_t position : runs) {
        order.positions.append(position);
        while (fallsIntoNext(position))
            order.positions.append(++position);
    }
    order.numberOfListed = order.positions.size();
    for (uint32_t position = 0; position < count; ++position) {
        if (!isOrdered.quickGet(position))
            order.positions.append(position);
    }
    RELEASE_ASSERT(order.positions.size() == count);
    return order;
}

ResolvedCallees reorderCallees(const ResolvedCallees& callees, std::span<const uint32_t> order, std::span<const uint32_t> newPositions, const ParallelLoops& loops)
{
    ResolvedCallees result;
    result.firstCallee.grow(order.size() + 1);
    uint32_t numberOfCallees = 0;
    for (size_t position = 0; position < order.size(); ++position) {
        result.firstCallee[position] = numberOfCallees;
        numberOfCallees += callees.firstCallee[order[position] + 1] - callees.firstCallee[order[position]];
    }
    RELEASE_ASSERT(numberOfCallees == callees.positions.size());
    result.firstCallee[order.size()] = numberOfCallees;
    result.positions.grow(numberOfCallees);
    forEachRangeOfIndicesInParallel(loops, order.size(), [&](size_t first, size_t end) {
        for (size_t position = first; position < end; ++position) {
            std::span<const OptionalIndex> before = callees.positions.span().subspan(callees.firstCallee[order[position]], callees.firstCallee[order[position] + 1] - callees.firstCallee[order[position]]);
            std::span<OptionalIndex> after = result.positions.mutableSpan().subspan(result.firstCallee[position], before.size());
            for (size_t index = 0; index < before.size(); ++index)
                after[index] = before[index] ? OptionalIndex { newPositions[*before[index]] } : OptionalIndex { };
        }
    });
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
