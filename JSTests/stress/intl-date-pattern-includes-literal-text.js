function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

// Bun: upstream expects '23 h'. CLDR 49 (ICU 79) has no "HH 'h'" for fr.
const hour = $vm.icuVersion() >= 79 ? '23' : '23 h';

shouldBe(new Intl.DateTimeFormat("fr", {hour: "numeric", hour12: false}).resolvedOptions().hour12, false);
shouldBe(new Intl.DateTimeFormat("fr", {hour: "numeric", hour12: false}).format(new Date(2021, 2, 3, 23)), hour);

shouldBe(new Intl.DateTimeFormat("fr", {hour: "numeric", hourCycle: 'h24'}).format(new Date(2021, 2, 3, 23)), hour);
shouldBe(new Intl.DateTimeFormat("fr", {hour: "numeric", hourCycle: 'h23'}).format(new Date(2021, 2, 3, 23)), hour);

// Bun: not in upstream's test. fr-CA has literal text in its pattern ("HH 'h' mm") with CLDR 49 too.
shouldBe(new Intl.DateTimeFormat("fr-CA", {hour: "numeric", minute: "numeric", hour12: false}).resolvedOptions().hour12, false);
shouldBe(new Intl.DateTimeFormat("fr-CA", {hour: "numeric", minute: "numeric", hour12: false}).format(new Date(2021, 2, 3, 23, 5)), '23 h 05');
shouldBe(new Intl.DateTimeFormat("fr-CA", {hour: "numeric", minute: "numeric", hourCycle: 'h24'}).format(new Date(2021, 2, 3, 23, 5)), '23 h 05');
shouldBe(new Intl.DateTimeFormat("fr-CA", {hour: "numeric", minute: "numeric", hourCycle: 'h23'}).format(new Date(2021, 2, 3, 23, 5)), '23 h 05');

shouldBe(JSON.stringify(new Intl.Locale("fr", {hourCycle: 'h24'}).getHourCycles()), `["h24"]`);
shouldBe(JSON.stringify(new Intl.Locale("fr", {hourCycle: 'h23'}).getHourCycles()), `["h23"]`);
shouldBe(JSON.stringify(new Intl.Locale("fr").getHourCycles()), `["h23"]`);
