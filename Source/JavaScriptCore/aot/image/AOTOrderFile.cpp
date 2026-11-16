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
#include "AOTOrderRecorder.h"
#include "AOTPlan.h"
#include "AOTStartOrder.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/ASCIICType.h>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

constexpr uint32_t absentUnit = std::numeric_limits<uint32_t>::max();
constexpr uint32_t ambiguousUnit = absentUnit - 1;
constexpr unsigned bitsOfIndexOfUnit = 24;
constexpr unsigned bitsOfRankOfUnit = 14;
constexpr unsigned bitsOfKindOfUnit = 2;

struct OrderFile {
    static constexpr uint32_t noFile = UINT32_MAX - 1;
    static constexpr uint32_t wholeProgram = UINT32_MAX - 2;

    struct Unit {
        uint32_t file;
        uint32_t number;
        uint32_t column;
        uint32_t hashOfNames;
        uint32_t rank;
        uint8_t kind;
        std::span<const uint8_t> name;
    };

    bool takeNext(std::span<const uint8_t>& rest);

    Vector<std::span<const uint8_t>> files;
    Vector<Unit> units;
    std::span<const uint8_t> entry;
    uint32_t numberOfFunctions { absentUnit };

private:
    void addLine(std::span<const uint8_t>);
};

std::span<const uint8_t> takeWord(std::span<const uint8_t>& rest, uint8_t separator = ' ')
{
    size_t end = 0;
    while (end < rest.size() && rest[end] != separator)
        ++end;
    std::span<const uint8_t> word = rest.first(end);
    rest = rest.subspan(std::min(end + 1, rest.size()));
    return word;
}

uint32_t parseOrderNumber(std::span<const uint8_t> digits)
{
    constexpr size_t maxDigits = 9;
    if (digits.empty() || digits.size() > maxDigits)
        return absentUnit;
    uint32_t number = 0;
    for (uint8_t digit : digits) {
        if (!isASCIIDigit(digit))
            return absentUnit;
        number = number * 10 + (digit - '0');
    }
    return number;
}

void OrderFile::addLine(std::span<const uint8_t> line)
{
    std::span<const uint8_t> kind = takeWord(line);
    if (kind.size() != 1)
        return;
    if (kind[0] == 'M') {
        if (parseOrderNumber(takeWord(line)) == files.size())
            files.append(std::span<const uint8_t> { line });
        return;
    }
    if (kind[0] == 'E') {
        entry = line;
        return;
    }
    if (kind[0] == 'P') {
        numberOfFunctions = parseOrderNumber(takeWord(line));
        return;
    }
    if (kind[0] != 'F')
        return;
    std::span<const uint8_t> file = takeWord(line);
    Unit unit;
    unit.number = parseOrderNumber(takeWord(line));
    std::span<const uint8_t> kindAndRank = takeWord(line);
    unit.hashOfNames = parseOrderNumber(takeWord(line));
    std::span<const uint8_t> position = takeWord(line);
    if (position.empty() || kindAndRank.size() < 2 || (kindAndRank[0] != 'b' && kindAndRank[0] != 'e' && kindAndRank[0] != 'o'))
        return;
    unit.kind = kindAndRank[0];
    unit.rank = parseOrderNumber(kindAndRank.subspan(1));
    unit.name = line;
    takeWord(position, ':');
    unit.column = parseOrderNumber(position);
    if (file.size() == 1 && file[0] == '=')
        unit.file = wholeProgram;
    else if (file.size() == 1 && file[0] == '-')
        unit.file = noFile;
    else if (unit.file = parseOrderNumber(file); unit.file >= files.size())
        return;
    if (unit.number != absentUnit && unit.rank != absentUnit)
        units.append(unit);
}

bool OrderFile::takeNext(std::span<const uint8_t>& rest)
{
    constexpr auto head = "aot-order v1"_s;
    files.shrink(0);
    units.shrink(0);
    numberOfFunctions = absentUnit;
    entry = { };
    bool hasHead = false;
    while (!rest.empty()) {
        size_t length = 0;
        while (length < rest.size() && rest[length] != '\n')
            ++length;
        if (length == rest.size())
            break;
        std::span<const uint8_t> line = rest.first(length);
        bool isHead = equalSpans(line, byteCast<uint8_t>(head.span()));
        if (isHead && hasHead)
            return true;
        if (!isHead && !hasHead)
            break;
        rest = rest.subspan(length + 1);
        if (isHead)
            hasHead = true;
        else
            addLine(line);
    }
    rest = { };
    return hasHead;
}

