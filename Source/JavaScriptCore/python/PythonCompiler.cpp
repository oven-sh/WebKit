/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonCompiler.h"

#include "BytecodeGenerator.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironmentInlines.h"
#include "ParserError.h"
#include "PyDict.h"
#include "PyObjects.h"
#include "PythonASTModule.h"
#include "PythonASTOptimizer.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodeGenerator.h"
#include "PythonCodecs.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "PythonParser.h"
#include "PythonSymbolTable.h"
#include "PythonSyntaxTreeSource.h"
#include "PythonSyntaxWarnings.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace Python {

static bool generateAll(VM&, UnlinkedFunctionExecutable*, const SourceCode& parentSource, ParserError&);

UnlinkedFunctionCodeBlock* generateFunctionCodeBlock(VM& vm, UnlinkedFunctionExecutable* executable, const SourceCode& source, CodeSpecializationKind kind, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, SourceParseMode parseMode)
{
    const FunctionInfo* info = executable->pythonInfo();
    RELEASE_ASSERT(info);
    StringView text = source.provider()->source();
    unsigned start = source.startOffset();
    unsigned end = source.endOffset();
    // Not what someone wrote, but what compile() was given as a tree.
    bool isTree = source.provider()->isPythonSyntaxTree();

    Arena arena;
    arena.usesLessGreater = info->futureFeatures & FutureBarryAsFLUFL;
    SyntaxError syntaxError;
    std::unique_ptr<SymbolTable> table;
    void* root = nullptr;
    const void* blockKey = nullptr;
    const Identifier* privateName = info->privateName.isNull() ? nullptr : &info->privateName;
    auto disagrees = [&] {
        syntaxError.message = concatenate("what is said of '"_s, info->name.string(), "' is not so of its source"_s);
        error = ParserError(syntaxError);
        return nullptr;
    };
    bool belongsToSomethingElse = info->kind == CodeKind::Annotations || info->kind == CodeKind::TypeParameters || info->kind == CodeKind::Evaluator;
    if (belongsToSomethingElse != (info->owner != OwnerKind::None))
        return disagrees();
    // What belongs to something else is compiled from the source of that.
    CodeKind sourceKind = info->kind;
    switch (info->owner) {
    case OwnerKind::None:
        break;
    case OwnerKind::Function:
    case OwnerKind::TypeAlias:
        sourceKind = CodeKind::Function;
        break;
    case OwnerKind::Class:
        sourceKind = CodeKind::Class;
        break;
    case OwnerKind::Module:
        sourceKind = CodeKind::Module;
        break;
    case OwnerKind::Interactive:
        sourceKind = CodeKind::Interactive;
        break;
    }

    switch (sourceKind) {
    case CodeKind::Annotations:
    case CodeKind::TypeParameters:
    case CodeKind::Evaluator:
        RELEASE_ASSERT_NOT_REACHED();
    case CodeKind::Module:
    case CodeKind::Expression:
    case CodeKind::Interactive: {
        Vector<SyntaxWarning> warnings;
        Module::Kind moduleKind = sourceKind == CodeKind::Module ? Module::Kind::Module : sourceKind == CodeKind::Expression ? Module::Kind::Expression : Module::Kind::Interactive;
        Module* module = isTree ? readSyntaxTree(vm, arena, text, moduleKind) : parse(vm, arena, text, moduleKind, warnings, syntaxError);
        if (module)
            table = SymbolTable::build(vm, arena, *module, info->futureFeatures, syntaxError);
        root = module;
        blockKey = module;
        break;
    }
    case CodeKind::Function:
    case CodeKind::Class: {
        // The names in a class are mangled for it, but its own name and its bases are not.
        const Identifier* outerPrivateName = sourceKind == CodeKind::Class ? nullptr : privateName;
        Statement* statement = isTree ? readDefinition(vm, arena, text, start, end) : parseDefinition(vm, arena, text, start, end, info->line);
        if (statement)
            table = SymbolTable::buildFragment(vm, arena, statement, nullptr, info->freeVariables, outerPrivateName, info->futureFeatures, info->owner != OwnerKind::None && info->canSeeClassScope, info->isNested,
                info->kind == CodeKind::Annotations || info->kind == CodeKind::Evaluator ? FragmentIs::WhatHasWhatIsCompiled : FragmentIs::WhatIsCompiled);
        root = statement;
        blockKey = statement;
        break;
    }
    case CodeKind::Lambda:
    case CodeKind::GeneratorExpression:
    case CodeKind::Comprehension: {
        Expression* expression = isTree ? readExpression(vm, arena, text, start, end) : parseExpression(vm, arena, text, start, end, info->line);
        if (expression)
            table = SymbolTable::buildFragment(vm, arena, nullptr, expression, info->freeVariables, privateName, info->futureFeatures, false, info->isNested);
        root = expression;
        blockKey = expression;
        break;
    }
    }

    if (!table) {
        // Only a module can fail here. All that is in it was parsed along with it.
        if (!syntaxError)
            syntaxError.message = concatenate("internal error: '"_s, info->name.string(), "' on line "_s, info->line, " was Python and is Python no more"_s);
        error = ParserError(syntaxError);
        return nullptr;
    }
    // What each kind of block goes by is for PythonSymbolTable.cpp to say, and this follows it.
    auto typeParametersOf = [&] () -> Sequence<TypeParameter*> {
        auto& statement = *static_cast<Statement*>(root);
        switch (info->owner) {
        case OwnerKind::Function:
            return statement.as<FunctionDef>().typeParameters;
        case OwnerKind::Class:
            return statement.as<ClassDef>().typeParameters;
        case OwnerKind::TypeAlias:
            return statement.as<TypeAlias>().typeParameters;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    // What is known about the code may be what a program has said of it, code(...), and not what was found when what it is in was compiled. It is believed as far as the
    // source bears it out, where to be wrong about it would be to read what is not there.
    auto isBorneOut = [&] {
        bool isModule = info->kind == CodeKind::Module || info->kind == CodeKind::Expression || info->kind == CodeKind::Interactive;
        if (info->parameterNames.size() != info->parameterCount() || info->positionalOnlyCount > info->positionalCount)
            return false;
        Arguments* arguments = nullptr;
        switch (sourceKind) {
        case CodeKind::Function: {
            auto* statement = static_cast<Statement*>(root);
            if (info->owner == OwnerKind::TypeAlias ? !statement->is<TypeAlias>() : !statement->is<FunctionDef>())
                return false;
            if (info->kind == CodeKind::Function)
                arguments = statement->as<FunctionDef>().arguments;
            break;
        }
        case CodeKind::Class:
            if (!static_cast<Statement*>(root)->is<ClassDef>())
                return false;
            break;
        case CodeKind::Lambda:
            if (!static_cast<Expression*>(root)->is<Lambda>())
                return false;
            arguments = static_cast<Expression*>(root)->as<Lambda>().arguments;
            break;
        case CodeKind::GeneratorExpression:
            if (!static_cast<Expression*>(root)->is<GeneratorExp>())
                return false;
            break;
        case CodeKind::Comprehension: {
            auto* expression = static_cast<Expression*>(root);
            if (!expression->is<ListComp>() && !expression->is<SetComp>() && !expression->is<DictComp>())
                return false;
            break;
        }
        default:
            break;
        }
        if (arguments) {
            if (info->positionalOnlyCount != arguments->positionalOnly.size() || info->positionalCount != arguments->positionalOnly.size() + arguments->positional.size() || info->keywordOnlyCount != arguments->keywordOnly.size()
                || info->hasVariadic != !!arguments->variadic || info->hasKeywordVariadic != !!arguments->keywordVariadic)
                return false;
        }
        // These are called by what comes with the engine, with the one argument.
        bool takesOneArgument = info->usesNamespace || info->kind == CodeKind::Class || info->kind == CodeKind::Annotations || info->kind == CodeKind::Evaluator || info->kind == CodeKind::Comprehension || info->kind == CodeKind::GeneratorExpression;
        if (takesOneArgument && (info->positionalCount != 1 || info->parameterCount() != 1))
            return false;
        if (!isModule && info->usesNamespace != (info->kind == CodeKind::Class))
            return false;
        if (info->canSeeClassScope && !belongsToSomethingElse)
            return false;
        bool hasTypeParameters = info->owner == OwnerKind::Function || info->owner == OwnerKind::Class || info->owner == OwnerKind::TypeAlias;
        switch (info->kind) {
        case CodeKind::Annotations:
            return info->owner != OwnerKind::TypeAlias;
        case CodeKind::TypeParameters:
            return hasTypeParameters && !typeParametersOf().empty();
        case CodeKind::Evaluator: {
            if (!hasTypeParameters)
                return false;
            if (info->evaluates == Evaluates::Value)
                return info->owner == OwnerKind::TypeAlias;
            if (info->typeParameterIndex >= typeParametersOf().size())
                return false;
            TypeParameter* parameter = typeParametersOf()[info->typeParameterIndex];
            return info->evaluates == Evaluates::Bound ? !!parameter->bound : !!parameter->defaultValue;
        }
        default:
            return true;
        }
    };
    if (!isBorneOut())
        return disagrees();

    bool wantsAnnotationBlock = false;
    switch (info->kind) {
    case CodeKind::Annotations:
        if (info->owner == OwnerKind::Function)
            blockKey = static_cast<Statement*>(root)->as<FunctionDef>().arguments;
        else
            wantsAnnotationBlock = true;
        break;
    case CodeKind::TypeParameters:
        blockKey = typeParametersOf().data();
        break;
    case CodeKind::Evaluator:
        if (info->evaluates != Evaluates::Value) {
            TypeParameter* parameter = typeParametersOf()[info->typeParameterIndex];
            // Only a TypeVar can have both, and then the default goes by the address after.
            bool isSecond = info->evaluates == Evaluates::Default && parameter->kind == TypeParameter::Kind::TypeVar;
            blockKey = reinterpret_cast<const char*>(parameter) + isSecond;
        }
        break;
    default:
        break;
    }
    Block* block = table->blockFor(blockKey);
    if (block && wantsAnnotationBlock)
        block = block->annotationBlock;
    if (!block)
        return disagrees();
    // Only a function is a generator or a coroutine, and a module that has been allowed an `await`. A `yield` where there is no function is found when it is come to, and
    // said to be that.
    bool isFunctionLike = block->isFunctionLike();
    bool canAwait = isFunctionLike || (block->type == BlockType::Module && (info->futureFeatures & AllowTopLevelAwait));
    if (info->isGenerator != (isFunctionLike && block->isGenerator) || info->isCoroutine != (canAwait && block->isCoroutine))
        return disagrees();

    executable->recordParse(NoFeatures, StrictModeLexicallyScopedFeature, false);
    UnlinkedFunctionCodeBlock* result = UnlinkedFunctionCodeBlock::create(vm, FunctionCode, ExecutableInfo(kind == CodeSpecializationKind::CodeForConstruct, executable->privateBrandRequirement(), false, executable->constructorKind(), executable->scriptMode(), executable->superBinding(), parseMode, executable->derivedContextType(), executable->needsClassFieldInitializer(), false, false, executable->evalContextType(), false), codeGenerationMode);

    ParserArena parserArena;
    auto node = makeUnique<ScopeNode>(parserArena, source, arena, *table, *block, *info, root);
    error = BytecodeGenerator::generate(vm, node.get(), source, result, codeGenerationMode, nullptr, nullptr, nullptr);
    if (node->error()) {
        // CPython generates the code of a function when it comes to it, in the midst of what it is in. So if there is something wrong with one that was come to first, it is that which is wrong.
        for (unsigned i = 0; i < node->functionsBeforeError(); ++i) {
            if (!isGeneratedLast(*result->functionExpr(i)->pythonInfo()) && !generateAll(vm, result->functionExpr(i), source, error))
                return nullptr;
        }
        error = ParserError(node->error());
    }
    if (error.isValid())
        return nullptr;
    return result;
}

// What is wrong is found either in taking the source apart, or afterwards in what came of that.
enum class FoundIn : uint8_t { Parsing, WhatWasParsed };

static JSValue raiseSyntaxError(JSGlobalObject* globalObject, ThrowScope& scope, const SyntaxError& error, const SourceCode& givenSource, FoundIn foundIn)
{
    VM& vm = globalObject->vm();
    BuiltinType type = BuiltinType::SyntaxError;
    switch (error.kind) {
    case SyntaxError::Kind::SyntaxError:
        break;
    case SyntaxError::Kind::IndentationError:
        type = BuiltinType::IndentationError;
        break;
    case SyntaxError::Kind::TabError:
        type = BuiltinType::TabError;
        break;
    case SyntaxError::Kind::IncompleteInputError:
        type = BuiltinType::IncompleteInputError;
        break;
    case SyntaxError::Kind::SystemError:
        return raise(globalObject, scope, BuiltinType::SystemError, error.message);
    case SyntaxError::Kind::ValueError:
        return raise(globalObject, scope, BuiltinType::ValueError, error.message);
    case SyntaxError::Kind::TypeError:
        return raise(globalObject, scope, BuiltinType::TypeError, error.message);
    }
    // The line that it is on. While the source is being taken apart it is at hand. Afterwards CPython has it no more, and looks in the file that it is
    // said to be from, if there is such a file. So there is no line for what compile() was given with a name that was made up.
    SourceCode source = givenSource;
    if (foundIn == FoundIn::WhatWasParsed) {
        source = readSourceIfPresent(globalObject, givenSource.provider()->sourceURL());
        RETURN_IF_EXCEPTION(scope, { });
    }
    StringView text = source.isNull() ? StringView() : source.provider()->source();
    unsigned lineStart = 0;
    for (unsigned line = 1; line < error.line && lineStart < text.length(); ++lineStart) {
        if (text[lineStart] == '\n')
            ++line;
    }
    unsigned lineEnd = lineStart;
    while (lineEnd < text.length() && text[lineEnd] != '\n')
        ++lineEnd;
    bool endsLine = lineEnd < text.length();
    if (endsLine)
        ++lineEnd;
    if (error.lineGoesOnToTheEnd) {
        lineEnd = text.length();
        endsLine = lineEnd > lineStart && text[lineEnd - 1] == '\n';
    }
    JSValue lineText = jsUndefined();
    StringView line = text.substring(lineStart, lineEnd - lineStart - endsLine);
    // Where there is no source there is no line to be on, and the line is empty.
    bool hasLine = (error.line || foundIn == FoundIn::Parsing) && !source.isNull() && (lineStart < text.length() || foundIn == FoundIn::Parsing);
    // What is taken apart always ends with the end of a line, which is added if it is not there. The line comes with that if it is the one that the tokenizer is on, and is fetched again without it
    // if the tokenizer has gone on. What the tokenizer raises for itself never has it.
    bool hasEndOfLine = foundIn == FoundIn::Parsing ? !error.isFromTokenizer && error.tokenizerLine <= error.line && (endsLine || error.lastLineIsEnded) : endsLine;
    if (hasLine)
        lineText = strOrMemoryError(globalObject, concatenate(line, hasEndOfLine ? "\n"_s : ""_s));

    // _PyPegen_byte_offset_to_character_offset(): how many characters there are in so many bytes of the line. It is the line that is wrong that is gone by, though it end on another.
    auto characterOffset = [&] (int bytes) -> int {
        if (!hasLine)
            return bytes;
        int count = 0;
        int used = 0;
        for (char32_t character : line.codePoints()) {
            if (used >= bytes)
                return count;
            used += character < 0x80 ? 1 : character < 0x800 ? 2 : character < 0x10000 ? 3 : 4;
            ++count;
        }
        // And no more than one beyond the end of it.
        return count + (used < bytes && hasEndOfLine) + (used + hasEndOfLine < bytes);
    };
    int offset = characterOffset(std::max(error.column + 1, 0));
    int endOffset = error.endColumn + 1 > 0 ? characterOffset(error.endColumn + 1) : error.endColumn + 1;

    PyTuple* details = PyTuple::create(globalObject, { jsString(vm, givenSource.provider()->sourceURL()), jsNumber(error.line), jsNumber(offset), lineText, jsNumber(error.endLine), jsNumber(endOffset) });
    JSValue exception = call(globalObject, globalObject->pyRealm()->type(type), jsString(vm, error.message), details);
    RETURN_IF_EXCEPTION(scope, { });
    throwException(globalObject, scope, exception);
    return { };
}

// _PyErr_EmitSyntaxWarning(), for each. False if something has been raised, as it is if the program has asked for such warnings to be errors: SyntaxError then, which says more about where.
static bool issueWarnings(JSGlobalObject* globalObject, Vector<SyntaxWarning>& warnings, const SourceCode& source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (SyntaxWarning& warning : std::exchange(warnings, { })) {
        bool succeeded = warnExplicit(globalObject, BuiltinType::SyntaxWarning, warning.message, source.provider()->sourceURL(), warning.line);
        if (succeeded)
            continue;
        if (catchException(globalObject, BuiltinType::SyntaxWarning)) {
            SyntaxError error;
            error.message = warning.errorMessage.isNull() ? warning.message : warning.errorMessage;
            error.line = warning.line;
            error.column = warning.column;
            error.endLine = warning.endLine;
            error.endColumn = warning.endColumn;
            error.isFromTokenizer = warning.isFromTokenizer;
            raiseSyntaxError(globalObject, scope, error, source, warning.isFoundInParsing ? FoundIn::Parsing : FoundIn::WhatWasParsed);
        }
        return false;
    }
    return true;
}

// ---- From bytes to source
//
// This follows decode_str() in Parser/tokenizer/string_tokenizer.c of CPython, and what it calls in helpers.c.

// \r\n and \r are ends of lines, and from here on an end of line is \n, in a string that goes over several lines too.
template<typename Character>
static void appendTranslatingNewlines(auto& output, std::span<const Character> input)
{
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\r') {
            output.append(input[i]);
            continue;
        }
        output.append(static_cast<Character>('\n'));
        if (i + 1 < input.size() && input[i + 1] == '\n')
            ++i;
    }
}

