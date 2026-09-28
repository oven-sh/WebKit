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
#include "PythonBuiltins.h"

#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "JSBigInt.h"
#include "ParserError.h"
#include "PythonBytes.h"
#include "PythonCompiler.h"
#include "PythonSignatures.h"
#include "PythonSymbolTable.h"
#include "PythonSyntaxTreeSource.h"
#include "SourceProvider.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/unicode/UTF8Conversion.h>

// Code objects. One is a piece of source and what has to be known to compile it, which is what a FunctionExecutable of Python's is.

namespace JSC { namespace Python {

// ---- Code objects

// A code object is a PyNativeObject with the FunctionExecutable, and what has been made for it to give out, so that it gives out the same each time.
namespace CodeField {
enum Field : unsigned { Executable, Constants, Bytes };
}

FunctionExecutable* executableOfCode(JSValue code)
{
    return uncheckedDowncast<FunctionExecutable>(uncheckedDowncast<PyNativeObject>(code.asCell())->field(CodeField::Executable).asCell());
}

static FunctionExecutable* executableOf(JSValue code) { return executableOfCode(code); }

const FunctionInfo& infoOfExecutable(FunctionExecutable* executable)
{
    return *executable->unlinkedExecutable()->pythonInfo();
}

static const FunctionInfo& infoOf(FunctionExecutable* executable) { return infoOfExecutable(executable); }

bool isCode(JSGlobalObject* globalObject, JSValue value)
{
    return tryNativeObject(value) && typeOf(globalObject, value) == globalObject->pyRealm()->typeCode();
}

JSObject* codeObjectFor(JSGlobalObject* globalObject, FunctionExecutable* executable)
{
    // Of the two functions that a generator is made of, the code is the one that can be called.
    if (FunctionExecutable* generatorFunction = executable->pythonGeneratorFunction())
        executable = generatorFunction;
    if (JSObject* code = executable->pythonCodeObject())
        return code;
    JSObject* code = PyNativeObject::create(globalObject, BuiltinType::Code, executable);
    executable->setPythonCodeObject(globalObject->vm(), code);
    return code;
}

static UnlinkedFunctionCodeBlock* unlinkedCodeBlockOf(VM& vm, UnlinkedFunctionExecutable* unlinked, const SourceCode& source)
{
    ParserError error;
    return unlinked->unlinkedCodeBlockFor(vm, source, CodeSpecializationKind::CodeForCall, { }, error, unlinked->parseMode());
}

UnlinkedCodeBlock* unlinkedCodeBlockOf(VM& vm, FunctionExecutable* executable)
{
    return unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source());
}

void ensureCodeDetails(VM& vm, FunctionExecutable* executable)
{
    if (!infoOf(executable).details)
        unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source());
    RELEASE_ASSERT(infoOf(executable).details);
}

// What is known once it has been compiled, which it is now if it had not been.
static const CodeDetails& detailsOf(VM& vm, FunctionExecutable* executable)
{
    const FunctionInfo& info = infoOf(executable);
    if (info.isGenerator || info.isCoroutine) {
        // What was written is in the function that resumes it, which is the one function that this one makes.
        UnlinkedFunctionExecutable* body = unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source())->functionExpr(0);
        if (!body->pythonInfo()->details)
            unlinkedCodeBlockOf(vm, body, body->linkedSourceCode(executable->source()));
        return *body->pythonInfo()->details;
    }
    ensureCodeDetails(vm, executable);
    return *info.details;
}

static JSValue tupleOfNames(JSGlobalObject* globalObject, const Vector<Identifier>& names)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = PyTuple::create(globalObject, names.size());
    for (unsigned i = 0; i < names.size(); ++i)
        tuple->initializeAt(vm, i, jsString(vm, names[i].string()));
    return tuple;
}

Vector<Identifier> sortedFreeVariables(const FunctionInfo& info)
{
    Vector<Identifier> names = info.freeVariables;
    std::ranges::sort(names, [] (auto& a, auto& b) { return codePointCompareLessThan(a.string(), b.string()); });
    return names;
}

static constexpr unsigned iterableCoroutineFlag = 0x100; // CO_ITERABLE_COROUTINE

static unsigned flagsOf(const FunctionInfo& info)
{
    unsigned flags = 0;
    if (isFunctionKind(info.kind))
        flags |= 0x1 | 0x2; // CO_OPTIMIZED | CO_NEWLOCALS
    if (info.hasVariadic)
        flags |= 0x4; // CO_VARARGS
    if (info.hasKeywordVariadic)
        flags |= 0x8; // CO_VARKEYWORDS
    if (info.isNested && isFunctionKind(info.kind))
        flags |= 0x10; // CO_NESTED
    if (info.isGenerator && info.isCoroutine)
        flags |= 0x200; // CO_ASYNC_GENERATOR
    else if (info.isGenerator)
        flags |= 0x20; // CO_GENERATOR
    else if (info.isCoroutine)
        flags |= 0x80; // CO_COROUTINE
    if (info.hasDocstring && isFunctionKind(info.kind))
        flags |= 0x4000000; // CO_HAS_DOCSTRING
    if (info.isMethod)
        flags |= 0x8000000; // CO_METHOD
    if (info.isIterableCoroutine)
        flags |= iterableCoroutineFlag;
    return flags | (info.futureFeatures & FutureFeaturesMask);
}

// What was written, compiled. For a generator or a coroutine that is the function that resumes it, which is the one function that the function proper makes.
struct CompiledCode {
    UnlinkedCodeBlock* codeBlock;
    SourceCode source;
    const FunctionInfo* info;
};

