// The launcher's parts that need no window: hashing, the ISO reader, the hash
// database's JSON, build progress and version parsing, child processes, and
// paths under a user folder whose name is not ASCII.
#include "launcher_iso.h"
#include "launcher_json.h"
#include "launcher_payload.h"
#include "launcher_pipeline.h"
#include "launcher_process.h"
#include "launcher_sha256.h"
#include "launcher_tools.h"
#include "ui/ui_settings.h"

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
#include <thread>
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

// The weights behind the compile bar: measured before the function map was
// fixed, a 10.8 MB function took about an hour and a 2.6 MB one under a minute,
// against a fraction of a second for most.
void compileCosts() {
    require(compileCost("FUN_00100000_0x100000.cpp", 4000u) < 1.01, "a small function costs one unit");
    require(compileCost("FUN_00370c30_0x370c30.cpp", 10'772'527u) > 10000.0, "10.8 MB, about an hour");
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

// A release's payload: found from its manifest, unpacked once per version,
// never over a folder the launcher did not make.
void payloadUnpacking() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dq8-launcher-payload";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path payloadDir = root / "payload";
    for (const char *dir : {"source/config", "fetch/spirv_cross-src", "deps/lib/pkgconfig", "tools/python/bin",
                            "tools/cmake/bin", "tools/ninja/bin", "tools/pkgconf/bin"})
        std::filesystem::create_directories(payloadDir / dir, ec);
    std::ofstream(payloadDir / "source" / "setup.py") << "# setup";
    std::ofstream(payloadDir / "fetch" / "spirv_cross-src" / "CMakeLists.txt") << "project(x)";
    std::ofstream(payloadDir / "deps" / "lib" / "pkgconfig" / "sdl3.pc") << "Name: SDL3";
    std::ofstream(payloadDir / "tools" / "python" / "bin" / "python3") << "python";
    const auto manifest = [&](const char *version) {
        std::ofstream(payloadDir / "payload.json", std::ios::trunc)
            << R"({"version": ")" << version << R"(", "platform": "test", "paths": {"source": "source",
               "fetch": "fetch", "deps": "deps", "python": "tools/python/bin/python3", "cmake": "tools/cmake/bin",
               "ninja": "tools/ninja/bin", "pkgconf": "tools/pkgconf/bin/pkgconf",
               "recompiler": "tools/ps2recomp/ps2_recomp"},
               "versions": {"SDL3": "3.4.16", "CMake": "4.4.4"}, "fetched": ["spirv_cross-src"]})";
    };
    manifest("v1");
    std::string error;
    std::optional<Payload> payload = loadPayload(payloadDir, error);
    require(payload && payload->version == "v1", "a payload loads from its manifest: " + error);
    require(payload->versions.size() == 2u && payload->versions[0].first == "CMake",
            "its tools in the Tools page's order");
    require(payload->recompiler.empty(), "a recompiler the payload names but lacks is built instead");
    std::filesystem::create_directories(payloadDir / "tools" / "ps2recomp", ec);
    std::ofstream(payloadDir / "tools" / "ps2recomp" / "ps2_recomp") << "recompiler";
    payload = loadPayload(payloadDir, error);
    require(payload && payload->recompiler == payloadDir / "tools" / "ps2recomp" / "ps2_recomp",
            "the payload's own recompiler is used where it has one");
    const std::filesystem::path repo = root / "games" / "DQ8Recomp-source";
    const std::vector<std::string> args = payloadCMakeArgs(*payload, repo);
    require(std::find(args.begin(), args.end(),
                      "-DFETCHCONTENT_SOURCE_DIR_SPIRV_CROSS=" +
                          (repo / "build" / "fetch" / "spirv_cross-src").generic_string()) != args.end(),
            "each fetched source stands in for its git clone");
#if defined(_WIN32)
    const char *ninja = "ninja.exe";
#else
    const char *ninja = "ninja";
#endif
    require(std::find(args.begin(), args.end(),
                      "-DCMAKE_MAKE_PROGRAM=" + (payloadDir / "tools" / "ninja" / "bin" / ninja).generic_string()) !=
                args.end(),
            "the payload's own ninja, not one cached from a release unpacked elsewhere");

    require(unpackPayload(*payload, repo, {}, error) == Unpacked::Copied, "the first unpack copies: " + error);
    require(std::filesystem::exists(repo / "setup.py") && std::filesystem::exists(repo / "build" / "deps" / "lib"),
            "the source tree and the libraries land in the workspace");
    require(unpackPayload(*payload, repo, {}, error) == Unpacked::AlreadyThere, "the same version is not copied again");
    std::ofstream(repo / "build" / "kept.txt") << "a build";
    manifest("v2");
    payload = loadPayload(payloadDir, error);
    require(unpackPayload(*payload, repo, {}, error) == Unpacked::Copied &&
                std::filesystem::exists(repo / "build" / "kept.txt"),
            "a new version replaces the source but keeps build/");

    const std::filesystem::path checkout = root / "checkout";
    std::filesystem::create_directories(checkout / ".git", ec);
    require(unpackPayload(*payload, checkout, {}, error) == Unpacked::Failed && std::filesystem::exists(checkout / ".git"),
            "a folder the launcher did not make is left alone");
    std::filesystem::remove_all(root, ec);
}

