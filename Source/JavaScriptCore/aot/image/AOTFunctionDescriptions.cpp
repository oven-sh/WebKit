/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionDescriptions.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include <wtf/ASCIICType.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static FunctionID subjectOf(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    const CreatorRecord& creator = function.creator;
    FunctionID subject = id;
    switch (function.role) {
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
    case FunctionRole::TableLookup:
        break;
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
        subject = creator.as<CreatorRecord::Entrypoint>().target;
        break;
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
        subject = creator.as<CreatorRecord::Version>().provenVersion;
        break;
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
        subject = creator.as<CreatorRecord::Coroutine>().ramp;
        break;
    case FunctionRole::BoundFunction:
        subject = creator.as<CreatorRecord::BoundFunction>().target;
        break;
    case FunctionRole::ResultWorker:
        subject = creator.as<CreatorRecord::ResultWorker>().origin;
        break;
    }
    FunctionID asRead = findFunctionAsRead(&program, subject);
    return asRead ? asRead : subject;
}

static String lowerCaseWordsOf(ASCIILiteral enumeratorName)
{
    StringBuilder words;
    for (char character : enumeratorName.span()) {
        if (isASCIIUpper(character) && !words.isEmpty())
            words.append(' ');
        words.append(toASCIILower(character));
    }
    return words.toString();
}

static String roleWord(const Program& program, FunctionID id)
{
    FunctionID subject = subjectOf(program, id);
    if (program.at(subject).isModuleBody())
        return program.isEntry(subject) ? "entry"_s : "module"_s;

    auto& function = program.at(id);
    switch (function.role) {
    case FunctionRole::Source:
        return function.has(FunctionFlags::IsChecked) ? "typed"_s : "untyped"_s;
    case FunctionRole::GenericEntrypoint:
        return makeString("generic-"_s, roleWord(program, function.creator.as<CreatorRecord::Entrypoint>().target));
    case FunctionRole::GenericVersion:
        return "version"_s;
    case FunctionRole::CoroutineRamp:
        return "ramp"_s;
    case FunctionRole::BoundFunction:
        return "bound"_s;
    case FunctionRole::ResultWrapper:
        return "result-wrapper"_s;
    case FunctionRole::ResultWorker:
        return "result-worker"_s;
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
        return "parameter-wrapper"_s;
    case FunctionRole::OutlinedBody:
        return "outlined"_s;
    case FunctionRole::Serializer:
        return "serializer"_s;
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return lowerCaseWordsOf(nameOf(function.role));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static char heatLetter(const Program& program, const Heat& heat, FunctionID id)
{
    if (heat.isCompact(program, id))
        return 'k';
    switch (heat.of(program, id)) {
    case HeatClass::Unknown:
        return 'u';
    case HeatClass::RunsOnce:
        return 'o';
    case HeatClass::Cold:
        return 'c';
    case HeatClass::Warm:
        return 'w';
    case HeatClass::Hot:
        return 'h';
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<String> describeFunctions(const Program& program, const Heat& heat)
{
    auto text = [&](StringID id) {
        if (!id)
            return String();
        String result = program.strings.toWTFString(id);
        for (auto [character, written] : { std::pair { "\\"_s, "\\\\"_s }, std::pair { "\t"_s, "\\t"_s }, std::pair { "\n"_s, "\\n"_s }, std::pair { "\r"_s, "\\r"_s } })
            result = makeStringByReplacingAll(result, character, written);
        return result;
    };
    unsigned count = program.functions.size();
    Vector<String> classNames(count);
    for (auto& layout : program.layouts) {
        if (layout.kind != LayoutKind::Instance)
            continue;
        String className = text(layout.name);
        for (const ClassMember& member : layout.classFlow.namedMembers) {
            if (member.function)
                classNames[member.function.index()] = className;
        }
        for (const ClassMember& member : layout.classFlow.members)
            classNames[member.function.index()] = className;
    }
    Vector<String> paths(program.modules.size(), [&](size_t i) {
        return text(program.modules[i].path);
    });
    return Vector<String>(count, [&](size_t i) {
        FunctionID id(i);
        auto& function = program.functions[i];
        const String& className = classNames[subjectOf(program, id).index()];
        String path = function.module ? paths[function.module.index()] : String();
        return makeString(className, className.isEmpty() ? ""_s : "."_s, text(function.name), '\t', roleWord(program, id), '\t', i, '\t', path, '\t', function.position, '\t', heatLetter(program, heat, id));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