class OrderIndex {
public:
    explicit OrderIndex(size_t maxKeys)
        : m_keys(FillWith { }, WTF::roundUpToPowerOfTwo(static_cast<uint32_t>(2 * maxKeys + 2)), 0)
        , m_values(FillWith { }, m_keys.size(), absentUnit)
    {
    }

    uint32_t& valueOf(std::span<const uint8_t> text, uint64_t salt)
    {
        uint64_t key = 14695981039346656037ull ^ salt * 0x9e3779b97f4a7c15ull;
        for (uint8_t byte : text)
            key = (key ^ byte) * 1099511628211ull;
        key |= 1;
        size_t mask = m_keys.size() - 1;
        size_t slot = key >> 32 & mask;
        while (m_keys[slot] && m_keys[slot] != key)
            slot = (slot + 1) & mask;
        m_keys[slot] = key;
        return m_values[slot];
    }

private:
    Vector<uint64_t> m_keys;
    Vector<uint32_t> m_values;
};

constexpr uint32_t indexOfUnit(uint64_t unit)
{
    return unit & ((1u << bitsOfIndexOfUnit) - 1);
}

constexpr uint64_t placeOfFunction(uint32_t number, uint32_t column)
{
    return static_cast<uint64_t>(number) << 32 | column;
}

struct FunctionsOfFile {
    uint32_t first;
    uint32_t end;
    uint32_t next;
};

struct DerivedUnits {
    uint32_t find(uint32_t function, uint32_t rank) const
    {
        uint32_t place = firstOfFunction[function] + rank;
        return place < firstOfFunction[function + 1] ? numbers[place] : absentUnit;
    }

    Vector<uint32_t> firstOfFunction;
    Vector<uint32_t> numbers;
};

class ListedUnitFinder {
public:
    ListedUnitFinder(const OrderFile& file, const Typed::Program& program, const Typed::Plan& plan, const FunctionNumbers& numbers, const LinkedFunctions& functions)
        : m_file(file)
        , m_program(program)
        , m_conventions(plan.conventions())
        , m_numbers(numbers)
        , m_functions(functions)
        , m_index(2 * (file.files.size() + program.modules.size() + program.functions.size() + file.units.size()))
    {
    }

    void findInto(Vector<uint32_t>& listed, BitVector& isListed);

private:
    static constexpr uint64_t saltOfPaths = 1ull << 62;
    static constexpr uint64_t saltOfNamesOfFiles = 1ull << 61;
    static constexpr uint64_t saltOfPathsFromEntry = 1ull << 60;
    static constexpr uint64_t saltOfHashes = 1ull << 59;

    ModuleID moduleOf(uint32_t function) const;
    std::span<const uint8_t> nameOf(uint32_t function) const;
    DerivedUnits collectDerivedUnits(uint32_t begin, uint32_t end) const;
    std::span<const uint8_t> pathFrom(std::span<const uint8_t> folder, std::span<const uint8_t> path);
    void indexFiles();
    uint32_t findFile(const Typed::Module&);
    void indexProgram();
    std::span<const uint8_t> nameInFile(std::span<const uint8_t> name, std::span<const uint8_t> path, unsigned numberOfNames);
    void indexNewFiles();
    uint32_t findNewFile(uint32_t recordedFile, std::span<const uint64_t> units);
    uint32_t findFunction(const OrderFile::Unit&, uint32_t file, int64_t shift, unsigned numberOfFilesToSkip) const;
    uint32_t findFunctionByName(const OrderFile::Unit&, uint32_t file);
    uint32_t findFunctionWithoutFile(const OrderFile::Unit&);
    void findFunctionsOfFile(std::span<const uint64_t> unitsOfFile, uint32_t file, std::span<uint32_t> functions);
    uint32_t findPosition(const OrderFile::Unit&, uint32_t function) const;

