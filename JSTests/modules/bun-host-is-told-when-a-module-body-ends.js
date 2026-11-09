import { shouldBe } from "./resources/assert.js";
import { log } from "./bun-host-is-told-when-a-module-body-ends/log.js";
import "./bun-host-is-told-when-a-module-body-ends/sync-importer.js";
import "./bun-host-is-told-when-a-module-body-ends/tla-importer.js";

// JSModuleRecord::tellHostWhenBodyEnds(): the host hears of the end of a body after its last statement and before any module that imports it runs.
shouldBe(JSON.stringify(log.splice(0)), JSON.stringify([
    "sync flagged: true",
    "sync body starts",
    "sync body ends",
    "told of sync",
    "importer of sync runs",
    "tla flagged: true",
    "tla body starts",
    "tla body ends",
    "told of tla",
    "importer of tla runs",
]));

// A body that has ended is not waited for.
const ended = await import("./bun-host-is-told-when-a-module-body-ends/tla-target.js");
shouldBe(tellWhenModuleBodyEnds(ended, () => log.push("told of a body that had ended")), false);

// A body that throws ends without the call.
shouldBe(await import("./bun-host-is-told-when-a-module-body-ends/throws-target.js").then(() => "no error", error => error.message), "thrown by the body");

// What the host throws is the failure of the module.
shouldBe(await import("./bun-host-is-told-when-a-module-body-ends/host-throws-importer.js").then(() => "no error", error => error.message), "thrown by the host");
shouldBe(await import("./bun-host-is-told-when-a-module-body-ends/host-throws-target.js").then(() => "no error", error => error.message), "thrown by the host");

shouldBe(JSON.stringify(log), JSON.stringify(["body ends, then the host throws"]));
