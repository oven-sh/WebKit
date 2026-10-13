import { meta } from "../import-meta-first-evaluation.js";

export const beforeBody = meta();
export const fromArrow = () => import.meta;
export function* fromGenerator() {
    yield import.meta;
}
export const memberMeta = fromArrow();