    const OrderFile& m_file;
    const Typed::Program& m_program;
    const Typed::CallingConventions& m_conventions;
    const FunctionNumbers& m_numbers;
    const LinkedFunctions& m_functions;
    OrderIndex m_index;
    std::span<const uint8_t> m_folderOfEntry;
    bool m_comparesPathsFromEntry { false };
    Vector<uint8_t> m_path;
    Vector<uint32_t> m_positionOfBody;
    Vector<uint32_t> m_positionOfNumber;
    Vector<FunctionsOfFile> m_functionsOfFiles;
    Vector<uint32_t> m_firstOfFile;
    Vector<std::span<const uint8_t>> m_pathsOfNewFiles;
    std::optional<OrderIndex> m_newFilesByFunction;
    BitVector m_hasNamesOfFile;
    DerivedUnits m_entries;
    DerivedUnits m_otherUnits;
};

ModuleID ListedUnitFinder::moduleOf(uint32_t function) const
{
    if (m_positionOfBody[function] == absentUnit)
        return { };
    const CompiledCode& body = m_functions[m_positionOfBody[function]];
    return body.sites.positions.isEmpty() ? ModuleID() : body.sites.positions[0].source;
}

std::span<const uint8_t> ListedUnitFinder::nameOf(uint32_t function) const
{
    StringID name = m_program.functions[function].name;
    return name ? m_program.string(name) : std::span<const uint8_t>();
}

DerivedUnits ListedUnitFinder::collectDerivedUnits(uint32_t begin, uint32_t end) const
{
    auto functionOf = [&](uint32_t number) {
        return m_conventions.unitAt(m_numbers.unitOf(FunctionID(number))).function.index();
    };
    DerivedUnits result;
    result.firstOfFunction.fill(0, m_program.functions.size() + 2);
    result.numbers.grow(end - begin);
    for (uint32_t number = begin; number < end; ++number)
        ++result.firstOfFunction[functionOf(number) + 2];
    for (size_t function = 2; function < result.firstOfFunction.size(); ++function)
        result.firstOfFunction[function] += result.firstOfFunction[function - 1];
    for (uint32_t number = begin; number < end; ++number)
        result.numbers[result.firstOfFunction[functionOf(number) + 1]++] = number;
    return result;
}

bool isFileOfProgram(const Typed::Module& module)
{
    return module.displayPath && Typed::isProgramModuleOf(module.kind) && !module.flags.contains(Typed::ModuleFlags::IsLibrary);
}

size_t startOfLastNames(std::span<const uint8_t> path, unsigned numberOfNames)
{
    size_t start = path.size();
    while (start && (path[start - 1] != '/' || --numberOfNames))
        --start;
    return start;
}

std::span<const uint8_t> folderOf(std::span<const uint8_t> path)
{
    size_t start = startOfLastNames(path, 1);
    return path.first(start ? start - 1 : 0);
}

std::span<const uint8_t> ListedUnitFinder::pathFrom(std::span<const uint8_t> folder, std::span<const uint8_t> path)
{
    m_path.shrink(0);
    while (!folder.empty()) {
        std::span<const uint8_t> restOfFolder = folder;
        std::span<const uint8_t> restOfPath = path;
        if (!equalSpans(takeWord(restOfFolder, '/'), takeWord(restOfPath, '/')))
            break;
        folder = restOfFolder;
        path = restOfPath;
    }
    while (!folder.empty()) {
        takeWord(folder, '/');
        m_path.append("../"_span8);
    }
    m_path.append(path);
    return m_path.span();
}

void ListedUnitFinder::indexFiles()
{
    for (uint32_t number = 0; number < m_file.files.size(); ++number)
        m_index.valueOf(m_file.files[number], saltOfPaths) = number;
    for (size_t index : Typed::modulesThatHostAsksFor(m_program)) {
        const Typed::Module& module = m_program.modules[index];
        if (isFileOfProgram(module)) {
            m_folderOfEntry = folderOf(m_program.string(module.displayPath));
            break;
        }
    }
    std::span<const uint8_t> folderOfRecordedEntry = folderOf(m_file.entry);
    if (equalSpans(folderOfRecordedEntry, m_folderOfEntry))
        return;
    for (uint32_t number = 0; number < m_file.files.size(); ++number)
        m_index.valueOf(pathFrom(folderOfRecordedEntry, m_file.files[number]), saltOfPathsFromEntry) = number;
    int64_t numberGained = 0;
    for (const auto& module : m_program.modules) {
        if (!isFileOfProgram(module))
            continue;
        std::span<const uint8_t> path = m_program.string(module.displayPath);
        numberGained += m_index.valueOf(pathFrom(m_folderOfEntry, path), saltOfPathsFromEntry) != absentUnit;
        numberGained -= m_index.valueOf(path, saltOfPaths) != absentUnit;
    }
    m_comparesPathsFromEntry = numberGained > 0;
}

