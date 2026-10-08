// An ISO9660 disc image: its file tree, small files read whole, and extraction
// of the whole tree. PS2 DVDs carry ISO9660 beside UDF, with upper-case names,
// ";1" versions and single-extent files, which is all this reads.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dq8::launcher {

struct IsoEntry {
    std::string path; // "BIN/BATTLE.BIN": '/'-separated, without the ";1"
    uint32_t lba = 0;
    uint64_t size = 0;
    bool directory = false;
};

class IsoImage {
public:
    bool open(const std::string &path, std::string &error);
    const std::vector<IsoEntry> &entries() const { return m_entries; }
    const std::string &volumeId() const { return m_volumeId; }
    uint64_t imageSize() const { return m_imageSize; }
    // Case-insensitive, as PS2 games look their files up.
    const IsoEntry *find(const std::string &path) const;
    bool read(const IsoEntry &entry, std::string &out, std::string &error) const;
    // Writes every file under `destination`; progress(done, total) returning
    // false stops it.
    bool extract(const std::filesystem::path &destination,
                 const std::function<bool(uint64_t, uint64_t)> &progress, std::string &error) const;

private:
    bool walk(uint32_t lba, uint32_t size, const std::string &prefix, int depth, std::string &error);

    std::string m_path;
    std::string m_volumeId;
    uint64_t m_imageSize = 0;
    std::vector<IsoEntry> m_entries;
};

// The executable SYSTEM.CNF boots: "BOOT2 = cdrom0:\SLUS_212.07;1" gives
// "SLUS_212.07"; empty when there is no BOOT2 line.
std::string bootExecutable(const std::string &systemCnf);

// A UTF-8 path, as SDL hands them out, for the filesystem library on every
// platform (on Windows a narrow string would be read in the ANSI code page).
std::filesystem::path utf8Path(const std::string &path);
std::string pathUtf8(const std::filesystem::path &path);

} // namespace dq8::launcher
