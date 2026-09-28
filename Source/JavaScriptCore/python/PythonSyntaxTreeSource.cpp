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
#include "PythonSyntaxTreeSource.h"

#include "PythonASTBuilder.h"
#include "PythonASTWalker.h"
#include "VM.h"
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

static bool saysWhereItIs(ASTClass astClass)
{
    const ASDLClass& description = descriptionOf(astClass);
    // The whole does not, to a program. It is written as if it did, for what is from all of it.
    if (description.base == ASTClass::mod)
        return true;
    return !(description.kind == ASDLClass::Kind::Constructor ? descriptionOf(description.base) : description).attributes.empty();
}

// ---- Writing

namespace {

class Writer : public ASTWalker<Writer> {
public:
    explicit Writer(VM& vm)
        : m_vm(vm)
    {
    }

    String write(Module& module)
    {
        // From the beginning to the end of the last thing in it.
        m_whole.line = 1;
        m_whole.endLine = 1;
        const Node* last = module.kind == Module::Kind::Expression ? static_cast<Node*>(module.expression) : module.body.empty() ? nullptr : module.body.back();
        if (last && static_cast<int>(last->endLine) >= 1) {
            m_whole.endLine = last->endLine;
            m_whole.endColumn = last->endColumn;
        }
        walk(module);
        return m_hasFailed ? String() : m_out.toString();
    }

    void open(ASTClass astClass)
    {
        m_out.append('(', static_cast<unsigned>(astClass), ';');
        if (descriptionOf(astClass).base == ASTClass::mod)
            where(m_whole);
    }

    void open(ASTClass astClass, const Node& node)
    {
        m_out.append('(', static_cast<unsigned>(astClass), ';');
        where(node);
        if (astClass != ASTClass::Attribute || node.line == node.endLine)
            return;
        // update_start_location_to_match_attr(): if what has the attribute is on some earlier line, getting the attribute is on the line that its name is on.
        Node name = node;
        name.line = node.endLine;
        name.column = atLeastNothing(node.endColumn) - std::min<unsigned>(static_cast<const Attribute&>(node).attribute->length(), atLeastNothing(node.endColumn));
        m_out.append('@');
        where(name);
    }

    void name(ASCIILiteral) { }
    void close(const Node&) { m_out.append(')'); }
    void close() { m_out.append(')'); }
    void null() { m_out.append('~'); }
    void identifier(const Identifier& identifier) { characters('n', identifier.string()); }
    void text(const Text& text) { characters(text.isBytes ? 'y' : 's', text.text->string()); }
    void unicodePrefix() { m_out.append("s1:u"_s); }
    void integer(int number) { m_out.append('#', number, ';'); }
    void singleton(ASTClass astClass) { m_out.append('%', static_cast<unsigned>(astClass), ';'); }