uint32_t ListedUnitFinder::findFile(const Typed::Module& module)
{
    if (!module.displayPath)
        return absentUnit;
    std::span<const uint8_t> path = m_program.string(module.displayPath);
    if (!isFileOfProgram(module))
        return m_index.valueOf(path, saltOfPaths);
    uint32_t& file = m_comparesPathsFromEntry ? m_index.valueOf(pathFrom(m_folderOfEntry, path), saltOfPathsFromEntry) : m_index.valueOf(path, saltOfPaths);
    if (file == absentUnit) {
        file = m_firstOfFile.size();
        m_firstOfFile.append(absentUnit);
        m_pathsOfNewFiles.append(std::span<const uint8_t> { path });
    }
    return file;
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
    m_entries = collectDerivedUnits(m_numbers.firstDerived(), m_numbers.endOfClosureFunctions().index());
    m_otherUnits = collectDerivedUnits(m_numbers.endOfClosureFunctions().index(), m_numbers.numberOfFunctions());

    indexFiles();
    m_firstOfFile.fill(absentUnit, m_file.files.size());
    Vector<uint32_t> lastOfFile;
    Vector<uint32_t> namesakes(FillWith { }, m_program.strings.size() + 1, 0);
    ModuleID module;
    uint32_t current = absentUnit;
    for (uint32_t function = 0; function < numberOfFunctions; ++function) {
        ModuleID definedIn = moduleOf(function);
        if (!definedIn && m_positionOfBody[function] != absentUnit) {
            StringID name = m_program.functions[function].name;
            m_index.valueOf(nameOf(function), namesakes[name ? name.index() + 1 : 0]++) = function;
            continue;
        }
        if (definedIn && definedIn != module) {
            module = definedIn;
            uint32_t file = findFile(m_program.modules[module.index()]);
            current = absentUnit;
            if (file != absentUnit) {
                current = m_functionsOfFiles.size();
                while (lastOfFile.size() < m_firstOfFile.size())
                    lastOfFile.append(absentUnit);
                (lastOfFile[file] == absentUnit ? m_firstOfFile[file] : m_functionsOfFiles[lastOfFile[file]].next) = current;
                lastOfFile[file] = current;
                m_functionsOfFiles.append({ function, function, absentUnit });
            }
        }
        if (current != absentUnit)
            m_functionsOfFiles[current].end = function + 1;
    }
    m_hasNamesOfFile.ensureSize(m_firstOfFile.size());
}

std::span<const uint8_t> ListedUnitFinder::nameInFile(std::span<const uint8_t> name, std::span<const uint8_t> path, unsigned numberOfNames)
{
    m_path.shrink(0);
    m_path.append(name);
    m_path.append('/');
    m_path.append(path.subspan(startOfLastNames(path, numberOfNames)));
    return m_path.span();
}

