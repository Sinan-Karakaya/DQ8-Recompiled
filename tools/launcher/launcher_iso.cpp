#include "launcher_iso.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <set>
#include <string_view>
#include <system_error>

namespace dq8::launcher {
namespace {
constexpr uint64_t kSector = 2048u;
constexpr uint32_t kPrimaryVolume = 16u;

uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return text;
}

std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1u);
}

bool readAt(std::ifstream &file, uint64_t offset, void *out, size_t size) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(static_cast<char *>(out), static_cast<std::streamsize>(size));
    return static_cast<size_t>(file.gcount()) == size;
}
} // namespace

std::filesystem::path utf8Path(const std::string &path) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
}

std::string pathUtf8(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

bool IsoImage::open(const std::string &path, std::string &error) {
    m_path = path;
    m_entries.clear();
    std::error_code ec;
    m_imageSize = std::filesystem::file_size(utf8Path(path), ec);
    if (ec) {
        error = "The file cannot be read: " + ec.message();
        return false;
    }
    std::ifstream file(utf8Path(path), std::ios::binary);
    uint8_t volume[kSector];
    if (!file || !readAt(file, kPrimaryVolume * kSector, volume, sizeof(volume))) {
        error = "This file is too small to be a disc image.";
        return false;
    }
    if (volume[0] != 1u || std::string(reinterpret_cast<const char *>(volume + 1), 5) != "CD001") {
        error = "This is not an ISO disc image.";
        return false;
    }
    m_volumeId = trim(std::string(reinterpret_cast<const char *>(volume + 40), 32));
    const uint8_t *root = volume + 156;
    return walk(le32(root + 2), le32(root + 10), {}, 0, error);
}

bool IsoImage::walk(uint32_t lba, uint32_t size, const std::string &prefix, int depth, std::string &error) {
    if (depth > 32 || uint64_t(lba) * kSector + size > m_imageSize) {
        error = "The disc image's directory tree is damaged.";
        return false;
    }
    std::ifstream file(utf8Path(m_path), std::ios::binary);
    std::vector<uint8_t> data(size);
    if (!readAt(file, uint64_t(lba) * kSector, data.data(), data.size())) {
        error = "The disc image ends early; it may be incomplete.";
        return false;
    }
    for (size_t pos = 0; pos < data.size();) {
        const size_t length = data[pos];
        if (length == 0u) {
            // Records never straddle sectors; the rest of this one is padding.
            pos = (pos / kSector + 1u) * kSector;
            continue;
        }
        if (length < 34u || pos + length > data.size()) {
            error = "The disc image's directory tree is damaged.";
            return false;
        }
        const uint8_t *record = data.data() + pos;
        pos += length;
        const uint8_t flags = record[25];
        const size_t nameLength = record[32];
        if (33u + nameLength > length)
            return error = "The disc image's directory tree is damaged.", false;
        std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
        if (name == std::string(1, '\0') || name == std::string(1, '\1'))
            continue; // "." and ".."
        if (flags & 0x80u) {
            error = "The disc image uses multi-extent files, which this launcher does not read.";
            return false;
        }
        if (const auto semicolon = name.find(';'); semicolon != std::string::npos)
            name.resize(semicolon);
        if (!name.empty() && name.back() == '.')
            name.pop_back();
        // Names become host paths on extraction, so none may climb out of it.
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of(std::string_view("/\\:\0", 4)) != std::string::npos)
            return error = "The disc image's directory tree is damaged.", false;
        IsoEntry entry{prefix + name, le32(record + 2), le32(record + 10), (flags & 0x02u) != 0u};
        m_entries.push_back(entry);
        if (entry.directory &&
            !walk(entry.lba, static_cast<uint32_t>(entry.size), entry.path + "/", depth + 1, error))
            return false;
    }
    return true;
}

const IsoEntry *IsoImage::find(const std::string &path) const {
    const std::string wanted = upper(path);
    for (const IsoEntry &entry : m_entries)
        if (upper(entry.path) == wanted)
            return &entry;
    return nullptr;
}

bool IsoImage::read(const IsoEntry &entry, std::string &out, std::string &error) const {
    std::ifstream file(utf8Path(m_path), std::ios::binary);
    out.assign(entry.size, '\0');
    if (!readAt(file, uint64_t(entry.lba) * kSector, out.data(), out.size())) {
        error = "The disc image ends early; it may be incomplete.";
        return false;
    }
    return true;
}

bool IsoImage::extract(const std::filesystem::path &destination,
                       const std::function<bool(uint64_t, uint64_t)> &progress, std::string &error) const {
    uint64_t total = 0u, done = 0u;
    for (const IsoEntry &entry : m_entries)
        total += entry.directory ? 0u : entry.size;
    std::ifstream image(utf8Path(m_path), std::ios::binary);
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    constexpr size_t kChunk = 4u << 20;
    std::unique_ptr<char[]> buffer(new char[kChunk]);
    for (const IsoEntry &entry : m_entries) {
        const std::filesystem::path target = destination / utf8Path(entry.path);
        if (entry.directory) {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Cannot write " + pathUtf8(target);
            return false;
        }
        image.clear();
        image.seekg(static_cast<std::streamoff>(uint64_t(entry.lba) * kSector));
        for (uint64_t left = entry.size; left > 0u;) {
            const size_t step = static_cast<size_t>(std::min<uint64_t>(left, kChunk));
            image.read(buffer.get(), static_cast<std::streamsize>(step));
            if (static_cast<size_t>(image.gcount()) != step) {
                error = "The disc image ends early; it may be incomplete.";
                return false;
            }
            if (!out.write(buffer.get(), static_cast<std::streamsize>(step))) {
                error = "Cannot write " + pathUtf8(target) + ". Is the disk full?";
                return false;
            }
            left -= step;
            done += step;
            if (progress && !progress(done, total)) {
                error = "Cancelled.";
                return false;
            }
        }
    }
    return true;
}

std::string bootExecutable(const std::string &systemCnf) {
    size_t start = 0;
    while (start < systemCnf.size()) {
        size_t end = systemCnf.find('\n', start);
        if (end == std::string::npos)
            end = systemCnf.size();
        const std::string line = trim(systemCnf.substr(start, end - start));
        start = end + 1u;
        if (upper(line).rfind("BOOT2", 0) != 0u)
            continue;
        std::string value = trim(line.substr(line.find('=') == std::string::npos ? line.size() : line.find('=') + 1u));
        if (const auto colon = value.find(':'); colon != std::string::npos)
            value.erase(0, colon + 1u);
        while (!value.empty() && (value.front() == '\\' || value.front() == '/'))
            value.erase(0, 1);
        if (const auto semicolon = value.find(';'); semicolon != std::string::npos)
            value.resize(semicolon);
        return value;
    }
    return {};
}

} // namespace dq8::launcher
