/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRParser.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageTestSupport.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubCatalog.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <memory>
#include <wtf/text/StringBuilder.h>

namespace JSC::AOT {

using namespace ImageTests;
using Typed::Access;

namespace {

struct NamedProgram {
    explicit NamedProgram(const TestOptions& = { });

    StringID text(ASCIILiteral);
    PropertyKeyID key(ASCIILiteral, PropertyKeyKind = PropertyKeyKind::Text);
    KeyUse& use(PropertyKeyID);
    PropertyKeyID keyWithUses(ASCIILiteral, uint32_t uses, uint32_t tableLookupUses = 0);
    PropertyKeyID keyWithAccesses(ASCIILiteral, Access, uint32_t uses);
    IdentifierNumbering number();

    BuildContext context;
    std::unique_ptr<Typed::Program> program;
    KeyUses uses;
    BitVector keysWithColumn;
};

} // anonymous namespace

NamedProgram::NamedProgram(const TestOptions& options)
    : context(wireOptions(options))
    , program(Typed::parseProgram(programText(1), Typed::IRForm::AsRead, context))
{
    AOT_TEST_CHECK(program);
}

StringID NamedProgram::text(ASCIILiteral characters)
{
    return program->strings.intern(StringView(characters));
}

PropertyKeyID NamedProgram::key(ASCIILiteral characters, PropertyKeyKind kind)
{
    Typed::PropertyKey key;
    key.kind = kind;
    key.text = text(characters);
    return program->types.intern(key);
}

KeyUse& NamedProgram::use(PropertyKeyID key)
{
    uses.byKey.resize(program->types.numberOfPropertyKeys());
    return uses.byKey[key.index()];
}

PropertyKeyID NamedProgram::keyWithUses(ASCIILiteral characters, uint32_t numberOfUses, uint32_t tableLookupUses)
{
    PropertyKeyID result = key(characters);
    KeyUse& keyUse = use(result);
    keyUse.uses = numberOfUses;
    keyUse.tableLookupUses = tableLookupUses;
    if (tableLookupUses)
        keyUse.firstTableLookupFunction = FunctionID(0u);
    return result;
}

PropertyKeyID NamedProgram::keyWithAccesses(ASCIILiteral characters, Access access, uint32_t numberOfUses)
{
    PropertyKeyID result = keyWithUses(characters, numberOfUses);
    use(result).usesByAccess[static_cast<size_t>(access)] = numberOfUses;
    return result;
}

IdentifierNumbering NamedProgram::number()
{
    uses.byKey.resize(program->types.numberOfPropertyKeys());
    return numberIdentifiers(*program, uses, keysWithColumn, context);
}

static uint32_t numberForRank(const IdentifierNumbering& numbering, uint32_t rank)
{
    return RowDisplacementTable::numberForRank(rank, numbering.countWithLookupTableColumn() - 1);
}

static uint32_t rankOf(const IdentifierNumbering& numbering, PropertyKeyID key)
{
    std::optional<uint32_t> number = numbering.numberOf(key);
    AOT_TEST_CHECK(number);
    for (uint32_t rank = 1; rank < numbering.count(); ++rank) {
        if (numberForRank(numbering, rank) == *number)
            return rank;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

static void testMoreUsesFirst()
{
    NamedProgram named;
    PropertyKeyID a = named.keyWithUses("a"_s, 3);
    PropertyKeyID b = named.keyWithUses("b"_s, 5);
    PropertyKeyID c = named.keyWithUses("c"_s, 1);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(numbering.numberOf(b) == numberForRank(numbering, 1));
    AOT_TEST_CHECK(numbering.numberOf(a) == numberForRank(numbering, 2));
    AOT_TEST_CHECK(numbering.numberOf(c) == numberForRank(numbering, 3));
}

static void testEqualCountsByKeyID()
{
    NamedProgram named;
    PropertyKeyID q = named.keyWithUses("q"_s, 2);
    PropertyKeyID p = named.keyWithUses("p"_s, 2);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, std::min(q, p)) == 1);
    AOT_TEST_CHECK(rankOf(numbering, std::max(q, p)) == 2);
}

static void testZeroIsNoName()
{
    NamedProgram named;
    std::array keys { named.keyWithUses("a"_s, 3), named.keyWithUses("b"_s, 5), named.keyWithUses("c"_s, 1) };
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!numbering.at(0).key);
    AOT_TEST_CHECK(!numbering.at(0).text);
    for (PropertyKeyID key : keys) {
        const NumberedIdentifier& identifier = numbering.at(*numbering.numberOf(key));
        AOT_TEST_CHECK(identifier.key == key);
        AOT_TEST_CHECK(identifier.text == named.program->at(key).text);
        AOT_TEST_CHECK(numbering.numberOfText(identifier.text) == numbering.numberOf(key));
    }

