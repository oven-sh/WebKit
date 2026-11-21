//@ skip if $hostOS == "playstation"
//@ runDefault

// The identifiers of the time zones that a process names are kept in one place for all of its threads, and a thread
// reads them without a lock while another adds to them. Threads that name the same zones at the same time, in different
// orders and spellings, get the same identifiers and the same answers about them. So do threads that name zones for
// the first time while another thread lists every zone.
//
// What goes wrong here goes wrong in some runs only: the test is a chance to see it, not a proof.

const threadCount = 8;

const source = `
const names = [
    "UTC", "America/New_York", "America/Los_Angeles", "America/Chicago", "America/Denver", "America/Sao_Paulo",
    "America/Argentina/Buenos_Aires", "America/Mexico_City", "America/Toronto", "America/Anchorage", "Europe/London",
    "Europe/Berlin", "Europe/Paris", "Europe/Madrid", "Europe/Rome", "Europe/Moscow", "Europe/Istanbul", "Africa/Cairo",
    "Africa/Lagos", "Africa/Johannesburg", "Africa/Nairobi", "Asia/Tokyo", "Asia/Seoul", "Asia/Shanghai", "Asia/Dubai",
    "Asia/Singapore", "Asia/Jakarta", "Asia/Tehran", "Asia/Karachi", "Asia/Dhaka", "Australia/Sydney", "Australia/Perth",
    "Australia/Lord_Howe", "Pacific/Auckland", "Pacific/Honolulu", "Pacific/Chatham", "Pacific/Apia", "Etc/GMT+5",
    "Etc/GMT-14", "Antarctica/Troll",
    "US/Eastern", "US/Pacific", "US/Central", "Etc/UTC", "GMT", "Japan", "Singapore", "Turkey", "Europe/Belfast",
    "America/Indianapolis", "Australia/ACT", "NZ", "EST5EDT", "PST8PDT",
    "utc", "america/new_york", "EUROPE/BERLIN", "asia/TOKYO", "us/pacific", "etc/gmt+5", "japan", "est5edt",
    "Invalid/Zone", "America/New_Yrok", "US:Pacific", "UTC\\0x", "JST", "SystemV/EST5", "Etc/Unknown", "GMT+5", "",
];

function answers(rotation)
{
    const result = new Array(names.length);
    for (let step = 0; step < names.length; ++step) {
        const index = (step * 7 + rotation) % names.length;
        let answer;
        try {
            const zoned = new Temporal.ZonedDateTime(0n, names[index]);
            answer = zoned.timeZoneId + " " + zoned.offsetNanoseconds / 1e9 + " " + zoned.equals(new Temporal.ZonedDateTime(0n, "America/New_York")) + " " + new Intl.DateTimeFormat("en", { timeZone: names[index] }).resolvedOptions().timeZone;
        } catch (error) {
            answer = error.constructor.name;
        }
        result[index] = answer;
    }
    return result.join("|");
}
`;

eval(source);

for (let thread = 1; thread < threadCount; ++thread) {
    $.agent.start(source + `
        let report = "";
        for (let round = 0; round < 3; ++round)
            report += answers(${thread} * 11 + round) + "\\n";
        $.agent.report(report);
    `);
}

let expected = "";
for (let round = 0; round < 3; ++round)
    expected += answers(round) + "\n";
if (expected.split("\n")[0] !== expected.split("\n")[1])
    throw new Error("the answers of one thread changed between rounds");
if (!expected.startsWith("UTC 0 false UTC|America/New_York -18000 true America/New_York|"))
    throw new Error(`unexpected answers: ${expected.slice(0, 200)}`);
if (!expected.includes("|US/Eastern -18000 true US/Eastern|") || !expected.includes("|RangeError|"))
    throw new Error(`unexpected answers: ${expected}`);

for (let received = 1; received < threadCount;) {
    const report = $.agent.getReport();
    if (report === null) {
        $.agent.sleep(1);
        continue;
    }
    if (report !== expected)
        throw new Error(`a thread got other answers:\n${report}\nexpected:\n${expected}`);
    ++received;
}

