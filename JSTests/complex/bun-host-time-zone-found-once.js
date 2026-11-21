// ICU looks for the host's time zone by itself, the first time it needs its default zone. The first time this process
// needs the host's zone, JavaScriptCore takes the zone that ICU has, and does not make ICU look a second time: two
// looks at once can leave the process with the wrong zone, and with TZ unset and /etc/localtime a regular file each
// look compares that file with the files under /usr/share/zoneinfo.
// Only a time zone change notification makes JavaScriptCore ask the host again.
//
// $vm.hostTimeZoneQuestions() is what JavaScriptCore has asked ICU: icuDefault, whether it asked for ICU's default
// zone (ucal_getDefaultTimeZone), and hostLooks, how often it made ICU look for the host's zone (ucal_getHostTimeZone).
// A change of zone is seen when the VM is next entered, so each step after one is a task of its own.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function questions() {
    return JSON.stringify($vm.hostTimeZoneQuestions());
}

function defaultTimeZone() {
    return new Intl.DateTimeFormat().resolvedOptions().timeZone;
}

// A date format with a zone of its own: JavaScriptCore needs no host zone, and ICU needs its default zone.
shouldBe(new Intl.DateTimeFormat("en", { timeZone: "UTC", hour: "numeric", hourCycle: "h23" }).format(1700000000000), "22");
shouldBe(questions(), '{"icuDefault":false,"hostLooks":0}');

// The first need of the host's zone, and every use of it after that.
shouldBe(new Date(1700000000000).getHours(), 14);
shouldBe(questions(), '{"icuDefault":true,"hostLooks":0}');
shouldBe(new Date(1700000000000).toString(), "Tue Nov 14 2023 14:13:20 GMT-0800 (Pacific Standard Time)");
shouldBe(defaultTimeZone(), "America/Los_Angeles");
shouldBe(questions(), '{"icuDefault":true,"hostLooks":0}');

// An override is not a question to the host.
shouldBe($vm.setHostTimeZone("Asia/Tokyo"), true);
setTimeout(() => {
    shouldBe(defaultTimeZone(), "Asia/Tokyo");
    shouldBe(new Date(1700000000000).getHours(), 7);
    shouldBe(questions(), '{"icuDefault":true,"hostLooks":0}');

    // The override goes away, with a notification: the host is asked, once.
    shouldBe($vm.setHostTimeZone(""), true);
    setTimeout(() => {
        shouldBe(defaultTimeZone(), "America/Los_Angeles");
        shouldBe(new Date(1700000000000).getHours(), 14);
        shouldBe(new Date(1700000000000).toString(), "Tue Nov 14 2023 14:13:20 GMT-0800 (Pacific Standard Time)");
        shouldBe(questions(), '{"icuDefault":true,"hostLooks":1}');

        // And once for each notification after that.
        shouldBe($vm.setHostTimeZone(""), true);
        setTimeout(() => {
            shouldBe(defaultTimeZone(), "America/Los_Angeles");
            shouldBe(questions(), '{"icuDefault":true,"hostLooks":2}');
        }, 0);
    }, 0);
}, 0);
