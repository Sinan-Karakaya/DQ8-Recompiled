// The launcher's parts that need no window: hashing, the ISO reader, the hash
// database's JSON, build progress and version parsing, and child processes.
#include "launcher_iso.h"
#include "launcher_json.h"
#include "launcher_pipeline.h"
#include "launcher_process.h"
#include "launcher_sha256.h"
#include "launcher_tools.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace dq8::launcher;

namespace {
void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}

std::string sha(const std::string &text) {
    Sha256 hash;
    hash.update(text.data(), text.size());
    return hash.finishHex();
}

void sha256Vectors() {
    require(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    require(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    const std::string million(1000000u, 'a');
    require(sha(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "SHA-256 of a million a's");
    Sha256 pieces;
    for (size_t at = 0; at < million.size(); at += 997u)
        pieces.update(million.data() + at, std::min<size_t>(997u, million.size() - at));
    require(pieces.finishHex() == sha(million), "SHA-256 in odd-sized pieces");
}

// A directory record as ISO9660 lays it out: both-endian extent and size.
std::vector<uint8_t> record(uint32_t lba, uint32_t size, bool directory, const std::string &name) {
    std::vector<uint8_t> out(33u + name.size() + ((name.size() % 2u) == 0u ? 1u : 0u), 0u);
    out[0] = static_cast<uint8_t>(out.size());
    for (int i = 0; i < 4; ++i) {
        out[2 + i] = static_cast<uint8_t>(lba >> (8 * i));
        out[9 - i] = static_cast<uint8_t>(lba >> (8 * i));
        out[10 + i] = static_cast<uint8_t>(size >> (8 * i));
        out[17 - i] = static_cast<uint8_t>(size >> (8 * i));
    }
    out[25] = directory ? 0x02u : 0x00u;
    out[28] = 1u; // volume sequence number
    out[31] = 1u;
    out[32] = static_cast<uint8_t>(name.size());
    std::memcpy(out.data() + 33, name.data(), name.size());
    return out;
}

void writeSector(std::vector<uint8_t> &image, uint32_t lba, const std::vector<std::vector<uint8_t>> &records) {
    size_t at = size_t(lba) * 2048u;
    for (const auto &rec : records) {
        std::memcpy(image.data() + at, rec.data(), rec.size());
        at += rec.size();
    }
}

void isoReader() {
    const std::string cnf = "BOOT2 = cdrom0:\\SLUS_212.07;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
    std::string bin(3000u, '\0');
    for (size_t i = 0; i < bin.size(); ++i)
        bin[i] = static_cast<char>(i * 7u);
    std::vector<uint8_t> image(24u * 2048u, 0u);
    // Primary volume descriptor, then the set terminator.
    uint8_t *pvd = image.data() + 16u * 2048u;
    pvd[0] = 1u;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1u;
    std::memset(pvd + 40, ' ', 32);
    std::memcpy(pvd + 40, "TEST_DISC", 9);
    const auto root = record(18u, 2048u, true, std::string(1, '\0'));
    std::memcpy(pvd + 156, root.data(), 34u);
    image[17u * 2048u] = 255u;
    std::memcpy(image.data() + 17u * 2048u + 1u, "CD001", 5);
    writeSector(image, 18u,
                {record(18u, 2048u, true, std::string(1, '\0')), record(18u, 2048u, true, std::string(1, '\1')),
                 record(19u, 2048u, true, "BIN"), record(20u, static_cast<uint32_t>(cnf.size()), false, "SYSTEM.CNF;1")});
    writeSector(image, 19u,
                {record(19u, 2048u, true, std::string(1, '\0')), record(18u, 2048u, true, std::string(1, '\1')),
                 record(21u, static_cast<uint32_t>(bin.size()), false, "TEST.BIN;1")});
    std::memcpy(image.data() + 20u * 2048u, cnf.data(), cnf.size());
    std::memcpy(image.data() + 21u * 2048u, bin.data(), bin.size());

    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "dq8-launcher-tests";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path path = folder / "test.iso";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(image.data()),
                                               static_cast<std::streamsize>(image.size()));

    IsoImage iso;
    std::string error;
    require(iso.open(pathUtf8(path), error), "the image opens: " + error);
    require(iso.volumeId() == "TEST_DISC", "volume name, trimmed");
    require(iso.entries().size() == 3u, "three entries");
    const IsoEntry *system = iso.find("system.cnf");
    require(system && !system->directory && system->size == cnf.size(), "SYSTEM.CNF, found case-insensitively");
    const IsoEntry *test = iso.find("BIN/TEST.BIN");
    require(test && test->lba == 21u, "a file in a directory, without its ;1");
    std::string text;
    require(iso.read(*system, text, error) && text == cnf, "SYSTEM.CNF reads back");
    require(bootExecutable(text) == "SLUS_212.07", "the boot executable");

    uint64_t lastDone = 0u, lastTotal = 0u;
    const std::filesystem::path out = folder / "out";
    require(iso.extract(out,
                        [&](uint64_t done, uint64_t total) {
                            lastDone = done;
                            lastTotal = total;
                            return true;
                        },
                        error),
            "the tree extracts: " + error);
    require(lastDone == lastTotal && lastTotal == cnf.size() + bin.size(), "extraction reports every byte");
    std::ifstream copied(out / "BIN" / "TEST.BIN", std::ios::binary);
    std::string back((std::istreambuf_iterator<char>(copied)), std::istreambuf_iterator<char>());
    require(back == bin, "an extracted file matches byte for byte");

    // A crafted name that would climb out of the extraction folder.
    std::fill(image.begin() + 18 * 2048, image.begin() + 19 * 2048, uint8_t(0));
    writeSector(image, 18u,
                {record(18u, 2048u, true, std::string(1, '\0')), record(18u, 2048u, true, std::string(1, '\1')),
                 record(21u, static_cast<uint32_t>(bin.size()), false, "../ESCAPE.BIN;1")});
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
    require(!iso.open(pathUtf8(path), error), "a name with a path in it is refused");

    image.resize(10u * 2048u);
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
    require(!iso.open(pathUtf8(path), error), "a truncated image is refused");
    std::filesystem::remove_all(folder, ec);
}

void bootLines() {
    require(bootExecutable("boot2=cdrom0:\\SLES_539.74;1\n") == "SLES_539.74", "lower case, no spaces");
    require(bootExecutable("BOOT = cdrom0:\\SLPS_255.03;1\n").empty(), "a PS1 BOOT line is not a PS2 one");
    require(bootExecutable("").empty(), "no SYSTEM.CNF");
}

void jsonReader() {
    JsonValue value;
    std::string error;
    require(parseJson(R"({"iso": {"name": "DQ8_USA.iso", "sha256": "a3", "size": 4180148224},
                          "files": {"BIN/A.BIN": {"size": 2}}, "list": [1, true, null, "xé"]})",
                      value, error),
            "parses: " + error);
    require(value["iso"]["size"].number == 4180148224.0, "a large size survives");
    require(value["files"]["BIN/A.BIN"]["size"].number == 2.0, "nested objects");
    require(value["list"].array.size() == 4u && value["list"].array[3].string == "x\xC3\xA9", "arrays and escapes");
    require(value["missing"]["deeper"].kind == JsonValue::Kind::Null, "missing members read as null");
    require(!parseJson("{\"a\": }", value, error), "broken JSON is refused");
}

