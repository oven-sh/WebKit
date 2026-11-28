/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltProgramTestSupport.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageFlagNames.h"
#include "AOTShellProgramTestSupport.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

static std::unique_ptr<CompileOutput> buildProgram(ASCIILiteral programText, OptionWords optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(programText, optionWords);
    });
    return output;
}

OptionSet<ImageFlag> imageFlagsOf(ASCIILiteral programText, OptionWords optionWords)
{
    std::optional<OptionSet<ImageFlag>> flags = Image::flags(buildProgram(programText, optionWords)->section(Wire::ResultSection::Image));
    AOT_TEST_CHECK(flags);
    return *flags;
}

Vector<Wire::WorkerStartRecord> workerStartsWithoutNameOf(ASCIILiteral programText, OptionWords optionWords)
{
    return Vector<Wire::WorkerStartRecord>(buildProgram(programText, optionWords)->records<Wire::WorkerStartRecord>(Wire::ResultSection::WorkerStartsWithoutName));
}

static String namesOf(OptionSet<ImageFlag> flags)
{
    StringBuilder names;
    auto append = [&](const auto&... parts) {
        names.append(names.isEmpty() ? ""_s : ", "_s, parts...);
    };
    for (auto [flag, name] : namedImageFlags) {
        if (flags.contains(flag))
            append(name);
        flags.remove(flag);
    }
    if (!flags.isEmpty())
        append("0x"_s, hex(flags.toRaw(), Lowercase));
    return names.toString();
}

void checkImageFlags(std::initializer_list<ExpectedImageFlags> expectations)
{
    const ExpectedImageFlags* firstMismatch = nullptr;
    unsigned mismatchCount = 0;
    for (const ExpectedImageFlags& expected : expectations) {
        OptionSet<ImageFlag> found = imageFlagsOf(expected.programText, expected.optionWords);
        if (found == expected.flags)
            continue;
        dataLogLn("ImageFlags: line ", expected.location.line(), ": unexpected [", namesOf(found - expected.flags), "], missing [", namesOf(expected.flags - found), "]");
        if (!mismatchCount++)
            firstMismatch = &expected;
    }
    if (firstMismatch)
        failNativeTestBecause(firstMismatch->location.file_name(), static_cast<int>(firstMismatch->location.line()), makeString(mismatchCount, " of "_s, expectations.size(), " images differ"_s));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