// Play is offered only for the launcher's own finished build of this
// workspace, release and disc.
void builtMarker() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dq8-launcher-built";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path repo = root / "repo", workspace = root / "games";
    std::filesystem::create_directories(gamePath(repo).parent_path(), ec);
    std::filesystem::create_directories(extractedDisc(workspace), ec);
    std::ofstream(gamePath(repo)) << "game";
    const BuiltFrom from{workspace, "a1b2c3d", root / "DQ8.iso"};
    require(!launcherBuilt(repo, from), "a game built by hand does not count");
    markBuilt(repo, from);
    require(!launcherBuilt(repo, from), "nor does one whose game files are missing");
    std::ofstream(extractedDisc(workspace) / "SLUS_212.07") << "elf";
    require(launcherBuilt(repo, from), "the launcher's build with its files");
    require(!launcherBuilt(repo, {root / "elsewhere", from.version, from.disc}), "but not for another workspace");
    require(!launcherBuilt(repo, {workspace, "e4f5a6b", from.disc}), "nor for another release");
    require(!launcherBuilt(repo, {workspace, from.version, root / "other.iso"}), "nor for another disc");
    std::filesystem::remove_all(root, ec);
}

// On Windows, a build CMake configured with anything but Visual Studio's cl
// starts over.
void configuredCompiler() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dq8-launcher-compiler";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path build = root / "build", cl = root / "MSVC" / "14.44" / "CL.exe",
                                clang = root / "LLVM" / "clang++.exe";
    for (const std::filesystem::path &dir : {build, cl.parent_path(), clang.parent_path()})
        std::filesystem::create_directories(dir, ec);
    std::ofstream(cl) << "cl";
    std::ofstream(clang) << "clang";
    require(!configuredWith(build, "cl"), "a build not configured yet");
    const auto cache = [&](const std::filesystem::path &c, const std::filesystem::path &cxx) {
        std::ofstream(build / "CMakeCache.txt", std::ios::trunc)
            << "CMAKE_C_COMPILER:FILEPATH=" << c.generic_string() << "\nCMAKE_C_COMPILER_LAUNCHER:STRING=\n"
            << "CMAKE_CXX_COMPILER:FILEPATH=" << cxx.generic_string() << "\n";
    };
    cache(cl, cl);
    require(configuredWith(build, "cl"), "Visual Studio's cl, whatever its case");
    cache(cl, clang);
    require(!configuredWith(build, "cl"), "clang is another compiler");
    cache(root / "MSVC" / "14.38" / "cl.exe", root / "MSVC" / "14.38" / "cl.exe");
    require(!configuredWith(build, "cl"), "and so is a cl an update removed");
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

// Installers write to a file, which outlives the launcher, rather than a pipe.
void childProcessToFile() {
    const std::filesystem::path log = std::filesystem::temp_directory_path() / "dq8-launcher-log" / "install.log";
    std::error_code ec;
    std::filesystem::remove_all(log.parent_path(), ec);
#if defined(_WIN32)
    const std::vector<std::string> command = {"cmd", "/c", "echo one& echo two& exit /b 3"};
#else
    const std::vector<std::string> command = {"sh", "-c", "echo one; sleep 0.3; echo two 1>&2; exit 3"};
#endif
    std::vector<std::string> lines;
    std::string error;
    std::atomic<bool> stop{false};
    const int code = runProcessToFile(command, ChildEnvironment{}, log,
                                      [&](const std::string &line) { lines.push_back(line); }, stop, error);
    require(code == 3, "the exit code comes back with the output in a file: " + error);
    require(lines.size() == 2u && lines[0] == "one" && lines[1] == "two", "the file's lines, in order");
    const auto logText = [&] {
        std::ifstream file(log, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    require(logText().find("one") != std::string::npos && logText().find("two") != std::string::npos,
            "and the file keeps them");
    require(runProcessToFile({"dq8-no-such-command"}, ChildEnvironment{}, log, {}, stop, error) != 0,
            "a missing command fails");

    // Closing the launcher stops the watching, not the child.
    stop = true;
#if defined(_WIN32)
    const std::vector<std::string> later = {"cmd", "/c", "ping -n 2 127.0.0.1 >nul & echo late"};
#else
    const std::vector<std::string> later = {"sh", "-c", "sleep 1; echo late"};
#endif
    const auto started = std::chrono::steady_clock::now();
    require(runProcessToFile(later, ChildEnvironment{}, log, {}, stop, error) == -1, "stop ends the watching");
    require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500), "at once");
    bool wrote = false;
    while (!wrote && std::chrono::steady_clock::now() - started < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wrote = logText().find("late") != std::string::npos;
    }
    require(wrote, "and the child still writes its line");
    std::filesystem::remove_all(log.parent_path(), ec);
}