void ListedUnitFinder::indexNewFiles()
{
    size_t numberOfKeys = m_file.units.size();
    for (uint32_t file = m_file.files.size(); file < m_firstOfFile.size(); ++file) {
        for (uint32_t index = m_firstOfFile[file]; index != absentUnit; index = m_functionsOfFiles[index].next)
            numberOfKeys += m_functionsOfFiles[index].end - m_functionsOfFiles[index].first;
    }
    m_newFilesByFunction.emplace(3 * numberOfKeys);
    for (uint32_t file = m_file.files.size(); file < m_firstOfFile.size(); ++file) {
        auto add = [&](std::span<const uint8_t> name, uint64_t place) {
            uint32_t& known = m_newFilesByFunction->valueOf(name, place);
            known = known == absentUnit || known == file ? file : ambiguousUnit;
        };
        for (uint32_t index = m_firstOfFile[file]; index != absentUnit; index = m_functionsOfFiles[index].next) {
            HashOfNames hash;
            for (uint32_t function = m_functionsOfFiles[index].first; function < m_functionsOfFiles[index].end; ++function) {
                if (moduleOf(function) && function != m_functionsOfFiles[index].first)
                    hash.add(nameOf(function));
                for (unsigned numberOfNames : { 2, 1 })
                    add(nameInFile(nameOf(function), m_pathsOfNewFiles[file - m_file.files.size()], numberOfNames), numberOfNames);
                uint32_t number = function - m_functionsOfFiles[index].first;
                if (number && moduleOf(function))
                    add(nameOf(function), placeOfFunction(number, m_functions[m_positionOfBody[function]].sites.positions[0].column));
            }
            if (!hash.isEmpty())
                add({ }, saltOfHashes + hash.value());
        }
    }
}

uint32_t ListedUnitFinder::findNewFile(uint32_t recordedFile, std::span<const uint64_t> units)
{
    if (m_pathsOfNewFiles.isEmpty())
        return absentUnit;
    if (!m_newFilesByFunction)
        indexNewFiles();
    size_t numberOfKnownUnits = 0;
    Vector<uint64_t> found;
    for (uint64_t keyOfUnit : units) {
        const OrderFile::Unit& unit = m_file.units[indexOfUnit(keyOfUnit)];
        uint32_t file = absentUnit;
        if (unit.number)
            file = m_newFilesByFunction->valueOf(unit.name, placeOfFunction(unit.number, unit.column));
        else if (unit.hashOfNames != absentUnit)
            file = m_newFilesByFunction->valueOf({ }, saltOfHashes + unit.hashOfNames);
        for (unsigned numberOfNames : { 2, 1 }) {
            if (file >= ambiguousUnit)
                file = std::min(file, m_newFilesByFunction->valueOf(nameInFile(unit.name, m_file.files[recordedFile], numberOfNames), numberOfNames));
        }
        numberOfKnownUnits += file != ambiguousUnit;
        if (file < ambiguousUnit)
            found.append(file);
    }
    sortKeys(found.mutableSpan());
    for (size_t index = 0; index + numberOfKnownUnits / 2 < found.size(); ++index) {
        if (found[index] == found[index + numberOfKnownUnits / 2])
            return static_cast<uint32_t>(found[index]);
    }
    return absentUnit;
}

uint32_t ListedUnitFinder::findFunction(const OrderFile::Unit& unit, uint32_t file, int64_t shift, unsigned numberOfFilesToSkip) const
{
    for (uint32_t index = m_firstOfFile[file]; index != absentUnit; index = m_functionsOfFiles[index].next) {
        const FunctionsOfFile& functions = m_functionsOfFiles[index];
        int64_t function = static_cast<int64_t>(functions.first) + unit.number + shift;
        if (function < functions.first || function >= functions.end)
            continue;
        if ((unit.name.empty() || equalSpans(unit.name, nameOf(static_cast<uint32_t>(function)))) && !numberOfFilesToSkip--)
            return static_cast<uint32_t>(function);
    }
    return absentUnit;
}

uint32_t ListedUnitFinder::findFunctionByName(const OrderFile::Unit& unit, uint32_t file)
{
    if (unit.name.empty())
        return absentUnit;
    if (!m_hasNamesOfFile.quickSet(file)) {
        for (uint32_t index = m_firstOfFile[file]; index != absentUnit; index = m_functionsOfFiles[index].next) {
            for (uint32_t function = m_functionsOfFiles[index].first; function < m_functionsOfFiles[index].end; ++function) {
                uint32_t& named = m_index.valueOf(nameOf(function), saltOfNamesOfFiles + file);
                named = named == absentUnit ? function : ambiguousUnit;
            }
        }
    }
    uint32_t function = m_index.valueOf(unit.name, saltOfNamesOfFiles + file);
    return function < ambiguousUnit && equalSpans(unit.name, nameOf(function)) ? function : absentUnit;
}

