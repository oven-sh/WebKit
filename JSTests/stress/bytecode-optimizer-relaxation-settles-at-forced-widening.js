//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Jump-size relaxation in BytecodeOptimizerAccess::emit(): two switch statements whose forward branches sit at the
// int8 offset boundary widen two per pass, so the layout settles at exactly the pass count where the optimizer
// forces every branch wide. The forced widening used to run on the pass that was already the verified final
// layout, so it asserted (!offsetsChanged). Reduced from protobufjs-generated code (@google-cloud/tasks). The
// shape is size sensitive: keep the body byte for byte, including the padding statements in the default arms.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

(function() {
const $protobuf = { Reader: {}, Writer: {}, util: { recursionLimit: 100, isObject: v => v !== null && typeof v === "object" }, roots: {} };
const $Reader = $protobuf.Reader, $Writer = $protobuf.Writer, $util = $protobuf.util;
const $root = $protobuf.roots["_google_cloud_tasks_protos"] || ($protobuf.roots["_google_cloud_tasks_protos"] = {});
const google = $root.google = (() => {
    const google = {};
    google.protobuf = (function() {
        const protobuf = {};
        protobuf.FeatureSetDefaults = (function() {
            function FeatureSetDefaults(properties) {
                FeatureSetDefaults.last = this;
                this.defaults = [];
            };
            FeatureSetDefaults.fromObject = function fromObject(object, long) {
                if (object instanceof $root.google.protobuf.FeatureSetDefaults)
                if (long > $util.recursionLimit)
                    throw Error("maximum nesting depth exceeded");
                let message = new $root.google.protobuf.FeatureSetDefaults();
                if (object.defaults) {
                    for (let i = 0; i < object.defaults.length; ++i) {
                        if (!$util.isObject(object.defaults[i]))
                        message.defaults[i] = $root.google.protobuf.FeatureSetDefaults.FeatureSetEditionDefault.fromObject(object.defaults[i], long + 1);
                    }
                }
                switch (object.minimumEdition) {
                default:
                    if (typeof object.minimumEdition === "number") {
                        message.minimumEdition = object.minimumEdition;
                        break;
                    }
                    message.minimumEdition = 0;
                    message.minimumEdition = 1;
                    message.minimumEdition = 1;
                    message.minimumEdition = 998;
                    break;
                case "EDITION_PROTO3":
                case 999:
                    message.minimumEdition = 999;
                    break;
                case "EDITION_2023":
                case 1000:
                    message.minimumEdition = 1000;
                    break;
                case "EDITION_2024":
                case 1001:
                    message.minimumEdition = 1001;
                    break;
                case "EDITION_2026":
                case 1002:
                    message.minimumEdition = 1002;
                    break;
                case "EDITION_UNSTABLE":
                case 9999:
                    message.minimumEdition = 9999;
                    break;
                case "EDITION_1_TEST_ONLY":
                case 1:
                    message.minimumEdition = 1;
                    break;
                case "EDITION_2_TEST_ONLY":
                case 2:
                    message.minimumEdition = 2;
                    break;
                case "EDITION_99997_TEST_ONLY":
                    break;
                case "EDITION_99998_TEST_ONLY":
                case 99998:
                case "EDITION_99999_TEST_ONLY":
                case 99999:
                    message.minimumEdition = 99999;
                case "EDITION_MAX":
                case 2147483647:
                    break;
                }
                switch (object.maximumEdition) {
                default:
                    if (typeof object.maximumEdition === "number") {
                        message.maximumEdition = object.maximumEdition;
                        ++long;
                        break;
                    }
                case 0:
                    message.maximumEdition = 0;
                case 900:
                    message.maximumEdition = 900;
                case 998:
                    message.maximumEdition = 998;
                    break;
                case "EDITION_PROTO3":
                case 999:
                    message.maximumEdition = 999;
                    break;
                case "EDITION_2023":
                case 1000:
                    message.maximumEdition = 1000;
                    break;
                case "EDITION_2024":
                case 1001:
                    message.maximumEdition = 1001;
                    break;
                case "EDITION_2026":
                case 1002:
                    message.maximumEdition = 1002;
                    break;
                case "EDITION_UNSTABLE":
                case 9999:
                    message.maximumEdition = 9999;
                    break;
                case "EDITION_1_TEST_ONLY":
                case 1:
                    message.maximumEdition = 1;
                    break;
                case "EDITION_2_TEST_ONLY":
                case 2:
                    message.maximumEdition = 2;
                    break;
                case "EDITION_99997_TEST_ONLY":
                case 99997:
                    message.maximumEdition = 99997;
                    break;
                case "EDITION_99998_TEST_ONLY":
                case 99998:
                    message.maximumEdition = 99998;
                    break;
                case "EDITION_99999_TEST_ONLY":
                case 99999:
                    message.maximumEdition = 99999;
                    break;
                case "EDITION_MAX":
                    break;
                }
            };
            return FeatureSetDefaults;
        })();
        return protobuf;
    })();
    return google;
})();
const FeatureSetDefaults = google.protobuf.FeatureSetDefaults;
FeatureSetDefaults.fromObject({ minimumEdition: "EDITION_2023", maximumEdition: 9999, defaults: [] }, 0);
shouldBe(JSON.stringify(FeatureSetDefaults.last), '{"defaults":[],"minimumEdition":1000,"maximumEdition":9999}');
FeatureSetDefaults.fromObject({ minimumEdition: 5, maximumEdition: "EDITION_PROTO3" }, 0);
shouldBe(JSON.stringify(FeatureSetDefaults.last), '{"defaults":[],"minimumEdition":5,"maximumEdition":999}');
FeatureSetDefaults.fromObject({ minimumEdition: "EDITION_MAX", maximumEdition: 900, defaults: [{}] }, 1);
shouldBe(JSON.stringify(FeatureSetDefaults.last), '{"defaults":[],"maximumEdition":998}');

})();