static CompiledCode compiledCodeOf(VM& vm, FunctionExecutable* executable)
{
    const FunctionInfo& info = infoOf(executable);
    UnlinkedFunctionCodeBlock* codeBlock = unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source());
    if (!info.isGenerator && !info.isCoroutine)
        return { codeBlock, executable->source(), &info };
    UnlinkedFunctionExecutable* body = codeBlock->functionExpr(0);
    SourceCode source = body->linkedSourceCode(executable->source());
    return { unlinkedCodeBlockOf(vm, body, source), source, body->pythonInfo() };
}

static unsigned firstLineOf(const FunctionInfo& info) { return info.firstLine + info.lineDelta; }

static bool isAllOfItsSource(const FunctionInfo& info)
{
    return info.kind == CodeKind::Module || info.kind == CodeKind::Expression || info.kind == CodeKind::Interactive || info.owner == OwnerKind::Module || info.owner == OwnerKind::Interactive;
}

// ---- co_consts

static JSValue valueOfConstant(JSGlobalObject* globalObject, const CodeDetails::Constant& constant, FunctionExecutable* executable, const CompiledCode& compiled)
{
    VM& vm = globalObject->vm();
    using Kind = CodeDetails::Constant::Kind;
    switch (constant.kind) {
    case Kind::None:
        return jsUndefined();
    case Kind::True:
        return jsBoolean(true);
    case Kind::False:
        return jsBoolean(false);
    case Kind::Ellipsis:
        return globalObject->pyRealm()->ellipsis();
    case Kind::Integer: {
        if (constant.bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return intFromInt64(globalObject, constant.isNegative ? -static_cast<int64_t>(constant.bits) : static_cast<int64_t>(constant.bits));
        return JSBigInt::createFrom(globalObject, constant.bits);
    }
    case Kind::BigInteger:
        return JSBigInt::parseInt(globalObject, vm, constant.text, constant.radix, JSBigInt::ErrorParseMode::ThrowExceptions, JSBigInt::ParseIntSign::Unsigned);
    case Kind::Float:
        return jsTaggedFloat(std::bit_cast<double>(constant.bits));
    case Kind::Imaginary:
        return PyComplex::create(globalObject, 0, std::bit_cast<double>(constant.bits));
    case Kind::String:
        return jsString(vm, constant.text);
    case Kind::Bytes:
        return newBytes(globalObject, constant.text.is8Bit() ? byteCast<uint8_t>(constant.text.span8()) : std::span<const uint8_t>());
    case Kind::Code:
        return codeObjectFor(globalObject, compiled.codeBlock->functionExpr(constant.bits)->link(vm, executable->topLevelExecutable(), compiled.source));
    case Kind::Complex:
        return PyComplex::create(globalObject, std::bit_cast<double>(constant.bits), std::bit_cast<double>(constant.imaginaryBits));
    case Kind::Tuple:
    case Kind::FrozenSet: {
        MarkedArgumentBuffer elements;
        for (auto& element : constant.elements)
            elements.append(valueOfConstant(globalObject, element, executable, compiled));
        PyTuple* tuple = PyTuple::createFromArguments(globalObject, elements);
        if (constant.kind == Kind::Tuple)
            return tuple;
        return setFromIterable(globalObject, globalObject->pyRealm()->typeFrozenSet()->instanceStructure(), tuple);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue getConstants(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* code = uncheckedDowncast<PyNativeObject>(self.asCell());
    if (JSValue constants = code->field(CodeField::Constants))
        return constants;
    FunctionExecutable* executable = executableOf(self);
    CompiledCode compiled = compiledCodeOf(vm, executable);
    MarkedArgumentBuffer values;
    for (auto& constant : compiled.info->details->constants) {
        values.append(valueOfConstant(globalObject, constant, executable, compiled));
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue constants = PyTuple::createFromArguments(globalObject, values);
    code->setField(vm, CodeField::Constants, constants);
    return constants;
}

// ---- Where each instruction is from

// A place in the source as Python gives it: the line, and how many bytes into it, were it UTF-8.
struct Place {
    unsigned line;
    unsigned column;
};

static Place placeOf(SourceProvider& provider, unsigned offset, int lineDelta)
{
    StringView text = provider.source();
    unsigned lineStart = offset;
    while (lineStart && text[lineStart - 1] != '\n')
        --lineStart;
    unsigned column = 0;
    for (char32_t character : text.substring(lineStart, offset - lineStart).codePoints())
        column += character < 0x80 ? 1 : character < 0x800 ? 2 : character < 0x10000 ? 3 : 4;
    return { provider.documentLineColumnForOffset(offset).line + lineDelta, column };
}

// Calls the function with where each instruction is, and the part of the source that it is from. What comes before what was written, giving the arguments to the
// parameters, is from nowhere.
template<typename Function>
static void forEachInstruction(const CompiledCode& compiled, const Function& function)
{
    unsigned firstTraceableOffset = compiled.info->details->firstTraceableOffset;
    // Nor is what comes before the first that says where it is from.
    bool hasBegun = false;
    for (const auto& instruction : compiled.codeBlock->instructions()) {
        unsigned offset = instruction.offset();
        auto entry = compiled.codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(offset));
        hasBegun |= offset >= firstTraceableOffset && entry.instPC == offset && entry.endOffset;
        if (!hasBegun) {
            function(offset, std::nullopt);
            continue;
        }
        unsigned divot = entry.divot + compiled.source.startOffset();
        function(offset, std::optional { std::pair { divot - entry.startOffset, divot + entry.endOffset } });
    }
}

// code.co_lines(): (start, end, line) for each run of instructions that are from one line
PYTHON_NATIVE(codeLines)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    CompiledCode compiled = compiledCodeOf(vm, executableOf(args[0]));
    SourceProvider& provider = *compiled.source.provider();
    MarkedArgumentBuffer runs;
    unsigned runStart = 0;
    std::optional<unsigned> runLine;
    auto finishRun = [&] (unsigned end) {
        if (runLine && end > runStart)
            runs.append(PyTuple::create(globalObject, { jsNumber(runStart), jsNumber(end), jsNumber(*runLine) }));
        runStart = end;
    };
    forEachInstruction(compiled, [&] (unsigned offset, std::optional<std::pair<unsigned, unsigned>> range) {
        // Before a module has begun it is on no line at all.
        unsigned line = range ? provider.documentLineColumnForOffset(range->first).line + compiled.info->lineDelta : isAllOfItsSource(*compiled.info) ? 0 : firstLineOf(*compiled.info);
        if (runLine == line)
            return;
        finishRun(offset);
        runLine = line;
    });
    finishRun(compiled.codeBlock->instructions().size());
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::CodeLines, PyTuple::createFromArguments(globalObject, runs)));
}

// code.co_positions(): (line, end_line, column, end_column) for each instruction
PYTHON_NATIVE(codePositions)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    CompiledCode compiled = compiledCodeOf(vm, executableOf(args[0]));
    SourceProvider& provider = *compiled.source.provider();
    // What wants to know where an instruction is from takes what comes at half its offset, CPython's instructions being made of units of two bytes: traceback.py does with tb_lasti, and
    // dis. So there is one for each two bytes. Where two instructions begin in the same two, the first is a byte long, and it is the second that can be somewhere of its own.
    MarkedArgumentBuffer distinct;
    Vector<unsigned> indices;
    JSValue last;
    std::optional<std::pair<unsigned, unsigned>> lastRange;
    forEachInstruction(compiled, [&] (unsigned offset, std::optional<std::pair<unsigned, unsigned>> range) {
        while (indices.size() < offset / 2)
            indices.append(distinct.size() - 1);
        if (!last || range != lastRange) {
            if (range) {
                Place start;
                Place end;
                if (provider.isPythonSyntaxTree()) {
                    PlaceInSource place = placeOfNodeInSyntaxTree(provider.source(), range->first);
                    start = { place.line + compiled.info->lineDelta, place.column };
                    end = { place.endLine + compiled.info->lineDelta, place.endColumn };
                } else {
                    start = placeOf(provider, range->first, compiled.info->lineDelta);
                    end = placeOf(provider, range->second, compiled.info->lineDelta);
                }
                last = PyTuple::create(globalObject, { jsNumber(start.line), jsNumber(end.line), jsNumber(start.column), jsNumber(end.column) });
            } else
                // Before a module has begun it is on the line before its first.
                last = PyTuple::create(globalObject, { jsNumber(firstLineOf(*compiled.info) - isAllOfItsSource(*compiled.info)), jsNumber(firstLineOf(*compiled.info)), jsNumber(0), jsNumber(0) });
            lastRange = range;
            distinct.append(last);
        }
        if (indices.size() == offset / 2)
            indices.append(distinct.size() - 1);
        else
            indices.last() = distinct.size() - 1;
    });
    while (indices.size() < (compiled.codeBlock->instructions().size() + 1) / 2)
        indices.append(distinct.size() - 1);
    MarkedArgumentBuffer positions;
    for (unsigned index : indices)
        positions.append(distinct.at(index));
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::CodePositions, PyTuple::createFromArguments(globalObject, positions)));
}

