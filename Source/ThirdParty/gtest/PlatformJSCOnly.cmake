set(gtest_LIBRARY_TYPE STATIC)

# gtest includes <iostream>, which wtf/bun/BannedIncludes rejects in what ships.
list(APPEND gtest_DEFINITIONS BUN_ALLOW_IOSTREAM)
