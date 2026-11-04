/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTFacts.h"
#include "AOTImageTestBodies.h"
#include "AOTModuleData.h"
#include "AOTModuleLists.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTProgramTables.h"
#include "AOTTableParts.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

std::span<const NativeTest> tablesOfManyModulesTests();

static constexpr std::array<unsigned, 3> threadCountsForManyModules { 1, 2, 16 };

// main.js imports every user. A user imports barrel.js, which exports what one.js and two.js export, and some import a user in front of them.
// What a user exports, what it requires of the barrel and where it lies go by its number, so no two rows in a row are as long as each other.
static constexpr uint32_t numberOfManyModules = 1100;
static constexpr uint32_t barrelAmongManyModules = 1;
static constexpr uint32_t firstUserAmongManyModules = 4;
static constexpr uint32_t variablesInFrontOfThoseOfUsers = 3;
// The body of a module has the number of the module. No body has code: the image is built of the code of this function alone.
static constexpr uint32_t functionWithCodeAmongManyModules = numberOfManyModules;

static bool userExportsVariable(uint32_t module) { return !(module % 3); }
static bool userExportsNameOfBarrel(uint32_t module) { return !(module % 2); }
static bool userRequiresOneName(uint32_t module) { return module % 3 == 1; }
static bool userRequiresNameForExportStatement(uint32_t module) { return module % 3 == 2; }
static bool displayPathOfUserIsNoEndOfItsPath(uint32_t module) { return !(module % 13); }

static uint32_t variableOfUser(uint32_t module)
{
    return variablesInFrontOfThoseOfUsers + (module - 1) / 3 - 1;
}

// 0: none.
static uint32_t otherUserThatUserImports(uint32_t module)
{
    return module % 4 == 1 && module >= firstUserAmongManyModules + 2 ? firstUserAmongManyModules + (module - firstUserAmongManyModules) / 2 : 0;
}

// Two users in a row share a directory, the next two leave it for one beside it, and the one above changes with every fourth. Every directory comes again further on.
static String directoryOfUser(uint32_t module)
{
    return makeString('d', module / 4 % 7, "/e"_s, module / 2 % 3, '/');
}

static void appendUserToManyModules(StringBuilder& text, uint32_t module)
{
    String directory = directoryOfUser(module);
    text.append("module m"_s, module, " path=\"/root/"_s, directory, 'u', module, ".js\" displayPath=\""_s);
    if (displayPathOfUserIsNoEndOfItsPath(module))
        text.append("shown"_s, module, ".js"_s);
    else
        text.append(directory, 'u', module, ".js"_s);
    text.append("\" body=f"_s, module, " requests=[{target=m"_s, barrelAmongManyModules, " completes=true}"_s);
    if (uint32_t other = otherUserThatUserImports(module))
        text.append(", {target=m"_s, other, " completes=true}"_s);
    text.append(']');

    if (userExportsNameOfBarrel(module) || userExportsVariable(module)) {
        text.append(" exports=["_s);
        if (userExportsNameOfBarrel(module))
            text.append("{name=\"c"_s, module, "\" kind=Indirect module=m"_s, barrelAmongManyModules, " importName=\"single\"}"_s);
        if (userExportsVariable(module))
            text.append(userExportsNameOfBarrel(module) ? ", "_s : ""_s, "{name=\"v"_s, module, "\" variable=g"_s, variableOfUser(module), '}');
        text.append(']');
    }

    if (userRequiresOneName(module))
        text.append(" requiredNames=[{module=m"_s, barrelAmongManyModules, " name=\"single\"}]"_s);
    if (userRequiresNameForExportStatement(module))
        text.append(" requiredNames=[{module=m"_s, barrelAmongManyModules, " name=\"single\" mayBeAbsent=true}, {module=m"_s, barrelAmongManyModules, " name=\"shared\" exportName=\"s"_s, module, "\" specifier=\"./barrel"_s, module, "\"}]"_s);
    text.append('\n');
}