void progressAndVersions() {
    uint64_t done = 0u, total = 0u;
    require(parseNinjaProgress("[1234/12530] Building CXX object x.o", done, total) && done == 1234u &&
                total == 12530u,
            "a ninja progress line");
    require(!parseNinjaProgress("[x/1] nope", done, total) && !parseNinjaProgress("Linking", done, total),
            "other lines are not progress");
    require(parseVersion("cmake version 3.31.6") == std::vector<int>({3, 31, 6}), "CMake's version line");
    require(versionAtLeast(parseVersion("cmake version 3.31.6"), {3, 24}), "3.31 is at least 3.24");
    require(!versionAtLeast(parseVersion("Python 3.9.18"), {3, 10}), "3.9 is older than 3.10");
    require(versionAtLeast(parseVersion("Python 3.10.0"), {3, 10}), "3.10.0 is enough");
    require(parseVersion("1.12.1") == std::vector<int>({1, 12, 1}), "a bare version");
}

// The weights behind the compile bar: measured, a 10.8 MB function takes about
// an hour and a 2.6 MB one under a minute, against about a second for most.
void compileCosts() {
    require(compileCost("FUN_00100000_0x100000.cpp", 4000u) < 1.01, "a small function costs one unit");
    require(compileCost("FUN_00370c30_0x370c30.cpp", 10'772'527u) > 10000.0, "the largest one, about an hour");
    require(compileCost("FUN_00389498_0x389498.cpp", 2'580'000u) >= kLargeCompileCost, "2.6 MB counts as large");
    require(compileCost("register_functions.cpp", 10'634'706u) == 1.0, "a table is quick whatever its size");

    const std::string object = "src/runtime/CMakeFiles/dq8_generated.dir/__/__/build/generated/SLUS_212.07/";
    require(generatedKey("Building CXX object " + object + "FUN_1.cpp.o") == "SLUS_212.07/FUN_1.cpp",
            "a ninja line names its translated file");
    require(generatedKey("src\\runtime\\x.dir\\__\\build\\generated\\SLUS_212.07-overlays\\shop\\FUN_2.cpp.obj") ==
                "SLUS_212.07-overlays/shop/FUN_2.cpp",
            "backslashes and .obj too");
    require(generatedKey("Linking CXX executable src/runtime/dq8").empty(), "other steps name none");

    // A tree with one large file compiled before and one small one not yet.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dq8-launcher-plan";
    const std::filesystem::path generated = root / "generated", build = root / "game";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(generated / "SLUS_212.07", ec);
    std::filesystem::create_directories(build / object, ec);
    std::ofstream(generated / "SLUS_212.07" / "FUN_00200000_0x200000.cpp") << std::string(3'000'000u, ' ');
    std::ofstream(generated / "SLUS_212.07" / "FUN_00300000_0x300000.cpp") << "small";
    std::ofstream(build / object / "FUN_00200000_0x200000.cpp.o") << "object";
    std::filesystem::last_write_time(build / object / "FUN_00200000_0x200000.cpp.o",
                                     std::filesystem::last_write_time(generated / "SLUS_212.07" /
                                                                      "FUN_00200000_0x200000.cpp") +
                                         std::chrono::seconds(5),
                                     ec);
    CompilePlan plan;
    plan.scan(generated, build);
    require(plan.files.size() == 2u && plan.pending == 1u && plan.largeLeft == 0u,
            "an object newer than its source counts as compiled");
    require(plan.compiled > kLargeCompileCost && plan.total > plan.compiled, "costs add up by size");
    require(plan.finish("Building CXX object " + object + "FUN_00300000_0x300000.cpp.o") && plan.pending == 0u &&
                plan.compiled == plan.total,
            "a finished line marks its file");
    require(!plan.finish("Linking CXX executable src/runtime/dq8"), "a link is not a translated file");
    plan.forgetCompiled();
    require(plan.pending == 2u && plan.largeLeft == 1u && plan.compiled == 0.0, "and everything can start over");
    std::filesystem::remove_all(root, ec);
}

