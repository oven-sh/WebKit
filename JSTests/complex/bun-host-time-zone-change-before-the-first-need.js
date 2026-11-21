// A time zone change notification that comes before the process first needs the host's zone, when ICU has not found
// the host's zone for itself yet. JavaScriptCore asks for ICU's default zone before it makes ICU look for the host's
// zone. The answer is not used. The question is there so that ICU's own look has finished: ICU would look by itself
// the first time it needs its default zone, and that can be on another thread while JavaScriptCore's look runs. Two
// looks at once can leave the process with the wrong zone.
//
// $vm.hostTimeZoneQuestions() is what JavaScriptCore has asked ICU: icuDefault, whether it asked for ICU's default
// zone (ucal_getDefaultTimeZone), and hostLooks, how often it made ICU look for the host's zone (ucal_getHostTimeZone).
// A change of zone is seen when the VM is next entered, so the step after one is a task of its own.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function questions() {
    return JSON.stringify($vm.hostTimeZoneQuestions());
}

shouldBe(questions(), '{"icuDefault":false,"hostLooks":0}');
shouldBe($vm.setHostTimeZone(""), true);
setTimeout(() => {
    shouldBe(questions(), '{"icuDefault":false,"hostLooks":0}');
    shouldBe(new Date(1700000000000).getHours(), 14);
    shouldBe(questions(), '{"icuDefault":true,"hostLooks":1}');
    shouldBe(new Intl.DateTimeFormat().resolvedOptions().timeZone, "America/Los_Angeles");
    shouldBe(questions(), '{"icuDefault":true,"hostLooks":1}');
}, 0);