SourceCode makeSource(const String& given, const SourceOrigin& origin, const String& sourceURL, unsigned firstLine)
{
    String text = given;
    if (text.contains('\r')) {
        StringBuilder builder;
        if (text.is8Bit())
            appendTranslatingNewlines(builder, text.span8());
        else
            appendTranslatingNewlines(builder, text.span16());
        text = builder.toString();
    }
    return SourceCode(StringSourceProvider::create(text, origin, String(sourceURL), SourceTaintedOrigin::Untainted, TextPosition(OrdinalNumber::fromOneBasedInt(firstLine), OrdinalNumber()), SourceProviderSourceType::Python));
}

// The two encodings that the tokenizer knows by all their names.
static String normalizedEncodingName(const String& name)
{
    StringBuilder builder;
    for (unsigned i = 0; i < std::min(name.length(), 12u); ++i)
        builder.append(static_cast<char16_t>(name[i] == '_' ? '-' : toASCIILower(name[i])));
    String start = builder.toString();
    if (start == "utf-8"_s || start.startsWith("utf-8-"_s))
        return "utf-8"_s;
    for (ASCIILiteral latin1 : { "latin-1"_s, "iso-8859-1"_s, "iso-latin-1"_s }) {
        if (start == latin1 || start.startsWith(concatenate(latin1, '-')))
            return "iso-8859-1"_s;
    }
    return name;
}

