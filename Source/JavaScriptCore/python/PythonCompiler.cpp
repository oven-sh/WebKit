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
#include "PyObjects.h"
#include "PythonCodeGenerator.h"
#include "PythonOperations.h"
#include "PythonParser.h"
#include "PythonSymbolTable.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace Python {

static ParserError toParserError(const SyntaxError& error)
{
    return ParserError(ParserError::SyntaxError, ParserError::SyntaxErrorIrrecoverable, JSToken(), error.message, error.line);
}

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

    switch (info->kind) {
    case CodeKind::Module:
    case CodeKind::Expression:
    case CodeKind::Interactive: {
        Vector<SyntaxWarning> warnings;
        Module::Kind moduleKind = info->kind == CodeKind::Module ? Module::Kind::Module : info->kind == CodeKind::Expression ? Module::Kind::Expression : Module::Kind::Interactive;
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
        const Identifier* outerPrivateName = info->kind == CodeKind::Class ? nullptr : privateName;
        Statement* statement = parseDefinition(vm, arena, text, start, end, info->line);
        if (statement)
            table = SymbolTable::buildFragment(vm, arena, statement, nullptr, info->freeVariables, outerPrivateName, info->futureFeatures);
        root = statement;
        blockKey = statement;
        break;
    }
    case CodeKind::Lambda:
    case CodeKind::GeneratorExpression: {
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
        error = toParserError(syntaxError);
        vm.pythonNames().lastSyntaxError = syntaxError;
        return nullptr;
    }
    Block* block = table->blockFor(blockKey);
    RELEASE_ASSERT(block);

    executable->recordParse(NoFeatures, StrictModeLexicallyScopedFeature, false);
    UnlinkedFunctionCodeBlock* result = UnlinkedFunctionCodeBlock::create(vm, FunctionCode, ExecutableInfo(kind == CodeSpecializationKind::CodeForConstruct, executable->privateBrandRequirement(), false, executable->constructorKind(), executable->scriptMode(), executable->superBinding(), parseMode, executable->derivedContextType(), executable->needsClassFieldInitializer(), false, false, executable->evalContextType(), false), codeGenerationMode);

    ParserArena parserArena;
    auto node = makeUnique<ScopeNode>(parserArena, source, arena, *table, *block, *info, root);
    error = BytecodeGenerator::generate(vm, node.get(), source, result, codeGenerationMode, nullptr, nullptr, nullptr);
    if (node->error()) {
        error = toParserError(node->error());
        vm.pythonNames().lastSyntaxError = node->error();
    }
    if (error.isValid())
        return nullptr;
    return result;
}

static JSValue raiseSyntaxError(JSGlobalObject* globalObject, ThrowScope& scope, const SyntaxError& error, const SourceCode& source)
{
    VM& vm = globalObject->vm();
    BuiltinType type = error.kind == SyntaxError::Kind::SyntaxError ? BuiltinType::SyntaxError : error.kind == SyntaxError::Kind::IndentationError ? BuiltinType::IndentationError : BuiltinType::TabError;
    // The line that it is on, as it is in the source.
    StringView text = source.provider()->source();
    unsigned lineStart = 0;
    for (unsigned line = 1; line < error.line && lineStart < text.length(); ++lineStart) {
        if (text[lineStart] == '\n')
            ++line;
    }
    unsigned lineEnd = lineStart;
    while (lineEnd < text.length() && text[lineEnd] != '\n')
        ++lineEnd;
    if (lineEnd < text.length())
        ++lineEnd;
    JSValue lineText = error.line ? JSValue(jsString(vm, text.substring(lineStart, lineEnd - lineStart).toString())) : jsUndefined();

    PyTuple* details = PyTuple::create(globalObject, { jsString(vm, source.provider()->sourceURL()), jsNumber(error.line), jsNumber(error.column + 1), lineText, jsNumber(error.endLine), jsNumber(error.endColumn + 1) });
    JSValue exception = call(globalObject, globalObject->pyRealm()->type(type), jsString(vm, error.message), details);
    RETURN_IF_EXCEPTION(scope, { });
    throwException(globalObject, scope, exception);
    return { };
}

// Compiles a function and all that is in it. Python says what is wrong anywhere in a file before it runs any of it, and some of what can be
// wrong is only found by generating code. False if something is, and then it is in lastSyntaxError.
static bool generateAll(VM& vm, UnlinkedFunctionExecutable* executable, const SourceCode& parentSource)
{
    SourceCode source = executable->linkedSourceCode(parentSource);
    ParserError error;
    UnlinkedFunctionCodeBlock* codeBlock = executable->unlinkedCodeBlockFor(vm, source, CodeSpecializationKind::CodeForCall, { }, error, executable->parseMode());
    if (!codeBlock)
        return false;
    for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i) {
        if (!generateAll(vm, codeBlock->functionExpr(i), source))
            return false;
    }
    return true;
}

