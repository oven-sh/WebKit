/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramVerifier.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTOperationData.h"
#include "AOTPhaseTimes.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringTable.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

std::optional<InternalError> findErrorInStringAsRead(const Program& program, uint32_t index)
{
    const StringTable& strings = program.strings;
    StringID id(index);
    std::span<const uint8_t> bytes = strings[id];
    if (!isCanonicalProgramString(bytes))
        return InternalError { .kind = InternalErrorKind::StringNotCanonical, .count = index };
    BuiltinName name = strings.builtinName(id);
    bool carriesNameThatItDoesNotSpell = name && (name.index() >= program.builtins().textNameCount() || !equalSpans(program.builtins().textOf(name), bytes));
    if (classifyName(bytes) != strings.nameClass(id) || carriesNameThatItDoesNotSpell)
        return InternalError { .kind = InternalErrorKind::NameClassMismatch, .count = index };
    StringID found = strings.find(bytes);
    if (found != id)
        return InternalError { .kind = InternalErrorKind::DuplicateString, .count = std::max(found.index(), index) };
    return std::nullopt;
}

std::optional<InternalError> findErrorInStringsAsRead(const Program& program, const ParallelLoops& loops)
{
    Vector<InternalError> errors = collectInParallel<InternalError>(loops, static_cast<unsigned>(program.strings.size()), [&](unsigned index, Vector<InternalError>& found) {
        if (auto error = findErrorInStringAsRead(program, index))
            found.append(WTF::move(*error));
    });
    if (errors.isEmpty())
        return std::nullopt;
    return WTF::move(errors.first());
}

struct ListOrderError {
    InternalErrorKind kind;
    uint32_t index;
};

template<typename List, typename Projection = std::identity>
std::optional<ListOrderError> findOrderErrorInRecordList(const List& list, Projection projection = { })
{
    for (size_t index = 1; index < list.size(); ++index) {
        auto order = std::invoke(projection, list[index - 1]) <=> std::invoke(projection, list[index]);
        if (std::is_eq(order))
            return ListOrderError { InternalErrorKind::DuplicateListEntry, static_cast<uint32_t>(index) };
        if (std::is_gt(order))
            return ListOrderError { InternalErrorKind::UnsortedList, static_cast<uint32_t>(index) };
    }
    return std::nullopt;
}

std::optional<InternalError> findListOrderErrorOfFunctionRecord(FunctionID id, const Function& function)
{
    std::optional<ListOrderError> error = findOrderErrorInRecordList(function.functionTypes);
    if (!error)
        error = findOrderErrorInRecordList(function.builtinMembers);
    if (!error)
        return std::nullopt;
    return InternalError { .kind = error->kind, .function = id, .count = id.index() };
}