// code.co_branches()
PYTHON_NATIVE(codeBranches)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MarkedArgumentBuffer branches;
    forEachBranch(compiledCodeOf(vm, executableOf(args[0])).codeBlock, [&] (unsigned offset, unsigned notTaken, unsigned taken) {
        branches.append(PyTuple::create(globalObject, { jsNumber(offset), jsNumber(notTaken), jsNumber(taken) }));
    });
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::CodeLines, PyTuple::createFromArguments(globalObject, branches)));
}

// code._varname_from_oparg(oparg): the name of a local variable, a cell or a free variable, by where it comes among all of them
PYTHON_NATIVE(codeVariableName)
{
    NATIVE_PROLOGUE();
    auto index = toIndex(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    const auto& variables = detailsOf(vm, executableOf(args[0])).frameVariables;
    if (*index < 0 || static_cast<uint64_t>(*index) >= variables.size())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "tuple index out of range"_s));
    return JSValue::encode(jsString(vm, variables[*index].name.string()));
}

// ---- co_code
//
// What there is to run is the source, and what has to be known to compile it that the source does not say, because it depends on what the code is in. That is what
// co_code is: not instructions, which are the engine's business and are made when they are wanted, but what they are made from. It is what two pieces of code have in common
// if they are equal, and it does not have in it what a code object says besides: what it is called, what file it is from and what line it begins on.

