// version_compare.h — release version ordering for the update check (A5.4).
//
// "2.5.0", "v2.5.0-rc1", "2.5.0-beta2": numeric major.minor.patch, then a
// prerelease rank (alpha < beta < rc < final) with its own number (rc1 < rc2).
// Pure, header-only: shared by the update checker and the offline test.
#pragma once
#include <cstdlib>
#include <cstring>
#include <cctype>

struct ParsedVersion {
  int major = 0, minor = 0, patch = 0;
  int preRank = 3;   // 0 alpha, 1 beta, 2 rc, 3 final release
  int preNum = 0;
};

// False for anything that is not <digits>.<digits>[.<digits>][-<tag><n>].
inline bool ParseVersion(const char* s, ParsedVersion* out)
{
  if (!s || !out) return false;
  if (*s == 'v' || *s == 'V') s++;
  ParsedVersion v;
  char* end = nullptr;
  if (!isdigit((unsigned char)*s)) return false;
  v.major = (int)strtol(s, &end, 10);
  if (*end != '.') return false;
  s = end + 1;
  if (!isdigit((unsigned char)*s)) return false;
  v.minor = (int)strtol(s, &end, 10);
  s = end;
  if (*s == '.') {
    s++;
    if (!isdigit((unsigned char)*s)) return false;
    v.patch = (int)strtol(s, &end, 10);
    s = end;
  }
  if (*s == '-' || *s == '.') {
    s++;
    if (!strncmp(s, "alpha", 5)) { v.preRank = 0; s += 5; }
    else if (!strncmp(s, "beta", 4)) { v.preRank = 1; s += 4; }
    else if (!strncmp(s, "rc", 2)) { v.preRank = 2; s += 2; }
    else return false;
    if (*s == '.' || *s == '-') s++;
    if (isdigit((unsigned char)*s)) v.preNum = (int)strtol(s, &end, 10), s = end;
  }
  if (*s != '\0') return false;
  *out = v;
  return true;
}

// <0 when a is older than b, 0 when equal, >0 when newer. Unparseable input
// compares as equal (never an offer).
inline int CompareVersions(const char* a, const char* b)
{
  ParsedVersion va, vb;
  if (!ParseVersion(a, &va) || !ParseVersion(b, &vb)) return 0;
  if (va.major != vb.major) return va.major - vb.major;
  if (va.minor != vb.minor) return va.minor - vb.minor;
  if (va.patch != vb.patch) return va.patch - vb.patch;
  if (va.preRank != vb.preRank) return va.preRank - vb.preRank;
  return va.preNum - vb.preNum;
}

inline bool IsPrerelease(const char* s)
{
  ParsedVersion v;
  return ParseVersion(s, &v) && v.preRank < 3;
}
