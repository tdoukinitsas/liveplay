// ============================================================================
// media_names_test.cpp - standalone assertions for the media import rules.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed.
//
// The rule being pinned is #92: importing a file must never change the bytes
// behind a cue that already exists.
// ============================================================================
#include "liveplay/util/media_names.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace liveplay::util;

static int failures = 0;
static void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "   ", detail.c_str());
    if (!ok) ++failures;
}

static void write_file(const fs::path& p, const std::string& body) {
    std::ofstream f{p, std::ios::binary};
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
}

static std::string read_file(const fs::path& p) {
    std::ifstream f{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static PublishedMedia import_bytes(const fs::path& dir, const char* name, const std::string& body) {
    const MediaTempFile temp{media_temp_path(dir)};
    write_file(temp.path, body);
    return publish_media_file(temp.path, dir, name);
}

static std::size_t file_count(const fs::path& dir) {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator{dir},
                                                  fs::directory_iterator{}));
}

int main() {
    check("the first copy keeps its name", numbered_file_name("Intro.wav", 1) == "Intro.wav");
    check("a numbered name goes before the extension",
          numbered_file_name("Intro.wav", 2) == "Intro (2).wav");
    check("only the last extension is kept apart",
          numbered_file_name("Act.1.final.mp3", 3) == "Act.1.final (3).mp3");
    check("a name with no extension still numbers",
          numbered_file_name("Intro", 2) == "Intro (2)");

    const fs::path dir = fs::temp_directory_path() /
        ("liveplay-media-names-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);

    const auto first = import_bytes(dir, "Intro.wav", "first take");
    check("a free name is taken", first.path.filename() == "Intro.wav" && !first.reused);

    const auto second = import_bytes(dir, "Intro.wav", "a different file");
    check("a different file gets a numbered name",
          second.path.filename() == "Intro (2).wav" && !second.reused,
          second.path.filename().string());
    check("the first file's bytes are untouched (#92)", read_file(first.path) == "first take");
    check("the second file's bytes landed", read_file(second.path) == "a different file");

    const auto again = import_bytes(dir, "Intro.wav", "first take");
    check("the same bytes reuse the first copy", again.path == first.path && again.reused);

    const auto again2 = import_bytes(dir, "Intro.wav", "a different file");
    check("the same bytes reuse a numbered copy too", again2.path == second.path && again2.reused);

    const auto same_size = import_bytes(dir, "Intro.wav", "first tak3");
    check("equal size with different bytes is a different file",
          same_size.path.filename() == "Intro (3).wav" && !same_size.reused,
          same_size.path.filename().string());

    check("no temp files are left behind", file_count(dir) == 3,
          std::to_string(file_count(dir)));

    write_file(dir / "source.bin", "a different file");
    const auto found = find_identical_media(dir, "Intro.wav", dir / "source.bin");
    check("an identical copy is found without writing anything",
          found && *found == second.path);
    write_file(dir / "other.bin", "nothing like it");
    check("no identical copy reports none",
          !find_identical_media(dir, "Intro.wav", dir / "other.bin"));

    std::error_code ec;
    fs::remove_all(dir, ec);

    if (failures) std::printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