    void constant(Constant::Type type)
    {
        switch (type) {
        case Constant::Type::None:
            return m_out.append('N');
        case Constant::Type::True:
            return m_out.append('T');
        case Constant::Type::False:
            return m_out.append('F');
        case Constant::Type::Ellipsis:
            return m_out.append('E');
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void constant(Constant& node)
    {
        switch (node.type) {
        case Constant::Type::None:
        case Constant::Type::True:
        case Constant::Type::False:
        case Constant::Type::Ellipsis:
            return constant(node.type);
        case Constant::Type::Integer:
            return m_out.append('i', node.isNegative ? "-"_s : ""_s, hex(node.integer, Lowercase), ';');
        case Constant::Type::BigInteger:
            // Only what came of an object gets here, and its digits are hexadecimal.
            RELEASE_ASSERT(node.radix == 16);
            return m_out.append('i', node.isNegative ? "-"_s : ""_s, node.text->string(), ';');
        case Constant::Type::Float:
            return m_out.append('f', hex(std::bit_cast<uint64_t>(node.real), 16, Lowercase));
        case Constant::Type::Imaginary:
            return m_out.append('j', hex(std::bit_cast<uint64_t>(node.real), 16, Lowercase));
        case Constant::Type::Complex:
            return m_out.append('c', hex(std::bit_cast<uint64_t>(node.real), 16, Lowercase), hex(std::bit_cast<uint64_t>(node.imaginary), 16, Lowercase));
        case Constant::Type::String:
            return characters('u', node.text->string());
        case Constant::Type::Bytes:
            return characters('b', node.text->string());
        case Constant::Type::Tuple:
        case Constant::Type::FrozenSet:
            if (!canGoDeeper())
                return;
            m_out.append(node.type == Constant::Type::Tuple ? 't' : 'z', node.elements.size(), ':');
            for (Constant* element : node.elements)
                constant(*element);
            return;
        case Constant::Type::Invalid:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void openList(size_t size) { m_out.append('[', size, ':'); }
    void element() { }
    void closeList() { m_out.append(']'); }

    bool canGoDeeper()
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            m_hasFailed = true;
        return !m_hasFailed;
    }

private:
    // A program may say that a node is less than nowhere, and to CPython it is then nowhere.
    static unsigned atLeastNothing(unsigned number) { return std::max(static_cast<int>(number), 0); }

    void where(const Node& node)
    {
        m_out.append(atLeastNothing(node.line), ',', atLeastNothing(node.column), ',', atLeastNothing(node.endLine), ',', atLeastNothing(node.endColumn), ';');
    }

    void characters(char kind, StringView characters) { m_out.append(kind, characters.length(), ':', characters); }

    VM& m_vm;
    StringBuilder m_out;
    Node m_whole;
    bool m_hasFailed { false };
};

// ---- Reading

class Reader {
public:
    struct Value {
        bool isNone { false };
    };
    static constexpr bool attributesComeFirst = true;

    Reader(VM& vm, Arena& arena, StringView tree, unsigned start, unsigned end)
        : m_vm(vm)
        , m_arena(arena)
        , m_tree(tree)
        , m_position(start)
        , m_end(std::min(end, tree.length()))
    {
        if (start > m_end)
            m_hasFailed = true;
    }

    bool hasFailed() const { return m_hasFailed; }
    bool isAtEnd() const { return m_position == m_end; }
    void fail(ASTError::Kind, String&&) { m_hasFailed = true; }

    // What comes next.
    Value next() { return { consumeIf('~') }; }

    bool isNone(Value value) { return value.isNone; }

    bool canGoDeeper(ASTClass)
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            m_hasFailed = true;
        return !m_hasFailed;
    }

    // For a class that is not the sum of others, whether it is that.
    ASTClass constructorOf(Value value, ASTClass type)
    {
        bool isSum = descriptionOf(type).kind == ASDLClass::Kind::Sum;
        bool isSingleton = isSum && at('%');
        unsigned start = m_position;
        if (value.isNone || !(consumeIf('%') || consumeIf('(')))
            return failed();
        auto index = number(';');
        if (!index || *index <= 0 || *index >= numberOfASTClasses)
            return failed();
        auto found = static_cast<ASTClass>(*index);
        const ASDLClass& description = descriptionOf(found);
        if (isSum ? description.kind != ASDLClass::Kind::Constructor || description.base != type : found != type)
            return failed();
        // Load() and the like have nothing in them, and nothing else is written as they are.
        if (isSingleton != (isSum && description.fields.empty() && !saysWhereItIs(found)))
            return failed();
        if (!isSingleton)
            m_starts.append(start);
        return found;
    }

    bool positions(ASTClass constructor, Node& where)
    {
        m_attributeStart = 0;
        if (!saysWhereItIs(constructor))
            return true;
        if (!readWhere(where))
            return false;
        if (at('@')) {
            if (constructor != ASTClass::Attribute)
                return fails();
            m_attributeStart = m_position++;
            Node ignored;
            return readWhere(ignored);
        }
        return true;
    }

    unsigned attributeStart() const { return m_attributeStart; }

    void leave(Value, Node& where)
    {
        if (!consumeIf(')')) {
            m_hasFailed = true;
            return;
        }
        where.start = m_starts.takeLast();
        where.end = m_position;
    }

    std::optional<Value> field(Value, ASTClass, const ASDLField& field)
    {
        Value value = next();
        // Only what may be left out may be None. In a list it is for validate() to say.
        if (value.isNone && field.quantifier != ASDLField::Quantifier::Optional) {
            m_hasFailed = true;
            return std::nullopt;
        }
        return value;
    }

    template<typename Function>
    bool forEachElement(Value, ASTClass, const ASDLField&, const Function& function)
    {
        if (!consumeIf('['))
            return fails();
        auto count = this->count();
        if (!count)
            return false;
        for (unsigned i = 0; i < *count; ++i) {
            if (!function(next()))
                return false;
        }
        if (!consumeIf(']'))
            return fails();
        return true;
    }

    const Identifier* identifier(Value value)
    {
        if (value.isNone)
            return nullptr;
        return characters('n');
    }

    Text string(Value)
    {
        bool isBytes = at('y');
        const Identifier* text = characters(isBytes ? 'y' : 's');
        if (isBytes && text && !text->string().is8Bit())
            m_hasFailed = true;
        return { text, isBytes };
    }

    std::optional<int> integer(Value)
    {
        if (!consumeIf('#')) {
            m_hasFailed = true;
            return std::nullopt;
        }
        return number(';');
    }

    Constant* constant(Value)
    {
        if (!canGoDeeper(ASTClass::Constant) || m_position >= m_end) {
            m_hasFailed = true;
            return nullptr;
        }
        auto* result = m_arena.create<Constant>();
        char16_t kind = m_tree[m_position];
        switch (kind) {
        case 'N':
            ++m_position;
            return result;
        case 'T':
            ++m_position;
            result->type = Constant::Type::True;
            return result;
        case 'F':
            ++m_position;
            result->type = Constant::Type::False;
            return result;
        case 'E':
            ++m_position;
            result->type = Constant::Type::Ellipsis;
            return result;
        case 'i': {
            ++m_position;
            result->isNegative = consumeIf('-');
            unsigned start = m_position;
            while (m_position < m_end && isASCIIHexDigit(m_tree[m_position]) && !isASCIIUpper(m_tree[m_position]))
                ++m_position;
            StringView digits = m_tree.substring(start, m_position - start);
            // As it is written, and no other way: one number is written one way.
            if (digits.isEmpty() || (digits.length() > 1 && digits[0] == '0') || !consumeIf(';') || (result->isNegative && digits == "0"_s)) {
                m_hasFailed = true;
                return nullptr;
            }
            if (digits.length() <= 16) {
                result->type = Constant::Type::Integer;
                for (char16_t digit : digits.codeUnits())
                    result->integer = result->integer << 4 | toASCIIHexValue(digit);
                return result;
            }
            result->type = Constant::Type::BigInteger;
            result->radix = 16;
            result->text = identifierFor(digits);
            return result;
        }
        case 'f':
        case 'j':
        case 'c': {
            ++m_position;
            result->type = kind == 'f' ? Constant::Type::Float : kind == 'j' ? Constant::Type::Imaginary : Constant::Type::Complex;
            auto real = bits();
            auto imaginary = kind == 'c' ? bits() : std::optional<double>(0);
            if (!real || !imaginary)
                return nullptr;
            result->real = *real;
            result->imaginary = *imaginary;
            return result;
        }
        case 'u':
        case 'b':
            result->type = kind == 'u' ? Constant::Type::String : Constant::Type::Bytes;
            result->text = characters(kind);
            if (kind == 'b' && result->text && !result->text->string().is8Bit())
                m_hasFailed = true;
            return m_hasFailed ? nullptr : result;
        case 't':
        case 'z': {
            ++m_position;
            result->type = kind == 't' ? Constant::Type::Tuple : Constant::Type::FrozenSet;
            auto count = this->count();
            if (!count)
                return nullptr;
            Vector<Constant*, 8> elements;
            for (unsigned i = 0; i < *count; ++i) {
                Constant* element = constant({ });
                if (!element)
                    return nullptr;
                elements.append(element);
            }
            result->elements = m_arena.copy(elements);
            return result;
        }
        default:
            m_hasFailed = true;
            return nullptr;
        }
    }

private:
    ASTClass failed()
    {
        m_hasFailed = true;
        return ASTClass::AST;
    }

    bool fails()
    {
        m_hasFailed = true;
        return false;
    }

    bool at(char character) const { return m_position < m_end && m_tree[m_position] == character; }

    bool consumeIf(char character)
    {
        if (!at(character))
            return false;
        ++m_position;
        return true;
    }

    // In decimal, and what fits an int.
    std::optional<int> number(char terminator)
    {
        bool isNegative = consumeIf('-');
        unsigned start = m_position;
        int64_t result = 0;
        while (m_position < m_end && isASCIIDigit(m_tree[m_position]) && m_position - start < 10)
            result = result * 10 + (m_tree[m_position++] - '0');
        if (isNegative)
            result = -result;
        if (m_position == start || !consumeIf(terminator) || result > std::numeric_limits<int>::max() || result < std::numeric_limits<int>::min()) {
            m_hasFailed = true;
            return std::nullopt;
        }
        return static_cast<int>(result);
    }

    // How many there are of something, each of which takes up some room.
    std::optional<unsigned> count()
    {
        auto result = number(':');
        if (!result || *result < 0 || static_cast<unsigned>(*result) > m_end - m_position) {
            m_hasFailed = true;
            return std::nullopt;
        }
        return static_cast<unsigned>(*result);
    }

    bool readWhere(Node& where)
    {
        auto line = number(',');
        auto column = number(',');
        auto endLine = number(',');
        auto endColumn = number(';');
        if (!line || !column || !endLine || !endColumn || *line < 0 || *column < 0 || *endLine < 0 || *endColumn < 0)
            return fails();
        where.line = *line;
        where.column = *column;
        where.endLine = *endLine;
        where.endColumn = *endColumn;
        return true;
    }

    std::optional<double> bits()
    {
        if (m_end - m_position < 16) {
            m_hasFailed = true;
            return std::nullopt;
        }
        uint64_t result = 0;
        for (unsigned i = 0; i < 16; ++i) {
            char16_t digit = m_tree[m_position++];
            if (!isASCIIHexDigit(digit) || isASCIIUpper(digit)) {
                m_hasFailed = true;
                return std::nullopt;
            }
            result = result << 4 | toASCIIHexValue(digit);
        }
        return std::bit_cast<double>(result);
    }

    const Identifier* identifierFor(StringView characters)
    {
        if (characters.is8Bit())
            return &m_arena.identifiers().makeIdentifier(m_vm, characters.span8());
        return &m_arena.identifiers().makeIdentifier(m_vm, characters.span16());
    }

    const Identifier* characters(char kind)
    {
        if (!consumeIf(kind)) {
            m_hasFailed = true;
            return nullptr;
        }
        auto length = count();
        if (!length)
            return nullptr;
        const Identifier* result = identifierFor(m_tree.substring(m_position, *length));
        m_position += *length;
        return result;
    }

    VM& m_vm;
    Arena& m_arena;
    StringView m_tree;
    unsigned m_position;
    unsigned m_end;
    Vector<unsigned, 32> m_starts; // Of the nodes that have begun and not ended.
    unsigned m_attributeStart { 0 };
    bool m_hasFailed { false };
};

} // anonymous namespace

String writeSyntaxTree(VM& vm, Module& module)
{
    return Writer(vm).write(module);
}

Module* readSyntaxTree(VM& vm, Arena& arena, StringView tree, Module::Kind kind)
{
    Reader reader(vm, arena, tree, 0, tree.length());
    Module* module = ASTBuilder<Reader>(arena, reader).buildModule(reader.next());
    if (!module || reader.hasFailed() || !reader.isAtEnd() || module->kind != kind || validate(vm, *module))
        return nullptr;
    return module;
}

Statement* readDefinition(VM& vm, Arena& arena, StringView tree, unsigned start, unsigned end)
{
    Reader reader(vm, arena, tree, start, end);
    Statement* statement = ASTBuilder<Reader>(arena, reader).buildStatement(reader.next());
    if (!statement || reader.hasFailed() || !reader.isAtEnd() || validate(vm, *statement))
        return nullptr;
    return statement;
}

Expression* readExpression(VM& vm, Arena& arena, StringView tree, unsigned start, unsigned end)
{
    Reader reader(vm, arena, tree, start, end);
    Expression* expression = ASTBuilder<Reader>(arena, reader).buildExpression(reader.next());
    if (!expression || reader.hasFailed() || !reader.isAtEnd() || validate(vm, *expression))
        return nullptr;
    return expression;
}

// ---- Where a node says that it is

PlaceInSource placeOfNodeInSyntaxTree(StringView tree, unsigned offset)
{
    unsigned position = offset;
    auto number = [&] (char terminator) -> std::optional<unsigned> {
        unsigned start = position;
        uint64_t result = 0;
        while (position < tree.length() && isASCIIDigit(tree[position]) && position - start < 10)
            result = result * 10 + (tree[position++] - '0');
        if (position == start || position >= tree.length() || tree[position++] != terminator || result > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return std::nullopt;
        return static_cast<unsigned>(result);
    };
    if (position >= tree.length())
        return { };
    char16_t first = tree[position++];
    if (first == '(') {
        auto index = number(';');
        if (!index || !*index || *index >= numberOfASTClasses || !saysWhereItIs(static_cast<ASTClass>(*index)))
            return { };
    } else if (first != '@')
        return { };
    auto line = number(',');
    auto column = number(',');
    auto endLine = number(',');
    auto endColumn = number(';');
    if (!line || !column || !endLine || !endColumn)
        return { };
    return { *line, *column, *endLine, *endColumn };
}

LineColumn SyntaxTreeSourceProvider::lineColumnInTextForOffset(unsigned offset)
{
    PlaceInSource place = placeOfNodeInSyntaxTree(source(), offset);
    // From nought, both. A node that is on no line comes out as on line 0 of the document, one being added to this.
    return { place.line - 1, place.column };
}

} } // namespace JSC::Python
