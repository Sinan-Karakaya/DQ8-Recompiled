#include "game_jump.h"

#include "game_archive.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dq8 {
namespace {
// Handlers in the SLUS_212.07 script command table (0x395DA0).
constexpr uint32_t kCommandSetStoryPoint = 0x1B3A30u; // command 0x588
constexpr uint32_t kCommandChangeMap = 0x1CF3F0u;     // command 0x14
// The field loop's mode (gp-0x76C8) and the game's setter for it. Mode 1 is
// walking around; in mode 2 the loop runs the event script and then acts on
// what it asked for, such as a change of map (FUN_1A2630).
constexpr uint32_t kFieldMode = 0x3D20A8u;
constexpr uint32_t kSetFieldMode = 0x17BB70u;
constexpr uint32_t kFieldModeWalking = 1u;
constexpr uint32_t kFieldModeEvent = 2u;
// The party's records: 0x409830 is the block the game's party functions take,
// with each character's 0x68-byte record at +0x40 (hero, Yangus, Jessica,
// Angelo): max HP +0, HP +4, max MP +8, MP +0xC, level - 1 +0x10 (u16),
// EXP +0x18. Each one's EXP for every level is a 100-entry table at 0x41D19C.
constexpr uint32_t kParty = 0x409830u;
constexpr uint32_t kPartyRecords = kParty + 0x40u;
constexpr uint32_t kRecordBytes = 0x68u;
constexpr uint32_t kPartySize = 4u;
constexpr uint32_t kExpTables = 0x41D19Cu;
constexpr uint32_t kExpTableBytes = 0x190u;
constexpr uint32_t kTopLevelIndex = 98u; // level 99
constexpr uint32_t kLevelUp = 0x203D80u;    // (party, character, 0): one level and its stat gains
constexpr uint32_t kLearnSpells = 0x202650u; // (character, record, out, kind 1/2): what the level teaches
constexpr uint32_t kAddExp = 0x204240u;     // (party, character, amount)
constexpr uint32_t kHealHp = 0x2042F0u;     // (party, character, amount), up to max HP
constexpr uint32_t kHealMp = 0x204280u;     // (party, character, amount), up to max MP
// The field's random-encounter check (0x41FFD0 is its state): true when the
// encounter countdown has run out. The field loop starts a battle on it.
constexpr uint32_t kEncounterCheck = 0x2FA700u;
// Guest memory for command arguments: a ring of slots, one per queued call,
// each with the {type, value} pairs and then the strings they point to.
constexpr uint32_t kBufferBytes = 16u * 1024u;
constexpr uint32_t kSlotBytes = 128u;
constexpr uint32_t kMaxArgs = 4u;
constexpr uint32_t kMaxText = 24u;
// Script argument types.
constexpr uint32_t kArgInt = 0u;
constexpr uint32_t kArgString = 2u;
// The map script entry that arrives on a map the usual way.
constexpr int kArriveProgram = 100;

// The debug lists are Shift-JIS text with English in it; keeps the ASCII and
// the few full-width marks the English uses.
std::string toAscii(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t byte = static_cast<uint8_t>(text[i]);
        if (byte < 0x80u) {
            if (byte != '\r')
                out += static_cast<char>(byte);
        } else if ((byte >= 0x81u && byte <= 0x9Fu) || (byte >= 0xE0u && byte <= 0xFCu)) {
            const uint8_t next = i + 1 < text.size() ? static_cast<uint8_t>(text[i + 1]) : 0u;
            ++i;
            const uint16_t pair = static_cast<uint16_t>(byte << 8 | next);
            out += pair == 0x8169u ? "(" : pair == 0x816Au ? ")" : pair == 0x8140u ? " " : pair == 0x81A6u ? "*" : "";
        }
    }
    return out;
}