// The encoding that a line declares, or null. It has to be in a comment, with nothing else on the line. If there is nothing on the line but
// a comment that declares none, or nothing at all, the next line may.
static String declaredEncodingOf(std::span<const uint8_t> line, bool& nextLineMayDeclare)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\f'))
        ++i;
    nextLineMayDeclare = i == line.size() || line[i] == '#' || line[i] == '\r';
    if (i == line.size() || line[i] != '#')
        return { };
    static constexpr std::array<uint8_t, 6> coding { 'c', 'o', 'd', 'i', 'n', 'g' };
    for (; i + coding.size() < line.size(); ++i) {
        if (!spanHasPrefix(line.subspan(i), std::span<const uint8_t>(coding)))
            continue;
        size_t t = i + coding.size();
        if (line[t] != ':' && line[t] != '=')
            continue;
        do {
            ++t;
        } while (t < line.size() && (line[t] == ' ' || line[t] == '\t'));
        size_t begin = t;
        while (t < line.size() && (isASCIIAlphanumeric(line[t]) || line[t] == '-' || line[t] == '_' || line[t] == '.'))
            ++t;
        if (begin < t)
            return normalizedEncodingName(String(byteCast<Latin1Character>(line.subspan(begin, t - begin))));
    }
    return { };
}

// SyntaxError(message, (None, line, offset, text, line, endOffset)), as the tokenizer raises it, which does not know the name of the file.
static void raiseTokenizerError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message, const String& sourceURL, unsigned line, int offset, std::span<const uint8_t> text, int endOffset)
{
    VM& vm = globalObject->vm();
    JSValue lineText = jsString(vm, String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(text)));
    PyTuple* details = PyTuple::create(globalObject, { jsUndefined(), jsNumber(line), jsNumber(offset), lineText, jsNumber(line), jsNumber(endOffset) });
    JSValue exception = call(globalObject, globalObject->pyRealm()->typeSyntaxError(), jsString(vm, message), details);
    RETURN_IF_EXCEPTION(scope, void());
    setAttribute(globalObject, exception, Identifier::fromString(vm, "filename"_s), jsString(vm, sourceURL));
    RETURN_IF_EXCEPTION(scope, void());
    throwException(globalObject, scope, exception);
}

