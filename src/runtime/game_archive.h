#pragma once

// Reads files out of the game's DATA.HD6 / DATA.DAT archive, the way the
// game's own file system finds them: by their path in the archive, with
// backslashes ("dbg\\evview2_1.txt").

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dq8 {

class GameArchive {
public:
    // `base` without the extension: ".../DATA" for DATA.HD6 and DATA.DAT.
    bool open(const std::string &base, std::string &error);
    bool read(const std::string &name, std::vector<uint8_t> &data) const;
    bool contains(const std::string &name) const { return m_files.count(name) != 0; }
    // Every name in the archive, in the index's order.
    const std::vector<std::string> &names() const { return m_names; }

private:
    struct Entry {
        uint64_t offset;
        uint32_t size;
    };
    std::string m_dataPath;
    std::unordered_map<std::string, Entry> m_files;
    std::vector<std::string> m_names;
};

} // namespace dq8
