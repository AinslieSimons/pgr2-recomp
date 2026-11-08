#include <cstdlib>

#if __has_include(<toml++/toml.hpp>)  // not in the headless harness, which has no settings file
#include <toml++/toml.hpp>
#define PGR2_HAS_TOML 1
#endif

#include "app/app.h"
#include "core/input.h"

void InstallDrawFilter();  // src/game/draw_filter.cpp

namespace pgr2 {
extern bool skip_intro, hide_hud, dlc, cpu_car_shadows;
extern uint32_t fov_percent, draw_distance_percent, car_detail;
extern bool mirror_detail, full_textures, tree_detail, planar_water, car_reflections, cars_in_reflections, traced_reflections;
std::vector<achievements::Def> AchievementSet();    // src/game/achievement_set.cpp
void WheelFeel(input::WheelFeel& feel);             // src/game/wheel_feel.cpp
float MusicLevel();                                 // src/game/hooks.cpp
}

namespace pgr2 {
extern uint32_t race_fps;
// the frame rate cap, 60 unless changed, must not hold races under the rate chosen for them
void RaiseFrameCap() {
    app::Settings& s = app::Get();
    if (race_fps > 60 && s.fps_cap && s.fps_cap < race_fps) s.fps_cap = race_fps;
}
}  // namespace pgr2

namespace {
#ifdef PGR2_HAS_TOML
// a settings file from when this was the switch race_60fps
void LoadOld(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    if (!t.contains("race_fps") && t["race_60fps"].value_or(false)) pgr2::race_fps = 60;
}
#else
constexpr void (*LoadOld)(const void*) = nullptr;
#endif

// PGR2_SKIP_INTRO=1, PGR2_HIDE_HUD=1 and PGR2_FOV=percent set those options without a settings file (the headless harness has none)
void Init() {
    // always: no intro movies at start, whatever the settings file says (PGR2_SKIP_INTRO=0: the pad scripts' timing has them)
    const char* intro = std::getenv("PGR2_SKIP_INTRO");
    pgr2::skip_intro = !(intro && *intro == '0');
    if (const char* e = std::getenv("PGR2_DLC"); e && *e == '1') pgr2::dlc = true;
    if (const char* e = std::getenv("PGR2_HIDE_HUD"); e && *e == '1') pgr2::hide_hud = true;
    if (const char* e = std::getenv("PGR2_FOV")) pgr2::fov_percent = uint32_t(std::atoi(e));
    if (const char* e = std::getenv("PGR2_DRAW")) pgr2::draw_distance_percent = uint32_t(std::atoi(e));
    if (const char* e = std::getenv("PGR2_CAR_DETAIL")) pgr2::car_detail = uint32_t(std::atoi(e));
    if (const char* e = std::getenv("PGR2_MIRROR_DETAIL"); e) pgr2::mirror_detail = *e == '1';
    if (const char* e = std::getenv("PGR2_FULL_TEXTURES"); e) pgr2::full_textures = *e == '1';
    if (const char* e = std::getenv("PGR2_TREE_DETAIL"); e) pgr2::tree_detail = *e == '1';
    if (const char* e = std::getenv("PGR2_PLANAR_WATER"); e) pgr2::planar_water = *e == '1';
    if (const char* e = std::getenv("PGR2_CAR_REFLECTIONS"); e) pgr2::car_reflections = *e == '1';
    if (const char* e = std::getenv("PGR2_CARS_IN_REFLECTIONS"); e) pgr2::cars_in_reflections = *e == '1';
    if (const char* e = std::getenv("PGR2_TRACED_REFLECTIONS"); e) pgr2::traced_reflections = *e == '1';
    InstallDrawFilter();  // after the options: the DLC setting picks its part
    pgr2::RaiseFrameCap();
}
}  // namespace

namespace pgr2 {
uint32_t race_fps = 30;  // the frame rate of races, 30 as on the console (src/game/hooks.cpp)
bool cpu_car_shadows = true;  // every car with the followed car's sun shadow (src/game/car_shadows.cpp)
bool skip_intro = false;  // no intro movies at start (src/game/hooks.cpp)
bool hide_hud = false;    // no race HUD, for screenshots (src/game/hooks.cpp)
uint32_t fov_percent = 100;  // the race cameras' field of view, percent of the console's (src/game/hooks.cpp)
uint32_t draw_distance_percent = 100;  // how far the race cameras see, percent of the console's (src/game/hooks.cpp)
uint32_t car_detail = 2;  // 0 the console's, 1 high, 2 highest (src/game/detail.cpp)
bool mirror_detail = true;  // the mirror sees and draws as the main view does (src/game/detail.cpp)
bool tree_detail = true;  // trees at full detail at any distance (src/game/detail.cpp)
bool planar_water = true;  // water reflects the world in its plane (src/game/planar_water.cpp)
bool car_reflections = true;  // every car reflects the live cube, not only the followed one (src/game/car_reflections.cpp)
bool cars_in_reflections = true;  // the reflection cube shows the other cars (src/game/car_reflections.cpp)
bool traced_reflections = false;  // cars reflect each other by rays traced on the GPU, Metal (src/game/car_reflections.cpp)
bool full_textures = true;  // every streamed texture at its full size from the city load (src/game/full_textures.cpp)
bool dlc = false;  // the Xbox Live city packs in T:\$C, on the disc's game (src/game/dlc.cpp)
}