// Play is offered only for the launcher's own finished build of this workspace.
void builtMarker() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dq8-launcher-built";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path repo = root / "repo", workspace = root / "games";
    std::filesystem::create_directories(gamePath(repo).parent_path(), ec);
    std::filesystem::create_directories(extractedDisc(workspace), ec);
    std::ofstream(gamePath(repo)) << "game";
    require(!launcherBuilt(repo, workspace), "a game built by hand does not count");
    std::ofstream(repo / "build" / "game" / "launcher-built.txt") << pathUtf8(workspace) << "\n";
    require(!launcherBuilt(repo, workspace), "nor does one whose game files are missing");
    std::ofstream(extractedDisc(workspace) / "SLUS_212.07") << "elf";
    require(launcherBuilt(repo, workspace), "the launcher's build with its files");
    require(!launcherBuilt(repo, root / "elsewhere"), "but not for another workspace");
    std::filesystem::remove_all(root, ec);
}

void childProcess() {
#if !defined(_WIN32)
    std::vector<std::string> lines;
    std::string error;
    const std::atomic<bool> never{false};
    const int code = runProcess({"sh", "-c", "echo one; echo two 1>&2; exit 3"}, ChildEnvironment{},
                                [&](const std::string &line) { lines.push_back(line); }, never, error);
    require(code == 3, "the exit code comes back");
    require(lines.size() == 2u && lines[0] == "one" && lines[1] == "two", "stdout and stderr, line by line");
    require(runProcess({"dq8-no-such-command"}, ChildEnvironment{}, {}, never, error) != 0,
            "a missing command fails");
    std::atomic<bool> cancel{true};
    require(runProcess({"sleep", "30"}, ChildEnvironment{}, {}, cancel, error) == -1, "cancel stops a command");
    // The background sleep keeps the pipe open after its shell is gone.
    std::atomic<bool> later{false};
    const auto started = std::chrono::steady_clock::now();
    require(runProcess({"sh", "-c", "sleep 30 & echo started; sleep 30"}, ChildEnvironment{},
                       [&](const std::string &) { later = true; }, later, error) == -1,
            "cancel stops a command whose child holds the pipe");
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(10), "without waiting for the child");
#endif
}
} // namespace

int main() try {
    sha256Vectors();
    isoReader();
    bootLines();
    jsonReader();
    progressAndVersions();
    compileCosts();
    builtMarker();
    childProcess();
    std::puts("PASS: launcher hashing, ISO reader, JSON, progress, compile costs, versions and child processes");
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