class CodeWriter {
public:
    void byte(uint8_t value) { m_bytes.append(value); }
    void number(uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            m_bytes.append(static_cast<uint8_t>(value >> shift));
    }
    // A null string is not an empty one.
    void string(StringView value)
    {
        if (value.isNull())
            return number(std::numeric_limits<uint32_t>::max());
        number(value.length());
        bool isLatin1 = value.is8Bit() || WTF::charactersAreAllLatin1(value.span16());
        byte(isLatin1);
        for (char16_t unit : value.codeUnits()) {
            byte(static_cast<uint8_t>(unit));
            if (!isLatin1)
                byte(static_cast<uint8_t>(unit >> 8));
        }
    }
    void names(const Vector<Identifier>& names)
    {
        number(names.size());
        for (auto& name : names)
            string(name.string());
    }
    ByteVector take()
    {
        // As many as make up whole instructions in CPython, which is asked of it there.
        if (m_bytes.size() % 2)
            byte(0);
        return WTF::move(m_bytes);
    }

private:
    ByteVector m_bytes;
};

static constexpr std::array<uint8_t, 4> codeMagic { 0xF3, 'J', 'S', 'C' };
static constexpr uint8_t codeVersion = 1;

namespace CodeBit {
enum Bit : uint32_t {
    IsGenerator = 1 << 0,
    IsCoroutine = 1 << 1,
    HasVariadic = 1 << 2,
    HasKeywordVariadic = 1 << 3,
    UsesNamespace = 1 << 4,
    IsNested = 1 << 5,
    IsMethod = 1 << 6,
    HasDocstring = 1 << 7,
    CanSeeClassScope = 1 << 8,
    IsSyntaxTree = 1 << 9, // The source is a tree, written out: PythonSyntaxTreeSource.h.
};
}

static ByteVector bytesOf(FunctionExecutable* executable)
{
    const FunctionInfo& info = infoOf(executable);
    CodeWriter writer;
    for (uint8_t byte : codeMagic)
        writer.byte(byte);
    writer.byte(codeVersion);
    writer.byte(static_cast<uint8_t>(info.kind));
    writer.byte(static_cast<uint8_t>(info.owner));
    writer.byte(static_cast<uint8_t>(info.evaluates));
    writer.number((info.isGenerator ? CodeBit::IsGenerator : 0) | (info.isCoroutine ? CodeBit::IsCoroutine : 0) | (info.hasVariadic ? CodeBit::HasVariadic : 0)
        | (info.hasKeywordVariadic ? CodeBit::HasKeywordVariadic : 0) | (info.usesNamespace ? CodeBit::UsesNamespace : 0) | (info.isNested ? CodeBit::IsNested : 0)
        | (info.isMethod ? CodeBit::IsMethod : 0) | (info.hasDocstring ? CodeBit::HasDocstring : 0) | (info.canSeeClassScope ? CodeBit::CanSeeClassScope : 0)
        | (executable->source().provider()->isPythonSyntaxTree() ? CodeBit::IsSyntaxTree : 0));
    writer.number(info.typeParameterIndex);
    writer.number(info.futureFeatures);
    writer.byte(info.optimizationLevel);
    writer.string(info.qualifiedNameInSource.isNull() ? info.qualifiedName : info.qualifiedNameInSource);
    writer.string(info.qualifiedNamePrefix);
    writer.string(info.docstring);
    writer.string(info.privateName.string());
    writer.names(sortedFreeVariables(info));
    writer.names(info.parameterNames);
    writer.number(info.positionalOnlyCount);
    writer.number(info.positionalCount);
    writer.number(info.keywordOnlyCount);

    // The source, from the beginning of the line that it begins on, so that it is as far in as it was.
    const SourceCode& source = executable->source();
    StringView text = source.provider()->source();
    unsigned start = source.startOffset();
    unsigned end = source.endOffset();
    unsigned from = start;
    if (isAllOfItsSource(info)) {
        from = 0;
        end = text.length();
    } else if (source.provider()->isPythonSyntaxTree()) {
        // Each node says what line it is on, and this is what to take that from.
        writer.number(info.firstLine);
    } else {
        while (from && text[from - 1] != '\n')
            --from;
    }
    writer.number(start - from);
    writer.number(info.line - info.firstLine);
    writer.string(text.substring(from, end - from));
    return writer.take();
}

class CodeReader {
public:
    explicit CodeReader(std::span<const uint8_t> bytes)
        : m_bytes(bytes)
    {
    }

    bool hasFailed() const { return m_hasFailed; }
    uint8_t byte()
    {
        if (m_bytes.empty()) {
            m_hasFailed = true;
            return 0;
        }
        uint8_t value = m_bytes[0];
        m_bytes = m_bytes.subspan(1);
        return value;
    }
    uint32_t number()
    {
        uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<uint32_t>(byte()) << shift;
        return value;
    }
    String string()
    {
        uint32_t length = number();
        if (length == std::numeric_limits<uint32_t>::max())
            return { };
        bool isLatin1 = byte();
        if (m_hasFailed || static_cast<uint64_t>(length) * (isLatin1 ? 1 : 2) > m_bytes.size()) {
            m_hasFailed = true;
            return { };
        }
        if (isLatin1) {
            String result = String(byteCast<Latin1Character>(m_bytes.first(length)));
            m_bytes = m_bytes.subspan(length);
            return result.isNull() ? emptyString() : result;
        }
        Vector<char16_t> units(length, [&] (size_t i) { return static_cast<char16_t>(m_bytes[2 * i] | m_bytes[2 * i + 1] << 8); });
        m_bytes = m_bytes.subspan(2 * length);
        return String(units.span());
    }
    Identifier name(VM& vm)
    {
        String value = string();
        return value.isNull() ? Identifier() : Identifier::fromString(vm, value);
    }
    Vector<Identifier> names(VM& vm)
    {
        uint32_t count = number();
        Vector<Identifier> result;
        // Each takes up some room.
        if (count > m_bytes.size()) {
            m_hasFailed = true;
            return result;
        }
        for (uint32_t i = 0; i < count && !m_hasFailed; ++i) {
            result.append(name(vm));
            m_hasFailed |= result.last().isNull();
        }
        return result;
    }

private:
    std::span<const uint8_t> m_bytes;
    bool m_hasFailed { false };
};

