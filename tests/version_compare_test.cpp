// version_compare_test.cpp — offline ordering assertions for the update check (A5.4).
#include "version_compare.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(cond) do { if (cond) printf("PASS: %s\n", #cond); else { printf("FAIL: %s\n", #cond); fails++; } } while (0)

int main()
{
  CHECK(CompareVersions("2.4.0", "2.5.0") < 0);
  CHECK(CompareVersions("2.4.0", "2.5.0-rc1") < 0);
  CHECK(CompareVersions("2.5.0-rc1", "2.5.0") < 0);
  CHECK(CompareVersions("2.5.0-rc1", "2.5.0-rc2") < 0);
  CHECK(CompareVersions("2.5.0-beta3", "2.5.0-rc1") < 0);
  CHECK(CompareVersions("2.5.0-alpha1", "2.5.0-beta1") < 0);
  CHECK(CompareVersions("2.5.0", "2.5.1") < 0);
  CHECK(CompareVersions("2.5.0", "2.10.0") < 0);          // numeric, not lexical
  CHECK(CompareVersions("v2.5.0", "2.5.0") == 0);          // leading v
  CHECK(CompareVersions("2.5.0", "2.5") == 0);             // missing patch = 0
  CHECK(CompareVersions("2.5.0-rc1", "2.4.9") > 0);
  CHECK(CompareVersions("dev", "2.5.0") == 0);             // unparseable = never an offer
  CHECK(CompareVersions("2.5.0-dev", "9.9.9") == 0);
  CHECK(CompareVersions("", "2.5.0") == 0);
  CHECK(CompareVersions("2.5.0-rc", "2.5.0-rc1") < 0);     // bare rc = rc0
  CHECK(IsPrerelease("2.5.0-rc1"));
  CHECK(!IsPrerelease("2.5.0"));
  CHECK(!IsPrerelease("garbage"));
  ParsedVersion v;
  CHECK(ParseVersion("v2.5.0-rc2", &v) && v.major == 2 && v.minor == 5 && v.patch == 0 && v.preRank == 2 && v.preNum == 2);
  CHECK(!ParseVersion("2.5.0-rc1-extra", &v));
  CHECK(!ParseVersion("2.x", &v));
  printf(fails ? "VERSION COMPARE: %d FAILED\n" : "VERSION COMPARE: ALL PASS\n", fails);
  return fails ? 1 : 0;
}
