/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAstModule.h"

#include "Identifier.h"
#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyParser.h"
#include "PySet.h"
#include "PyStr.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// From the objects of a program to the tree the compiler reads. (The obj2ast functions of Python/Python-ast.c in CPython,
// which are all of one mould: here the mould is written once, and goes by what the grammar says of each class.)

namespace AstFromObject {

using namespace AST;

// The value of a field.
struct Slot {
    void* pointer { nullptr }; // A node, an identifier or a string. Null: None.
    int number { 0 }; // An int, or which of the constructors of a sum such as operator.
    ConstantValue constant;
    String invalidConstant; // The name of the type of what is no constant, though the field is to hold one. Null: it is one.
    void* items { nullptr }; // A sequence.
    unsigned count { 0 };

    template<typename T> T* as() const { return static_cast<T*>(pointer); }
    Name name() const { return static_cast<Name>(pointer); }
    StringImpl* string() const { return static_cast<StringImpl*>(pointer); }
    template<typename T> Seq<T> seq() const { return { static_cast<T*>(items), count }; }
    template<typename T> T choice() const { return static_cast<T>(number); }
};

class Converter {
public:
    Converter(JSGlobalObject* globalObject, const AstState& state, ParseTree& tree, AstInvalidConstants& invalidConstants)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_vm(m_runtime.vm())
        , m_state(state)
        , m_tree(tree)
        , m_arena(tree.arena)
        , m_invalidConstants(invalidConstants)
    {
    }

    Mod* convert(JSValue object)
    {
        Slot slot;
        if (!convertNode(AstClass::AnyMod, object, slot))
            return nullptr;
        return slot.as<Mod>();
    }

    // Text in which every position of the tree is where it says it is: the code has no other way to tell where it comes from.
    void makeSource(ParserHost& host, const String& filename)
    {
        Vector<uint8_t> text;
        m_tree.lineStarts.clear();
        for (uint32_t width : m_widths) {
            if (!m_tree.lineStarts.isEmpty())
                text.append('\n');
            m_tree.lineStarts.append(text.size());
            text.grow(text.size() + width);
            memset(text.mutableSpan().last(width).data(), ' ', width);
        }
        SyntaxErrorInfo error;
        bool ok = m_tree.source.initialize(text.span(), SourceText::Flag::IgnoreCookie, filename, host, error);
        ASSERT_UNUSED(ok, ok);
        m_tree.positionsAreArbitrary = true;
    }

private:
    static constexpr int maxLines = 1 << 22;
    static constexpr int maxWidth = 1 << 16;
    static constexpr size_t maxTextSize = 1 << 26;

    void notePosition(int line, int column)
    {
        if (line < 1 || line > maxLines || column < 0)
            return;
        uint32_t width = std::min(column, maxWidth);
        size_t index = line - 1;
        size_t growth = index < m_widths.size() ? (width > m_widths[index] ? width - m_widths[index] : 0) : (index + 1 - m_widths.size()) + width;
        if (m_textSize + growth > maxTextSize)
            return;
        m_textSize += growth;
        while (index >= m_widths.size())
            m_widths.append(0);
        m_widths[index] = std::max(m_widths[index], width);
    }

    bool enter(const AstClassInfo& owner)
    {
        if (m_vm.isSafeToRecurseSoft()) [[likely]]
            return true;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded while traversing '"_s, owner.name, "' node"_s);
        return false;
    }

    // ---- What is not a node ----

    Name makeIdentifier(const String& string)
    {
        if (string.is8Bit())
            return &m_tree.identifiers.makeIdentifier(m_vm, string.span8());
        return &m_tree.identifiers.makeIdentifier(m_vm, string.span16());
    }

