#include "game_archive.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace dq8 {
namespace {

uint32_t u32At(const std::vector<uint8_t> &bytes, size_t offset) {
    uint32_t value = 0u;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

// The index is tools/hd6/hd6_extract.py's format: a pool of name fragments,
// names as LEB128 lists of fragment numbers, and one u64 per file holding the
// name's offset (17 bits), the data offset in KiB (21) and the size in words.
bool GameArchive::open(const std::string &base, std::string &error) {
    std::ifstream index(base + ".HD6", std::ios::binary);
    if (!index) {
        error = "cannot open " + base + ".HD6";
        return false;
    }
    const std::vector<uint8_t> hd6((std::istreambuf_iterator<char>(index)), std::istreambuf_iterator<char>());
    if (hd6.size() < 0x34u || std::memcmp(hd6.data(), "HD6", 4) != 0) {
        error = base + ".HD6 is not an HD6 index";
        return false;
    }
    const uint32_t poolOffset = u32At(hd6, 0x04), poolSize = u32At(hd6, 0x08);
    const uint32_t tokenCount = u32At(hd6, 0x0C);
    const uint32_t namesOffset = u32At(hd6, 0x14), namesSize = u32At(hd6, 0x18);
    const uint32_t fileCount = u32At(hd6, 0x24), tableOffset = u32At(hd6, 0x28);
    if (uint64_t(poolOffset) + poolSize > hd6.size() || uint64_t(namesOffset) + namesSize > hd6.size() ||
        uint64_t(tableOffset) + uint64_t(fileCount) * 8u > hd6.size()) {
        error = base + ".HD6 is truncated";
        return false;
    }

    std::vector<std::string> tokens;
    tokens.reserve(tokenCount);
    for (uint32_t at = poolOffset, end = poolOffset + poolSize; at < end && tokens.size() < tokenCount;) {
        const char *text = reinterpret_cast<const char *>(hd6.data() + at);
        const size_t length = strnlen(text, end - at);
        tokens.emplace_back(text, length);
        at += static_cast<uint32_t>(length) + 1u;
    }

    m_files.clear();
    m_names.clear();
    for (uint32_t i = 0; i < fileCount; ++i) {
        uint64_t packed = 0;
        std::memcpy(&packed, hd6.data() + tableOffset + i * 8u, sizeof(packed));
        const uint32_t nameAt = static_cast<uint32_t>(packed & 0x1FFFFu);
        const uint64_t offset = ((packed >> 17) & 0x1FFFFFu) * 1024u;
        const uint32_t size = static_cast<uint32_t>(packed >> 38) * 4u;
        std::string name;
        for (uint32_t at = namesOffset + nameAt; at < namesOffset + namesSize && hd6[at] != 0u;) {
            uint32_t token = 0u, shift = 0u;
            uint8_t byte = 0u;
            do {
                byte = hd6[at++];
                token |= uint32_t(byte & 0x7Fu) << shift;
                shift += 7u;
            } while ((byte & 0x80u) != 0u && at < namesOffset + namesSize);
            if (token < tokens.size())
                name += tokens[token];
        }
        if (name.empty())
            continue;
        m_files[name] = {offset, size};
        m_names.push_back(std::move(name));
    }
    m_dataPath = base + ".DAT";
    return true;
}

bool GameArchive::read(const std::string &name, std::vector<uint8_t> &data) const {
    const auto it = m_files.find(name);
    if (it == m_files.end())
        return false;
    std::ifstream file(m_dataPath, std::ios::binary);
    if (!file.seekg(static_cast<std::streamoff>(it->second.offset)))
        return false;
    data.resize(it->second.size);
    file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(file.gcount()));
    return !data.empty();
}

} // namespace dq8