    uint32_t greatestNumber = 0;
    for (uint32_t index = 0; index < named.program->types.numberOfPropertyKeys(); ++index)
        greatestNumber = std::max(greatestNumber, numbering.numberOf(PropertyKeyID(index)).value_or(0));
    AOT_TEST_CHECK(numbering.count() == greatestNumber + 1);
    AOT_TEST_CHECK(IdentifierNumbering().count() == 1);
    AOT_TEST_CHECK(IdentifierNumbering().countWithLookupTableColumn() == 1);
}

static void testTextWithoutKeyHasNoNumber()
{
    NamedProgram named;
    named.keyWithUses("a"_s, 1);
    StringID onlyText = named.text("only text"_s);
    PropertyKeyID unusedKey = named.key("never used"_s);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!numbering.numberOfText(onlyText));
    AOT_TEST_CHECK(!numbering.numberOfText(StringID()));
    AOT_TEST_CHECK(!numbering.numberOfText(StringID(named.program->strings.size())));
    AOT_TEST_CHECK(!numbering.numberOf(unusedKey));
    AOT_TEST_CHECK(!numbering.numberOf(PropertyKeyID()));
    AOT_TEST_CHECK(!numbering.numberOf(PropertyKeyID(named.program->types.numberOfPropertyKeys())));
}

static void testTableNamesComeLast()
{
    NamedProgram named;
    std::array used { named.keyWithUses("a"_s, 3), named.keyWithUses("b"_s, 5), named.keyWithUses("c"_s, 1) };
    PropertyKeyID namedByTables = named.key("field"_s);
    named.uses.keysNamedByTables.set(namedByTables.index());
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, namedByTables) > used.size());

    BitVector numbersOfUsedKeys;
    for (uint32_t rank = 1; rank <= used.size(); ++rank)
        numbersOfUsedKeys.set(numberForRank(numbering, rank));
    unsigned numberOfBuiltinNames = 0;
    for (uint32_t index = 0; index < named.program->types.numberOfPropertyKeys(); ++index) {
        PropertyKeyID key(index);
        if (!named.program->builtinNameOf(key) || std::ranges::find(used, key) != used.end())
            continue;
        std::optional<uint32_t> number = numbering.numberOf(key);
        AOT_TEST_CHECK(number);
        AOT_TEST_CHECK(!numbersOfUsedKeys.get(*number));
        ++numberOfBuiltinNames;
    }
    AOT_TEST_CHECK(numberOfBuiltinNames);
}

static void testLookupsBeforeUses()
{
    NamedProgram named;
    PropertyKeyID rare = named.keyWithUses("rare"_s, 6, 0);
    PropertyKeyID often = named.keyWithUses("often"_s, 1, 1);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, often) == 1);
    AOT_TEST_CHECK(rankOf(numbering, rare) == 2);
}

static void testPromisedColumnsFirst()
{
    NamedProgram named;
    PropertyKeyID often = named.keyWithUses("often"_s, 9, 9);
    PropertyKeyID promised = named.keyWithUses("promised"_s, 1);
    named.keysWithColumn.set(promised.index());
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, promised) == 1);
    AOT_TEST_CHECK(rankOf(numbering, often) == 2);
}