uint32_t ListedUnitFinder::findFunctionWithoutFile(const OrderFile::Unit& unit)
{
    if (unit.file == OrderFile::wholeProgram)
        return m_file.numberOfFunctions == m_numbers.firstDerived() && unit.number < m_numbers.firstDerived() ? unit.number : absentUnit;
    uint32_t function = m_index.valueOf(unit.name, unit.number);
    return function < ambiguousUnit && equalSpans(unit.name, nameOf(function)) ? function : absentUnit;
}

void ListedUnitFinder::findFunctionsOfFile(std::span<const uint64_t> unitsOfFile, uint32_t file, std::span<uint32_t> functions)
{
    int64_t shift = 0;
    unsigned numberOfEqualUnits = 0;
    const OrderFile::Unit* previous = nullptr;
    for (uint64_t unitOfFile : unitsOfFile) {
        uint32_t index = indexOfUnit(unitOfFile);
        const OrderFile::Unit& unit = m_file.units[index];
        numberOfEqualUnits = previous && previous->number == unit.number && previous->kind == unit.kind && previous->rank == unit.rank ? numberOfEqualUnits + 1 : 0;
        previous = &unit;
        uint32_t function = findFunction(unit, file, shift, numberOfEqualUnits);
        if (function == absentUnit && shift)
            function = findFunction(unit, file, 0, numberOfEqualUnits);
        if (function == absentUnit && !numberOfEqualUnits)
            function = findFunctionByName(unit, file);
        functions[index] = function;
        for (uint32_t candidate = function == absentUnit ? absentUnit : m_firstOfFile[file]; candidate != absentUnit; candidate = m_functionsOfFiles[candidate].next) {
            if (function >= m_functionsOfFiles[candidate].first && function < m_functionsOfFiles[candidate].end) {
                shift = static_cast<int64_t>(function) - m_functionsOfFiles[candidate].first - unit.number;
                break;
            }
        }
    }
}

uint32_t ListedUnitFinder::findPosition(const OrderFile::Unit& unit, uint32_t function) const
{
    if (unit.kind == 'b')
        return m_positionOfBody[function];
    uint32_t number = (unit.kind == 'e' ? m_entries : m_otherUnits).find(function, unit.rank);
    return number == absentUnit ? absentUnit : m_positionOfNumber[number];
}

void ListedUnitFinder::findInto(Vector<uint32_t>& listed, BitVector& isListed)
{
    indexProgram();
    const uint32_t count = std::min<size_t>(m_file.units.size(), (1u << bitsOfIndexOfUnit) - 1);
    Vector<uint32_t> functions(FillWith { }, count, absentUnit);
    Vector<uint64_t> units;
    for (uint32_t index = 0; index < count; ++index) {
        const OrderFile::Unit& unit = m_file.units[index];
        if (unit.file >= OrderFile::wholeProgram)
            functions[index] = findFunctionWithoutFile(unit);
        else
            units.append(static_cast<uint64_t>(unit.file) << 32 | index);
    }
    sortKeys(units.mutableSpan());
    for (size_t first = 0; first < units.size();) {
        uint32_t file = units[first] >> 32;
        size_t end = first;
        for (; end < units.size() && units[end] >> 32 == file; ++end) {
            const OrderFile::Unit& unit = m_file.units[static_cast<uint32_t>(units[end])];
            uint64_t kind = unit.kind == 'b' ? 0 : unit.kind == 'e' ? 1 : 2;
            uint64_t key = static_cast<uint64_t>(unit.number) << bitsOfKindOfUnit | kind;
            key = key << bitsOfRankOfUnit | std::min<uint32_t>(unit.rank, (1u << bitsOfRankOfUnit) - 1);
            units[end] = key << bitsOfIndexOfUnit | static_cast<uint32_t>(units[end]);
        }
        std::span<uint64_t> unitsOfFile = units.mutableSpan().subspan(first, end - first);
        sortKeys(unitsOfFile);
        findFunctionsOfFile(unitsOfFile, file, functions.mutableSpan());
        size_t numberOfMissing = 0;
        for (uint64_t unit : unitsOfFile) {
            if (functions[indexOfUnit(unit)] == absentUnit)
                unitsOfFile[numberOfMissing++] = unit;
        }
        if (2 * numberOfMissing > unitsOfFile.size()) {
            if (uint32_t newFile = findNewFile(file, unitsOfFile.first(numberOfMissing)); newFile != absentUnit)
                findFunctionsOfFile(unitsOfFile.first(numberOfMissing), newFile, functions.mutableSpan());
        }
        first = end;
    }
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t position = functions[index] == absentUnit ? absentUnit : findPosition(m_file.units[index], functions[index]);
        if (position != absentUnit && !isListed.quickSet(position))
            listed.append(position);
    }
}