// A child gets one PATH, the extra directories before the launcher's own, and
// what the launcher sets replaces what it has, however it is spelled.
void childEnvironment() {
    const char *own = SDL_getenv_unsafe("PATH");
    require(own != nullptr, "the tests have a PATH");
    const std::string path = own;
#if defined(_WIN32)
    // A player's Windows spells it Path, which SDL, unlike Windows, tells
    // apart from PATH; a runner's may spell it either way.
    SDL_unsetenv_unsafe("PATH");
    SDL_setenv_unsafe("Path", path.c_str(), 1);
    SDL_setenv_unsafe("Dq8_Launcher_Test", "one", 1);
    const std::string extra = "C:\\dq8-extra";
    const std::string expected = extra + ";" + path;
    const std::vector<std::string> printEnvironment = {"cmd", "/d", "/c", "set"};
    const auto is = [](const std::string &name, const char *wanted) {
        return SDL_strcasecmp(name.c_str(), wanted) == 0;
    };
#else
    const std::string extra = "/dq8-extra";
    const std::string expected = extra + ":" + path;
    const std::vector<std::string> printEnvironment = {"sh", "-c", "env"};
    const auto is = [](const std::string &name, const char *wanted) { return name == wanted; };
#endif
    const ChildEnvironment environment{{extra}, {{"DQ8_LAUNCHER_TEST", "two"}}};
    std::vector<std::string> paths, tests;
    std::string error;
    const std::atomic<bool> never{false};
    require(runProcess(printEnvironment, environment,
                       [&](const std::string &line) {
                           const size_t equals = line.find('=');
                           if (equals == std::string::npos)
                               return;
                           const std::string name = line.substr(0, equals);
                           if (is(name, "PATH"))
                               paths.push_back(line.substr(equals + 1u));
                           if (is(name, "DQ8_LAUNCHER_TEST"))
                               tests.push_back(line.substr(equals + 1u));
                       },
                       never, error) == 0,
            "the child prints its environment: " + error);
    require(paths.size() == 1u, "the child has one PATH, not " + std::to_string(paths.size()));
    require(paths[0] == expected, "the extra directories, then the launcher's own PATH: " + paths[0]);
    require(tests.size() == 1u && tests[0] == "two", "a variable the launcher sets reaches the child once");
#if defined(_WIN32)
    // How vcvars64.bat finds the Windows SDK. With only the extra directories
    // in PATH it could not, and CMake then found cl.exe but neither rc nor mt.
    require(runProcess({"cmd", "/d", "/c", "reg", "query", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion", "/v",
                        "ProgramFilesDir"},
                       environment, {}, never, error) == 0,
            "cmd.exe finds Windows' own commands: " + error);
#endif
}

// A Portuguese and a Greek name, in UTF-8: no single ANSI code page holds both,
// and a player's folder may be named either way.
std::filesystem::path unicodeFolder() {
    return std::filesystem::temp_directory_path() / utf8Path("dq8-launcher-Jo\xc3\xa3o-\xce\x96\xcf\x89\xce\xae");
}

// The game's settings live under the user's folder, by the UTF-8 path SDL gives.
void settingsUnderUnicodeFolder() {
    const std::filesystem::path folder = unicodeFolder();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    const std::string path = pathUtf8(folder / "settings.ini");
    dq8::ui::Settings settings;
    settings.volume = 7;
    require(dq8::ui::saveSettings(path, settings) && std::filesystem::is_regular_file(folder / "settings.ini", ec),
            "settings save under " + path);
    dq8::ui::Settings loaded;
    require(dq8::ui::loadSettings(path, loaded) && loaded.volume == 7, "and load from there");
    std::filesystem::remove_all(folder, ec);
}

// The build's commands run from a batch file that sets up Visual Studio first,
// and a path under a user folder that is not ASCII reaches CMake whole.
void compilerEnvironment() {
#if defined(_WIN32)
    const std::filesystem::path folder = unicodeFolder();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    ChildEnvironment environment;
    const std::vector<std::string> command =
        withCompilerEnvironment({"cmake", "-E", "make_directory", pathUtf8(folder / "made")}, folder, environment);
    std::string error;
    const std::atomic<bool> never{false};
    require(runProcess(command, environment, {}, never, error) == 0, "CMake runs after vcvars64.bat: " + error);
    require(std::filesystem::is_directory(folder / "made", ec), "and gets the path it was given");
    std::filesystem::remove_all(folder, ec);
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
    payloadUnpacking();
    builtMarker();
    configuredCompiler();
    childProcess();
    childProcessToFile();
    childEnvironment();
    settingsUnderUnicodeFolder();
    compilerEnvironment();
    std::puts("PASS: launcher hashing, ISO reader, JSON, progress, compile costs, versions, child processes and "
              "their environment, paths under a non-ASCII folder");
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