static String lookupsInProgramText()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = Builtin builtin=GlobalThis\n"_s);
    text.append("    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    unsigned nextValue = 2;
    for (unsigned i = 0; i < 6; ++i) {
        unsigned read = nextValue++;
        text.append("    v"_s, read, " = PropGet v1 key=\"rare\"\n"_s);
        text.append("    v"_s, nextValue++, " = Call v1, -, v"_s, read, '\n');
    }
    unsigned closure = nextValue++;
    text.append("    v"_s, closure, " = NewClosure function=f1\n"_s);
    text.append("    v"_s, nextValue, " = Call v1, -, v"_s, closure, '\n');
    text.append("    Return\n}\n"_s);
    text.append("function f1 name=\"f1\" results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = Builtin builtin=GlobalThis\n"_s);
    text.append("    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = PropGet v1 key=\"often\"\n"_s);
    text.append("    v3 = Call v1, -, v2\n"_s);
    text.append("    Return\n}\n"_s);
    text.append(runtimeFunctionsText(2));
    return text.toString();
}

static PropertyKeyID findTextKey(const Typed::Program& program, ASCIILiteral characters)
{
    Typed::PropertyKey key;
    key.text = program.strings.find(byteCast<uint8_t>(characters.span()));
    AOT_TEST_CHECK(key.text);
    std::optional<PropertyKeyID> found = program.types.find(key);
    AOT_TEST_CHECK(found);
    return *found;
}

static void testLookupsOfProgram()
{
    TestBuild build(lookupsInProgramText(), { });
    const KeyUses& uses = build.plan().uses.keys;
    PropertyKeyID rare = findTextKey(build.program(), "rare"_s);
    PropertyKeyID often = findTextKey(build.program(), "often"_s);
    AOT_TEST_CHECK(uses.byKey[rare.index()].uses > uses.byKey[often.index()].uses);
    AOT_TEST_CHECK(uses.byKey[rare.index()].tableLookupUses < uses.byKey[often.index()].tableLookupUses);

    const IdentifierNumbering& numbering = build.context().identifierNumbering();
    AOT_TEST_CHECK(rankOf(numbering, often) < rankOf(numbering, rare));
}

static uint64_t statistic(BuildContext& context, StatisticID id)
{
    return context.statistics().values()[static_cast<unsigned>(id)];
}

static void testLookupTableFull()
{
    NamedProgram named(TestOptions { LimitID::LookupTableNames, 2 });
    std::array keys { named.keyWithUses("a"_s, 3, 3), named.keyWithUses("b"_s, 5, 5), named.keyWithUses("c"_s, 1, 1), named.keyWithUses("d"_s, 2, 2) };
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(numbering.countWithLookupTableColumn() == 3);
    for (PropertyKeyID key : keys)
        AOT_TEST_CHECK(numbering.numberOf(key));
    for (PropertyKeyID key : { keys[0], keys[1] })
        AOT_TEST_CHECK(*numbering.numberOf(key) < numbering.countWithLookupTableColumn());
    for (PropertyKeyID key : { keys[2], keys[3] })
        AOT_TEST_CHECK(*numbering.numberOf(key) >= numbering.countWithLookupTableColumn());

    AOT_TEST_CHECK(statistic(named.context, hitsOf(LimitID::LookupTableNames)) == 2);
    Vector<Wire::LossRecord> losses = named.context.takeLosses(named.program.get());
    AOT_TEST_CHECK(losses.size() == 1);
    AOT_TEST_CHECK(losses[0].limit == LimitID::LookupTableNames);
    AOT_TEST_CHECK(!losses[0].witness.function);
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());
}

static void testLookupTableFits()
{
    NamedProgram named(TestOptions { LimitID::LookupTableNames, 2 });
    named.keyWithUses("a"_s, 1, 1);
    named.keyWithUses("b"_s, 1, 1);
    named.keyWithUses("c"_s, 1, 0);
    named.number();
    AOT_TEST_CHECK(!statistic(named.context, hitsOf(LimitID::LookupTableNames)));
    AOT_TEST_CHECK(named.context.takeLosses(named.program.get()).isEmpty());
}