static String textOfProgramWithManyModules()
{
    StringBuilder text;
    text.append("variable g0 name=\"shared\" module=m2 declared={type=anything} initialization=Undefined\n"
        "variable g1 name=\"single\" module=m2 declared={type=anything} initialization=Undefined\n"
        "variable g2 name=\"shared\" module=m3 declared={type=anything} initialization=Undefined\n"_s);
    for (uint32_t module = firstUserAmongManyModules; module < numberOfManyModules; ++module) {
        if (userExportsVariable(module))
            text.append("variable g"_s, variableOfUser(module), " name=\"own\" module=m"_s, module, " declared={type=anything} initialization=Undefined\n"_s);
    }

    text.append("module m0 path=\"/root/main.js\" displayPath=\"main.js\" body=f0 requests=["_s);
    for (uint32_t module = firstUserAmongManyModules; module < numberOfManyModules; ++module)
        text.append(module > firstUserAmongManyModules ? ", "_s : ""_s, "{target=m"_s, module, " completes=true}"_s);
    text.append("]\n"
        "module m1 path=\"/root/barrel.js\" displayPath=\"barrel.js\" body=f1 requests=[{target=m2 isDynamicStarExport=true completes=true}, {target=m3 isDynamicStarExport=true completes=true}]\n"
        "module m2 path=\"/root/one.js\" displayPath=\"one.js\" body=f2 exports=[{name=\"shared\" variable=g0}, {name=\"single\" variable=g1}]\n"
        "module m3 path=\"/root/two.js\" displayPath=\"two.js\" body=f3 exports=[{name=\"shared\" variable=g2}]\n"_s);
    for (uint32_t module = firstUserAmongManyModules; module < numberOfManyModules; ++module)
        appendUserToManyModules(text, module);
    text.append("entry module=m0\n"
        "function f0 name=\"main\" module=m0 kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
        "    v2 = NewClosure function=f"_s, functionWithCodeAmongManyModules, "\n"
        "    v3 = Call v1, -, v2\n"
        "    Return\n}\n"_s);
    for (uint32_t module = 1; module < numberOfManyModules; ++module)
        text.append("function f"_s, module, " name=\"body"_s, module, "\" module=m"_s, module, " kind=Module results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append("function f"_s, functionWithCodeAmongManyModules, " name=\"handedOut\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append(runtimeFunctionsText(functionWithCodeAmongManyModules + 1));
    return text.toString();
}

template<typename T>
static const T& entryOfTableOfManyModules(std::span<const T> table, size_t index)
{
    AOT_TEST_CHECK(index < table.size());
    return table[index];
}

static String stringInTablesOfManyModules(const Typed::Program& program, uint32_t number)
{
    AOT_TEST_CHECK(number < program.strings.size());
    return program.strings.toWTFString(StringID(number));
}

static String nameInTablesOfManyModules(const Typed::Program& program, const ImageNumbers& numbers, uint32_t name)
{
    return program.strings.toWTFString(numbers.identifiers().at(name).text);
}

static String pathInTablesOfManyModules(const ProgramTables& tables, const Typed::Program& program, const ModuleData& record)
{
    String path = stringInTablesOfManyModules(program, record.publicPathOrName);
    std::optional<uint32_t> directory = optionalIDInImage<uint32_t>(record.directory);
    while (directory) {
        const ModuleDirectoryData& entry = entryOfTableOfManyModules(tables.moduleDirectories(), *directory);
        path = makeString(stringInTablesOfManyModules(program, entry.name), path);
        directory = entry.parentDirectory();
    }
    return path;
}

// Goes through the records of the program in order, with one count for each list, as one thread would write them. Where the lists of a chunk were put at a wrong place, or an entry still counts from the start of its chunk, a row holds what belongs to another module.
static void checkRowsOfManyModules(const ProgramTables& tables, const TableInputs& inputs, const AnswersAboutModules& answers, const ImageNumbers& numbers)
{
    const Typed::Program& program = inputs.program;
    std::span<const ModuleData> records = tables.modules();
    AOT_TEST_CHECK(records.size() == program.modules.size() + 1);
    uint32_t requests = 0;
    uint32_t exports = 0;
    uint32_t indirectExports = 0;
    uint32_t requiredNames = 0;
    uint32_t exportStatements = 0;
    uint32_t displayPaths = 0;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Typed::Module& module = program.modules[index];
        const ModuleData& record = records[index];
        AOT_TEST_CHECK(record.firstRequest == requests && record.firstExport == exports && !record.firstSyntheticExport && record.firstRequiredName == requiredNames);

        String path = program.strings.toWTFString(module.path);
        String displayPath = program.strings.toWTFString(module.displayPath);
        AOT_TEST_CHECK(record.directory != Number24::max);
        AOT_TEST_CHECK(pathInTablesOfManyModules(tables, program, record) == path);
        if (record.charactersInFrontOfDisplayPath == ModuleData::displayPathIsListed) {
            const ModuleDisplayPathData& listed = entryOfTableOfManyModules(tables.moduleDisplayPaths(), displayPaths++);
            AOT_TEST_CHECK(listed.module == index && stringInTablesOfManyModules(program, listed.path) == displayPath);
            AOT_TEST_CHECK(!path.endsWith(displayPath));
        } else
            AOT_TEST_CHECK(path.substring(record.charactersInFrontOfDisplayPath) == displayPath);

        for (const Typed::ModuleRequest& request : module.requests) {
            if (answers.ofModules[request.target.index()].containsAll({ AnswerAboutModule::IsNotWorthVisiting, AnswerAboutModule::IsNeverEvaluated }))
                continue;
            const ModuleRequestData& entry = entryOfTableOfManyModules(tables.moduleRequests(), requests++);
            AOT_TEST_CHECK(entry.target() == request.target.index() && entry.isDynamicStarExport == request.isDynamicStarExport && !entry.isRequire);
        }
        for (const Typed::ModuleExport& moduleExport : module.exports) {
            if (moduleExport.kind == ModuleExportKind::Variable && !inputs.plan.variable(moduleExport.variable).isDescribedToRunTime)
                continue;
            const ModuleExportData& entry = entryOfTableOfManyModules(tables.moduleExports(), exports++);
            AOT_TEST_CHECK(entry.kind() == moduleExport.kind);
            AOT_TEST_CHECK(nameInTablesOfManyModules(program, numbers, entry.name) == program.strings.toWTFString(moduleExport.name));
            if (moduleExport.kind != ModuleExportKind::Indirect)
                continue;
            AOT_TEST_CHECK(entry.indirectExport == indirectExports);
            const IndirectExportData& indirect = entryOfTableOfManyModules(tables.indirectExports(), indirectExports++);
            AOT_TEST_CHECK(indirect.module == moduleExport.module.index());
            AOT_TEST_CHECK(nameInTablesOfManyModules(program, numbers, indirect.importName) == program.strings.toWTFString(moduleExport.importName));
        }
        if (answers.has(ModuleID(index), AnswerAboutModule::IsNeverEvaluated))
            continue;
        for (const Typed::RequiredName& requiredName : module.requiredNames) {
            if (requiredName.specifier) {
                const ExportStatementData& statement = entryOfTableOfManyModules(tables.exportStatements(), exportStatements++);
                AOT_TEST_CHECK(statement.requiredName == requiredNames);
                AOT_TEST_CHECK(stringInTablesOfManyModules(program, statement.exportName) == program.strings.toWTFString(requiredName.exportName));
                AOT_TEST_CHECK(stringInTablesOfManyModules(program, statement.specifier) == program.strings.toWTFString(requiredName.specifier));
            }
            const RequiredNameData& entry = entryOfTableOfManyModules(tables.requiredNames(), requiredNames++);
            AOT_TEST_CHECK(entry.exporter == requiredName.module.index() && entry.mayBeAbsent == requiredName.mayBeAbsent);
            AOT_TEST_CHECK(nameInTablesOfManyModules(program, numbers, entry.name) == program.strings.toWTFString(requiredName.name));
        }
    }

    const ModuleData& endRecord = records.back();
    AOT_TEST_CHECK(endRecord.firstRequest == requests && endRecord.firstExport == exports && !endRecord.firstSyntheticExport && endRecord.firstRequiredName == requiredNames);
    AOT_TEST_CHECK(tables.moduleRequests().size() == requests && tables.moduleExports().size() == exports && tables.indirectExports().size() == indirectExports);
    AOT_TEST_CHECK(tables.requiredNames().size() == requiredNames && tables.exportStatements().size() == exportStatements && tables.moduleDisplayPaths().size() == displayPaths);

    // Each list goes through several chunks.
    constexpr uint32_t numberOfUsers = numberOfManyModules - firstUserAmongManyModules;
    AOT_TEST_CHECK(requests >= 2 * numberOfUsers && indirectExports == numberOfUsers / 2 && requiredNames >= numberOfUsers - 2);
    AOT_TEST_CHECK(exportStatements >= numberOfUsers / 3 && displayPaths >= numberOfUsers / 13);
}