std::vector<std::string> splitLines(const std::vector<uint8_t> &bytes) {
    std::vector<std::string> lines;
    std::string line;
    for (uint8_t byte : bytes) {
        if (byte == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else if (byte != '\r' && byte != 0u) {
            line += static_cast<char>(byte);
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

// "@100\nvalue\n@101\nvalue..." message files, as {id: value}.
std::map<int, std::string> readMessages(const std::vector<uint8_t> &bytes) {
    std::map<int, std::string> messages;
    int id = -1;
    for (const std::string &line : splitLines(bytes)) {
        if (!line.empty() && line[0] == '@')
            id = std::atoi(line.c_str() + 1);
        else if (id >= 0 && !messages.count(id))
            messages[id] = toAscii(line);
    }
    return messages;
}

std::string trimmed(const std::string &text) {
    const size_t begin = text.find_first_not_of(' ');
    const size_t end = text.find_last_not_of(' ');
    return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
}

bool isNumber(const std::string &text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace

// The story points in event/flag_set.cfg, in the file's (and the story's)
// order. Each one's FSET lines set the variables that change at that point.
const std::vector<GameJump::StoryPoint> &GameJump::storyPoints() {
    static const std::vector<StoryPoint> points = {
    {1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6}, {1, 7}, {2, 1}, {2, 2}, {2, 3}, {2, 4},
    {2, 5}, {2, 6}, {2, 7}, {2, 8}, {2, 9}, {2, 11}, {2, 12}, {3, 0}, {3, 1}, {3, 2}, {3, 3},
    {3, 4}, {3, 5}, {3, 6}, {3, 7}, {3, 8}, {3, 9}, {3, 10}, {3, 11}, {3, 12}, {3, 13}, {3, 14},
    {4, 0}, {4, 1}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6}, {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4},
    {5, 5}, {5, 6}, {5, 7}, {6, 0}, {6, 1}, {6, 2}, {6, 3}, {6, 4}, {6, 5}, {6, 6}, {7, 0}, {7, 1},
    {7, 2}, {7, 3}, {7, 4}, {7, 5}, {7, 6}, {7, 7}, {7, 8}, {7, 9}, {7, 10}, {7, 11}, {7, 12},
    {8, 0}, {8, 1}, {8, 2}, {9, 0}, {10, 0}, {10, 1}, {10, 2}, {10, 3}, {10, 4}, {10, 5}, {10, 6},
    {10, 7}, {10, 8}, {10, 9}, {10, 10}, {10, 11}, {10, 12}, {10, 13}, {10, 14}, {11, 1}, {11, 2},
    {11, 3}, {11, 4}, {12, 1}, {12, 2}, {12, 3}, {12, 4}, {12, 5}, {12, 6}, {12, 7}, {12, 8},
    {12, 9}, {12, 10}, {13, 1}, {13, 2}, {13, 3}, {13, 4}, {13, 5}, {13, 6}, {13, 7}, {14, 1},
    {14, 2}, {14, 3}, {14, 4}, {14, 5}, {14, 6}, {15, 1}, {16, 0}, {16, 1}, {16, 2}, {16, 3},
    {16, 4}, {16, 5}, {16, 6}, {16, 7}, {16, 8}, {17, 1}, {17, 2}, {17, 3}, {18, 1}, {18, 2},
    {18, 3}, {19, 1}, {19, 2}, {19, 3}, {19, 4},
    };
    return points;
}

uint32_t GameJump::allocateSlot() {
    std::lock_guard lock(m_mutex);
    if (m_buffer == 0u) {
        const uint32_t top = m_runtime.reserveAsyncCallbackStack(kBufferBytes, 16u);
        m_buffer = top != 0u ? top - kBufferBytes : 0u;
        if (m_buffer == 0u)
            return 0u;
    }
    // Calls run in order, so a slot is free again long before the ring wraps.
    const uint32_t slot = m_buffer + m_nextSlot * kSlotBytes;
    m_nextSlot = (m_nextSlot + 1u) % (kBufferBytes / kSlotBytes);
    return slot;
}

void GameJump::callCommand(uint32_t handler, const Arg *args, uint32_t count) {
    uint8_t *rdram = m_runtime.memory().getRDRAM();
    if (!rdram || count > kMaxArgs)
        return;
    const uint32_t base = allocateSlot();
    if (base == 0u)
        return;
    // {type, value} per argument, then the strings they point to.
    uint32_t strings = base + count * 8u;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pair[2] = {args[i].isString ? kArgString : kArgInt, static_cast<uint32_t>(args[i].value)};
        if (args[i].isString) {
            const size_t length = std::min<size_t>(args[i].text.size(), kMaxText - 1u);
            std::memcpy(rdram + strings, args[i].text.data(), length);
            rdram[strings + length] = 0u;
            pair[1] = strings;
            strings += kMaxText;
        }
        std::memcpy(rdram + base + i * 8u, pair, sizeof(pair));
    }
    m_runtime.eeScheduler().requestGuestCall(handler, {base, count, 8u + count * 8u, 0u});
}

void GameJump::applyStoryPoint(int chapter, int step) {
    // The command's own "whole state at this point" mode reads event/fs.cfg
    // through a function the retail game leaves empty. Its other mode applies
    // one point's flag_set.cfg changes, as the story does on reaching it, so
    // the state at a point is every point's changes up to it, in order.
    for (const StoryPoint &point : storyPoints()) {
        if (point.chapter > chapter || (point.chapter == chapter && point.step > step))
            break;
        const Arg args[3] = {{false, point.chapter, {}}, {false, point.step, {}}, {false, 1, {}}};
        callCommand(kCommandSetStoryPoint, args, 3u);
    }
    std::fprintf(stderr, "[jump] story point %d-%d\n", chapter, step);
}

void GameJump::warp(const std::string &map, int program) {
    // Mode -1 queues the change for the field loop, as the game's own scripts
    // do on leaving a town. An entry the map's script lacks hangs the game.
    const Arg args[3] = {{false, -1, {}}, {true, 0, map}, {false, program, {}}};
    callCommand(kCommandChangeMap, args, 3u);
    // The loop only acts on it in event mode, where a script that asked for
    // it would be running. Walking around, put it there the way the game does
    // when an event starts (0x33F380: entry 100 started -> mode 2).
    uint32_t mode = 0u;
    std::memcpy(&mode, m_runtime.memory().getRDRAM() + kFieldMode, sizeof(mode));
    if (mode == kFieldModeWalking)
        m_runtime.eeScheduler().requestGuestCall(kSetFieldMode, {kFieldModeEvent, 0u, 0u, 0u});
    std::fprintf(stderr, "[jump] %s, script entry %d (field mode %u)\n", map.c_str(), program, mode);
}

namespace {
std::atomic<bool> g_encountersOff{false};
PS2Runtime::RecompiledFunction g_encounterCheck = nullptr;

void encounterCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    if (!g_encountersOff.load(std::memory_order_relaxed)) {
        g_encounterCheck(rdram, ctx, runtime);
        return;
    }
    SET_GPR_U32(ctx, 2, 0u);
    ctx->pc = GPR_U32(ctx, 31);
}
} // namespace

void GameJump::installHooks() {
    if (g_encounterCheck)
        return;
    g_encounterCheck = m_runtime.lookupFunction(kEncounterCheck);
    if (g_encounterCheck)
        m_runtime.registerFunction(kEncounterCheck, encounterCheck);
}

void GameJump::setRandomEncounters(bool enabled) {
    g_encountersOff.store(!enabled, std::memory_order_relaxed);
    std::fprintf(stderr, "[jump] random encounters %s\n", enabled ? "on" : "off");
}

bool GameJump::randomEncounters() const {
    return !g_encountersOff.load(std::memory_order_relaxed);
}

void GameJump::partyToTopLevel() {
    const uint8_t *rdram = m_runtime.memory().getRDRAM();
    EeScheduler &scheduler = m_runtime.eeScheduler();
    for (uint32_t character = 0; character < kPartySize; ++character) {
        const uint32_t record = kPartyRecords + character * kRecordBytes;
        uint16_t level = 0;
        uint32_t exp = 0, topExp = 0;
        std::memcpy(&level, rdram + record + 0x10u, sizeof(level));
        std::memcpy(&exp, rdram + record + 0x18u, sizeof(exp));
        std::memcpy(&topExp, rdram + kExpTables + character * kExpTableBytes + kTopLevelIndex * 4u, sizeof(topExp));
        // As the end of a battle does, level by level, without its messages.
        for (uint32_t step = level; step < kTopLevelIndex; ++step)
            scheduler.requestGuestCall(kLevelUp, {kParty, character, 0u, 0u});
        if (topExp > exp)
            scheduler.requestGuestCall(kAddExp, {kParty, character, topExp - exp, 0u});
        for (uint32_t kind : {1u, 2u})
            if (const uint32_t learned = allocateSlot())
                scheduler.requestGuestCall(kLearnSpells, {character, record, learned, kind});
        scheduler.requestGuestCall(kHealHp, {kParty, character, 999u, 0u});
        scheduler.requestGuestCall(kHealMp, {kParty, character, 999u, 0u});
    }
    std::fprintf(stderr, "[jump] party to level 99\n");
}

std::shared_ptr<gfx::TestMenuData> GameJump::buildTestMenu(const std::string &archiveBase) {
    GameArchive archive;
    std::string error;
    if (!archive.open(archiveBase, error)) {
        std::fprintf(stderr, "[jump] test menu unavailable: %s\n", error.c_str());
        return nullptr;
    }
    auto data = std::make_shared<gfx::TestMenuData>();

    // Maps: each one's script (map\m01\m01i03.stb), and the world map's
    // tiles (map\f01\e_22.pak).
    auto maps = std::make_shared<std::unordered_set<std::string>>();
    for (const std::string &name : archive.names()) {
        if (name.rfind("map\\", 0) != 0)
            continue;
        const size_t slash = name.find_last_of('\\'), dot = name.find_last_of('.');
        if (dot == std::string::npos || dot < slash)
            continue;
        const std::string extension = name.substr(dot + 1);
        if (extension == "stb" || (extension == "pak" && std::count(name.begin(), name.end(), '\\') == 2))
            maps->insert(name.substr(slash + 1, dot - slash - 1));
    }

    std::vector<uint8_t> bytes;
    // The developers' event viewer: tab-separated map, script entry, event
    // number, part, scenario letter (A = chapter 1) and name; then a map and
    // entry for checking the event's sound, which is where the events listed
    // under the field's debug script (f01dbg) play in the game itself.
    if (archive.read("dbg\\evview2_1.txt", bytes)) {
        const std::vector<std::string> lines = splitLines(bytes);
        for (size_t i = 1; i < lines.size(); ++i) {
            std::vector<std::string> columns;
            std::stringstream stream(lines[i]);
            for (std::string column; std::getline(stream, column, '\t');)
                columns.push_back(trimmed(column));
            if (columns.size() < 6 || !isNumber(columns[1]))
                continue;
            gfx::TestMenuData::Event event;
            event.map = columns[0];
            event.program = std::atoi(columns[1].c_str());
            if (event.map == "f01dbg" && columns.size() > 8 && isNumber(columns[8])) {
                event.map = columns[7];
                event.program = std::atoi(columns[8].c_str());
            }
            if (!maps->count(event.map))
                continue;
            event.number = columns[2] == "-" ? std::string() : columns[2] + columns[3];
            event.chapter = columns[4].size() == 1 ? columns[4][0] - 'A' + 1 : 0;
            event.name = toAscii(columns[5]);
            const std::string setupMark = "<blackstar>";
            if (event.name.rfind(setupMark, 0) == 0) {
                event.setup = true;
                event.name.erase(0, setupMark.size());
            }
            data->events.push_back(std::move(event));
        }
    }

    // Story point names, as @CCSS (chapter, step); only the points the
    // game's flag_set.cfg has, since that is what setting one replays.
    if (archive.read("dbg\\dbgscn_1.txt", bytes)) {
        const std::map<int, std::string> names = readMessages(bytes);
        for (const StoryPoint &point : storyPoints()) {
            const auto it = names.find(point.chapter * 100 + point.step);
            data->storyPoints.push_back(
                {point.chapter, point.step, it != names.end() ? trimmed(it->second) : std::string()});
        }
    }

    // The developers' field-jump list: @N0 world-map tile, @N1 map, @N2 name.
    if (archive.read("dbg\\fjump_1.str", bytes)) {
        const std::map<int, std::string> messages = readMessages(bytes);
        for (const auto &[id, name] : messages) {
            if (id % 10 != 2)
                continue;
            const auto map = messages.find(id - 1);
            const std::string mapName = map != messages.end() ? trimmed(map->second) : std::string();
            if (mapName.empty() || name.find("====") != std::string::npos || !maps->count(mapName))
                continue;
            data->places.push_back({mapName, trimmed(name)});
        }
    }

    data->setStory = [this](int chapter, int step) { applyStoryPoint(chapter, step); };
    data->warp = [this](const std::string &map, int program) { warp(map, program); };
    data->hasMap = [maps](const std::string &map) { return maps->count(map) != 0; };
    data->setRandomEncounters = [this](bool enabled) { setRandomEncounters(enabled); };
    data->randomEncounters = [this] { return randomEncounters(); };
    data->partyToTopLevel = [this] { partyToTopLevel(); };
    std::fprintf(stderr, "[jump] F2 test menu: %zu events, %zu story points, %zu places\n", data->events.size(),
                 data->storyPoints.size(), data->places.size());
    return data;
}

void GameJump::startEnvironmentTriggers() {
    if (std::getenv("DQ8_DEBUG_NO_ENCOUNTERS"))
        setRandomEncounters(false);
    if (const char *level = std::getenv("DQ8_DEBUG_LEVEL99"))
        std::thread([this, frame = std::strtoull(level, nullptr, 10)] {
            while (m_runtime.eeScheduler().currentVSyncTick() < frame)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            partyToTopLevel();
        }).detach();
    const char *story = std::getenv("DQ8_DEBUG_STORY");
    const char *warpTo = std::getenv("DQ8_DEBUG_WARP");
    if (!story && !warpTo)
        return;
    std::string storySpec = story ? story : "", warpSpec = warpTo ? warpTo : "";
    std::thread([this, storySpec, warpSpec] {
        auto frameOf = [](const std::string &spec) {
            const size_t at = spec.find('@');
            return at == std::string::npos ? 0ull : std::strtoull(spec.c_str() + at + 1, nullptr, 10);
        };
        bool storyDone = storySpec.empty(), warpDone = warpSpec.empty();
        while (!storyDone || !warpDone) {
            const uint64_t tick = m_runtime.eeScheduler().currentVSyncTick();
            if (!storyDone && tick >= frameOf(storySpec)) {
                int chapter = 0, step = 0;
                std::sscanf(storySpec.c_str(), "%d,%d", &chapter, &step);
                applyStoryPoint(chapter, step);
                storyDone = true;
            }
            if (!warpDone && tick >= frameOf(warpSpec)) {
                char map[32] = {};
                int program = kArriveProgram;
                std::sscanf(warpSpec.c_str(), "%31[^,@],%d", map, &program);
                warp(map, program);
                warpDone = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

} // namespace dq8