bool canConstruct(const Function& constructor, const Layout& layout)
{
    bool hasClassConstructorFlags = constructor.has(FunctionFlags::HasNewTarget) && !constructor.has(FunctionFlags::HasThis);
    switch (constructor.constructorForm()) {
    case ConstructorForm::Base:
        return !layout.has(LayoutFlags::HasHeritage) && (constructor.kind == FunctionKind::Plain || hasClassConstructorFlags);
    case ConstructorForm::Derived:
        return layout.has(LayoutFlags::HasHeritage) && hasClassConstructorFlags;
    case ConstructorForm::NotConstructor:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isCanonicalTypeWithoutRecord(TypeID id)
{
    if (homeOf(id) == IDHome::Value && (id.bits() >> maximumLandmarkShift & 7) >= maximumLandmarks.size())
        return false;
    uint16_t primitives = primitivesInID(id);
    Range integers = integersInID(id);
    if (primitives & Primitive::Integer && integers.isEmpty())
        return false;
    return tryMakeTypeOfPrimitives(primitives, integers) == id;
}

class RecordReferenceChecker {
public:
    explicit RecordReferenceChecker(const Program& program)
        : m_program(program)
        , m_numberOfPropertyKeys(program.types.numberOfPropertyKeys())
        , m_numberOfTypes(program.types.numberOfTypes())
    {
    }

    bool hasInvalidReference() const { return m_hasInvalidReference; }

    void operator()(StringID id) { check(id, m_program.strings.size()); }
    void operator()(PropertyKeyID id) { check(id, m_numberOfPropertyKeys); }
    void operator()(FunctionID id) { check(id, m_program.functions.size()); }
    void operator()(LayoutID id) { check(id, m_program.layouts.size()); }
    void operator()(VariableID id) { check(id, m_program.variables.size()); }
    void operator()(ModuleID id) { check(id, m_program.modules.size()); }
    void operator()(ModuleFailureID id) { check(id, m_program.moduleFailures.size()); }
    void operator()(TypeSiteID id) { check(id, m_program.typeSites.size()); }
    void operator()(EvalScopeID id) { check(id, m_program.evalScopes.size()); }
    void operator()(DataID id) { check(id, m_program.data.size()); }
    void operator()(ExpectedResultID id) { check(id, m_program.expectedResults.size()); }
    void operator()(ObjectID id) { check(id, m_program.types.objects().size()); }
    void operator()(LayoutSetID id) { checkSet<LayoutID>(id); }

    void operator()(TypeID id)
    {
        switch (homeOf(id)) {
        case IDHome::Value:
        case IDHome::SmallRange:
            if (!isCanonicalTypeWithoutRecord(id))
                m_hasInvalidReference = true;
            return;
        case IDHome::Program:
            if (positionOf(id) >= m_numberOfTypes)
                m_hasInvalidReference = true;
            return;
        case IDHome::Thread:
            m_hasInvalidReference = true;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    template<typename Tag>
    void operator()(ID<Tag>)
    {
        m_hasInvalidReference = true;
    }

    template<typename Tag, typename Storage>
    void operator()(BuiltinTableID<Tag, Storage> id)
    {
        if (!m_program.builtins().contains(id))
            m_hasInvalidReference = true;
    }

    template<typename IDType>
    void checkIfSet(IDType id)
    {
        if (id)
            (*this)(id);
    }

    template<typename IDType>
    void checkRequired(IDType id)
    {
        if (id)
            (*this)(id);
        else
            m_hasInvalidReference = true;
    }

    template<typename MemberID, typename SetID>
    void checkSet(SetID set)
    {
        if (!set)
            return;
        switch (homeOf(set)) {
        case IDHome::Value:
            (*this)(MemberID(positionOf(set)));
            return;
        case IDHome::Program:
            if (auto members = m_program.types.at(set); !members.isEmpty())
                (*this)(members.last());
            return;
        case IDHome::SmallRange:
        case IDHome::Thread:
            m_hasInvalidReference = true;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE void checkClassParts(ClassPartsID parts)
    {
        if (!parts)
            return;
        switch (homeOf(parts)) {
        case IDHome::Value:
            (*this)(BuiltinClassID(positionOf(parts)));
            return;
        case IDHome::Program:
            for (const ClassPart& part : m_program.types.at(parts)) {
                (*this)(part.builtinClass);
                for (TypeID contents : part.writtenContents)
                    checkIfSet(contents);
            }
            return;
        case IDHome::SmallRange:
        case IDHome::Thread:
            m_hasInvalidReference = true;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    template<typename IDType>
    void check(IDType id, size_t size)
    {
        if (id.index() >= size)
            m_hasInvalidReference = true;
    }

    const Program& m_program;
    size_t m_numberOfPropertyKeys;
    uint32_t m_numberOfTypes;
    bool m_hasInvalidReference { false };
};

struct LayoutRecordWithOwnFieldsAlone {
    StringID name;
    ModuleID module;
    LayoutID base;
    FunctionID constructor;
    std::span<const Field> fields;
    std::span<const StaticField> staticFields;
    std::span<const LayoutID> supertypes;
    ModuleID namespaceOf;
};

template<typename Visitor>
void walkReferences(const LayoutRecordWithOwnFieldsAlone& subject, Visitor& visitor)
{
    walkLayoutRecord(subject, visitor);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const FunctionRecord& function)
{
    checker.checkRequired(function.module);
    for (FunctionID functionType : function.functionTypes)
        checker.checkRequired(functionType);
    for (BuiltinID member : function.builtinMembers)
        checker.checkRequired(member);
    for (StringID specifier : function.requestedOriginals)
        checker.checkRequired(specifier);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const TypeSite& site)
{
    checker.checkRequired(site.module);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const ExpectedResult& expected)
{
    checker.checkRequired(expected.type);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const Module& module)
{
    for (StringID name : module.otherNames)
        checker.checkRequired(name);
    for (StringID name : module.namesOfStatements)
        checker.checkRequired(name);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const ReplaceableBuiltin& replaceable)
{
    checker.checkRequired(replaceable.member);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const BuiltinUsedAtStart& used)
{
    checker.checkRequired(used.builtin);
}

void checkIDsThatRecordWalkerSkips(RecordReferenceChecker& checker, const OpenedName& opened)
{
    checker.checkRequired(opened.holder);
    checker.checkRequired(opened.name);
}

template<typename Record>
void checkIDsThatRecordWalkerSkips(RecordReferenceChecker&, const Record&)
{
}

Vector<uint64_t> markNumbersByWord(size_t count, NOESCAPE const Invocable<bool(uint32_t)> auto& matches)
{
    Vector<uint64_t> words(FillWith { }, (count + 63) / 64, 0ull);
    for (uint32_t number = 0; number < count; ++number) {
        if (matches(number))
            words[number / 64] |= 1ull << number % 64;
    }
    return words;
}

class AnswersForMembersOfVerifiedTypes {
public:
    explicit AnswersForMembersOfVerifiedTypes(const Program& program)
        : environments(markNumbersByWord(program.layouts.size(), [&](uint32_t layout) {
            return program.at(LayoutID(layout)).isEnvironment();
        }))
        , m_objects(program.types.objects())
    {
        m_traitsSharedBy64Objects.fill(notAsked, (m_objects.size() + 63) / 64);
    }

    std::optional<OptionSet<ObjectTrait>> traitsSharedBy64Objects(uint32_t word) const
    {
        uint16_t known = AtomicRef(m_traitsSharedBy64Objects[word]).load(std::memory_order_relaxed);
        if (known == notAsked) {
            OptionSet<ObjectTrait> traits = m_objects.at(ObjectID(word * 64)).traits;
            bool areAlike = !traits.containsAny({ ObjectTrait::HasLayout, ObjectTrait::HasClass });
            for (uint32_t object = word * 64 + 1, end = std::min(m_objects.size(), word * 64 + 64); areAlike && object < end; ++object)
                areAlike = m_objects.at(ObjectID(object)).traits == traits;
            known = areAlike ? traits.toRaw() : differ;
            AtomicRef(m_traitsSharedBy64Objects[word]).store(known, std::memory_order_relaxed);
        }
        if (known == differ)
            return std::nullopt;
        return OptionSet<ObjectTrait>::fromRaw(known);
    }

    const Vector<uint64_t> environments;

private:
    static constexpr uint16_t notAsked = 0;
    static constexpr uint16_t differ = UINT16_MAX;

    const ObjectTable& m_objects;
    mutable Vector<uint16_t> m_traitsSharedBy64Objects;
};

bool hasValidReferences(const Program& program, const Type& type, RecordReferenceChecker checker)
{
    checker.checkSet<StringID>(type.stringLiterals);
    checker.checkSet<PropertyKeyID>(type.symbolLiterals);
    checker.checkSet<ObjectID>(type.objects);
    checker.checkSet<LayoutID>(type.unnamedLayouts);
    checker.checkSet<LayoutID>(type.layouts);
    checker.checkClassParts(type.unnamedClasses);
    checker.checkClassParts(type.classes);
    checker.checkIfSet(type.environment);
    if (checker.hasInvalidReference())
        return false;
    if (program.types.at(type.objects).contains(ObjectTable::unknown()))
        return false;
    return program.types.at(type.symbolLiterals).allOf([&](PropertyKeyID symbol) {
        return isSymbolKind(program.at(symbol).kind);
    });
}

bool hasCanonicalTypeParts(const Type& type)
{
    if (type.primitives & ~Primitive::Defined)
        return false;
    bool hasIntegers = type.primitives & Primitive::Integer;
    bool hasCanonicalRange = hasIntegers ? !type.integers.isEmpty() && type.integers.isSubsetOf(Range::full()) : type.integers == Range::empty();
    if (!hasCanonicalRange)
        return false;
    if (!!(type.primitives & Primitive::LiteralString) != !!type.stringLiterals || (type.primitives & Primitive::AnyString) == Primitive::AnyString)
        return false;
    if (!!(type.primitives & Primitive::LiteralSymbol) != !!type.symbolLiterals || (type.primitives & Primitive::AnySymbol) == Primitive::AnySymbol)
        return false;
    if (type.anyObject && (!type.anyFunction || type.unnamedLayouts || type.unnamedClasses))
        return false;
    if (!type.seeds.isEmpty() && !type.hasUnknown())
        return false;
    if (!type.isJavaScriptValue())
        return !(type.opaque && type.environment) && !type.hasObjects() && !(type.primitives & ~(Primitive::Absent | Primitive::Unspecified));
    return type.hasObjects() || !tryMakeTypeOfPrimitives(type.primitives, type.integers);
}

template<typename SetID>
bool isCanonicalSetOfVerifiedType(const Program& program, SetID set)
{
    return !liesInProgram(set) || program.types.at(set).size() >= 2;
}

std::span<const ClassPart> classPartsOfVerifiedType(const Program& program, ClassPartsID parts)
{
    return parts ? program.types.at(parts) : std::span<const ClassPart>();
}

bool hasCanonicalClassPartsOfVerifiedType(const Program& program, ClassPartsID id)
{
    if (!liesInProgram(id))
        return true;
    std::span<const ClassPart> parts = program.types.at(id);
    if (parts.empty() || (parts.size() == 1 && !parts[0].hasWrittenContents()))
        return false;
    return std::ranges::adjacent_find(parts, std::ranges::greater_equal { }, &ClassPart::builtinClass) == parts.end();
}

bool hasCanonicalListsOfVerifiedType(const Program& program, const Type& type)
{
    return isCanonicalSetOfVerifiedType(program, type.stringLiterals)
        && isCanonicalSetOfVerifiedType(program, type.symbolLiterals)
        && isCanonicalSetOfVerifiedType(program, type.objects)
        && isCanonicalSetOfVerifiedType(program, type.unnamedLayouts)
        && isCanonicalSetOfVerifiedType(program, type.layouts)
        && hasCanonicalClassPartsOfVerifiedType(program, type.unnamedClasses)
        && hasCanonicalClassPartsOfVerifiedType(program, type.classes);
}

bool derivedPartsCoverObjectsOfType(const Program& program, const Type& type, const AnswersForMembersOfVerifiedTypes& answers)
{
    SetView<LayoutID, LayoutSetID> layouts = program.at(type.layouts);
    std::span<const ClassPart> classes = classPartsOfVerifiedType(program, type.classes);
    auto listsClass = [&](BuiltinClassID builtinClass) {
        return std::ranges::binary_search(classes, builtinClass, { }, &ClassPart::builtinClass);
    };

    if (!isSubset(program.at(type.unnamedLayouts).untyped(), layouts.untyped()))
        return false;
    for (const ClassPart& part : classPartsOfVerifiedType(program, type.unnamedClasses)) {
        if (!listsClass(part.builtinClass))
            return false;
    }

    OptionSet<ObjectTrait> traits;
    auto isCovered = [&](ObjectID object) {
        const ObjectRecord& record = program.types.objects().at(object);
        traits.add(record.traits);
        if (record.has(ObjectTrait::HasLayout) && !layouts.contains(record.layout))
            return false;
        return !record.has(ObjectTrait::HasClass) || listsClass(record.builtinClass);
    };
    SetView<ObjectID, ObjectSetID> named = program.types.at(type.objects);
    bool coversNamedObjects = named.isList() ? named.allOf(isCovered) : !named.untyped().anyWordOf([&](uint32_t word, uint64_t members) {
        if (std::optional<OptionSet<ObjectTrait>> shared = answers.traitsSharedBy64Objects(word)) {
            traits.add(*shared);
            return false;
        }
        for (; members; members &= members - 1) {
            if (!isCovered(ObjectID(word * 64 + static_cast<uint32_t>(std::countr_zero(members)))))
                return true;
        }
        return false;
    });
    return coversNamedObjects && traits == type.traitsOfNamed;
}

static bool namesBuiltinConstantAmongObjects(const Program& program, TypeID id)
{
    const ObjectTable& objects = program.types.objects();
    auto isBuiltinConstant = [&](ObjectID object) {
        const ObjectRecord& record = objects.at(object);
        return record.kind == ObjectKind::Builtin && program.builtins().at(BuiltinID(record.payload)).kind == BuiltinKind::Constant;
    };
    SetView<ObjectID, ObjectSetID> named = program.types.at(program.at(id).objects);
    return named.anyOfWithin(objects.firstOf(ObjectKind::Builtin), objects.endOf(ObjectKind::Builtin), isBuiltinConstant);
}

std::optional<InternalError> findTypeError(const Program& program, uint32_t position, RecordReferenceChecker checker, const AnswersForMembersOfVerifiedTypes& answers)
{
    Type type = program.at(idAt<TypeID>(IDHome::Program, position));
    if (!hasValidReferences(program, type, checker))
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .count = position };

    LayoutID environmentAmongObjects;
    program.at(type.layouts).untyped().anyWordOf([&](uint32_t word, uint64_t members) {
        uint64_t environments = members & answers.environments[word];
        if (environments)
            environmentAmongObjects = LayoutID(word * 64 + static_cast<uint32_t>(std::countr_zero(environments)));
        return !!environments;
    });
    if (environmentAmongObjects)
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = environmentAmongObjects, .count = position };
    if (type.environment && !program.at(type.environment).isEnvironment())
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = type.environment, .count = position };

    if (!hasCanonicalTypeParts(type) || !hasCanonicalListsOfVerifiedType(program, type) || !derivedPartsCoverObjectsOfType(program, type, answers))
        return InternalError { .kind = InternalErrorKind::InvalidType, .count = position };
    return std::nullopt;
}

bool isValidPosition(const Program& program, const DeclaredType& declared)
{
    if (!declared.written())
        return false;
    if (!declared.has(PositionFlags::IsInt32Promise))
        return true;
    Type type = program.at(declared.written());
    return !declared.isExpected() && !(type.primitives & Primitive::Double) && type.integers.isSubsetOf(Range::int32());
}

bool canOutliveActivation(const Program& program, const DeclaredType& declared)
{
    std::optional<OpaqueKind> opaque = program.at(declared.written()).opaque;
    return !opaque || !isFunctionLocal(*opaque);
}

bool listsFieldsOnce(const Layout& layout, const Vector<uint32_t>& order)
{
    BitVector isListed(layout.fields.size());
    for (uint32_t field : order) {
        if (field >= layout.fields.size() || isListed.set(field))
            return false;
    }
    return true;
}

std::optional<InternalError> findLayoutError(const Program& program, uint64_t maxFields, LayoutID id, const Layout& layout)
{
    if (auto error = findOrderErrorInRecordList(layout.supertypes))
        return InternalError { .kind = error->kind, .layout = id, .count = id.index() };
    bool hasValidBuiltinLayout = layout.kind == LayoutKind::BuiltinResult ? program.builtins().contains(layout.builtinLayout) : !layout.builtinLayout;
    if (!hasValidBuiltinLayout
        || !!layout.namespaceOf != (layout.kind == LayoutKind::ModuleNamespace)
        || layout.ownFieldCount > layout.fields.size()
        || layout.supertypes.contains(LayoutID())
        || !listsFieldsOnce(layout, layout.declaredOrder)
        || (layout.constructor && layout.kind != LayoutKind::Instance))
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id };
    if (isObjectLayoutOf(layout.kind) && layout.fields.size() > maxFields)
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .limit = LimitID::FieldsInKeyOrder, .count = static_cast<uint32_t>(layout.fields.size()) };
    return std::nullopt;
}

bool mayHaveUnnamedFields(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return true;
    case LayoutKind::BuiltinResult:
        return program.builtins().contains(layout.builtinLayout) && program.builtins().at(layout.builtinLayout).isTuple;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasValidOrigin(const Program& program, const Layout& layout, uint32_t index)
{
    const Field& field = layout.fields[index];
    bool isInherited = field.flags.contains(FieldFlags::IsInherited);
    if (!field.originLayout)
        return !field.originField && !isInherited;

    const Layout& origin = program.at(field.originLayout);
    if (!field.originField || *field.originField >= origin.fields.size())
        return false;
    if (isInherited != (index >= layout.ownFieldCount))
        return false;
    return !isInherited || !origin.fields[*field.originField].flags.contains(FieldFlags::IsInherited);
}

bool hasValidKey(const Program& program, const Layout& layout, uint32_t index, bool mayBeUnnamed)
{
    const Field& field = layout.fields[index];
    if (field.computedKey) {
        const Layout& declaringLayout = field.flags.contains(FieldFlags::IsInherited) ? program.at(field.originLayout) : layout;
        return !field.key && *field.computedKey < declaringLayout.computedKeyCount;
    }
    if (!field.key)
        return mayBeUnnamed || field.flags.contains(FieldFlags::IsHidden);
    if (layout.findField(field.key) != index)
        return false;
    return layout.kind == LayoutKind::ModuleNamespace || !program.isIndexLike(field.key);
}

bool isValidField(const Program& program, const Layout& layout, uint32_t index, bool mayBeUnnamed)
{
    const Field& field = layout.fields[index];
    if (!hasValidOrigin(program, layout, index) || !hasValidKey(program, layout, index, mayBeUnnamed))
        return false;
    bool hasPrivateName = field.key && program.at(field.key).kind == PropertyKeyKind::PrivateName;
    if (field.flags.contains(FieldFlags::IsPrivate) != hasPrivateName)
        return false;
    return !field.flags.contains(FieldFlags::IsBrand) || field.flags.containsAll({ FieldFlags::IsPrivate, FieldFlags::IsHidden });
}

std::optional<InternalError> findFieldError(const Program& program, LayoutID id, const Layout& layout)
{
    bool mayBeUnnamed = mayHaveUnnamedFields(program, layout);
    auto numberOfFieldsAsked = static_cast<uint32_t>(layout.fields.size());
    if (LayoutID base = baseWhoseFieldListIsCopiedWhole(program, id); base && program.at(base).kind == layout.kind && mayHaveUnnamedFields(program, program.at(base)) == mayBeUnnamed)
        numberOfFieldsAsked = layout.ownFieldCount;
    for (uint32_t index = 0; index < numberOfFieldsAsked; ++index) {
        if (!isValidField(program, layout, index, mayBeUnnamed))
            return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .count = index };
    }
    return std::nullopt;
}

} // anonymous namespace

ALWAYS_INLINE_INTO_OTHER_FILES bool isValidPropertyKey(const PropertyKey& key)
{
    bool hasCreationSite = key.module || key.callNumber;
    switch (key.kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::RegisteredSymbol:
        return key.text && !key.owner && !key.symbol && !hasCreationSite;
    case PropertyKeyKind::PrivateName:
        return key.text && key.owner && !key.symbol && !hasCreationSite;
    case PropertyKeyKind::WellKnownSymbol:
        return key.symbol && !key.text && !key.owner && !hasCreationSite;
    case PropertyKeyKind::CreatedSymbol:
        return key.module && key.callNumber && !key.owner && !key.symbol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProgramVerifier::ProgramVerifier(const Program& program, BuildContext& build)
    : m_program(program)
    , m_build(build)
{
}

bool ProgramVerifier::run()
{
    StepTimes steps(m_build, "ReadAndVerify: verify records"_s);
    if (auto error = findErrorInStringsAsRead(m_program, m_build.parallelLoops())) {
        fail(WTF::move(*error));
        return false;
    }
    steps.didFinish("strings"_s);

    struct Group {
        void (ProgramVerifier::*verify)();
        ASCIILiteral name;
    };
    static constexpr Group groups[] = {
        { &ProgramVerifier::verifyIDs, "ids"_s },
        { &ProgramVerifier::verifyTypes, "types"_s },
        { &ProgramVerifier::verifyPropertyKeys, "property keys"_s },
        { &ProgramVerifier::verifyDeclaredPositions, "declared positions"_s },
        { &ProgramVerifier::verifyLayouts, "layouts"_s },
        { &ProgramVerifier::verifyClasses, "classes"_s },
        { &ProgramVerifier::verifyVariables, "variables"_s },
        { &ProgramVerifier::verifyModules, "modules"_s },
        { &ProgramVerifier::verifyModuleGraph, "module graph"_s },
        { &ProgramVerifier::verifyEntries, "entries"_s },
        { &ProgramVerifier::verifyData, "data"_s },
        { &ProgramVerifier::verifySmallRecords, "small records"_s },
        { &ProgramVerifier::verifyFunctionHeaders, "function headers"_s },
    };
    for (const Group& group : groups) {
        (this->*group.verify)();
        if (hasFailed())
            return false;
        steps.didFinish(group.name);
    }
    return true;
}

void ProgramVerifier::fail(InternalError&& error)
{
    m_build.diagnostics().add(error);
    m_hasFailed = true;
}

void ProgramVerifier::verifyIDs()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    RecordReferenceChecker checkerOfProgram(m_program);
    auto verifyEach = [&](size_t count, const auto& recordAt, const auto& createError) {
        Vector<uint32_t> invalidRecords = findIndicesInParallel(loops, count, [&](unsigned index) {
            const auto& record = recordAt(index);
            RecordReferenceChecker checker = checkerOfProgram;
            forEachReference(record, checker);
            checkIDsThatRecordWalkerSkips(checker, record);
            return checker.hasInvalidReference();
        });
        for (uint32_t index : invalidRecords)
            fail(createError(index));
    };
    auto inRecord = [](uint32_t index) {
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .count = index };
    };
    auto inLayout = [](uint32_t index) {
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .layout = LayoutID(index), .count = index };
    };
    auto verifyList = [&](const auto& list) {
        verifyEach(list.size(), [&](unsigned index) -> const auto& { return list[index]; }, inRecord);
    };

    const TypeTable& types = m_program.types;
    std::span<const TypeID> typesOfWireIndexes = types.typesOfWireIndexes();
    Vector<uint32_t> invalidTypes = findIndicesInParallel(loops, static_cast<unsigned>(typesOfWireIndexes.size()), [&](unsigned index) {
        RecordReferenceChecker checker = checkerOfProgram;
        checker.checkRequired(typesOfWireIndexes[index]);
        return checker.hasInvalidReference() || namesBuiltinConstantAmongObjects(m_program, typesOfWireIndexes[index]);
    });
    for (uint32_t index : invalidTypes)
        fail(inRecord(index));

    Vector<uint32_t> invalidLayouts = findIndicesInParallel(loops, m_program.layouts.size(), [&](unsigned index) {
        const LayoutRecord& record = m_program.layouts[index];
        RecordReferenceChecker checker = checkerOfProgram;
        if (baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index))) {
            const LayoutRecordWithOwnFieldsAlone own { record.name, record.module, record.base, record.constructor, record.fields.span().first(record.ownFieldCount), record.staticFields.span(), record.supertypes.span(), record.namespaceOf };
            walkWithoutRoles(own, checker);
        } else
            forEachReference(record, checker);
        return checker.hasInvalidReference();
    });
    for (uint32_t index : invalidLayouts)
        fail(inLayout(index));
    verifyEach(m_program.functions.size(), [&](unsigned index) -> const FunctionRecord& { return m_program.functions[index]; }, [](uint32_t index) {
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .function = FunctionID(index), .count = index };
    });
    verifyList(m_program.typeSites);
    verifyList(m_program.expectedResults);
    verifyList(m_program.variables);
    verifyList(m_program.modules);
    verifyList(m_program.moduleFailures);
    verifyList(m_program.ambiguousExports);
    verifyList(m_program.sourceStamps);
    verifyList(m_program.declaredRecords);
    verifyList(m_program.evalScopes);
    verifyList(m_program.data);
    verifyList(m_program.entries);
    verifyList(m_program.runtimeFunctions);
    verifyList(m_program.webAssemblyModules);
    verifyList(m_program.replaceableBuiltins);
    verifyList(m_program.openedNames);
    verifyList(m_program.builtinsUsedAtStart);
    verifyEach(types.numberOfPropertyKeys(), [&](unsigned index) -> const PropertyKey& { return types.at(PropertyKeyID(index)); }, inRecord);

    Vector<uint32_t> layoutsWithInvalidMembers = findIndicesInParallel(loops, m_program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!m_program.hasDeclaredMembers(id))
            return false;
        RecordReferenceChecker checker = checkerOfProgram;
        checker(m_program.declaredMembersOf(id));
        return checker.hasInvalidReference();
    });
    for (uint32_t index : layoutsWithInvalidMembers)
        fail(inLayout(index));

    Vector<InternalError> orderErrors = collectInParallel<InternalError>(loops, m_program.functions.size(), [&](unsigned index, Vector<InternalError>& found) {
        if (auto error = findListOrderErrorOfFunctionRecord(FunctionID(index), m_program.functions[index]))
            found.append(WTF::move(*error));
    });
    for (InternalError& error : orderErrors)
        fail(WTF::move(error));
    if (auto error = findOrderErrorInRecordList(m_program.replaceableBuiltins, &ReplaceableBuiltin::member))
        fail({ .kind = error->kind, .count = error->index });
}

