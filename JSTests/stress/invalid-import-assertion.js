// Bun: a type JSC does not know is one the host loads (ScriptFetchParameters::parseType() gives Type::HostDefined), and
// the shell loads the file as it would without one, so this resolves; upstream rejects it.
import("./resources/empty.js", { with: { type: "<invalid>" } }).then(function () {}, $vm.abort);
