#pragma once

// What the test menu (F2) offers: the game's events, story points and places,
// from the developers' debug lists left on the disc, the actions that go
// there, and a few cheats for testing. The runtime builds it from the game's files; the menu only draws it.

#include <functional>
#include <string>
#include <vector>

namespace dq8::gfx {

struct TestMenuData {
    struct Event {
        std::string map;     // where it plays: "m01", "m01i03", "e_22"
        int program = 0;     // the map script's entry that plays it
        std::string number;  // the developers' event number: "102", "105-a"
        int chapter = 0;     // 1..19, from the scenario letter (A = 1)
        std::string name;
        bool setup = false;  // sets the game up for the events after it
    };
    struct StoryPoint {
        int chapter = 0;
        int step = 0;
        std::string name;
    };
    struct Place {
        std::string map;
        std::string name;
    };
    std::vector<Event> events;
    std::vector<StoryPoint> storyPoints;
    std::vector<Place> places;

    // Puts the story at (chapter, step). Called from the menu, any thread.
    std::function<void(int chapter, int step)> setStory;
    // Goes to `map` and runs its script entry `program` there (100: arrive
    // the usual way).
    std::function<void(const std::string &map, int program)> warp;
    // Whether `map` is one the game has, for the menu's free-form entry.
    std::function<bool(const std::string &map)> hasMap;
    // Random battles on the field, on or off.
    std::function<void(bool enabled)> setRandomEncounters;
    std::function<bool()> randomEncounters;
    // The whole party to level 96 with the EXP of 99 minus one; the next
    // battle's level-ups hand out the skipped levels' skill points.
    std::function<void()> partyToTopLevel;
};

} // namespace dq8::gfx
