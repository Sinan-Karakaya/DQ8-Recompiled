#pragma once

// Drives DQ8 through its own script commands, for testing: put the story at
// any of the game's story points, and go to any map, running any entry of its
// script there -- which is how each of the game's events is played. The calls
// run as guest code on the EE thread, one after another, at its next safe
// point.

#include "gfx/test_menu.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class PS2Runtime;

namespace dq8 {

class GameJump {
public:
    explicit GameJump(PS2Runtime &runtime) : m_runtime(runtime) {}

    struct StoryPoint {
        int chapter;
        int step;
    };
    static const std::vector<StoryPoint> &storyPoints();

    // Puts the story's variables where they are at story point (chapter,
    // step), through the game's own "story point" script command.
    void applyStoryPoint(int chapter, int step);
    // The game's "go to map" script command: a town ("m01"), an interior
    // ("m01i02"), a dungeon ("d01"), a world-map tile ("e_22"). On arrival
    // the map's script runs entry `program`: 100 arrives the usual way, and
    // the developers' event list gives the entry that plays each event.
    void warp(const std::string &map, int program);

    // Wraps the game's functions this needs to change; before the game runs.
    void installHooks();
    // Whether walking around the field starts random battles (the default).
    void setRandomEncounters(bool enabled);
    bool randomEncounters() const;
    // Every party member (and the two who join later) to level 99, as
    // battles level them -- stats, spells and abilities -- without the
    // messages, then HP and MP refilled. Skill points are not handed out.
    void partyToTopLevel();

    // The test menu's lists, from the developers' debug files in the game's
    // DATA archive (`archiveBase` is the path without ".HD6"/".DAT"), with
    // its actions bound to this object. Null when the archive can't be read.
    std::shared_ptr<gfx::TestMenuData> buildTestMenu(const std::string &archiveBase);

    // DQ8_DEBUG_STORY=chapter,step@frame, DQ8_DEBUG_WARP=map[,program]@frame
    // and DQ8_DEBUG_LEVEL99=frame fire once the guest reaches that frame, and
    // DQ8_DEBUG_NO_ENCOUNTERS turns random battles off, for scripted tests.
    void startEnvironmentTriggers();

private:
    // Writes script-command arguments into guest memory set aside for this
    // and calls the command's handler with them.
    struct Arg {
        bool isString;
        int32_t value;
        std::string text;
    };
    void callCommand(uint32_t handler, const Arg *args, uint32_t count);
    // A 128-byte block of guest memory for one queued call's data.
    uint32_t allocateSlot();

    PS2Runtime &m_runtime;
    std::mutex m_mutex;
    uint32_t m_buffer = 0u;
    uint32_t m_nextSlot = 0u;
};

} // namespace dq8
