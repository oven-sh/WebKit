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
#include "PythonBytes.h"
#include "PythonCodeGenerator.h"
#include "PythonCodecs.h"
#include "PythonOperations.h"
#include "PythonParser.h"
#include "PythonSymbolTable.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace Python {

UnlinkedFunctionCodeBlock* generateFunctionCodeBlock(VM& vm, UnlinkedFunctionExecutable* executable, const SourceCode& source, CodeSpecializationKind kind, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, SourceParseMode parseMode)
{
    const FunctionInfo* info = executable->pythonInfo();
    RELEASE_ASSERT(info);
    StringView text = source.provider()->source();
    unsigned start = source.startOffset();
    unsigned end = source.endOffset();

    Arena arena;
    SyntaxError syntaxError;
    std::unique_ptr<SymbolTable> table;
    void* root = nullptr;
    const void* blockKey = nullptr;
    const Identifier* privateName = info->privateName.isNull() ? nullptr : &info->privateName;
    // What evaluates annotations is compiled from the source of what they are the annotations of.
    bool isAnnotations = info->kind == CodeKind::Annotations;
    CodeKind sourceKind = isAnnotations ? info->annotationsOf : info->kind;

    switch (sourceKind) {
    case CodeKind::Annotations:
        RELEASE_ASSERT_NOT_REACHED();
    case CodeKind::Module:
    case CodeKind::Expression:
    case CodeKind::Interactive: {
        Vector<SyntaxWarning> warnings;
        Module::Kind moduleKind = sourceKind == CodeKind::Module ? Module::Kind::Module : sourceKind == CodeKind::Expression ? Module::Kind::Expression : Module::Kind::Interactive;
        Module* module = parse(vm, arena, text, moduleKind, warnings, syntaxError);
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
        Statement* statement = parseDefinition(vm, arena, text, start, end, info->line);
        if (statement)
            table = SymbolTable::buildFragment(vm, arena, statement, nullptr, info->freeVariables, outerPrivateName, info->futureFeatures, isAnnotations && sourceKind == CodeKind::Function && info->canSeeClassScope);
        root = statement;
        blockKey = statement;
        // The block for those of a function goes by its parameters.
        if (statement && isAnnotations && sourceKind == CodeKind::Function)
            blockKey = statement->as<FunctionDef>().arguments;
        break;
    }
    case CodeKind::Lambda:
    case CodeKind::GeneratorExpression:
    case CodeKind::Comprehension: {
        Expression* expression = parseExpression(vm, arena, text, start, end, info->line);
        if (expression)
            table = SymbolTable::buildFragment(vm, arena, nullptr, expression, info->freeVariables, privateName, info->futureFeatures);
        root = expression;
        blockKey = expression;
        break;
    }
    }

    if (!table) {
        // Only a module can fail here. All that is in it was parsed along with it.
        if (!syntaxError)
            syntaxError.message = makeString("internal error: '"_s, info->name.string(), "' on line "_s, info->line, " was Python and is Python no more"_s);
        error = ParserError(syntaxError);
        return nullptr;
    }
    Block* block = table->blockFor(blockKey);
    RELEASE_ASSERT(block);
    if (isAnnotations && sourceKind != CodeKind::Function) {
        block = block->annotationBlock;
        RELEASE_ASSERT(block);
    }

    executable->recordParse(NoFeatures, StrictModeLexicallyScopedFeature, false);
    UnlinkedFunctionCodeBlock* result = UnlinkedFunctionCodeBlock::create(vm, FunctionCode, ExecutableInfo(kind == CodeSpecializationKind::CodeForConstruct, executable->privateBrandRequirement(), false, executable->constructorKind(), executable->scriptMode(), executable->superBinding(), parseMode, executable->derivedContextType(), executable->needsClassFieldInitializer(), false, false, executable->evalContextType(), false), codeGenerationMode);

    ParserArena parserArena;
    auto node = makeUnique<ScopeNode>(parserArena, source, arena, *table, *block, *info, root);
    error = BytecodeGenerator::generate(vm, node.get(), source, result, codeGenerationMode, nullptr, nullptr, nullptr);
    if (node->error())
        error = ParserError(node->error());
    if (error.isValid())
        return nullptr;
    return result;
}

// What is wrong is found either in taking the source apart, or afterwards in what came of that.
enum class FoundIn : uint8_t { Parsing, WhatWasParsed };