FunctionExecutable* compileSource(JSGlobalObject* globalObject, const SourceCode& source, CodeKind kind, bool usesNamespace, unsigned inheritedFutureFeatures)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(source.provider()->language() == SourceLanguage::Python);

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
            raiseSyntaxError(globalObject, scope, error, source);
            return nullptr;
        }
        futureFeatures = table->futureFeatures();
        hasDocstring = table->blockFor(module)->hasDocstring;
    }

    auto info = adoptRef(*new FunctionInfo);
    info->kind = kind;
    info->futureFeatures = futureFeatures;
    info->hasDocstring = hasDocstring;
    info->name = Identifier::fromString(vm, "<module>"_s);
    info->qualifiedName = "<module>"_s;
    if (usesNamespace) {
        info->usesNamespace = true;
        info->parameterNames.append(Identifier::fromString(vm, ".namespace"_s));
        info->positionalCount = 1;
    }
    unsigned parameterCount = info->parameterCount();

    FunctionMetadataNode metadata(JSTokenLocation(), JSTokenLocation(), source.startOffset(), source.startOffset(), source.startOffset(), ImplementationVisibility::Public, StrictModeLexicallyScopedFeature, ConstructorKind::None, SuperBinding::NotNeeded, parameterCount, SourceParseMode::MethodMode, false);
    metadata.finishParsing(source, info->name, FunctionMode::FunctionExpression);
    auto* unlinked = UnlinkedFunctionExecutable::create(vm, source, &metadata, UnlinkedNormalFunction, ConstructAbility::CannotConstruct, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, { }, std::nullopt, DerivedContextType::None, EvalContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);
    unlinked->setPythonInfo(WTF::move(info));

    if (!generateAll(vm, unlinked, source)) {
        raiseSyntaxError(globalObject, scope, vm.pythonNames().lastSyntaxError, source);
        return nullptr;
    }
    return unlinked->link(vm, nullptr, source);
}

JSFunction* bindToGlobals(JSGlobalObject* globalObject, FunctionExecutable* executable, JSObject* namespaceObject)
{
    VM& vm = globalObject->vm();
    // The outermost environment of everything in the module. Its one variable is the namespace.
    JSC::SymbolTable* symbolTable = JSC::SymbolTable::create(vm);
    symbolTable->setScopeType(JSC::SymbolTable::ScopeType::LexicalScope);
    ScopeOffset offset = symbolTable->takeNextScopeOffset(NoLockingNecessary);
    symbolTable->set(NoLockingNecessary, vm.pythonNames().globals.impl(), SymbolTableEntry(VarOffset(offset)));
    JSLexicalEnvironment* environment = JSLexicalEnvironment::create(vm, globalObject->activationStructure(), globalObject->globalScope(), symbolTable, jsUndefined());
    environment->variableAt(offset).set(vm, environment, namespaceObject);
    return JSFunction::create(vm, globalObject, executable, environment);
}

JSFunction* compileModule(JSGlobalObject* globalObject, const SourceCode& source, JSObject* namespaceObject)
{
    FunctionExecutable* executable = compileSource(globalObject, source, CodeKind::Module, false, 0);
    if (!executable)
        return nullptr;
    return bindToGlobals(globalObject, executable, namespaceObject);
}

static void reportException(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String name = typeName(globalObject, exception);
    String message = str(globalObject, exception);
    if (scope.exception()) {
        scope.clearException();
        message = "<exception str() failed>"_s;
    }
    // FIXME: The traceback.
    if (message.isEmpty())
        dataLogLn(name);
    else
        dataLogLn(name, ": ", message);
}

int runMain(JSGlobalObject* globalObject, const SourceCode& source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();

    PyModule* module = PyModule::create(globalObject, "__main__"_s);
    module->namespaceObject()->putDirect(vm, vm.pythonNames().dunder_file, jsString(vm, source.provider()->sourceURL()));
    registerModule(globalObject, "__main__"_s, module);

    JSFunction* function = compileModule(globalObject, source, module->namespaceObject());
    if (function)
        call(globalObject, function);

    flushStandardOutput(globalObject);
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
        dataLogLn(str(globalObject, tuple->at(0)));
        scope.clearException();
        return 1;
    }
    reportException(globalObject, value);
    return 1;
}

} } // namespace JSC::Python
