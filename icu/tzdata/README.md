# ICU tzdata overlay

ICU release tarballs ship whatever IANA tzdata was current when that ICU
version was cut (75.1 = 2024a). IANA publishes several tzdata releases a year,
and shipping a stale one means Bun returns wrong wall-clock times for any zone
whose rules changed since: for example, tzdata 2024a still has Paraguay
observing DST, so `America/Asuncion` is reported as UTC-4 for half the year
when real Paraguay has been permanent UTC-3 since October 2024.

ICU supports overlaying newer tzdata onto an existing build via four resource
bundles that are binary-compatible back to ICU 4.4. These are the little-endian
builds of those bundles, vendored from
https://github.com/unicode-org/icu-data/tree/main/tzdata/icunew and injected
into the data package with `icupkg -a` during the ICU build step. All Bun
targets are little-endian, so only the `le` flavour is needed.

Current version: **2026c** (from unicode-org/icu-data@1c3d36e741bd).

## Updating

1. Pick the newest directory under `tzdata/icunew/` in unicode-org/icu-data.
2. Replace the four `.res` files here with the ones from its `44/le/`
   subdirectory (`zoneinfo64.res`, `metaZones.res`, `timezoneTypes.res`,
   `windowsZones.res`).
3. Update the version line above.

`ucal_getTZDataVersion()` reads the version string out of `zoneinfo64.res`, so
Bun's `process.versions.tz` reflects whatever is checked in here.