// The other way. Null if that is not what they are.
static FunctionExecutable* executableFromBytes(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& filename, const String& name, const String& qualifiedName, unsigned firstLine)
{
    VM& vm = globalObject->vm();
    CodeReader reader(bytes);
    for (uint8_t byte : codeMagic) {
        if (reader.byte() != byte)
            return nullptr;
    }
    if (reader.byte() != codeVersion)
        return nullptr;
    auto info = adoptRef(*new FunctionInfo);
    uint8_t kind = reader.byte();
    uint8_t owner = reader.byte();
    uint8_t evaluates = reader.byte();
    if (kind > static_cast<uint8_t>(CodeKind::Evaluator) || owner > static_cast<uint8_t>(OwnerKind::Interactive) || evaluates > static_cast<uint8_t>(Evaluates::Value))
        return nullptr;
    info->kind = static_cast<CodeKind>(kind);
    info->owner = static_cast<OwnerKind>(owner);
    info->evaluates = static_cast<Evaluates>(evaluates);
    uint32_t bits = reader.number();
    info->isGenerator = bits & CodeBit::IsGenerator;
    info->isCoroutine = bits & CodeBit::IsCoroutine;
    info->hasVariadic = bits & CodeBit::HasVariadic;
    info->hasKeywordVariadic = bits & CodeBit::HasKeywordVariadic;
    info->usesNamespace = bits & CodeBit::UsesNamespace;
    info->isNested = bits & CodeBit::IsNested;
    info->isMethod = bits & CodeBit::IsMethod;
    info->hasDocstring = bits & CodeBit::HasDocstring;
    info->canSeeClassScope = bits & CodeBit::CanSeeClassScope;
    info->typeParameterIndex = reader.number();
    info->futureFeatures = reader.number() & (FutureFeaturesMask | AllowTopLevelAwait);
    info->optimizationLevel = std::min<uint8_t>(reader.byte(), 2);
    String qualifiedNameInSource = reader.string();
    info->qualifiedNamePrefix = reader.string();
    info->docstring = reader.string();
    info->privateName = reader.name(vm);
    info->freeVariables = reader.names(vm);
    info->parameterNames = reader.names(vm);
    info->positionalOnlyCount = reader.number();
    info->positionalCount = reader.number();
    info->keywordOnlyCount = reader.number();
    bool isTree = bits & CodeBit::IsSyntaxTree;
    uint32_t firstLineInTree = isTree && !isAllOfItsSource(info.get()) ? reader.number() : 1;
    uint32_t start = reader.number();
    uint32_t linesOfDecorators = reader.number();
    String text = reader.string();
    if (reader.hasFailed() || text.isNull() || qualifiedNameInSource.isNull() || start > text.length())
        return nullptr;
    // Too many to be added up.
    if (info->positionalCount > info->parameterNames.size() || info->keywordOnlyCount > info->parameterNames.size())
        return nullptr;

    info->name = Identifier::fromString(vm, name);
    info->qualifiedName = qualifiedName;
    if (qualifiedName != qualifiedNameInSource)
        info->qualifiedNameInSource = qualifiedNameInSource;
    // All of a source begins on its first line, whatever the code says.
    bool isWhole = isAllOfItsSource(info.get());
    if (linesOfDecorators > static_cast<uint32_t>(std::numeric_limits<int>::max()) - firstLine)
        return nullptr;
    info->firstLine = isWhole ? 1 : firstLine;
    info->line = info->firstLine + (isWhole ? 0 : linesOfDecorators);
    info->lineDelta = isWhole ? static_cast<int>(firstLine) - 1 : 0;
    if (isTree) {
        if (start || firstLineInTree > static_cast<uint32_t>(std::numeric_limits<int>::max()) - linesOfDecorators)
            return nullptr;
        info->firstLine = firstLineInTree;
        info->line = firstLineInTree + linesOfDecorators;
        info->lineDelta = static_cast<int>(firstLine) - static_cast<int>(firstLineInTree);
        return executableFromProgram(globalObject, SourceCode(SyntaxTreeSourceProvider::create(text, SourceOrigin(), filename)), WTF::move(info));
    }
    SourceCode source = makeSource(text, SourceOrigin(), filename, info->line);
    // What that does to the ends of lines has been done to this already, or it is not what was written out.
    if (source.provider()->source().length() != text.length())
        return nullptr;
    return executableFromProgram(globalObject, SourceCode(*source.provider(), isWhole ? 0 : start, text.length()), WTF::move(info));
}

static JSValue getBytes(JSGlobalObject* globalObject, JSValue self)
{
    auto* code = uncheckedDowncast<PyNativeObject>(self.asCell());
    if (JSValue bytes = code->field(CodeField::Bytes))
        return bytes;
    JSValue bytes = newBytes(globalObject, bytesOf(executableOf(self)).span());
    code->setField(globalObject->vm(), CodeField::Bytes, bytes);
    return bytes;
}

// ---- repr(), == and hash()