static void checkDirectoriesOfManyModules(const ProgramTables& tables, const Typed::Program& program)
{
    HashSet<String> startsOfPaths;
    for (const Typed::Module& module : program.modules) {
        String path = program.strings.toWTFString(module.path);
        for (unsigned index = 0; index < path.length(); ++index) {
            if (path[index] == '/')
                startsOfPaths.add(path.left(index + 1));
        }
    }
    std::span<const ModuleDirectoryData> directories = tables.moduleDirectories();
    AOT_TEST_CHECK(directories.size() == startsOfPaths.size());
    for (size_t index = 0; index < directories.size(); ++index)
        AOT_TEST_CHECK(!directories[index].parentDirectory() || *directories[index].parentDirectory() < index);
}

static void checkManyModulesAreFoundByPath(const ProgramTables& tables, const Typed::Program& program, const AnswersAboutModules& answers)
{
    size_t numberOfRecords = tables.modules().size();
    ModuleNumbers byPath(tables.modulesByPath(), numberOfRecords);
    size_t numberEvaluated = std::ranges::count_if(answers.ofModules, [](OptionSet<AnswerAboutModule> ofModule) {
        return !ofModule.contains(AnswerAboutModule::IsNeverEvaluated);
    });
    AOT_TEST_CHECK(byPath.size() == numberEvaluated && numberEvaluated == numberOfManyModules);
    for (size_t place = 1; place < byPath.size(); ++place)
        AOT_TEST_CHECK(std::ranges::lexicographical_compare(program.string(program.modules[byPath[place - 1]].path), program.string(program.modules[byPath[place]].path)));

    size_t numberOfBuckets = ModuleByHashOfPath::numberOfBuckets(byPath.size());
    ModuleNumbers firstInBucket(tables.firstModulesByHashOfPath(), numberOfRecords);
    std::span<const uint32_t> entries = tables.modulesByHashOfPath();
    AOT_TEST_CHECK(firstInBucket.size() == numberOfBuckets + 1 && entries.size() == byPath.size() && firstInBucket[numberOfBuckets] == entries.size());
    for (uint32_t module : byPath) {
        String path = program.strings.toWTFString(program.modules[module].path);
        ModuleByHashOfPath sought(numberOfRecords, StringView(path).hash());
        size_t bucket = sought.bucket(numberOfBuckets);
        bool isFound = false;
        for (size_t place = firstInBucket[bucket]; place < firstInBucket[bucket + 1]; ++place) {
            uint32_t moduleOrMore = sought.moduleOrMore(entries[place]);
            isFound |= sought.isModule(moduleOrMore) && moduleOrMore == module;
        }
        AOT_TEST_CHECK(isFound);
    }
}