static void testPrivateAndTextName()
{
    NamedProgram named;
    PropertyKeyID textKey = named.keyWithUses("p"_s, 1);
    PropertyKeyID privateKey = named.key("p"_s, PropertyKeyKind::EnginePrivateName);
    named.use(privateKey).uses = 4;
    AOT_TEST_CHECK(textKey != privateKey);

    IdentifierNumbering numbering = named.number();
    std::optional<uint32_t> textNumber = numbering.numberOf(textKey);
    std::optional<uint32_t> privateNumber = numbering.numberOf(privateKey);
    AOT_TEST_CHECK(textNumber && privateNumber);
    AOT_TEST_CHECK(textNumber != privateNumber);
    AOT_TEST_CHECK(numbering.at(*textNumber).text == numbering.at(*privateNumber).text);
    AOT_TEST_CHECK(numbering.at(*privateNumber).key == privateKey);
    AOT_TEST_CHECK(numbering.numberOfText(named.text("p"_s)) == textNumber);
}

static void testPrivateNameAlone()
{
    NamedProgram named;
    PropertyKeyID privateKey = named.key("name of no built-in"_s, PropertyKeyKind::EnginePrivateName);
    named.use(privateKey).uses = 1;
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(numbering.numberOf(privateKey));
    AOT_TEST_CHECK(!numbering.numberOfText(named.text("name of no built-in"_s)));
}

static void testExportNameWithoutKey()
{
    NamedProgram named;
    PropertyKeyID used = named.keyWithUses("used"_s, 1);
    StringID exported = named.text("exported"_s);
    named.uses.stringsNamedAsKeysByTables.set(exported.index());
    named.uses.stringsNamedAsKeysByTables.set(named.text("used"_s).index());

    IdentifierNumbering numbering = named.number();
    std::optional<uint32_t> number = numbering.numberOfText(exported);
    AOT_TEST_CHECK(number);
    AOT_TEST_CHECK(!numbering.at(*number).key);
    AOT_TEST_CHECK(numbering.at(*number).text == exported);
    AOT_TEST_CHECK(numbering.numberOfText(named.text("used"_s)) == numbering.numberOf(used));

    unsigned numberOfIdentifiersNamedUsed = 0;
    for (uint32_t candidate = 1; candidate < numbering.count(); ++candidate)
        numberOfIdentifiersNamedUsed += numbering.at(candidate).text == named.text("used"_s);
    AOT_TEST_CHECK(numberOfIdentifiersNamedUsed == 1);
}

static bool hasOnly(const BitVector& names, std::optional<uint32_t> number)
{
    return number && names.bitCount() == 1 && names.get(*number);
}

static void testOwnReadStub()
{
    NamedProgram named;
    named.keyWithAccesses("a"_s, Access::Read, StubPolicy::minUsesForOwnStub - 1);
    PropertyKeyID b = named.keyWithAccesses("b"_s, Access::Read, StubPolicy::minUsesForOwnStub);
    IdentifierNumbering numbering = named.number();
    StubPolicy policy = chooseStubPolicy(named.uses, numbering, named.context);
    AOT_TEST_CHECK(hasOnly(policy.namesWithOwnStub(Access::Read), numbering.numberOf(b)));
    AOT_TEST_CHECK(policy.namesWithOwnStub(Access::Store).isEmpty());
    AOT_TEST_CHECK(policy.namesWithOwnStub(Access::Define).isEmpty());
}