PYTHON_NATIVE(codeRepr)
{
    VM& vm = globalObject->vm();
    FunctionExecutable* executable = executableOf(callFrame->argument(0));
    const FunctionInfo& info = infoOf(executable);
    return JSValue::encode(jsString(vm, makeString("<code object "_s, info.name.string(), " at 0x"_s, hex(std::bit_cast<uintptr_t>(callFrame->argument(0).asCell()), Lowercase), ", file \""_s, executable->source().provider()->sourceURL(), "\", line "_s, firstLineOf(info), '>')));
}

// By what they are called, where they begin and what they do. Not by what file they say they are from.
static bool areEqual(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    FunctionExecutable* first = executableOf(a);
    FunctionExecutable* second = executableOf(b);
    if (first->unlinkedExecutable() == second->unlinkedExecutable())
        return true;
    const FunctionInfo& firstInfo = infoOf(first);
    const FunctionInfo& secondInfo = infoOf(second);
    if (firstInfo.name != secondInfo.name || firstLineOf(firstInfo) != firstLineOf(secondInfo) || flagsOf(firstInfo) != flagsOf(secondInfo))
        return false;
    return equalSpans(*builtinBufferOf(getBytes(globalObject, a)), *builtinBufferOf(getBytes(globalObject, b)));
}

PYTHON_NATIVE(codeEq)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (!isEquality(op) || !isCode(globalObject, args.at(1)))
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(jsBoolean(areEqual(globalObject, args[0], args[1]) == (op == ComparisonOperator::Eq)));
}

PYTHON_NATIVE(codeHash)
{
    NATIVE_PROLOGUE();
    const FunctionInfo& info = infoOf(executableOf(args[0]));
    int64_t bytesHash = hash(globalObject, getBytes(globalObject, args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    uint64_t result = static_cast<uint64_t>(bytesHash) * 1000003 ^ info.name.impl()->hash() ^ (static_cast<uint64_t>(firstLineOf(info)) << 20) ^ flagsOf(info);
    int64_t value = static_cast<int64_t>(result >> 3);
    return JSValue::encode(intFromInt64(globalObject, value));
}

// ---- code.replace()

namespace {

// What a code object is made from, in the order that code() takes them.
struct CodeParts {
    JSValue argumentCount, positionalOnlyCount, keywordOnlyCount, localCount, stackSize, flags, bytes, constants, names, variableNames, filename, name, qualifiedName, firstLine, lineTable, exceptionTable, freeVariables, cellVariables;
};

enum class PartType : uint8_t { Int, Bytes, Tuple, Str };

struct Part {
    ASCIILiteral attribute;
    JSValue CodeParts::* member;
    PartType type;
};

constexpr Part parts[] = {
    { "co_argcount"_s, &CodeParts::argumentCount, PartType::Int },
    { "co_posonlyargcount"_s, &CodeParts::positionalOnlyCount, PartType::Int },
    { "co_kwonlyargcount"_s, &CodeParts::keywordOnlyCount, PartType::Int },
    { "co_nlocals"_s, &CodeParts::localCount, PartType::Int },
    { "co_stacksize"_s, &CodeParts::stackSize, PartType::Int },
    { "co_flags"_s, &CodeParts::flags, PartType::Int },
    { "co_code"_s, &CodeParts::bytes, PartType::Bytes },
    { "co_consts"_s, &CodeParts::constants, PartType::Tuple },
    { "co_names"_s, &CodeParts::names, PartType::Tuple },
    { "co_varnames"_s, &CodeParts::variableNames, PartType::Tuple },
    { "co_filename"_s, &CodeParts::filename, PartType::Str },
    { "co_name"_s, &CodeParts::name, PartType::Str },
    { "co_qualname"_s, &CodeParts::qualifiedName, PartType::Str },
    { "co_firstlineno"_s, &CodeParts::firstLine, PartType::Int },
    { "co_linetable"_s, &CodeParts::lineTable, PartType::Bytes },
    { "co_exceptiontable"_s, &CodeParts::exceptionTable, PartType::Bytes },
    { "co_freevars"_s, &CodeParts::freeVariables, PartType::Tuple },
    { "co_cellvars"_s, &CodeParts::cellVariables, PartType::Tuple },
};

} // namespace

// Another like `code`, with what it says of itself changed. All else about it follows from co_code, and can be given only as what it is.
enum class MakesNoCopy : bool { Never, IfTheSame };

static JSValue codeWithParts(JSGlobalObject* globalObject, ThrowScope& scope, JSValue code, const CodeParts& given, MakesNoCopy makesNoCopy = MakesNoCopy::Never)
{
    VM& vm = globalObject->vm();
    FunctionExecutable* executable = executableOf(code);
    auto info = infoOf(executable).copy();
    for (auto& part : parts) {
        JSValue value = given.*part.member;
        if (!value || part.member == &CodeParts::filename || part.member == &CodeParts::name || part.member == &CodeParts::qualifiedName || part.member == &CodeParts::firstLine || part.member == &CodeParts::flags)
            continue;
        JSValue current = getAttribute(globalObject, code, Identifier::fromString(vm, part.attribute));
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = isEqual(globalObject, value, current);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isSame)
            return raiseValueError(globalObject, scope, makeString("code: "_s, part.attribute, " is what follows from co_code, which is compiled from source, and cannot be given as anything else"_s));
    }
    if (given.flags) {
        unsigned flags = given.flags.asInt32();
        if ((flags & ~iterableCoroutineFlag) != (flagsOf(info.get()) & ~iterableCoroutineFlag))
            return raiseValueError(globalObject, scope, "code: of co_flags, only CO_ITERABLE_COROUTINE does not follow from co_code, which is compiled from source"_s);
        info->isIterableCoroutine = flags & iterableCoroutineFlag;
    }
    if (given.name)
        info->name = Identifier::fromString(vm, asString(given.name)->value(globalObject).data);
    if (given.qualifiedName) {
        if (info->qualifiedNameInSource.isNull())
            info->qualifiedNameInSource = info->qualifiedName;
        info->qualifiedName = asString(given.qualifiedName)->value(globalObject).data;
    }
    if (given.firstLine)
        info->lineDelta = given.firstLine.asInt32() - static_cast<int>(info->firstLine);
    String filename;
    if (given.filename)
        filename = asString(given.filename)->value(globalObject).data;
    RETURN_IF_EXCEPTION(scope, { });
    if (!given.filename && !given.name && !given.qualifiedName && !given.firstLine && info->isIterableCoroutine == infoOf(executable).isIterableCoroutine && makesNoCopy == MakesNoCopy::IfTheSame)
        return code;
    return codeObjectFor(globalObject, cloneExecutable(globalObject, executable, WTF::move(info), filename));
}

static void auditNewCode(JSGlobalObject* globalObject, JSValue code, const CodeParts& given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!globalObject->pyRealm()->auditHooks())
        return;
    MarkedArgumentBuffer audited;
    for (auto member : { &CodeParts::bytes, &CodeParts::filename, &CodeParts::name, &CodeParts::argumentCount, &CodeParts::positionalOnlyCount, &CodeParts::keywordOnlyCount, &CodeParts::localCount, &CodeParts::stackSize, &CodeParts::flags }) {
        JSValue value = given.*member;
        if (!value) {
            for (auto& part : parts) {
                if (part.member == member)
                    value = getAttribute(globalObject, code, Identifier::fromString(vm, part.attribute));
            }
            RETURN_IF_EXCEPTION(scope, void());
        }
        audited.append(value);
    }
    scope.release();
    auditSlow(globalObject, "code.__new__"_s, audited);
}

