// ============================================================================
// device_name_test.cpp — standalone assertions for normalise_device_name.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed.
//
// The normaliser decides whether a show's saved device name still refers to
// an interface that is plugged in. Too strict and a renumbered interface goes
// silent (the bug this exists for); too loose and two different outputs
// collapse into one. Both directions are pinned.
// ============================================================================
#include "liveplay/audio/device_name.hpp"

#include <cstdio>
#include <string>

using liveplay::audio::normalise_device_name;

static int failures = 0;
static void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "   ", detail.c_str());
    if (!ok) ++failures;
}
static bool same(const char* a, const char* b) {
    return normalise_device_name(a) == normalise_device_name(b);
}

int main() {
    // The reported case: Windows renumbered the interface.
    check("'(2- ' renumbering inside the parentheses is ignored",
          same("OUT 3-4 (BEHRINGER UMC 404HD 192k)", "OUT 3-4 (2- BEHRINGER UMC 404HD 192k)"));
    check("multi-digit renumbering too",
          same("Speakers (Realtek Audio)", "Speakers (12- Realtek Audio)"));
    check("a renumbering prefix at the very start is ignored",
          same("USB Audio Device", "3- USB Audio Device"));
    check("case is ignored", same("Headphones (Oculus Virtual Audio Device)",
                                  "headphones (OCULUS virtual audio device)"));
    check("runs of whitespace collapse", same("OUT 1-2  (BEHRINGER)", "OUT 1-2 (BEHRINGER)"));

    // What must stay distinct.
    check("different channel pairs of one interface stay different",
          !same("OUT 1-2 (2- BEHRINGER UMC 404HD 192k)", "OUT 3-4 (2- BEHRINGER UMC 404HD 192k)"));
    check("a leading channel range is not mistaken for a prefix",
          !same("3-4 Main", "4 Main"),
          normalise_device_name("3-4 Main"));
    check("digits inside the name are kept",
          normalise_device_name("OUT 3-4 (2- BEHRINGER UMC 404HD 192k)") ==
              "out 3-4 (behringer umc 404hd 192k)",
          normalise_device_name("OUT 3-4 (2- BEHRINGER UMC 404HD 192k)"));
    check("a digit-dash without a space is part of the name",
          !same("Speakers (2-Channel USB)", "Speakers (Channel USB)"));
    check("different devices stay different",
          !same("Speakers (Realtek USB Audio Front)", "Speakers (Blackmagic Intensity Pro 4K Audio)"));
    check("empty stays empty", normalise_device_name("").empty());

    std::printf(failures ? "\nFAILURES: %d\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