static void testVectorPerAccess()
{
    NamedProgram named;
    PropertyKeyID stored = named.keyWithAccesses("s"_s, Access::Store, StubPolicy::minUsesForOwnStub);
    PropertyKeyID defined = named.keyWithAccesses("d"_s, Access::Define, StubPolicy::minUsesForOwnStub);
    IdentifierNumbering numbering = named.number();
    StubPolicy policy = chooseStubPolicy(named.uses, numbering, named.context);
    AOT_TEST_CHECK(hasOnly(policy.namesWithOwnStub(Access::Store), numbering.numberOf(stored)));
    AOT_TEST_CHECK(hasOnly(policy.namesWithOwnStub(Access::Define), numbering.numberOf(defined)));
    AOT_TEST_CHECK(policy.namesWithOwnStub(Access::Read).isEmpty());
}

static void checkNoOwnStub(const StubPolicy& policy)
{
    for (const BitVector& names : policy.namesWithOwnStubByAccess)
        AOT_TEST_CHECK(names.isEmpty());
}

static void testOtherAccessesHaveNone()
{
    NamedProgram named;
    named.keyWithAccesses("x"_s, Access::None, 600);
    named.keyWithAccesses("y"_s, Access::Has, 600);
    IdentifierNumbering numbering = named.number();
    checkNoOwnStub(chooseStubPolicy(named.uses, numbering, named.context));
}

static void testNoNameIsSpecial()
{
    NamedProgram named;
    named.keyWithAccesses("get"_s, Access::Read, 1);
    IdentifierNumbering numbering = named.number();
    checkNoOwnStub(chooseStubPolicy(named.uses, numbering, named.context));
}

static IdentifierNumbering numberWithThreads(unsigned threadCount)
{
    NamedProgram named(TestOptions { .threadCount = threadCount });
    named.keyWithUses("a"_s, 3);
    named.keyWithUses("b"_s, 5, 2);
    named.keyWithUses("c"_s, 1);
    return named.number();
}

static void testSameResultTwice()
{
    IdentifierNumbering serial = numberWithThreads(1);
    IdentifierNumbering parallel = numberWithThreads(8);
    AOT_TEST_CHECK(serial.count() == parallel.count());
    AOT_TEST_CHECK(serial.countWithLookupTableColumn() == parallel.countWithLookupTableColumn());
    for (uint32_t number = 1; number < serial.count(); ++number) {
        AOT_TEST_CHECK(serial.at(number).key == parallel.at(number).key);
        AOT_TEST_CHECK(serial.at(number).text == parallel.at(number).text);
    }
}

std::span<const NativeTest> identifierNumberingTests()
{
    static constexpr NativeTest tests[] = {
        { "image/identifier-numbering/more-uses-first"_s, testMoreUsesFirst },
        { "image/identifier-numbering/equal-counts-by-key-id"_s, testEqualCountsByKeyID },
        { "image/identifier-numbering/zero-is-no-name"_s, testZeroIsNoName },
        { "image/identifier-numbering/text-without-key-has-no-number"_s, testTextWithoutKeyHasNoNumber },
        { "image/identifier-numbering/table-names-come-last"_s, testTableNamesComeLast },
        { "image/identifier-numbering/lookups-before-uses"_s, testLookupsBeforeUses },
        { "image/identifier-numbering/promised-columns-first"_s, testPromisedColumnsFirst },
        { "image/identifier-numbering/lookups-of-program"_s, testLookupsOfProgram },
        { "image/identifier-numbering/lookup-table-full"_s, testLookupTableFull },
        { "image/identifier-numbering/lookup-table-fits"_s, testLookupTableFits },
        { "image/identifier-numbering/private-and-text-name"_s, testPrivateAndTextName },
        { "image/identifier-numbering/private-name-alone"_s, testPrivateNameAlone },
        { "image/identifier-numbering/export-name-without-key"_s, testExportNameWithoutKey },
        { "image/identifier-numbering/own-read-stub"_s, testOwnReadStub },
        { "image/identifier-numbering/vector-per-access"_s, testVectorPerAccess },
        { "image/identifier-numbering/other-accesses-have-none"_s, testOtherAccessesHaveNone },
        { "image/identifier-numbering/no-name-is-special"_s, testNoNameIsSpecial },
        { "image/identifier-numbering/same-result-twice"_s, testSameResultTwice },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