void ProgramVerifier::verifyTypes()
{
    RecordReferenceChecker checkerOfProgram(m_program);
    AnswersForMembersOfVerifiedTypes answers(m_program);
    Vector<InternalError> errors = collectInParallel<InternalError>(m_build.parallelLoops(), m_program.types.numberOfTypes(), [&](unsigned position, Vector<InternalError>& found) {
        if (auto error = findTypeError(m_program, position, checkerOfProgram, answers))
            found.append(WTF::move(*error));
    });
    for (InternalError& error : errors)
        fail(WTF::move(error));
}

void ProgramVerifier::verifyPropertyKeys()
{
    Vector<uint32_t> invalidKeys = findIndicesInParallel(m_build.parallelLoops(), m_program.types.numberOfPropertyKeys(), [&](unsigned index) {
        const PropertyKey& key = m_program.at(PropertyKeyID(index));
        return !isValidPropertyKey(key) || (key.kind == PropertyKeyKind::PrivateName && m_program.at(key.owner).isEnvironment());
    });
    for (uint32_t index : invalidKeys)
        fail({ .kind = InternalErrorKind::InvalidPropertyKey, .count = index });
}

void ProgramVerifier::verifyDeclaredPositions()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    auto isValid = [&](const DeclaredType& declared) {
        return isValidPosition(m_program, declared);
    };

    Vector<uint32_t> invalidLayouts = findIndicesInParallel(loops, m_program.layouts.size(), [&](unsigned index) {
        const Layout& layout = m_program.layouts[index];
        return !std::ranges::all_of(layout.fields.span().first(baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index)) ? layout.ownFieldCount : layout.fields.size()), [&](const Field& field) {
            return isValid(field.declared) && canOutliveActivation(m_program, field.declared);
        });
    });
    for (uint32_t index : invalidLayouts)
        fail({ .kind = InternalErrorKind::InvalidLayout, .layout = LayoutID(index) });

    Vector<uint32_t> invalidVariables = findIndicesInParallel(loops, m_program.variables.size(), [&](unsigned index) {
        const DeclaredType& declared = m_program.variables[index].declared;
        return !isValid(declared) || !canOutliveActivation(m_program, declared);
    });
    for (uint32_t index : invalidVariables)
        fail({ .kind = InternalErrorKind::InvalidVariable, .count = index });

    Vector<uint32_t> invalidFunctions = findIndicesInParallel(loops, m_program.functions.size(), [&](unsigned index) {
        const Function& function = m_program.functions[index];
        return !std::ranges::all_of(function.captures, isValid) || !isValid(function.thisType)
            || !std::ranges::all_of(function.parameters, isValid) || !std::ranges::all_of(function.results, isValid);
    });
    for (uint32_t index : invalidFunctions)
        fail({ .kind = InternalErrorKind::EntryParametersMismatch, .function = FunctionID(index) });
}