SourceCode makeSource(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const SourceOrigin& origin, const String& sourceURL)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (WTF::find(bytes, static_cast<uint8_t>(0)) != notFound) {
        raise(globalObject, scope, BuiltinType::SyntaxError, "source code string cannot contain null bytes"_s);
        return { };
    }

    static constexpr std::array<uint8_t, 3> byteOrderMark { 0xEF, 0xBB, 0xBF };
    bool hasByteOrderMark = spanHasPrefix(bytes, std::span<const uint8_t>(byteOrderMark));
    if (hasByteOrderMark)
        bytes = bytes.subspan(byteOrderMark.size());

    // A line ends at \n, \r\n or \r.
    auto endOfLine = [&] (size_t start) {
        size_t end = start;
        while (end < bytes.size() && bytes[end] != '\n' && bytes[end] != '\r')
            ++end;
        return end;
    };
    auto startOfNextLine = [&] (size_t end) {
        if (end < bytes.size() && bytes[end] == '\r' && end + 1 < bytes.size() && bytes[end + 1] == '\n')
            return end + 2;
        return std::min(end + 1, bytes.size());
    };

    String encoding;
    size_t lineStart = 0;
    for (unsigned line = 1; line <= 2; ++line) {
        size_t lineEnd = endOfLine(lineStart);
        bool nextLineMayDeclare;
        encoding = declaredEncodingOf(bytes.subspan(lineStart, lineEnd - lineStart), nextLineMayDeclare);
        if (!encoding.isNull()) {
            if (hasByteOrderMark && encoding != "utf-8"_s) {
                // CPython counts the end of the line before as part of the second line.
                raiseTokenizerError(globalObject, scope, concatenate("encoding problem: "_s, encoding, " with BOM"_s), sourceURL, line, 0, bytes.subspan(lineStart, lineEnd - lineStart), lineEnd - lineStart + line - 1);
                return { };
            }
            break;
        }
        if (!nextLineMayDeclare)
            break;
        lineStart = startOfNextLine(lineEnd);
    }

    if (encoding.isNull() || encoding == "utf-8"_s) {
        String text = String::fromUTF8(byteCast<char8_t>(bytes));
        if (!text.isNull())
            return makeSource(text, origin, sourceURL);
        if (bytes.empty())
            return makeSource(emptyString(), origin, sourceURL);

        // Which byte is at fault, and where that is.
        unsigned line = 1;
        unsigned column = 0;
        size_t start = 0;
        size_t i = 0;
        while (i < bytes.size()) {
            size_t next = i;
            char32_t character;
            U8_NEXT(bytes.data(), next, bytes.size(), character);
            if (static_cast<int32_t>(character) < 0)
                break;
            ++column;
            if (bytes[i] == '\n' || (bytes[i] == '\r' && !(next < bytes.size() && bytes[next] == '\n'))) {
                ++line;
                column = 0;
                start = next;
            }
            i = next;
        }
        raiseTokenizerError(globalObject, scope, concatenate("Non-UTF-8 code starting with '\\x"_s, hex(bytes[i], 2, Lowercase), "' on line "_s, line, ", but no encoding declared; see https://peps.python.org/pep-0263/ for details"_s), sourceURL, line, column + 1, bytes.subspan(start, endOfLine(start) - start), column + 1);
        return { };
    }

    // The ends of lines are seen to first, as bytes, and there is one at the end. In an encoding that is not a superset of ASCII that makes
    // nonsense of it, which is what CPython makes of it.
    Vector<uint8_t> translated;
    appendTranslatingNewlines(translated, bytes);
    if (!translated.isEmpty() && translated.last() != '\n')
        translated.append('\n');
    JSValue object = newBytes(globalObject, translated.span());
    RETURN_IF_EXCEPTION(scope, { });
    String text = decodeBytes(globalObject, translated.span(), encoding, String());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        // That there is no such encoding, or that this is not in it, is something wrong with the source.
        JSValue error = exception->value();
        PyType* type = typeOf(globalObject, error);
        if (!type->isSubtypeOf(globalObject->pyRealm()->typeLookupError()) && !type->isSubtypeOf(globalObject->pyRealm()->typeUnicodeDecodeError()))
            return { };
        if (!scope.tryClearException())
            return { };
        String message = str(globalObject, error);
        RETURN_IF_EXCEPTION(scope, { });
        PyTuple* details = PyTuple::create(globalObject, { jsString(vm, sourceURL), jsNumber(0), jsNumber(-1), jsUndefined() });
        JSValue syntaxError = call(globalObject, globalObject->pyRealm()->typeSyntaxError(), jsString(vm, message), details);
        RETURN_IF_EXCEPTION(scope, { });
        throwException(globalObject, scope, syntaxError);
        return { };
    }
    return makeSource(text, origin, sourceURL);
}