// The list of a module holds some of what the module requests, in the order of the requests, and no module of this program requests another twice.
static void checkWalkOfManyModules(const ProgramTables& tables, std::span<const uint16_t> words, Number24 ModuleData::* firstInList)
{
    std::span<const ModuleData> records = tables.modules();
    ModuleNumbers targets(words, records.size());
    AOT_TEST_CHECK(records.back().*firstInList == targets.size());
    for (uint32_t module = 0; module + 1 < records.size(); ++module) {
        uint32_t end = records[module + 1].*firstInList;
        AOT_TEST_CHECK(records[module].*firstInList <= end);
        uint32_t request = records[module].firstRequest;
        uint32_t endOfRequests = records[module + 1].firstRequest;
        for (uint32_t place = records[module].*firstInList; place < end; ++place) {
            AOT_TEST_CHECK(targets[place] != module);
            while (request < endOfRequests && tables.moduleRequests()[request].target() != targets[place])
                ++request;
            AOT_TEST_CHECK(request++ < endOfRequests);
        }
    }
}

static void appendBitsOfManyModules(Vector<uint8_t>& bytes, const BitVector& bits)
{
    for (size_t bit : bits) {
        uint32_t word = static_cast<uint32_t>(bit);
        bytes.append(asByteSpan(word));
    }
    uint32_t end = UINT32_MAX;
    bytes.append(asByteSpan(end));
}

