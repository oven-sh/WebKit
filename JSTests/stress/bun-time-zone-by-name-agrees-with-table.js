//@ skip if $hostOS == "playstation"
//@ requireOptions("--useDollarVM=1")
//@ runDefault

// Bun finds a time zone identifier by asking ICU about the one name, and Intl.supportedValuesOf("timeZone") asks the
// same about every name that ICU lists. Upstream builds a table of every identifier. That table is what says which
// names are zones, how a zone is spelled and what its primary is, and in Bun only this test builds it. This holds the
// answer by name against the table: for every identifier of the table, for other spellings of each, and for names that
// are no zones.
//
// $vm.timeZoneInTable(name) is the table's answer and $vm.timeZoneByName(name) the answer by name. Both are
// [identifier, identifier of the primary], or null.

function describe(value)
{
    return value === null ? "null" : JSON.stringify(value);
}

let checked = 0;

function check(name)
{
    const inTable = describe($vm.timeZoneInTable(name));
    const byName = describe($vm.timeZoneByName(name));
    if (inTable !== byName)
        throw new Error(`${JSON.stringify(name)}: the table has ${inTable}, the name gives ${byName}`);
    ++checked;
}

function otherCase(name)
{
    let result = "";
    for (const character of name)
        result += character === character.toLowerCase() ? character.toUpperCase() : character.toLowerCase();
    return result;
}

// Spellings that are near an identifier. Most are no zones. ICU's own lookup of one name takes some of them for the
// identifier: it stops at a NUL, and it reads ":" as "/".
function spellingsNear(name)
{
    return [
        name.toLowerCase(),
        name.toUpperCase(),
        otherCase(name),
        name.replaceAll("/", ":"),
        name.replaceAll("/", "//"),
        name.replaceAll("/", "\\"),
        name.replaceAll("_", " "),
        name.replaceAll("_", "-"),
        name.replaceAll("-", "_"),
        name + "\0",
        name + "\0x",
        name + " ",
        " " + name,
        name + "/",
        "/" + name,
        name + "x",
        name.slice(1),
        name.slice(0, -1),
        "posix/" + name,
        "right/" + name,
        name.replace("a", "\u0430"), // CYRILLIC SMALL LETTER A
        name.replace("k", "\u212A"), // KELVIN SIGN, which lower-cases to "k" outside ASCII
        name.replace("s", "\u017F"), // LATIN SMALL LETTER LONG S, which upper-cases to "S" outside ASCII
        name.replace("i", "\u0131"), // LATIN SMALL LETTER DOTLESS I
        name + "\uD800",
    ];
}

const otherNames = [
    "", " ", "\0", "/", ":", "UTC", "utc", "Utc", "GMT", "gmt", "Etc/UTC", "etc/utc", "Etc/GMT", "Etc/GMT0", "Etc/GMT+0",
    "Etc/GMT-0", "Etc/Greenwich", "Etc/UCT", "Etc/Universal", "Etc/Zulu", "GMT0", "GMT+0", "GMT-0", "Greenwich", "UCT",
    "Universal", "Zulu", "Z", "Etc/Unknown", "Unknown", "Factory", "SystemV/AST4", "SystemV/EST5EDT", "systemv/pst8pdt",
    "ACT", "AET", "AGT", "ART", "AST", "BET", "BST", "CAT", "CNT", "CST", "CTT", "EAT", "ECT", "IET", "IST", "JST", "MIT",
    "NET", "NST", "PLT", "PNT", "PRT", "PST", "SST", "VST", "jst", "pst", "EST", "MST", "HST", "CET", "EET", "MET", "WET",
    "est", "EST5EDT", "CST6CDT", "MST7MDT", "PST8PDT", "est5edt", "Est5Edt", "EST5", "EST5EDT4", "GMT+5", "GMT-5",
    "GMT+05:00", "GMT+05:30", "UTC+1", "UTC+01:00", "+05:30", "-08:00", "Etc/GMT+5", "Etc/GMT-14", "Etc/GMT+12",
    "Etc/GMT+13", "Etc/GMT-15", "CST6CDT5,J129,J131/19:30", "CET-1CEST,M3.5.0,M10.5.0/3", "MSK-3", "<+03>-3",
    ":America/New_York", "/etc/localtime", ":/etc/localtime", "/usr/share/zoneinfo/Europe/Berlin", "Invalid/Zone",
    "America/New_Yrok", "Europe", "America", "Europe/", "America/Argentina", "America/Argentina/", "Mars/Olympus_Mons",
    "US/Pacific-New", "US:Pacific", "Asia:Kolkata", "Asia:Calcutta", "America:Argentina:Buenos_Aires",
    "America/Argentina:Buenos_Aires", "A".repeat(128), "A".repeat(129), "A/".repeat(5000), "Europe/Berlin".repeat(10),
    "\u00C5land", "Europe/Z\u00FCrich", "\uFF35\uFF34\uFF23", "UTC\uFEFF", "\uD83D\uDE00",
];

const table = $vm.timeZoneTable();
if (table.length < 500)
    throw new Error(`the table has only ${table.length} identifiers`);
const primaries = table.filter(identifier => $vm.timeZoneInTable(identifier)[1] === identifier);
if (primaries.length < 400 || primaries.length === table.length)
    throw new Error(`${primaries.length} of ${table.length} identifiers are primaries`);

// Every primary identifier is found by its name alone, without the list of ICU's zone names.
for (const primary of primaries)
    check(primary);
if ($vm.hasListedICUTimeZoneNames())
    throw new Error("a primary identifier needed the list of ICU's zone names");

// Everything else needs that list. A name of the list that is no zone is remembered as none: the second time it is
// asked about, that is the answer.
for (let times = 0; times < 2; ++times) {
    for (const name of otherNames) {
        check(name);
        for (const spelling of spellingsNear(name))
            check(spelling);
    }
}
if (!$vm.hasListedICUTimeZoneNames())
    throw new Error("no name needed the list of ICU's zone names");

// The primary identifiers that Bun lists are the table's, in the table's order.
const listed = Intl.supportedValuesOf("timeZone");
if (listed.join() !== primaries.join()) {
    const missing = primaries.filter(identifier => !listed.includes(identifier));
    const added = listed.filter(identifier => !primaries.includes(identifier));
    throw new Error(`Intl.supportedValuesOf("timeZone") has ${listed.length} identifiers and the table ${primaries.length} primaries; only the table has [${missing}], only the list has [${added}]`);
}

// The identifiers differ in more than case, so a search that ignores case finds one of them or none.
const lowerCased = new Set(table.map(identifier => identifier.toLowerCase()));
if (lowerCased.size !== table.length)
    throw new Error("two identifiers of the table differ only in case");

// The primaries are asked again, now that the list is there, and every alias, with other spellings of each.
for (const identifier of table) {
    check(identifier);
    for (const spelling of spellingsNear(identifier))
        check(spelling);
}

if (checked < 15000)
    throw new Error(`only ${checked} names were compared`);