void ProgramVerifier::verifyLayouts()
{
    uint64_t maxFields = m_build.limit(LimitID::FieldsInKeyOrder);
    Vector<uint32_t> invalidLayouts = findIndicesInParallel(m_build.parallelLoops(), m_program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        const Layout& layout = m_program.layouts[index];
        return findLayoutError(m_program, maxFields, id, layout) || findFieldError(m_program, id, layout);
    });
    for (uint32_t index : invalidLayouts) {
        LayoutID id(index);
        const Layout& layout = m_program.layouts[index];
        verifyLayout(id, layout);
        verifyFields(id, layout);
    }
}

void ProgramVerifier::verifyLayout(LayoutID id, const Layout& layout)
{
    if (auto error = findLayoutError(m_program, m_build.limit(LimitID::FieldsInKeyOrder), id, layout))
        fail(WTF::move(*error));
}

void ProgramVerifier::verifyFields(LayoutID id, const Layout& layout)
{
    if (auto error = findFieldError(m_program, id, layout))
        fail(WTF::move(*error));
}

void ProgramVerifier::verifyClasses()
{
    Vector<uint32_t> invalidClasses = findIndicesInParallel(m_build.parallelLoops(), m_program.layouts.size(), [&](unsigned index) {
        return !isValidClass(LayoutID(index), m_program.layouts[index]);
    });
    for (uint32_t index : invalidClasses)
        fail({ .kind = InternalErrorKind::InvalidClass, .layout = LayoutID(index) });
    BitVector rowsThatEnd = answerAlongRows(m_program.layouts.size(), false, [&](LayoutID id) -> StepAlongRow {
        LayoutID base = m_program.at(id).base;
        if (!base)
            return { true, { } };
        return { std::nullopt, base };
    });
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        if (!rowsThatEnd.quickGet(index))
            fail({ .kind = InternalErrorKind::BaseChainCycle, .layout = LayoutID(index) });
    }
}

