// A time zone change notification that comes before the process first needs the host's zone, when ICU has found the
// host's zone for itself already. The zone that ICU keeps as its default can be from before the notification, so it
// is not taken for the host's zone: JavaScriptCore makes ICU look, as it does after every other notification.
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

// ICU finds the host's zone for itself: a date format with a zone of its own needs ICU's default zone.
shouldBe(new Intl.DateTimeFormat("en", { timeZone: "UTC", hour: "numeric", hourCycle: "h23" }).format(1700000000000), "22");

shouldBe($vm.setHostTimeZone("Asia/Tokyo"), true);
setTimeout(() => {
    shouldBe(defaultTimeZone(), "Asia/Tokyo");
    shouldBe(questions(), '{"icuDefault":false,"hostLooks":0}');

    shouldBe($vm.setHostTimeZone(""), true);
    setTimeout(() => {
        // The first need of the host's zone in this process.
        shouldBe(new Date(1700000000000).getHours(), 14);
        shouldBe(questions(), '{"icuDefault":true,"hostLooks":1}');
        shouldBe(defaultTimeZone(), "America/Los_Angeles");
        shouldBe(new Date(1700000000000).toString(), "Tue Nov 14 2023 14:13:20 GMT-0800 (Pacific Standard Time)");
        shouldBe(questions(), '{"icuDefault":true,"hostLooks":1}');
    }, 0);
}, 0);