// Compiles a function and all that is in it. Python says what is wrong anywhere in a file before it runs any of it, and some of what can be
// wrong is only found by generating code. False if something is, and then `error` says what.
static bool generateAll(VM& vm, UnlinkedFunctionExecutable* executable, const SourceCode& parentSource, ParserError& error)
{
    SourceCode source = executable->linkedSourceCode(parentSource);
    UnlinkedFunctionCodeBlock* codeBlock = executable->unlinkedCodeBlockFor(vm, source, CodeSpecializationKind::CodeForCall, { }, error, executable->parseMode());
    if (!codeBlock)
        return false;
    for (bool last : { false, true }) {
        for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i) {
            if (isGeneratedLast(*codeBlock->functionExpr(i)->pythonInfo()) == last && !generateAll(vm, codeBlock->functionExpr(i), source, error))
                return false;
        }
    }
    return true;
}

bool isGeneratedLast(const FunctionInfo& info)
{
    return info.kind == CodeKind::Annotations && (info.owner == OwnerKind::Module || info.owner == OwnerKind::Interactive);
}

// _PyCompile_AstPreprocess(), and then PyAST_mod2obj(). Nothing is warned of.
static JSValue treeFor(JSGlobalObject* globalObject, Arena& arena, Module& module, const SourceCode& source, const TreeOptions& options)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    SyntaxError error;
    auto futureFeatures = SymbolTable::futureFeaturesOf(vm, arena, module, error);
    if (!futureFeatures) {
        raiseSyntaxError(globalObject, scope, error, source, FoundIn::WhatWasParsed);
        return { };
    }
    if (!optimize(vm, arena, module, options.optimizationLevel, *futureFeatures | options.futureFeatures, !options.isOptimized))
        return raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded during compilation"_s);
    RELEASE_AND_RETURN(scope, objectFromAST(globalObject, module));
}