// The image for the address 0, and behind it what the group of the modules names.
static Vector<uint8_t> bytesOfImageWithManyModules(unsigned threadCount)
{
    TestBuild build(textOfProgramWithManyModules(), { .threadCount = threadCount });
    BuildContext& context = build.context();
    const Typed::Program& program = build.program();
    AOT_TEST_CHECK(program.modules.size() == numberOfManyModules && divideIntoChunks(numberOfManyModules).size() >= 4);

    TestImage image = builtImage(build, bodies(plainBody(functionWithCodeAmongManyModules)));
    const ProgramTables& tables = image.image->programTables();
    const ImageNumbers& numbers = *image.built.numbers;
    BodyFactsForTables bodyFacts = collectBodyFactsForTables(program, build.plan(), image.built.functions, context);
    TableInputs inputs { program, build.facts(), build.plan(), image.built.functions, bodyFacts, context.options(), &context };

    // Both halves of the group once more, for the numbers of the image.
    GatheredMentions noMentions;
    NamesOfTables names(program);
    nameWhatModuleTablesName(inputs, noMentions, names);
    for (size_t string : names.strings)
        AOT_TEST_CHECK(numbers.namedStrings().get(string));
    for (size_t text : names.textsNamedAsKeys)
        AOT_TEST_CHECK(numbers.identifiers().numberOfText(StringID(static_cast<uint32_t>(text))));
    // The namer leaves what it has asked, and the encoder takes it.
    AOT_TEST_CHECK(inputs.answersAboutModules);
    AnswersAboutModules answers = *inputs.answersAboutModules;
    FirstTableParts firstParts;
    TableEncoder encoder(inputs, numbers, &firstParts, TablePartID::Modules, context);
    encodeModuleTables(encoder);
    AOT_TEST_CHECK(!inputs.answersAboutModules && !context.diagnostics().hasErrors());

    uint64_t startOfModules = 0;
    for (unsigned part = 0; part < static_cast<unsigned>(TablePartID::Modules); ++part)
        startOfModules = roundUpToMultipleOf<8>(startOfModules) + image.built.bytesOfTableParts[part];
    startOfModules = roundUpToMultipleOf<8>(startOfModules);
    size_t sizeOfModules = encoder.part.size();
    AOT_TEST_CHECK(image.built.bytesOfTableParts[static_cast<unsigned>(TablePartID::Modules)] == sizeOfModules && sizeOfModules > numberOfManyModules * sizeof(ModuleData));
    AOT_TEST_CHECK(equalSpans(std::span<const uint8_t>(tables.at<uint8_t>(static_cast<uint32_t>(startOfModules)), sizeOfModules), std::span<const uint8_t>(encoder.part.bytesAt(0, sizeOfModules))));

    // An encoder that finds nothing left for it asks for itself.
    TableEncoder encoderWithoutNamer(inputs, numbers, &firstParts, TablePartID::Modules, context);
    encodeModuleTables(encoderWithoutNamer);
    AOT_TEST_CHECK(encoderWithoutNamer.part.size() == sizeOfModules && equalSpans(encoderWithoutNamer.part.bytesAt(0, sizeOfModules), encoder.part.bytesAt(0, sizeOfModules)));

    checkRowsOfManyModules(tables, inputs, answers, numbers);
    checkDirectoriesOfManyModules(tables, program);
    checkManyModulesAreFoundByPath(tables, program, answers);
    checkWalkOfManyModules(tables, tables.linkRequests(), &ModuleData::firstLinkRequest);
    checkWalkOfManyModules(tables, tables.evaluationRequests(), &ModuleData::firstEvaluationRequest);
    AOT_TEST_CHECK(ModuleNumbers(tables.linkRequests(), tables.modules().size()).size() > numberOfManyModules / 2);

    Vector<uint8_t> bytes(std::as_const(image.bytes).span());
    relocateImage(bytes.mutableSpan(), 0);
    appendBitsOfManyModules(bytes, names.strings);
    appendBitsOfManyModules(bytes, names.textsNamedAsKeys);
    appendBitsOfManyModules(bytes, names.variables);
    appendBitsOfManyModules(bytes, names.functionsWithClosureCell);
    return bytes;
}