// Intl.supportedValuesOf("timeZone") asks about every name that ICU lists, and adds each zone to the same place. While
// it does, each of the other threads names zones that no thread has named, and then names them again and again: the
// place is made larger several times under them.

const namedLater = [
    "ACT", "ART", "Africa/Accra", "Africa/Asmara", "Africa/Bangui", "Africa/Blantyre", "Africa/Casablanca",
    "Africa/Dakar", "Africa/Douala", "Africa/Gaborone", "Africa/Kampala", "Africa/Kinshasa", "Africa/Luanda",
    "Africa/Malabo", "Africa/Mbabane", "Africa/Ndjamena", "Africa/Ouagadougou", "Africa/Timbuktu", "Africa/Windhoek",
    "America/Antigua", "America/Argentina/ComodRivadavia", "America/Argentina/La_Rioja", "America/Argentina/Salta",
    "America/Argentina/Tucuman", "America/Asuncion", "America/Bahia", "America/Belem", "America/Boa_Vista",
    "America/Buenos_Aires", "America/Cancun", "America/Cayenne", "America/Ciudad_Juarez", "America/Costa_Rica",
    "America/Cuiaba", "America/Dawson", "America/Dominica", "America/El_Salvador", "America/Fort_Wayne",
    "America/Godthab", "America/Grenada", "America/Guayaquil", "America/Havana", "America/Indiana/Knox",
    "America/Indiana/Tell_City", "America/Indiana/Winamac", "America/Jamaica", "America/Kentucky/Louisville",
    "America/Kralendijk", "America/Louisville", "America/Managua", "America/Martinique", "America/Mendoza",
    "America/Metlakatla", "America/Monterrey", "America/Montserrat", "America/Nome", "America/North_Dakota/Center",
    "America/Ojinaga", "America/Paramaribo", "America/Port_of_Spain", "America/Puerto_Rico", "America/Rankin_Inlet",
    "America/Resolute", "America/Santa_Isabel", "America/Santo_Domingo", "America/Sitka", "America/St_Kitts",
    "America/St_Vincent", "America/Thule", "America/Tortola", "America/Whitehorse", "America/Yellowknife",
    "Antarctica/DumontDUrville", "Antarctica/McMurdo", "Antarctica/South_Pole", "Arctic/Longyearbyen", "Asia/Amman",
    "Asia/Aqtobe", "Asia/Atyrau", "Asia/Baku", "Asia/Beirut", "Asia/Calcutta", "Asia/Chongqing", "Asia/Dacca",
    "Asia/Dushanbe", "Asia/Harbin", "Asia/Hong_Kong", "Asia/Istanbul", "Asia/Kabul", "Asia/Kathmandu", "Asia/Kolkata",
    "Asia/Kuching", "Asia/Macau", "Asia/Manila", "Asia/Novokuznetsk", "Asia/Oral", "Asia/Pyongyang", "Asia/Qyzylorda",
    "Asia/Saigon", "Asia/Srednekolymsk", "Asia/Tbilisi", "Asia/Thimphu", "Asia/Ulaanbaatar", "Asia/Ust-Nera",
    "Asia/Yakutsk", "Asia/Yerevan", "Atlantic/Canary", "Atlantic/Faroe", "Atlantic/Reykjavik", "Atlantic/Stanley",
    "Australia/Broken_Hill", "Australia/Darwin", "Australia/LHI", "Australia/NSW", "Australia/South", "Australia/West",
    "BST", "Brazil/East", "CET", "CST6CDT", "Canada/Central", "Canada/Mountain", "Canada/Saskatchewan",
    "Chile/EasterIsland", "ECT", "Egypt", "Etc/GMT+0", "Etc/GMT+11", "Etc/GMT+3", "Etc/GMT+7", "Etc/GMT-0",
    "Etc/GMT-11", "Etc/GMT-2", "Etc/GMT-5", "Etc/GMT-8", "Etc/Greenwich", "Etc/Zulu", "Europe/Astrakhan",
    "Europe/Bratislava", "Europe/Budapest", "Europe/Copenhagen", "Europe/Guernsey", "Europe/Jersey", "Europe/Kirov",
    "Europe/Ljubljana", "Europe/Mariehamn", "Europe/Nicosia", "Europe/Prague", "Europe/San_Marino", "Europe/Simferopol",
    "Europe/Stockholm", "Europe/Tiraspol", "Europe/Vaduz", "Europe/Vilnius", "Europe/Zagreb", "Factory", "GMT+0",
    "Greenwich", "IET", "Indian/Antananarivo", "Indian/Cocos", "Indian/Mahe", "Indian/Mayotte", "Israel", "Libya",
    "MST", "Mexico/BajaSur", "NST", "PLT", "PRT", "Pacific/Chuuk", "Pacific/Enderbury", "Pacific/Funafuti",
    "Pacific/Guadalcanal", "Pacific/Kanton", "Pacific/Kwajalein", "Pacific/Midway", "Pacific/Norfolk", "Pacific/Palau",
    "Pacific/Ponape", "Pacific/Saipan", "Pacific/Tarawa", "Pacific/Wake", "Poland", "ROK", "SystemV/AST4ADT",
    "SystemV/EST5EDT", "SystemV/MST7MDT", "SystemV/YST9", "US/Alaska", "US/East-Indiana", "US/Michigan", "US/Samoa",
    "W-SU", "africa/accra", "ASIA/CALCUTTA", "europe/Prague", "Pacific/No_Such_Zone", "Europe:Prague",
];