JSValue compileTree(JSGlobalObject* globalObject, JSValue tree, const String& filename, Module::Kind kind, const TreeOptions& options)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Arena arena;
    Module* module = astFromObject(globalObject, arena, tree, kind);
    RETURN_IF_EXCEPTION(scope, { });
    if (options.wantsTree)
        RELEASE_AND_RETURN(scope, treeFor(globalObject, arena, *module, makeSource(String(), SourceOrigin(), filename), options));
    String text = writeSyntaxTree(vm, *module);
    if (text.isNull())
        return raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded during compilation"_s);
    SourceCode source(SyntaxTreeSourceProvider::create(text, SourceOrigin(), filename));
    CodeKind codeKind = kind == Module::Kind::Module ? CodeKind::Module : kind == Module::Kind::Expression ? CodeKind::Expression : CodeKind::Interactive;
    FunctionExecutable* executable = compileSource(globalObject, source, codeKind, true, options.futureFeatures, ImplementationVisibility::Public, options.optimizationLevel);
    RETURN_IF_EXCEPTION(scope, { });
    return codeObjectFor(globalObject, executable);
}

JSValue parseSource(JSGlobalObject* globalObject, const SourceCode& source, Module::Kind kind, const TreeOptions& options)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Arena arena;
    arena.maximumDigitsOfIntLiteral = globalObject->pyRealm()->maximumDigitsOfIntAsString;
    arena.usesLessGreater = options.futureFeatures & FutureBarryAsFLUFL;
    arena.impliesDedent = !(options.futureFeatures & DoNotImplyDedent);
    arena.allowsIncompleteInput = options.futureFeatures & AllowIncompleteInput;
    arena.hasTypeComments = options.futureFeatures & TypeComments;
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    Module* module = parse(vm, arena, source.provider()->source(), kind, warnings, error);
    if (!issueWarnings(globalObject, warnings, source))
        return { };
    if (!module) {
        raiseSyntaxError(globalObject, scope, error, source, FoundIn::Parsing);
        return { };
    }
    RELEASE_AND_RETURN(scope, treeFor(globalObject, arena, *module, source, options));
}