    bool convertIdentifier(JSValue value, Slot& out)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isNone(value))
            return true;
        if (!isExactStr(value)) {
            raiseTypeError(m_globalObject, scope, "AST identifier must be of type str"_s);
            return false;
        }
        String string = asString(value)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        out.pointer = const_cast<Identifier*>(makeIdentifier(string));
        return true;
    }

    bool convertString(JSValue value, Slot& out)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isExactStr(value)) {
            raiseTypeError(m_globalObject, scope, "AST string must be of type str"_s);
            return false;
        }
        String string = asString(value)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        out.pointer = m_arena.retain(string);
        return true;
    }

    bool convertInt(JSValue value, int& out)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isInt(value)) {
            String text = reprForMessage(m_globalObject, value);
            raiseValueError(m_globalObject, scope, "invalid integer value: "_s, text);
            return false;
        }
        out = toCInt(m_globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    // The name of the type of what is no constant, or of the first such thing in it. Null: it is one. (Or an exception.)
    String convertConstant(JSValue value, ConstantValue& out)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyType* type = m_runtime.typeOf(value);
        if (isNone(value)) {
            out = ConstantValue(ConstantValue::Kind::None);
            return { };
        }
        if (value.isBoolean()) {
            out = ConstantValue(value.asBoolean() ? ConstantValue::Kind::True : ConstantValue::Kind::False);
            return { };
        }
        if (value == m_runtime.ellipsis()) {
            out = ConstantValue(ConstantValue::Kind::Ellipsis);
            return { };
        }
        if (isExactInt(value)) {
            if (auto small = tryInt64(value)) {
                out = ConstantValue(ConstantValue::Kind::Int);
                out.integer = *small;
                return { };
            }
            out = ConstantValue(ConstantValue::Kind::BigInt);
            out.isNegative = intSign(value) < 0;
            JSValue magnitude = intAbsolute(m_globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            String digits = intToString(m_globalObject, magnitude, 16);
            RETURN_IF_EXCEPTION(scope, { });
            ASSERT(digits.is8Bit());
            char* data = static_cast<char*>(m_arena.allocate(digits.length()));
            memcpy(data, digits.span8().data(), digits.length());
            out.radix = 16;
            out.digits = { data, digits.length() };
            return { };
        }
        if (isFloat(value) && type == m_runtime.floatType()) {
            out = ConstantValue(ConstantValue::Kind::Float);
            out.number = floatValue(value);
            return { };
        }
        if (type == m_runtime.complexType()) {
            auto* complex = PyComplex::from(value);
            out = ConstantValue(ConstantValue::Kind::Complex);
            out.complex = { complex->real(), complex->imaginary() };
            return { };
        }
        if (isExactStr(value)) {
            String string = asString(value)->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            out = ConstantValue(ConstantValue::Kind::Str);
            out.string = m_arena.retain(string);
            return { };
        }
        if (type == m_runtime.bytesType()) {
            auto bytes = PyBytes::from(value)->span();
            auto* data = static_cast<uint8_t*>(m_arena.allocate(bytes.size()));
            memcpy(data, bytes.data(), bytes.size());
            out = ConstantValue(ConstantValue::Kind::Bytes);
            out.bytes = { data, bytes.size() };
            return { };
        }
        if (type == m_runtime.tupleType() || type == m_runtime.frozensetType()) {
            if (!m_vm.isSafeToRecurseSoft()) {
                raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded during compilation"_s);
                return { };
            }
            PyTuple* items = type == m_runtime.tupleType() ? PyTuple::from(value) : tupleFromIterable(m_globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            auto* data = static_cast<ConstantValue*>(m_arena.allocate(items->length() * sizeof(ConstantValue)));
            for (unsigned i = 0; i < items->length(); ++i) {
                new (NotNull, &data[i]) ConstantValue();
                String invalid = convertConstant(items->at(i), data[i]);
                RETURN_IF_EXCEPTION(scope, { });
                if (!invalid.isNull())
                    return invalid;
            }
            out = ConstantValue(type == m_runtime.tupleType() ? ConstantValue::Kind::Tuple : ConstantValue::Kind::FrozenSet);
            out.items = { data, items->length() };
            return { };
        }
        return type->name();
    }

    // ---- Fields ----

    bool convertValue(const AstField& field, JSValue value, Slot& out)
    {
        switch (field.type) {
        case AstField::Type::Identifier:
            return convertIdentifier(value, out);
        case AstField::Type::String:
            return convertString(value, out);
        case AstField::Type::Int:
            return convertInt(value, out.number);
        case AstField::Type::Constant: {
            auto scope = DECLARE_THROW_SCOPE(m_vm);
            out.invalidConstant = convertConstant(value, out.constant);
            RETURN_IF_EXCEPTION(scope, false);
            return true;
        }
        case AstField::Type::Node:
            return convertNode(field.node, value, out);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool convertField(const AstClassInfo& owner, const AstField& field, JSString* name, JSValue object, Slot& out)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue value = getAttrIfPresent(m_globalObject, object, name);
        RETURN_IF_EXCEPTION(scope, false);

        if (!field.isSequence) {
            if (!value) {
                if (field.isOptional)
                    return true;
                raiseTypeError(m_globalObject, scope, "required field \""_s, field.name, "\" missing from "_s, owner.name);
                return false;
            }
            if (field.isOptional && isNone(value))
                return true;
            if (!enter(owner))
                return false;
            RELEASE_AND_RETURN(scope, convertValue(field, value, out));
        }

        if (!value)
            return true;
        if (!isList(value)) {
            raiseTypeError(m_globalObject, scope, owner.name, " field \""_s, field.name, "\" must be a list, not a "_s, m_runtime.typeOf(value)->name());
            return false;
        }
        JSArray* list = asList(value);
        unsigned count = listLength(list);
        bool isChoice = field.type == AstField::Type::Node && infoOfAstClass(field.node).isSimpleSum;
        ASSERT(isChoice || field.type == AstField::Type::Node || field.type == AstField::Type::Identifier);
        out.count = count;
        out.items = m_arena.allocate(count * (isChoice ? sizeof(uint8_t) : sizeof(void*)));
        for (unsigned i = 0; i < count; ++i) {
            if (!enter(owner))
                return false;
            Slot item;
            bool ok = convertValue(field, listGet(list, i), item);
            RETURN_IF_EXCEPTION(scope, false);
            if (!ok)
                return false;
            if (listLength(list) != count) {
                raiseRuntimeError(m_globalObject, scope, owner.name, " field \""_s, field.name, "\" changed size during iteration"_s);
                return false;
            }
            if (isChoice)
                static_cast<uint8_t*>(out.items)[i] = static_cast<uint8_t>(item.number);
            else
                static_cast<void**>(out.items)[i] = item.pointer;
        }
        return true;
    }

    bool convertFields(AstClass id, const AstClassInfo& info, JSValue object, Slot* slots)
    {
        for (unsigned i = 0; i < info.fields.size(); ++i) {
            if (!convertField(info, info.fields[i], m_state.field(id, i), object, slots[i]))
                return false;
        }
        return true;
    }

    bool convertAttributes(const AstClassInfo& info, JSValue object, Range& range)
    {
        struct Attribute {
            AstName name;
            ASCIILiteral text;
            int Range::*member;
            int Range::*defaultMember;
        };
        static constexpr Attribute attributes[] = {
            { AstName::Lineno, "lineno"_s, &Range::lineno, nullptr },
            { AstName::ColOffset, "col_offset"_s, &Range::colOffset, nullptr },
            { AstName::EndLineno, "end_lineno"_s, &Range::endLineno, &Range::lineno },
            { AstName::EndColOffset, "end_col_offset"_s, &Range::endColOffset, &Range::colOffset },
        };
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        for (auto& attribute : attributes) {
            bool isOptional = attribute.defaultMember && info.attributes == AstAttributes::OptionalEnd;
            JSValue value = getAttrIfPresent(m_globalObject, object, m_state.name(attribute.name));
            RETURN_IF_EXCEPTION(scope, false);
            if (!value && !isOptional) {
                raiseTypeError(m_globalObject, scope, "required field \""_s, attribute.text, "\" missing from "_s, info.name);
                return false;
            }
            if (isOptional && (!value || isNone(value))) {
                range.*attribute.member = range.*attribute.defaultMember;
                continue;
            }
            bool ok = convertInt(value, range.*attribute.member);
            RETURN_IF_EXCEPTION(scope, false);
            if (!ok)
                return false;
        }
        notePosition(range.lineno, range.colOffset);
        notePosition(range.endLineno, range.endColOffset);
        return true;
    }

    // What the functions that make the nodes insist on.
    bool checkRequiredFields(const AstClassInfo& info, const Slot* slots)
    {
        for (unsigned i = 0; i < info.fields.size(); ++i) {
            auto& field = info.fields[i];
            if (field.isOptional || field.isSequence || field.type == AstField::Type::Int || field.type == AstField::Type::Constant)
                continue;
            if (field.type == AstField::Type::Node && infoOfAstClass(field.node).isSimpleSum)
                continue;
            if (slots[i].pointer)
                continue;
            auto scope = DECLARE_THROW_SCOPE(m_vm);
            raiseValueError(m_globalObject, scope, "field '"_s, field.name, "' is required for "_s, info.name);
            return false;
        }
        return true;
    }

    // ---- Nodes ----

    // The constructor of the sum that the object is an instance of. AST: none (or an exception).
    AstClass classify(AstClass sum, const AstClassInfo& info, JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unsigned first = static_cast<unsigned>(sum) + 1;
        unsigned last = static_cast<unsigned>(info.lastConstructor);
        PyType* type = m_runtime.typeOf(object);
        for (unsigned i = first; i <= last; ++i) {
            if (type == m_state.type(static_cast<AstClass>(i)))
                return static_cast<AstClass>(i);
        }
        for (unsigned i = first; i <= last; ++i) {
            bool matches = isInstance(m_globalObject, object, m_state.type(static_cast<AstClass>(i)));
            RETURN_IF_EXCEPTION(scope, AstClass::AST);
            if (matches)
                return static_cast<AstClass>(i);
        }
        return AstClass::AST;
    }

    bool convertNode(AstClass id, JSValue object, Slot& out)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const AstClassInfo& info = infoOfAstClass(id);
        Slot slots[7];
        Range range;

        if (info.kind == AstClassKind::Product) {
            if (!convertFields(id, info, object, slots))
                return false;
            if (info.attributes != AstAttributes::None && !convertAttributes(info, object, range))
                return false;
            if (failed(scope, checkRequiredFields(info, slots)))
                return false;
            out.pointer = makeProduct(id, slots, range);
            return true;
        }

        ASSERT(info.kind == AstClassKind::Sum);
        if (!info.isSimpleSum) {
            if (isNone(object))
                return true;
            if (info.attributes != AstAttributes::None && !convertAttributes(info, object, range))
                return false;
        }
        AstClass constructor = classify(id, info, object);
        RETURN_IF_EXCEPTION(scope, false);
        if (constructor == AstClass::AST) {
            JSString* text = repr(m_globalObject, object);
            RETURN_IF_EXCEPTION(scope, false);
            String string = text->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            raiseTypeError(m_globalObject, scope, "expected some sort of "_s, info.name, ", but got "_s, string);
            return false;
        }
        if (info.isSimpleSum) {
            out.number = static_cast<int>(constructor) - static_cast<int>(id) - 1;
            return true;
        }

        const AstClassInfo& constructorInfo = infoOfAstClass(constructor);
        if (!convertFields(constructor, constructorInfo, object, slots))
            return false;
        if (failed(scope, checkRequiredFields(constructorInfo, slots)))
            return false;
        const ConstantValue* constant = nullptr;
        switch (id) {
        case AstClass::AnyMod:
            out.pointer = makeMod(constructor, slots);
            break;
        case AstClass::AnyStmt:
            out.pointer = makeStmt(constructor, slots, range);
            break;
        case AstClass::AnyExpr:
            out.pointer = makeExpr(constructor, slots, range, constant);
            break;
        case AstClass::AnyPattern:
            out.pointer = makePattern(constructor, slots, range, constant);
            break;
        case AstClass::AnyExceptHandler: {
            auto* handler = m_arena.create<ExceptHandler>();
            handler->type = slots[0].as<Expr>();
            handler->name = slots[1].name();
            handler->body = slots[2].seq<Stmt*>();
            handler->range = range;
            out.pointer = handler;
            break;
        }
        case AstClass::AnyTypeIgnore: {
            auto* ignore = m_arena.create<TypeIgnore>();
            ignore->lineno = slots[0].number;
            ignore->tag = slots[1].string();
            out.pointer = ignore;
            break;
        }
        case AstClass::AnyTypeParam: {
            auto* parameter = m_arena.create<TypeParam>();
            parameter->kind = constructor == AstClass::TypeVar ? TypeParamKind::TypeVar : constructor == AstClass::ParamSpec ? TypeParamKind::ParamSpec : TypeParamKind::TypeVarTuple;
            parameter->name = slots[0].name();
            parameter->bound = constructor == AstClass::TypeVar ? slots[1].as<Expr>() : nullptr;
            parameter->defaultValue = slots[constructor == AstClass::TypeVar ? 2 : 1].as<Expr>();
            parameter->range = range;
            out.pointer = parameter;
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        for (auto& slot : slots) {
            if (slot.invalidConstant.isNull())
                continue;
            ASSERT(constant);
            m_invalidConstants.add(constant, slot.invalidConstant);
        }
        return true;
    }

    void* makeProduct(AstClass id, const Slot* slots, const Range& range)
    {
        switch (id) {
        case AstClass::Comprehension: {
            auto* node = m_arena.create<Comprehension>();
            node->target = slots[0].as<Expr>();
            node->iter = slots[1].as<Expr>();
            node->ifs = slots[2].seq<Expr*>();
            node->isAsync = slots[3].number;
            return node;
        }
        case AstClass::Arguments: {
            auto* node = m_arena.create<Arguments>();
            node->posonlyargs = slots[0].seq<Arg*>();
            node->args = slots[1].seq<Arg*>();
            node->vararg = slots[2].as<Arg>();
            node->kwonlyargs = slots[3].seq<Arg*>();
            node->kwDefaults = slots[4].seq<Expr*>();
            node->kwarg = slots[5].as<Arg>();
            node->defaults = slots[6].seq<Expr*>();
            return node;
        }
        case AstClass::Arg: {
            auto* node = m_arena.create<Arg>();
            node->arg = slots[0].name();
            node->annotation = slots[1].as<Expr>();
            node->typeComment = slots[2].string();
            node->range = range;
            return node;
        }
        case AstClass::Keyword: {
            auto* node = m_arena.create<Keyword>();
            node->arg = slots[0].name();
            node->value = slots[1].as<Expr>();
            node->range = range;
            return node;
        }
        case AstClass::Alias: {
            auto* node = m_arena.create<Alias>();
            node->name = slots[0].name();
            node->asname = slots[1].name();
            node->range = range;
            return node;
        }
        case AstClass::WithItem: {
            auto* node = m_arena.create<WithItem>();
            node->contextExpr = slots[0].as<Expr>();
            node->optionalVars = slots[1].as<Expr>();
            return node;
        }
        case AstClass::MatchCase: {
            auto* node = m_arena.create<MatchCase>();
            node->pattern = slots[0].as<Pattern>();
            node->guard = slots[1].as<Expr>();
            node->body = slots[2].seq<Stmt*>();
            return node;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Mod* makeMod(AstClass id, const Slot* slots)
    {
        auto* node = m_arena.create<Mod>();
        switch (id) {
        case AstClass::Module:
            node->kind = ModKind::Module;
            node->body = slots[0].seq<Stmt*>();
            node->typeIgnores = slots[1].seq<TypeIgnore*>();
            break;
        case AstClass::Interactive:
            node->kind = ModKind::Interactive;
            node->body = slots[0].seq<Stmt*>();
            break;
        case AstClass::Expression:
            node->kind = ModKind::Expression;
            node->expression = slots[0].as<Expr>();
            break;
        case AstClass::FunctionType:
            node->kind = ModKind::FunctionType;
            node->argtypes = slots[0].seq<Expr*>();
            node->expression = slots[1].as<Expr>();
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return node;
    }

    Stmt* makeStmt(AstClass id, const Slot* slots, const Range& range)
    {
        switch (id) {
        case AstClass::FunctionDef:
        case AstClass::AsyncFunctionDef: {
            auto* node = m_arena.create<FunctionDef>(id == AstClass::AsyncFunctionDef, range);
            node->name = slots[0].name();
            node->args = slots[1].as<Arguments>();
            node->body = slots[2].seq<Stmt*>();
            node->decoratorList = slots[3].seq<Expr*>();
            node->returns = slots[4].as<Expr>();
            node->typeComment = slots[5].string();
            node->typeParams = slots[6].seq<TypeParam*>();
            return node;
        }
        case AstClass::ClassDef: {
            auto* node = m_arena.create<ClassDef>(range);
            node->name = slots[0].name();
            node->bases = slots[1].seq<Expr*>();
            node->keywords = slots[2].seq<Keyword*>();
            node->body = slots[3].seq<Stmt*>();
            node->decoratorList = slots[4].seq<Expr*>();
            node->typeParams = slots[5].seq<TypeParam*>();
            return node;
        }
        case AstClass::Return: {
            auto* node = m_arena.create<Return>(range);
            node->value = slots[0].as<Expr>();
            return node;
        }
        case AstClass::Delete: {
            auto* node = m_arena.create<Delete>(range);
            node->targets = slots[0].seq<Expr*>();
            return node;
        }
        case AstClass::Assign: {
            auto* node = m_arena.create<Assign>(range);
            node->targets = slots[0].seq<Expr*>();
            node->value = slots[1].as<Expr>();
            node->typeComment = slots[2].string();
            return node;
        }
        case AstClass::TypeAlias: {
            auto* node = m_arena.create<TypeAlias>(range);
            node->name = slots[0].as<Expr>();
            node->typeParams = slots[1].seq<TypeParam*>();
            node->value = slots[2].as<Expr>();
            return node;
        }
        case AstClass::AugAssign: {
            auto* node = m_arena.create<AugAssign>(range);
            node->target = slots[0].as<Expr>();
            node->op = slots[1].choice<Operator>();
            node->value = slots[2].as<Expr>();
            return node;
        }
        case AstClass::AnnAssign: {
            auto* node = m_arena.create<AnnAssign>(range);
            node->target = slots[0].as<Expr>();
            node->annotation = slots[1].as<Expr>();
            node->value = slots[2].as<Expr>();
            node->simple = slots[3].number;
            return node;
        }
        case AstClass::For:
        case AstClass::AsyncFor: {
            auto* node = m_arena.create<For>(id == AstClass::AsyncFor, range);
            node->target = slots[0].as<Expr>();
            node->iter = slots[1].as<Expr>();
            node->body = slots[2].seq<Stmt*>();
            node->orelse = slots[3].seq<Stmt*>();
            node->typeComment = slots[4].string();
            return node;
        }
        case AstClass::While: {
            auto* node = m_arena.create<While>(range);
            node->test = slots[0].as<Expr>();
            node->body = slots[1].seq<Stmt*>();
            node->orelse = slots[2].seq<Stmt*>();
            return node;
        }
        case AstClass::If: {
            auto* node = m_arena.create<If>(range);
            node->test = slots[0].as<Expr>();
            node->body = slots[1].seq<Stmt*>();
            node->orelse = slots[2].seq<Stmt*>();
            return node;
        }
        case AstClass::With:
        case AstClass::AsyncWith: {
            auto* node = m_arena.create<With>(id == AstClass::AsyncWith, range);
            node->items = slots[0].seq<WithItem*>();
            node->body = slots[1].seq<Stmt*>();
            node->typeComment = slots[2].string();
            return node;
        }
        case AstClass::Match: {
            auto* node = m_arena.create<Match>(range);
            node->subject = slots[0].as<Expr>();
            node->cases = slots[1].seq<MatchCase*>();
            return node;
        }
        case AstClass::Raise: {
            auto* node = m_arena.create<Raise>(range);
            node->exc = slots[0].as<Expr>();
            node->cause = slots[1].as<Expr>();
            return node;
        }
        case AstClass::Try:
        case AstClass::TryStar: {
            auto* node = m_arena.create<Try>(id == AstClass::TryStar, range);
            node->body = slots[0].seq<Stmt*>();
            node->handlers = slots[1].seq<ExceptHandler*>();
            node->orelse = slots[2].seq<Stmt*>();
            node->finalbody = slots[3].seq<Stmt*>();
            return node;
        }
        case AstClass::Assert: {
            auto* node = m_arena.create<Assert>(range);
            node->test = slots[0].as<Expr>();
            node->msg = slots[1].as<Expr>();
            return node;
        }
        case AstClass::Import: {
            auto* node = m_arena.create<Import>(range);
            node->names = slots[0].seq<Alias*>();
            node->isLazy = slots[1].number;
            return node;
        }
        case AstClass::ImportFrom: {
            auto* node = m_arena.create<ImportFrom>(range);
            node->module = slots[0].name();
            node->names = slots[1].seq<Alias*>();
            node->level = slots[2].number;
            node->isLazy = slots[3].number;
            return node;
        }
        case AstClass::Global: {
            auto* node = m_arena.create<Global>(range);
            node->names = slots[0].seq<Name>();
            return node;
        }
        case AstClass::Nonlocal: {
            auto* node = m_arena.create<Nonlocal>(range);
            node->names = slots[0].seq<Name>();
            return node;
        }
        case AstClass::Expr: {
            auto* node = m_arena.create<ExprStmt>(range);
            node->value = slots[0].as<Expr>();
            return node;
        }
        case AstClass::Pass:
            return m_arena.create<Pass>(range);
        case AstClass::Break:
            return m_arena.create<Break>(range);
        case AstClass::Continue:
            return m_arena.create<Continue>(range);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Expr* makeExpr(AstClass id, const Slot* slots, const Range& range, const ConstantValue*& constant)
    {
        switch (id) {
        case AstClass::BoolOp: {
            auto* node = m_arena.create<BoolOp>(range);
            node->op = slots[0].choice<BoolOperator>();
            node->values = slots[1].seq<Expr*>();
            return node;
        }
        case AstClass::NamedExpr: {
            auto* node = m_arena.create<NamedExpr>(range);
            node->target = slots[0].as<Expr>();
            node->value = slots[1].as<Expr>();
            return node;
        }
        case AstClass::BinOp: {
            auto* node = m_arena.create<BinOp>(range);
            node->left = slots[0].as<Expr>();
            node->op = slots[1].choice<Operator>();
            node->right = slots[2].as<Expr>();
            return node;
        }
        case AstClass::UnaryOp: {
            auto* node = m_arena.create<UnaryOp>(range);
            node->op = slots[0].choice<UnaryKind>();
            node->operand = slots[1].as<Expr>();
            return node;
        }
        case AstClass::Lambda: {
            auto* node = m_arena.create<Lambda>(range);
            node->args = slots[0].as<Arguments>();
            node->body = slots[1].as<Expr>();
            return node;
        }
        case AstClass::IfExp: {
            auto* node = m_arena.create<IfExp>(range);
            node->test = slots[0].as<Expr>();
            node->body = slots[1].as<Expr>();
            node->orelse = slots[2].as<Expr>();
            return node;
        }
        case AstClass::Dict: {
            auto* node = m_arena.create<Dict>(range);
            node->keys = slots[0].seq<Expr*>();
            node->values = slots[1].seq<Expr*>();
            return node;
        }
        case AstClass::Set: {
            auto* node = m_arena.create<Set>(range);
            node->elts = slots[0].seq<Expr*>();
            return node;
        }
        case AstClass::ListComp: {
            auto* node = m_arena.create<ListComp>(range);
            node->elt = slots[0].as<Expr>();
            node->generators = slots[1].seq<Comprehension*>();
            return node;
        }
        case AstClass::SetComp: {
            auto* node = m_arena.create<SetComp>(range);
            node->elt = slots[0].as<Expr>();
            node->generators = slots[1].seq<Comprehension*>();
            return node;
        }
        case AstClass::DictComp: {
            auto* node = m_arena.create<DictComp>(range);
            node->key = slots[0].as<Expr>();
            node->value = slots[1].as<Expr>();
            node->generators = slots[2].seq<Comprehension*>();
            return node;
        }
        case AstClass::GeneratorExp: {
            auto* node = m_arena.create<GeneratorExp>(range);
            node->elt = slots[0].as<Expr>();
            node->generators = slots[1].seq<Comprehension*>();
            return node;
        }
        case AstClass::Await: {
            auto* node = m_arena.create<Await>(range);
            node->value = slots[0].as<Expr>();
            return node;
        }
        case AstClass::Yield: {
            auto* node = m_arena.create<Yield>(range);
            node->value = slots[0].as<Expr>();
            return node;
        }
        case AstClass::YieldFrom: {
            auto* node = m_arena.create<YieldFrom>(range);
            node->value = slots[0].as<Expr>();
            return node;
        }
        case AstClass::Compare: {
            auto* node = m_arena.create<Compare>(range);
            node->left = slots[0].as<Expr>();
            node->ops = slots[1].seq<CompareKind>();
            node->comparators = slots[2].seq<Expr*>();
            return node;
        }
        case AstClass::Call: {
            auto* node = m_arena.create<Call>(range);
            node->func = slots[0].as<Expr>();
            node->args = slots[1].seq<Expr*>();
            node->keywords = slots[2].seq<Keyword*>();
            return node;
        }
        case AstClass::FormattedValue: {
            auto* node = m_arena.create<FormattedValue>(range);
            node->value = slots[0].as<Expr>();
            node->conversion = slots[1].number;
            node->formatSpec = slots[2].as<Expr>();
            return node;
        }
        case AstClass::Interpolation: {
            auto* node = m_arena.create<Interpolation>(range);
            node->value = slots[0].as<Expr>();
            node->str = slots[1].constant;
            node->conversion = slots[2].number;
            node->formatSpec = slots[3].as<Expr>();
            constant = &node->str;
            return node;
        }
        case AstClass::JoinedStr: {
            auto* node = m_arena.create<JoinedStr>(range);
            node->values = slots[0].seq<Expr*>();
            return node;
        }
        case AstClass::TemplateStr: {
            auto* node = m_arena.create<TemplateStr>(range);
            node->values = slots[0].seq<Expr*>();
            return node;
        }
        case AstClass::Constant: {
            auto* node = m_arena.create<Constant>(range);
            node->value = slots[0].constant;
            node->kindIsU = slots[1].string() && equal(slots[1].string(), "u"_s);
            constant = &node->value;
            return node;
        }
        case AstClass::Attribute: {
            auto* node = m_arena.create<Attribute>(range);
            node->value = slots[0].as<Expr>();
            node->attr = slots[1].name();
            node->ctx = slots[2].choice<ExprContext>();
            return node;
        }
        case AstClass::Subscript: {
            auto* node = m_arena.create<Subscript>(range);
            node->value = slots[0].as<Expr>();
            node->slice = slots[1].as<Expr>();
            node->ctx = slots[2].choice<ExprContext>();
            return node;
        }
        case AstClass::Starred: {
            auto* node = m_arena.create<Starred>(range);
            node->value = slots[0].as<Expr>();
            node->ctx = slots[1].choice<ExprContext>();
            return node;
        }
        case AstClass::Name: {
            auto* node = m_arena.create<NameExpr>(range);
            node->id = slots[0].name();
            node->ctx = slots[1].choice<ExprContext>();
            return node;
        }
        case AstClass::List: {
            auto* node = m_arena.create<List>(range);
            node->elts = slots[0].seq<Expr*>();
            node->ctx = slots[1].choice<ExprContext>();
            return node;
        }
        case AstClass::Tuple: {
            auto* node = m_arena.create<Tuple>(range);
            node->elts = slots[0].seq<Expr*>();
            node->ctx = slots[1].choice<ExprContext>();
            return node;
        }
        case AstClass::Slice: {
            auto* node = m_arena.create<Slice>(range);
            node->lower = slots[0].as<Expr>();
            node->upper = slots[1].as<Expr>();
            node->step = slots[2].as<Expr>();
            return node;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Pattern* makePattern(AstClass id, const Slot* slots, const Range& range, const ConstantValue*& constant)
    {
        switch (id) {
        case AstClass::MatchValue: {
            auto* node = m_arena.create<MatchValue>(range);
            node->value = slots[0].as<Expr>();
            return node;
        }
        case AstClass::MatchSingleton: {
            auto* node = m_arena.create<MatchSingleton>(range);
            node->value = slots[0].constant;
            constant = &node->value;
            return node;
        }
        case AstClass::MatchSequence: {
            auto* node = m_arena.create<MatchSequence>(range);
            node->patterns = slots[0].seq<Pattern*>();
            return node;
        }
        case AstClass::MatchMapping: {
            auto* node = m_arena.create<MatchMapping>(range);
            node->keys = slots[0].seq<Expr*>();
            node->patterns = slots[1].seq<Pattern*>();
            node->rest = slots[2].name();
            return node;
        }
        case AstClass::MatchClass: {
            auto* node = m_arena.create<MatchClass>(range);
            node->cls = slots[0].as<Expr>();
            node->patterns = slots[1].seq<Pattern*>();
            node->kwdAttrs = slots[2].seq<Name>();
            node->kwdPatterns = slots[3].seq<Pattern*>();
            return node;
        }
        case AstClass::MatchStar: {
            auto* node = m_arena.create<MatchStar>(range);
            node->name = slots[0].name();
            return node;
        }
        case AstClass::MatchAs: {
            auto* node = m_arena.create<MatchAs>(range);
            node->pattern = slots[0].as<Pattern>();
            node->name = slots[1].name();
            return node;
        }
        case AstClass::MatchOr: {
            auto* node = m_arena.create<MatchOr>(range);
            node->patterns = slots[0].seq<Pattern*>();
            return node;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    VM& m_vm;
    const AstState& m_state;
    ParseTree& m_tree;
    Arena& m_arena;
    AstInvalidConstants& m_invalidConstants;
    Vector<uint32_t> m_widths; // By line, from 0: the last column anything is said to be at.
    size_t m_textSize { 0 };
};

} // namespace AstFromObject

JSValue compileAST(JSGlobalObject* globalObject, JSValue node, const String& filename, ASCIILiteral mode, int flags, int optimize, JSValue module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    AstState state(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    AstClass expected = mode == "eval"_s ? AstClass::Expression : mode == "single"_s ? AstClass::Interactive : mode == "func_type"_s ? AstClass::FunctionType : AstClass::Module;
    bool isExpected = isInstance(globalObject, node, state.type(expected));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isExpected)
        return raiseTypeError(globalObject, scope, "expected "_s, nameOfAstClass(expected), " node, got "_s, typeOf(globalObject, node)->name());

    ParseTree tree;
    AstInvalidConstants invalidConstants;
    AstFromObject::Converter converter(globalObject, state, tree, invalidConstants);
    tree.root = converter.convert(node);
    RETURN_IF_EXCEPTION(scope, { });
    if (!tree.root)
        return { };
    bool isValid = validateTree(globalObject, tree.root, invalidConstants);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isValid)
        return { };

    if (flags & CompileOnlyAST) {
        SyntaxErrorInfo error;
        bool syntaxCheckOnly = (flags & CompileOptimizedAST) == CompileOnlyAST;
        if (!preprocessTree(vm, tree, optimize < 0 ? rt.optimizationLevel : optimize, flags & static_cast<int>(CodeFlag::FutureAnnotations), syntaxCheckOnly, error))
            RELEASE_AND_RETURN(scope, raiseSyntaxErrorInfo(globalObject, error, filename));
        RELEASE_AND_RETURN(scope, astFromTree(globalObject, tree.root));
    }

    auto host = makeParserHost(globalObject, filename, module);
    converter.makeSource(*host, filename);
    RELEASE_AND_RETURN(scope, compileTree(globalObject, tree, *host, filename, flags, optimize));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