bool ProgramVerifier::isValidClass(LayoutID id, const Layout& layout) const
{
    if (layout.base && m_program.at(layout.base).kind != LayoutKind::Instance)
        return false;
    if (layout.kind != LayoutKind::Instance)
        return !layout.constructor && layout.staticFields.isEmpty() && !layout.has(LayoutFlags::HasHeritage);
    if (layout.constructor) {
        const Function& constructor = m_program.at(layout.constructor);
        if (constructor.constructedClass != id || !canConstruct(constructor, layout))
            return false;
    }
    return std::ranges::all_of(layout.staticFields, [&](const StaticField& field) {
        return field.variable && m_program.at(field.variable).owner == id;
    });
}

BitVector variablesThatRunTimeMayFill(const Program& program)
{
    BitVector found(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            found.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
        add(module.exportsVariable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (isAssignableBinding(binding.kind) || binding.kind == EvalBindingKind::This)
                add(binding.variable);
        }
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.flags.containsAny({ VariableFlags::HoldsMock, VariableFlags::IsReplaceable }) || variable.owner)
            found.quickSet(index);
    }
    return found;
}

void ProgramVerifier::verifyVariables()
{
    BitVector mayBeFilledByRunTime = variablesThatRunTimeMayFill(m_program);
    TypeReader types(m_program);
    BitVector isStaticField(m_program.variables.size());
    for (const Layout& layout : m_program.layouts) {
        for (const StaticField& field : layout.staticFields)
            isStaticField.quickSet(field.variable.index());
    }

    BitVector hasVariableThatHoldsMock(m_program.modules.size());
    auto isValid = [&](unsigned index) {
        const Variable& variable = m_program.variables[index];
        bool startsAsFunction = variable.initialization == VariableInitialization::Function;
        if (!variable.module || !!variable.function != startsAsFunction)
            return false;
        if (startsAsFunction && !hoistsFunctionsIntoVariables(m_program.at(variable.module).kind))
            return false;
        if (variable.flags.contains(VariableFlags::IsReplaceable) && (variable.flags.contains(VariableFlags::IsImmutable) || variable.owner))
            return false;
        if (variable.flags.contains(VariableFlags::HoldsMock)) {
            if (!variable.flags.contains(VariableFlags::IsReplaceable) || variable.initialization != VariableInitialization::Uninitialized || variable.name
                || !m_program.at(variable.module).flags.contains(ModuleFlags::MayBeMocked) || hasVariableThatHoldsMock.quickSet(variable.module.index()))
                return false;
        }
        if (variable.flags.contains(VariableFlags::OriginalMayRun) && !variable.flags.contains(VariableFlags::HoldsMock))
            return false;
        if (mayBeFilledByRunTime.quickGet(index) && variable.declared.isEstablished() && !types.at(variable.declared.written()).isAnything())
            return false;
        return !variable.owner || (m_program.at(variable.owner).kind == LayoutKind::Instance && isStaticField.quickGet(index));
    };
    for (unsigned index = 0; index < m_program.variables.size(); ++index) {
        if (!isValid(index))
            fail({ .kind = InternalErrorKind::InvalidVariable, .function = m_program.variables[index].function, .count = index });
    }
}

bool ProgramVerifier::runOnModulesNamedInBodies(Vector<MentionInCode>&& mentionsInBodies)
{
    verifyModulesNamedInBodies(WTF::move(mentionsInBodies));
    return !hasFailed();
}

bool verifyRecords(const Program& program, BuildContext& build)
{
    return ProgramVerifier(program, build).run();
}

bool verifyModulesNamedInBodies(const Program& program, BuildContext& build, Vector<MentionInCode>&& mentionsInBodies)
{
    return ProgramVerifier(program, build).runOnModulesNamedInBodies(WTF::move(mentionsInBodies));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