FunctionExecutable* compileSource(JSGlobalObject* globalObject, const SourceCode& source, CodeKind kind, bool usesNamespace, unsigned inheritedFutureFeatures, ImplementationVisibility visibility, unsigned optimizationLevel)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(source.provider()->isPython());

    unsigned parsingFlags = inheritedFutureFeatures & (DoNotImplyDedent | AllowIncompleteInput | TypeComments);
    inheritedFutureFeatures &= ~parsingFlags;
    unsigned futureFeatures = inheritedFutureFeatures;
    bool hasDocstring = false;
    bool isCoroutine = false;
    // What CPython warns of as it generates code.
    Vector<SyntaxWarning> codeWarnings;
    {
        Arena arena;
        arena.maximumDigitsOfIntLiteral = globalObject->pyRealm()->maximumDigitsOfIntAsString;
        arena.usesLessGreater = inheritedFutureFeatures & FutureBarryAsFLUFL;
        arena.impliesDedent = !(parsingFlags & DoNotImplyDedent);
        arena.allowsIncompleteInput = parsingFlags & AllowIncompleteInput;
        arena.hasTypeComments = parsingFlags & TypeComments;
        Vector<SyntaxWarning> warnings;
        SyntaxError error;
        Module::Kind moduleKind = kind == CodeKind::Module ? Module::Kind::Module : kind == CodeKind::Expression ? Module::Kind::Expression : Module::Kind::Interactive;
        bool isTree = source.provider()->isPythonSyntaxTree();
        Module* module = isTree ? readSyntaxTree(vm, arena, source.provider()->source(), moduleKind) : parse(vm, arena, source.provider()->source(), moduleKind, warnings, error);
        // It was written out a moment ago.
        RELEASE_ASSERT(module || !isTree);
        // What was warned of on the way to something that is wrong was warned of first.
        if (!issueWarnings(globalObject, warnings, source))
            return nullptr;
        std::unique_ptr<SymbolTable> table;
        if (module) {
            collectControlFlowWarnings(*module, warnings);
            if (!issueWarnings(globalObject, warnings, source))
                return nullptr;
            table = SymbolTable::build(vm, arena, *module, inheritedFutureFeatures, error);
        }
        if (!table) {
            raiseSyntaxError(globalObject, scope, error, source, module ? FoundIn::WhatWasParsed : FoundIn::Parsing);
            return nullptr;
        }
        collectCodeWarnings(*module, table->futureFeatures(), optimizationLevel, codeWarnings);
        futureFeatures = table->futureFeatures();
        hasDocstring = table->blockFor(module)->hasDocstring;
        isCoroutine = table->blockFor(module)->isCoroutine;
    }

    auto info = adoptRef(*new FunctionInfo);
    info->kind = kind;
    info->futureFeatures = futureFeatures;
    info->optimizationLevel = optimizationLevel;
    info->visibility = visibility;
    info->hasDocstring = hasDocstring && optimizationLevel < 2;
    info->isCoroutine = isCoroutine;
    info->name = Identifier::fromString(vm, "<module>"_s);
    info->qualifiedName = "<module>"_s;
    if (usesNamespace) {
        info->usesNamespace = true;
        info->parameterNames.append(Identifier::fromString(vm, ".namespace"_s));
        info->positionalCount = 1;
    }
    unsigned parameterCount = info->parameterCount();

    FunctionMetadataNode metadata(JSTokenLocation(), JSTokenLocation(), source.startOffset(), source.startOffset(), source.startOffset(), visibility, StrictModeLexicallyScopedFeature, ConstructorKind::None, SuperBinding::NotNeeded, parameterCount, SourceParseMode::MethodMode, false);
    metadata.finishParsing(source, info->name, FunctionMode::FunctionExpression);
    auto* unlinked = UnlinkedFunctionExecutable::create(vm, source, &metadata, UnlinkedNormalFunction, ConstructAbility::CannotConstruct, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, { }, std::nullopt, DerivedContextType::None, EvalContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);
    unlinked->setPythonInfo(WTF::move(info));

    ParserError error;
    bool wasGenerated = generateAll(vm, unlinked, source, error);
    // It stops at what is wrong, and so has warned of what comes before that and no more.
    if (!wasGenerated && error.pythonError()) {
        std::pair place { error.pythonError()->line, error.pythonError()->column };
        codeWarnings.removeAllMatching([&] (const SyntaxWarning& warning) {
            return std::pair { warning.line, warning.column } >= place;
        });
    }
    if (!issueWarnings(globalObject, codeWarnings, source))
        return nullptr;
    if (!wasGenerated) {
        // Anything else that can go wrong with generating code is the engine's to say.
        if (error.pythonError())
            raiseSyntaxError(globalObject, scope, *error.pythonError(), source, FoundIn::WhatWasParsed);
        else
            throwException(globalObject, scope, error.toErrorObject(globalObject, source));
        return nullptr;
    }
    return unlinked->link(vm, nullptr, source);
}

// Where a name that is not among the globals is looked for is settled when code is given its globals: it is what they have as __builtins__, a module
// or a dict, and the module builtins if that is neither or they have none.
static JSObject* builtinsFor(JSGlobalObject* globalObject, JSObject* globals)
{
    VM& vm = globalObject->vm();
    JSValue builtins = getStoredAttribute(vm, globals, Identifier::fromString(vm, "__builtins__"_s));
    if (builtins) {
        if (JSObject* module = tryModule(globalObject, builtins))
            return module;
        if (isDict(builtins))
            return uncheckedDowncast<PyDict>(builtins.asCell())->ensureBacking(globalObject);
    }
    return globalObject->pyRealm()->builtinsModule();
}

static FunctionExecutable* executableFor(VM& vm, const SourceCode& source, Ref<FunctionInfo>&& info)
{
    SourceCode whole(*source.provider());
    unsigned start = source.startOffset();
    FunctionMetadataNode metadata(JSTokenLocation(), JSTokenLocation(), start, start, start, info->visibility, StrictModeLexicallyScopedFeature, ConstructorKind::None, SuperBinding::NotNeeded, info->parameterCount(), SourceParseMode::MethodMode, false);
    metadata.finishParsing(source, info->name, FunctionMode::FunctionExpression);
    auto* unlinked = UnlinkedFunctionExecutable::create(vm, whole, &metadata, UnlinkedNormalFunction, ConstructAbility::CannotConstruct, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, { }, std::nullopt, DerivedContextType::None, EvalContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);
    unlinked->setPythonInfo(WTF::move(info));
    return unlinked->link(vm, nullptr, whole);
}

FunctionExecutable* cloneExecutable(JSGlobalObject* globalObject, FunctionExecutable* original, Ref<FunctionInfo>&& info, const String& sourceURL)
{
    VM& vm = globalObject->vm();
    const SourceCode& source = original->source();
    RefPtr<SourceProvider> provider = source.provider();
    // The name of the file goes with the source. The text is the same text and not a copy.
    if (!sourceURL.isNull() && sourceURL != provider->sourceURL()) {
        if (provider->isPythonSyntaxTree())
            provider = SyntaxTreeSourceProvider::create(provider->source().toString(), provider->sourceOrigin(), sourceURL);
        else
            provider = makeSource(provider->source().toString(), provider->sourceOrigin(), sourceURL).provider();
    }
    return executableFor(vm, SourceCode(*provider, source.startOffset(), source.endOffset()), WTF::move(info));
}