// An argument as one of the kinds that these are. False if it raised.
static bool checkPart(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, const String& which, PartType type, JSValue& value)
{
    auto complain = [&] (ASCIILiteral wanted) {
        raiseTypeError(globalObject, scope, makeString(function, "() argument "_s, which, " must be "_s, wanted, ", not "_s, typeNameOfArgument(globalObject, value)));
        return false;
    };
    switch (type) {
    case PartType::Int: {
        auto number = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        value = jsNumber(*number);
        return true;
    }
    case PartType::Bytes:
        return isBytes(value) || complain("bytes"_s);
    case PartType::Tuple:
        return isTuple(value) || complain("tuple"_s);
    case PartType::Str:
        return stringIn(value) || complain("str"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// code.replace(**changes), and code.__replace__(), which is what copy.replace() calls
PYTHON_NATIVE(codeReplace)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "replace() takes no positional arguments"_s));
    CodeParts given;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        auto name = args.keywordName(i)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        const Part* found = nullptr;
        for (auto& part : parts) {
            if (name.data == part.attribute)
                found = &part;
        }
        if (!found) {
            Vector<String> candidates;
            for (auto& part : parts)
                candidates.append(part.attribute);
            String suggestion = calculateSuggestion(candidates, name.data);
            return JSValue::encode(raiseTypeError(globalObject, scope, suggestion.isNull() ? makeString("replace() got an unexpected keyword argument '"_s, name.data, '\'') : makeString("replace() got an unexpected keyword argument '"_s, name.data, "'. Did you mean '"_s, suggestion, "'?"_s)));
        }
        JSValue value = args.keywordValue(i);
        if (!checkPart(globalObject, scope, "replace"_s, makeString('\'', found->attribute, '\''), found->type, value))
            return { };
        given.*found->member = value;
    }
    // In the order that CPython looks at them.
    for (auto member : { &CodeParts::argumentCount, &CodeParts::positionalOnlyCount, &CodeParts::keywordOnlyCount, &CodeParts::localCount, &CodeParts::stackSize, &CodeParts::flags, &CodeParts::firstLine }) {
        if (JSValue value = given.*member; value && value.asInt32() < 0) {
            for (auto& part : parts) {
                if (part.member == member)
                    return JSValue::encode(raiseValueError(globalObject, scope, makeString(part.attribute, " must be a positive integer"_s)));
            }
        }
    }
    auditNewCode(globalObject, args[0], given);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(codeWithParts(globalObject, scope, args[0], given)));
}

