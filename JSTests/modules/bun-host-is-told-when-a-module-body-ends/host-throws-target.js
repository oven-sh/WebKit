import "./host-throws-flag.js";
import { log } from "./log.js";

await null;
log.push("body ends, then the host throws");