const laterSource = `
const namedLater = ${JSON.stringify(namedLater)};

function answer(name)
{
    try {
        const zoned = new Temporal.ZonedDateTime(0n, name);
        return zoned.timeZoneId + " " + zoned.offsetNanoseconds / 1e9 + " " + zoned.equals(new Temporal.ZonedDateTime(0n, "America/New_York"));
    } catch (error) {
        return error.constructor.name;
    }
}

// The names of one thread, which no other thread of this phase names.
function answersOf(thread, threads)
{
    const result = [];
    for (let index = thread; index < namedLater.length; index += threads)
        result.push(namedLater[index] + "=" + answer(namedLater[index]));
    return result;
}
`;

eval(laterSource);

const gate = new Int32Array(new SharedArrayBuffer(8));
for (let thread = 1; thread < threadCount; ++thread) {
    $.agent.start(laterSource + `
        $.agent.receiveBroadcast(buffer => {
            const gate = new Int32Array(buffer);
            Atomics.add(gate, 1, 1);
            Atomics.wait(gate, 0, 0);
            const first = answersOf(${thread - 1}, ${threadCount - 1});
            for (let round = 0; round < 80; ++round) {
                const again = answersOf(${thread - 1}, ${threadCount - 1});
                for (let index = 0; index < first.length; ++index) {
                    if (again[index] !== first[index])
                        first[index] += " and then " + again[index];
                }
            }
            $.agent.report(JSON.stringify(first));
        });
    `);
}
$.agent.broadcast(gate.buffer);
while (Atomics.load(gate, 1) < threadCount - 1)
    $.agent.sleep(1);
Atomics.store(gate, 0, 1);
Atomics.notify(gate, 0);

const zones = Intl.supportedValuesOf("timeZone");
if (zones.length < 400)
    throw new Error(`only ${zones.length} time zones`);

let compared = 0;
for (let received = 1; received < threadCount;) {
    const report = $.agent.getReport();
    if (report === null) {
        $.agent.sleep(1);
        continue;
    }
    for (const line of JSON.parse(report)) {
        const name = line.slice(0, line.indexOf("="));
        const expectedLine = name + "=" + answer(name);
        if (line !== expectedLine)
            throw new Error(`a thread got ${line}, expected ${expectedLine}`);
        // A primary identifier keeps its spelling.
        if (zones.includes(name) && !line.startsWith(name + "=" + name + " "))
            throw new Error(`a thread got ${line} for a primary identifier`);
        ++compared;
    }
    ++received;
}
if (compared !== namedLater.length)
    throw new Error(`${compared} of ${namedLater.length} names were compared`);