FunctionExecutable* executableFromProgram(JSGlobalObject* globalObject, const SourceCode& source, Ref<FunctionInfo>&& info)
{
    VM& vm = globalObject->vm();
    FunctionExecutable* executable = executableFor(vm, source, WTF::move(info));
    // All of it is compiled now, to find out whether it can be.
    ParserError error;
    return generateAll(vm, executable->unlinkedExecutable(), SourceCode(*source.provider()), error) ? executable : nullptr;
}

JSScope* environmentForCells(JSGlobalObject* globalObject, JSScope* next, const Vector<Identifier>& names, PyTuple* cells)
{
    VM& vm = globalObject->vm();
    if (names.isEmpty())
        return next;
    JSC::SymbolTable* symbolTable = JSC::SymbolTable::create(vm);
    symbolTable->setScopeType(JSC::SymbolTable::ScopeType::LexicalScope);
    Vector<ScopeOffset, 8> offsets;
    for (auto& name : names) {
        offsets.append(symbolTable->takeNextScopeOffset(NoLockingNecessary));
        symbolTable->set(NoLockingNecessary, name.impl(), SymbolTableEntry(VarOffset(offsets.last())));
    }
    JSLexicalEnvironment* environment = JSLexicalEnvironment::create(vm, globalObject->activationStructure(), next, symbolTable, jsUndefined());
    for (unsigned i = 0; i < names.size(); ++i)
        environment->variableAt(offsets[i]).set(vm, environment, cells->at(i));
    return environment;
}

JSScope* environmentForGlobals(JSGlobalObject* globalObject, JSObject* globals)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    // The outermost environment of everything in the module.
    JSC::SymbolTable* symbolTable = JSC::SymbolTable::create(vm);
    symbolTable->setScopeType(JSC::SymbolTable::ScopeType::LexicalScope);
    ScopeOffset globalsOffset = symbolTable->takeNextScopeOffset(NoLockingNecessary);
    symbolTable->set(NoLockingNecessary, names.globals.impl(), SymbolTableEntry(VarOffset(globalsOffset)));
    ScopeOffset builtinsOffset = symbolTable->takeNextScopeOffset(NoLockingNecessary);
    symbolTable->set(NoLockingNecessary, names.builtins.impl(), SymbolTableEntry(VarOffset(builtinsOffset)));
    JSLexicalEnvironment* environment = JSLexicalEnvironment::create(vm, globalObject->activationStructure(), globalObject->globalScope(), symbolTable, jsUndefined());
    environment->variableAt(globalsOffset).set(vm, environment, globals);
    environment->variableAt(builtinsOffset).set(vm, environment, builtinsFor(globalObject, globals));
    return environment;
}

JSFunction* bindToGlobals(JSGlobalObject* globalObject, FunctionExecutable* executable, JSObject* globals)
{
    return JSFunction::create(globalObject->vm(), globalObject, executable, environmentForGlobals(globalObject, globals));
}

JSFunction* compileModule(JSGlobalObject* globalObject, const SourceCode& source, JSObject* namespaceObject, ImplementationVisibility visibility)
{
    FunctionExecutable* executable = compileSource(globalObject, source, CodeKind::Module, false, 0, visibility);
    if (!executable)
        return nullptr;
    return bindToGlobals(globalObject, executable, namespaceObject);
}

int runMain(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const SourceOrigin& origin, const String& sourceURL)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    startPython(globalObject);
    if (Exception* exception = scope.exception()) {
        JSValue value = exception->value();
        scope.clearException();
        reportUncaughtException(globalObject, value);
        return 1;
    }

    JSObject* module = newModule(globalObject, "__main__"_s);
    module->putDirect(vm, Identifier::fromString(vm, "__builtins__"_s), realm->builtinsModule());
    module->putDirect(vm, vm.pythonNames().dunder_file, jsString(vm, sourceURL));
    module->putDirect(vm, Identifier::fromString(vm, "__cached__"_s), jsUndefined());
    registerModule(globalObject, "__main__"_s, module);

    SourceCode source = makeSource(globalObject, bytes, origin, sourceURL);
    JSFunction* function = source.isNull() ? nullptr : compileModule(globalObject, source, module);
    if (function)
        call(globalObject, function);

    Exception* exception = scope.exception();
    if (!exception)
        return 0;
    JSValue value = exception->value();
    scope.clearException();
    if (isInstance(globalObject, value, realm->typeSystemExit())) {
        // sys.exit(): its argument is the status, or a message.
        JSValue arguments = asObject(value)->getDirect(vm, vm.pythonNames().private_args);
        auto* tuple = arguments ? uncheckedDowncast<PyTuple>(arguments.asCell()) : nullptr;
        if (!tuple || !tuple->length() || isNone(tuple->at(0)))
            return 0;
        if (tuple->at(0).isInt32())
            return tuple->at(0).asInt32();
        JSValue file = sysAttribute(globalObject, "stderr"_s);
        String message = str(globalObject, tuple->at(0));
        if (!scope.exception() && file && !isNone(file)) {
            JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
            if (!scope.exception())
                call(globalObject, write, strOrMemoryError(globalObject, concatenate(message, '\n')));
        }
        scope.clearException();
        return 1;
    }
    reportUncaughtException(globalObject, value);
    return 1;
}

} } // namespace JSC::Python