struct OrderOfUnits {
    Vector<uint32_t> positions;
    uint32_t numberOfListed;
};

OrderOfUnits orderUnits(std::span<const uint32_t> listed, std::span<const FinalJump> finalJumps, const LinkedFunctions& functions)
{
    const uint32_t count = static_cast<uint32_t>(functions.size());
    auto fallsIntoNext = [&](uint32_t position) {
        return position + 1 < count && finalJumps[position].jumpsToNextBody;
    };
    BitVector isOrdered(count);
    Vector<uint64_t> runs;
    uint64_t piece = 0;
    uint64_t sizeOfPiece = 0;
    for (uint32_t position : listed) {
        while (position && fallsIntoNext(position - 1))
            --position;
        if (isOrdered.quickSet(position))
            continue;
        runs.append(piece << 32 | position);
        for (uint32_t body = position;; ++body) {
            sizeOfPiece += functions[body].body.bytes.size();
            if (!fallsIntoNext(body))
                break;
            isOrdered.quickSet(body + 1);
        }
        if (sizeOfPiece >= MB) {
            ++piece;
            sizeOfPiece = 0;
        }
    }
    sortKeys(runs.mutableSpan());

    OrderOfUnits order;
    for (uint64_t run : runs) {
        uint32_t position = static_cast<uint32_t>(run);
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

ResolvedCallees reorderCallees(const ResolvedCallees& callees, std::span<const uint32_t> order, std::span<const uint32_t> newPositions)
{
    ResolvedCallees result;
    for (uint32_t position : order) {
        result.firstCallee.append(static_cast<uint32_t>(result.positions.size()));
        for (uint32_t index = callees.firstCallee[position]; index < callees.firstCallee[position + 1]; ++index) {
            OptionalIndex callee = callees.positions[index];
            result.positions.append(callee ? OptionalIndex { newPositions[*callee] } : OptionalIndex { });
        }
    }
    result.firstCallee.append(static_cast<uint32_t>(result.positions.size()));
    RELEASE_ASSERT(result.positions.size() == callees.positions.size());
    return result;
}

} // anonymous namespace

void LinkedFunctions::reorder(std::span<const uint32_t> order)
{
    RELEASE_ASSERT(m_newPositions.isEmpty() && order.size() == size());
    m_newPositions.fill(UINT32_MAX, order.size());
    Vector<CompiledCode*> code(order.size());
    for (size_t position = 0; position < order.size(); ++position) {
        RELEASE_ASSERT(m_newPositions[order[position]] == UINT32_MAX);
        m_newPositions[order[position]] = static_cast<uint32_t>(position);
        code[position] = &m_functions[order[position]];
    }
    m_functions = ListOfKeptCode(WTF::move(code));
}

Vector<FinalJump> orderCodeByFiles(const Typed::Program& program, const Typed::Plan& plan, const FunctionNumbers& numbers, LinkedFunctions& functions, ResolvedCallees& callees, BuildContext& context)
{
    Vector<uint32_t> listed;
    BitVector isListed(functions.size());
    OrderFile file;
    for (std::span<const uint8_t> rest = context.orderFile(); file.takeNext(rest);)
        ListedUnitFinder(file, program, plan, numbers, functions).findInto(listed, isListed);
    if (listed.isEmpty()) {
        context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::UnmatchedOrderFiles });
        return { };
    }
    context.setStartRanks(rankFunctionsByListedUnits(listed.span(), functions, program.functions.size()));
    Vector<FinalJump> finalJumps = findFinalJumps(functions, callees, context);
    OrderOfUnits order = orderUnits(listed.span(), finalJumps.span(), functions);
    context.setNumberOfUnitsOfStart(order.numberOfListed);
    functions.reorder(order.positions.span());
    callees = reorderCallees(callees, order.positions.span(), functions.newPositions());
    return order.positions.map([&](uint32_t position) {
        return finalJumps[position];
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