static JSValue raiseSyntaxError(JSGlobalObject* globalObject, ThrowScope& scope, const SyntaxError& error, const SourceCode& givenSource, FoundIn foundIn)
{
    VM& vm = globalObject->vm();
    BuiltinType type = error.kind == SyntaxError::Kind::SyntaxError ? BuiltinType::SyntaxError : error.kind == SyntaxError::Kind::IndentationError ? BuiltinType::IndentationError : BuiltinType::TabError;
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
    JSValue lineText = jsUndefined();
    if (error.line && !source.isNull() && (lineStart < text.length() || foundIn == FoundIn::Parsing)) {
        // What is taken apart always ends with the end of a line, which is added if it is not there.
        lineText = jsString(vm, makeString(text.substring(lineStart, lineEnd - lineStart), !endsLine && foundIn == FoundIn::Parsing ? "\n"_s : ""_s));
    }

    PyTuple* details = PyTuple::create(globalObject, { jsString(vm, givenSource.provider()->sourceURL()), jsNumber(error.line), jsNumber(error.column + 1), lineText, jsNumber(error.endLine), jsNumber(error.endColumn + 1) });
    JSValue exception = call(globalObject, globalObject->pyRealm()->type(type), jsString(vm, error.message), details);
    RETURN_IF_EXCEPTION(scope, { });
    throwException(globalObject, scope, exception);
    return { };
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

SourceCode makeSource(const String& given, const SourceOrigin& origin, const String& sourceURL)
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
    return SourceCode(StringSourceProvider::create(text, origin, String(sourceURL), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::Python));
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
        if (start == latin1 || start.startsWith(makeString(latin1, '-')))
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
                raiseTokenizerError(globalObject, scope, makeString("encoding problem: "_s, encoding, " with BOM"_s), sourceURL, line, 0, bytes.subspan(lineStart, lineEnd - lineStart), lineEnd - lineStart + line - 1);
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
        raiseTokenizerError(globalObject, scope, makeString("Non-UTF-8 code starting with '\\x"_s, hex(bytes[i], 2, Lowercase), "' on line "_s, line, ", but no encoding declared; see https://peps.python.org/pep-0263/ for details"_s), sourceURL, line, column + 1, bytes.subspan(start, endOfLine(start) - start), column + 1);
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
    String text = decodeBytes(globalObject, object, translated.span(), encoding, String());
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
    for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i) {
        if (!generateAll(vm, codeBlock->functionExpr(i), source, error))
            return false;
    }
    return true;
}

FunctionExecutable* compileSource(JSGlobalObject* globalObject, const SourceCode& source, CodeKind kind, bool usesNamespace, unsigned inheritedFutureFeatures, ImplementationVisibility visibility)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(source.provider()->isPython());

    unsigned futureFeatures = inheritedFutureFeatures;
    bool hasDocstring = false;
    {
        Arena arena;
        Vector<SyntaxWarning> warnings;
        SyntaxError error;
        Module::Kind moduleKind = kind == CodeKind::Module ? Module::Kind::Module : kind == CodeKind::Expression ? Module::Kind::Expression : Module::Kind::Interactive;
        Module* module = parse(vm, arena, source.provider()->source(), moduleKind, warnings, error);
        std::unique_ptr<SymbolTable> table;
        if (module)
            table = SymbolTable::build(vm, arena, *module, inheritedFutureFeatures, error);
        if (!table) {
            raiseSyntaxError(globalObject, scope, error, source, module ? FoundIn::WhatWasParsed : FoundIn::Parsing);
            return nullptr;
        }
        futureFeatures = table->futureFeatures();
        hasDocstring = table->blockFor(module)->hasDocstring;
    }

    auto info = adoptRef(*new FunctionInfo);
    info->kind = kind;
    info->futureFeatures = futureFeatures;
    info->visibility = visibility;
    info->hasDocstring = hasDocstring;
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
    if (!generateAll(vm, unlinked, source, error)) {
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

JSFunction* bindToGlobals(JSGlobalObject* globalObject, FunctionExecutable* executable, JSObject* globals)
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
    return JSFunction::create(vm, globalObject, executable, environment);
}

JSFunction* compileModule(JSGlobalObject* globalObject, const SourceCode& source, JSObject* namespaceObject, ImplementationVisibility visibility)
{
    FunctionExecutable* executable = compileSource(globalObject, source, CodeKind::Module, false, 0, visibility);
    if (!executable)
        return nullptr;
    return bindToGlobals(globalObject, executable, namespaceObject);
}

static void reportException(JSGlobalObject* globalObject, JSValue exception)
{
    // To sys.stderr, if there is one and it can be written to.
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String text = formatException(globalObject, exception);
    JSValue file = sysAttribute(globalObject, "stderr"_s);
    if (!file || isNone(file))
        return;
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    if (!scope.exception())
        call(globalObject, write, jsString(vm, text));
    scope.clearException();
}

int runMain(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const SourceOrigin& origin, const String& sourceURL)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();

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
                call(globalObject, write, jsString(vm, makeString(message, '\n')));
        }
        scope.clearException();
        return 1;
    }
    reportException(globalObject, value);
    return 1;
}

} } // namespace JSC::Python