// code(argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize, flags, codestring, constants, names, varnames, filename, name, qualname, firstlineno, linetable,
//     exceptiontable, freevars=(), cellvars=(), /)
PYTHON_NATIVE(codeNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "code"_s))
        return { };
    unsigned count = args.size() - 1;
    if (count < 16 || count > std::size(parts))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("code expected at "_s, count < 16 ? "least 16"_s : "most 18"_s, " arguments, got "_s, count)));
    CodeParts given;
    for (unsigned i = 0; i < count; ++i) {
        JSValue value = args[i + 1];
        if (!checkPart(globalObject, scope, "code"_s, String::number(i + 1), parts[i].type, value))
            return { };
        given.*parts[i].member = value;
    }
    auditNewCode(globalObject, JSValue(), given);
    RETURN_IF_EXCEPTION(scope, { });
    static constexpr std::pair<JSValue CodeParts::*, ASCIILiteral> counts[] = { { &CodeParts::argumentCount, "argcount"_s }, { &CodeParts::positionalOnlyCount, "posonlyargcount"_s }, { &CodeParts::keywordOnlyCount, "kwonlyargcount"_s }, { &CodeParts::localCount, "nlocals"_s } };
    for (auto [member, name] : counts) {
        if ((given.*member).asInt32() < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, makeString("code: "_s, name, " must not be negative"_s)));
    }
    for (auto member : { &CodeParts::names, &CodeParts::variableNames, &CodeParts::freeVariables, &CodeParts::cellVariables }) {
        JSValue tuple = given.*member;
        if (!tuple)
            continue;
        for (auto& item : asTuple(tuple)->span()) {
            if (!stringIn(item.get()))
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("name tuples must contain only strings, not '"_s, typeName(globalObject, item.get()), '\'')));
        }
    }
    if (given.stackSize.asInt32() < 0 || given.flags.asInt32() < 0 || given.argumentCount.asInt32() < given.positionalOnlyCount.asInt32())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "bad argument to internal function"_s));

    String filename = asString(given.filename)->value(globalObject);
    String name = asString(given.name)->value(globalObject);
    String qualifiedName = asString(given.qualifiedName)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    FunctionExecutable* executable = executableFromBytes(globalObject, *builtinBufferOf(given.bytes), filename, name, qualifiedName, std::max(given.firstLine.asInt32(), 0));
    if (!executable)
        return JSValue::encode(raiseValueError(globalObject, scope, "code: co_code is malformed"_s));
    // The rest is to be what follows from that. What has been seen to already is not looked at again.
    given.bytes = given.filename = given.name = given.qualifiedName = given.firstLine = JSValue();
    if (!given.freeVariables)
        given.freeVariables = realm->emptyTuple();
    if (!given.cellVariables)
        given.cellVariables = realm->emptyTuple();
    RELEASE_AND_RETURN(scope, JSValue::encode(codeWithParts(globalObject, scope, codeObjectFor(globalObject, executable), given, MakesNoCopy::IfTheSame)));
}

// ---- Setting it up

void initializeCodeType(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    PyType* code = realm->typeCode();
    addMethods(globalObject, code, {
        { "__repr__"_s, codeRepr },
        { "__hash__"_s, codeHash },
        { "__new__"_s, codeNew, PyNativeFunction::Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "replace"_s, codeReplace, PyNativeFunction::Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__replace__"_s, codeReplace, PyNativeFunction::Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "co_lines"_s, codeLines },
        { "co_positions"_s, codePositions },
        { "co_branches"_s, codeBranches },
        { "_varname_from_oparg"_s, codeVariableName },
    });
    addComparisons(globalObject, code, codeEq);
    addMember(globalObject, code, "co_name"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), infoOf(executableOf(self)).name.string()); });
    addMember(globalObject, code, "co_qualname"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), infoOf(executableOf(self)).qualifiedName); });
    addMember(globalObject, code, "co_filename"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), executableOf(self)->source().provider()->sourceURL()); });
    addMember(globalObject, code, "co_firstlineno"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(firstLineOf(infoOf(executableOf(self)))); });
    addMember(globalObject, code, "co_flags"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(flagsOf(infoOf(executableOf(self)))); });
    addMember(globalObject, code, "co_argcount"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
        const FunctionInfo& info = infoOf(executableOf(self));
        return jsNumber(info.usesNamespace ? 0 : info.positionalCount);
    });
    addMember(globalObject, code, "co_posonlyargcount"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { 
        const FunctionInfo& info = infoOf(executableOf(self));
        return jsNumber(info.usesNamespace || info.kind == CodeKind::GeneratorExpression || info.kind == CodeKind::Comprehension || info.kind == CodeKind::TypeParameters ? 0 : info.positionalOnlyCount);
    });
    addMember(globalObject, code, "co_kwonlyargcount"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(infoOf(executableOf(self)).keywordOnlyCount); });
    addGetSet(globalObject, code, "co_varnames"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, detailsOf(globalObject->vm(), executableOf(self)).variableNames); });
    addMember(globalObject, code, "co_nlocals"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsNumber(detailsOf(globalObject->vm(), executableOf(self)).variableNames.size()); });
    addMember(globalObject, code, "co_names"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, detailsOf(globalObject->vm(), executableOf(self)).names); });
    addGetSet(globalObject, code, "co_cellvars"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, detailsOf(globalObject->vm(), executableOf(self)).cellVariables); });
    addGetSet(globalObject, code, "co_freevars"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, sortedFreeVariables(infoOf(executableOf(self)))); });

    addMember(globalObject, code, "co_consts"_s, getConstants);
    addGetSet(globalObject, code, "co_code"_s, getBytes);
    addGetSet(globalObject, code, "_co_code_adaptive"_s, getBytes);
    addMember(globalObject, code, "co_stacksize"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsNumber(compiledCodeOf(globalObject->vm(), executableOf(self)).codeBlock->numCalleeLocals()); });
    // Where the lines are and what catches what are the engine's to know, and are asked of it: co_lines() and co_positions().
    auto noBytes = [] (JSGlobalObject* globalObject, JSValue) -> JSValue { return newBytes(globalObject, { }); };
    addMember(globalObject, code, "co_linetable"_s, noBytes);
    addMember(globalObject, code, "co_exceptiontable"_s, noBytes);
    addGetSet(globalObject, code, "co_lnotab"_s, [] (JSGlobalObject* globalObject, JSValue) -> JSValue {
        if (!warn(globalObject, BuiltinType::DeprecationWarning, "co_lnotab is deprecated, use co_lines instead."_s))
            return { };
        return newBytes(globalObject, { });
    });
}

} } // namespace JSC::Python