static void testTablesOfManyModules()
{
    Vector<uint8_t> expected = bytesOfImageWithManyModules(threadCountsForManyModules[0]);
    for (unsigned threadCount : std::span(threadCountsForManyModules).subspan(1))
        AOT_TEST_CHECK(bytesOfImageWithManyModules(threadCount) == expected);
}

static void testLoopsBesideEachOther()
{
    constexpr std::array<unsigned, 6> counts { 1, 0, 700, 1, 2 * indicesInRangeOfLoop + 3, 0 };
    for (unsigned threadCount : threadCountsForManyModules) {
        ParallelLoops loops(threadCount);
        std::array<Vector<unsigned>, counts.size()> visits;
        for (size_t loop = 0; loop < counts.size(); ++loop)
            visits[loop].fill(0, counts[loop]);
        forEachIndexOfLoopsInParallel(loops, {
            { counts[0], [&](unsigned index) { ++visits[0][index]; } },
            { counts[1], [&](unsigned index) { ++visits[1][index]; } },
            { counts[2], [&](unsigned index) { ++visits[2][index]; } },
            { counts[3], [&](unsigned index) { ++visits[3][index]; } },
            { counts[4], [&](unsigned index) { ++visits[4][index]; } },
            { counts[5], [&](unsigned index) { ++visits[5][index]; } },
        });
        for (const Vector<unsigned>& visitsOfLoop : visits) {
            AOT_TEST_CHECK(std::ranges::all_of(visitsOfLoop, [](unsigned times) {
                return times == 1;
            }));
        }
        forEachIndexOfLoopsInParallel(loops, { });
    }

    size_t covered = 0;
    for (unsigned range = 0; range < numberOfRangesOfIndices(counts[4]); ++range) {
        auto [first, end] = boundsOfRangeOfIndices(range, counts[4]);
        AOT_TEST_CHECK(first == covered && end > first);
        covered = end;
    }
    AOT_TEST_CHECK(covered == counts[4]);
}

std::span<const NativeTest> tablesOfManyModulesTests()
{
    static constexpr NativeTest tests[] = {
        { "image/module-tables/loops-beside-each-other"_s, testLoopsBesideEachOther },
        { "image/module-tables/many-modules-read-back-at-any-number-of-threads"_s, testTablesOfManyModules },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