// PGR2 asks for one vertical blank a frame in its menus and two in its races (D3D_PresentInterval), and steps its
// races by a fixed 1/30 s a frame; the runtime paces by that, so the default fps_cap of 60 runs both at console speed.

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Project Gotham Racing 2",  // window and splash
    .short_title = L"PGR2",               // message boxes
    .name = L"PGR2",                      // window classes, crash dumps, handover event
    .log = "pgr2",                        // log file prefix
    .settings = "pgr2.toml",              // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "PGR2"}},  // each recompiled xbe and its exe, for title launches
    // game settings kept in the settings file
    .options = {{"skip_intro", &pgr2::skip_intro, nullptr, "Skip intro movies", "At the next start."},
                {"cpu_car_shadows", &pgr2::cpu_car_shadows, nullptr, "Sun shadows on every car", "Not on the console, where only the followed car has one; at once."},
                {"hide_hud", &pgr2::hide_hud, nullptr, "Hide the race HUD", "For screenshots and replays; at once."},
                {"fov_percent", nullptr, &pgr2::fov_percent, "Race camera field of view (%)",
                 "100 is the console's; 50 to 200, at once."},
                {"draw_distance_percent", nullptr, &pgr2::draw_distance_percent, "Draw distance (%)",
                 "100 is the console's; 25 to 400, from the next race. Experimental: most streets end before it."},
                {"car_detail", nullptr, &pgr2::car_detail, "Car detail",
                 "0 the console's, 1 high, 2 highest (the default): how far other cars keep their full shape; at once."},
                {"mirror_detail", &pgr2::mirror_detail, nullptr, "Full mirror detail",
                 "On by default: the mirror sees as far and draws as much as the main view; may cost frame rate on slower machines in the views with a mirror; at once."},
                {"tree_detail", &pgr2::tree_detail, nullptr, "Full tree detail",
                 "On by default: trees keep all their leaves and their fullest shape at any distance; may cost frame rate where there are many; at once."},
                {"planar_water", &pgr2::planar_water, nullptr, "Water reflections",
                 "On by default: water mirrors the city and cars around it, rather than a fixed picture of the city; draws the scene a second time where water is in view, so may cost frame rate there; at once."},
                {"car_reflections", &pgr2::car_reflections, nullptr, "Live reflections on every car",
                 "On by default: other cars reflect the street as yours does, rather than a fixed picture of the city; at once."},
                {"cars_in_reflections", &pgr2::cars_in_reflections, nullptr, "Cars in reflections",
                 "On by default: your car reflects the cars around it, which the console's reflections leave out; draws them into each reflection face, so may cost frame rate; at once."},
                {"traced_reflections", &pgr2::traced_reflections, nullptr, "Ray traced car reflections",
                 "Off by default: cars reflect each other and their own mirrors and spoilers by rays traced on the GPU, the city still from its reflection cube; macOS (Metal) only; may cost frame rate; at once."},
                {"full_textures", &pgr2::full_textures, nullptr, "Full size textures",
                 "On by default: every city texture at full size from the start of a race, rather than the small version until you are near; uses up to 60 MB more memory; from the next race."},
                {"race_fps", nullptr, &pgr2::race_fps, "Race frame rate",
                 "30 (the console's), 60, 90, 120 or 144; at the next start. Same physics. Above 60 busy streets dip on most CPUs."},
                {"dlc", &pgr2::dlc, nullptr, "Xbox Live DLC cities (experimental)",
                 "Paris and Long Beach from your packs in hdd\\E\\TDATA\\4d53004b\\$C (WINDOWS.md); at the next start."}},
    .load = LoadOld,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = Init,                         // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = nullptr,                       // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
    .achievements = pgr2::AchievementSet(),  // conditions on guest memory, checked each frame
    // the North American release (region NA+Japan, version 5): its default.xbe differs from the PAL one (version 2)
    // only in the signature and certificate, every section byte for byte the same. Media patched copies of either (run
    // from a modded console's hard disk): one byte, 0x193964 jge to jmp after XapiVerifyMediaInDrive in the XAPI start,
    // code the host replaces
    .same_code = {"676895b86f01938804c5f1cdcad8b44044295cb4", "ec6e902fc167de349f1dc1838a7441dfff932c62",
                  "acd776330a9b903fd567b544cf7ece0f612f6c7d", "702b79fd3bc844a413cbc26404718b1bf7e44e87"},  // a package is built from one of them
    .wheel = pgr2::WheelFeel,  // a steering wheel's force feedback follows the player's car
    .send_level = pgr2::MusicLevel,  // the radio, heard through the effect sends, at the music volume
    .sharpen_2d = true,
    .languages = {1, 2, 3, 4, 5, 6, 7, 8},  // the disc's; Portuguese has text files but the game runs it as English              // menus, HUD and text drawn from enlarged, sharpened copies of their textures
    .auto_fills_screen = true,  // "From Screen" on an ultrawide: the 3D views widened to it, the HUD 16:9 (hooks.cpp ApplyAspect)
    .fill_follows_live = true,  // its views are reshaped when that changes in play (hooks.cpp Reshape)
};
