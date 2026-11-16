// Tyre smoke and dust: longer lived, the same at any frame rate, and more of it (FINDINGS.md, "Tyre smoke and other
// particles"). Setting smoke_duration, percent of the console's (100 the console's look).
//
// The wheels' smoke emitters (0xF87C0 set up, 0xF87F0 steered, 0xF8D70 emitted) take their particles from one pool for
// every wheel of every car, 400 records of 0x2C at 0x3CFAA8 (+0 the next index to try, +4 the records in use, +8 the
// records: +0 their class 0x27B7D4, +0x10 colour, alpha its top byte, +0x28 the link, -1 free). Each emitter holds at
// most 50 (0xF8C00) and asks for 50 (0x232C0, through 0xF87C0). A particle's life (0xF8630) ends when its alpha is 0:
// from its start to 60 it falls by (1 - 2 dt) a frame, then by one a frame, which ties the faint tail to the frame rate
// (2 s at the console's 30, 1 s at 60).
//
// Here: the pool has 9600 more records past the game's (10000 in all), handed out when its own are taken (the game's 400
// ran out with 24 wheels of up to 50 each). An emitter holds up to 50 x smoke_density, more with longer lives (WheelMax,
// at most 400), keeps a steady rate besides the game's replacements (the game's alone come in waves), and each particle
// starts smaller with the density: the smoke draw (0xF8F50) copies an emitter's particles into 16 byte entries of an
// array on its stack (from esp+0x4C, a 0x35C byte frame), room for 50, and more (an earlier try) ran over the saved
// registers of its callers (a crash in the view, 0x17FCC0, its camera in esi), so a longer list is drawn 48 at a time.
// A new particle (0xF8A50) and a particle's frame (0xF8630) are the game's with every number from the tuning (Tune, read
// live from smoke_tune.txt; tools/smoke-lab runs the same model in a browser and writes that file). The game's rand() is
// called where it was, and put back after anything extra, so nothing else it draws from changes because of this.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace pgr2 {
extern uint32_t smoke_duration, smoke_density;  // src/common/game_info.cpp
extern bool smoke_physics;       // src/common/game_info.cpp
extern bool smoke_lighting;      // src/common/game_info.cpp
}

namespace {

using kernel::R32;
using kernel::W32;

constexpr uint32_t kPool = 0x3CFAA8, kPoolUsed = kPool + 4, kPoolRecords = kPool + 8;
constexpr uint32_t kRecord = 0x2C, kGameRecords = 400, kAllRecords = 10000, kExtra = kAllRecords - kGameRecords;
constexpr uint32_t kClass = 0x27B7D4, kLink = 0x28, kFree = 0xFFFFFFFFu;
constexpr uint32_t kRand = 0x1B2944, kTidData = 0x1B5E78, kRandState = 0x14;  // rand()'s state: its thread data's +0x14
// an emitter (the wheel's +0x134): +0x10 type, +0x14 stopped, +0x18 particles, +0x1C most it asks for, +0x24 place,
// +0x30 last place, +0x4C start delay, +0x50 its list (+0 head, +4 tail, +0xC cursor, +0x10 count)
constexpr uint32_t kEmType = 0x10, kEmStopped = 0x14, kEmCount = 0x18, kEmMax = 0x1C, kEmPlace = 0x24, kEmLast = 0x30,
                   kEmDelay = 0x4C, kEmSpawn = 0xF8D20;
constexpr uint32_t kCamera = 0x3CD804, kProject = 0x15D70, kUpdate = 0xF8630;  // the view drawn, its projection (thiscall: out, place, 0)
constexpr uint32_t kListHead = 0, kListTail = 4, kListCursor = 0x0C, kListCount = 0x10, kListCap = 400, kDrawChunk = 48;

float F(uint32_t va) {
    float f;
    const uint32_t u = R32(va);
    std::memcpy(&f, &u, 4);
    return f;
}
void PutF(uint32_t va, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    W32(va, u);
}

// Cars stirring the smoke (smoke_physics). Smoke is carried by the air: a particle has next to no inertia of its own,
// so it takes the air's velocity within a fraction of a second. The game models that as a damping towards still air
// (velocity x (1 - 4 dt) a frame, 0xF8630); here the damping is towards the air's velocity at the particle, which moving
// cars set, the way games advect smoke through a velocity field built from what moves through it (e.g. Assassin's Creed
// IV's GPU smoke: collision data from the physics feeding a velocity field) rather than simulating the fluid.
// The air near the cars, all of it procedural:
// - Each car leaves a wake behind it: every 1/30 s a sample of its place, velocity and side, kept 2.5 s. At a particle,
//   the nearest sample of each car's wake gives
//   - its slipstream: the air dragged along at up to 0.3 of the car's velocity, spreading (0.8 m + 1.2 m a second wide)
//     and dying away (e^(-age / 1 s));
//   - its trailing vortices: a counter-rotating pair, one behind each flank (0.8 m out, 0.3 m up), turning about the
//     car's direction of travel, the air between them going down and then out along the road (smoke blown aside; turned
//     the other way they lifted the smoke into a haze over the cars), each a Lamb-Oseen vortex, v = G / (2 pi r) (1 - e^(-r^2 / rc^2)), G 1.2 m x the car's
//     speed, its core spreading as rc^2 = 0.5^2 + 4 x 0.08 m^2/s x age and its strength dying with the slipstream;
// - the air pushed round the car's body as it is now: potential flow past a sphere of 1.8 m moving at the car's
//   velocity, u = R^3 / (2 d^3) (3 (U.n) n - U), so smoke ahead is pushed aside and smoke behind drawn into the gap;
// - turbulence where the wake is: the curl of a few sines, divergence free so it swirls without bunching (in the spirit
//   of Bridson's curl noise), as strong as 60% of the wake's speed there.
// Taken once a frame on the game thread before the frame step (SmokeFrame), read by every particle's update.
constexpr uint32_t kCars = 0x297390, kCarCount = 0x0C, kCarFirst = 0x1C00, kCarStride = 0x6990, kCarAt = 0x44;
constexpr uint32_t kCarDynamics = 0x94, kDynVelocity = 0x18B4, kFrameSeconds = 0x28F4EC;
constexpr uint32_t kMaxCars = 16, kSamples = 128;  // 30 a second: 4.2 s, the longest wake_life
constexpr float kSampleEvery = 1.0f / 30.0f, kMinSpeed = 3.0f, kReach = 4.0f, kPi = 3.14159265f;

// Every number of the smoke's look and motion, the game's own where it has one (FINDINGS.md), read from a tuning file
// (PGR2_SMOKE_TUNE, else smoke_tune.txt beside the exe and pgr2.toml: "name = value" lines, # comments), checked once a
// second, so a change shows in a running game. tools/smoke-lab writes the same file and runs the same model.
struct Tune {
    // a new particle (0xF8A50)
    // (the game's own values in brackets; the defaults here are heavier, thicker smoke, tried in tools/smoke-lab)
    float spawn_size = 0.35f;    // m (0.15; type 2 0.1), then / sqrt(smoke_density)
    float spawn_jitter = 0.15f;  // m, +- across and along
    float rise = 1.5f;           // m/s, upward speed 0 to this (4)
    float spread = 1.2f;         // m/s, +- across and along (0.5)
    float drift = 1.0f;          // x the game's drift (half the car's velocity)
    float opacity = 1.3f;        // x the alpha it starts with (intensity x 196, at most 255) (1)
    // its life (0xF8630)
    float hold = 0.7f;           // s at full alpha before it fades (0)
    float fade = 0.7f;           // alpha x (1 - fade dt / duration) a frame, down to 60 (2)
    float tail = 30.0f;          // tail steps a second (of 60), / duration
    float growth = 1.6f;         // size += dt x growth / size (type 1) (1/6)
    float damping = 2.5f;        // velocity towards the air's at this rate a second (4)
    float lift = 1.0f;           // m/s of buoyant rise in the air it takes, e^(-age / 1 s) (0)
    // the air (smoke_physics)
    float slip = 0.3f;           // x the car's velocity, dragged along behind it
    float slip_width = 0.8f;     // m, the slipstream's width as it is made
    float slip_spread = 1.2f;    // m a second it widens
    float wake_fade = 1.0f;      // s, the wake dying away, e^(-age / this)
    float wake_life = 2.5f;      // s kept (at most 4.2)
    float flank = 0.8f;          // m out from the middle, each trailing vortex
    float vortex_up = 0.3f;      // m above the car's place
    float core = 0.5f;           // m, a vortex's core as it is made
    float nu = 0.08f;            // m^2/s, the core spreading
    float circulation = 1.2f;    // m x the car's speed
    float vortex_length = 2.0f;  // m, how far along the road each wake sample's vortex reaches
    float hull_long = 2.2f, hull_wide = 0.9f, hull_tall = 0.7f;  // m, half sizes, the body the air goes round
    float body = 1.0f;           // x that flow
    float turbulence = 0.4f;     // x the wake's speed
    float turbulence_scale = 1.3f;  // 1/m, its eddies' wavenumber
    float ground = 0.05f;        // m below where it was made that a particle may sink
    float shade = 0.8f;          // smoke made in full shade, x the sun's (smoke_lighting): a little darker, not shadow
    float car_height = 0.3f;     // m, the car's place (+0x44) above the road
    float turbulence_floor = 1.0f;  // m over which the turbulence's vertical part grows from the road
    float image_vortices = 1.0f;    // 1: each vortex mirrored under the road, turning the other way (the road stops its flow)
    float own_wake = 0.0f;       // x the air of the car that made the smoke (0: only other cars stir it)
    // the look of a plume (none of it the game's)
    float size_var = 0.45f;      // +- fraction of a new puff's size
    float opacity_var = 0.4f;    // +- fraction of a new puff's alpha
    float fade_in = 0.15f;       // s from clear to full as it leaves the tyre
    float billow = 2.2f;         // m/s, slow eddies in all smoke (curl noise), growing with its age
    float billow_scale = 0.25f;  // 1/m, their wavenumber
    float billow_grow = 1.5f;    // s of age to full billow
    float shade_low = 0.7f;      // colour at the road, x (1 at shade_height up)
    float shade_height = 2.0f;   // m over which smoke lightens with height
    // tighter at speed, followed closely
    float wake_follow = 12.0f;   // per second added to damping where the cars' air is strong (smoke follows tight vortices)
    float core_ref_speed = 15.0f;  // m/s up to which a trailing vortex's core is its full size
    float core_min = 0.5f;       // its smallest, x core, as speed rises
    // burnouts: the tyre's own swirl
    float burn_speed = 8.0f;     // m/s: a smoking wheel is a burnout at a standstill, a skid from this car speed up
    float wheel_swirl = 6.0f;    // m^2/s round a burning-out tyre's axle at full intensity
    float wheel_core = 0.2f;     // m, its core
    float tyre_radius = 0.33f;   // m, the axle above the road
    float wheel_width = 0.35f;   // m, how far along the axle the swirl reaches (gaussian)
    // the tread's heat (SetBurn)
    float ambient = 25.0f;       // degrees C, the air and a cold tyre
    float tread_heat = 150.0f;   // degrees C a second the tread heats sliding at full intensity
    float tread_cool = 0.25f;    // per second it cools towards the air, standing
    float tread_cool_speed = 0.01f;  // more per second for each m/s of the car (airflow)
    float smoke_onset = 200.0f;  // degrees C the rubber starts to break down into smoke
    float smoke_full = 400.0f;   // degrees C at which the smoke is full
    float smoke_min = 0.35f;     // a cold tyre's smoke, x full (alpha)
    float burn_arc = 140.0f;     // degrees of tread, from the road up the back of the tyre, a burnout puff comes off
    float burn_eject = 4.0f;     // m/s along the tread a burnout puff leaves at
    float burn_spill = 1.2f;     // m/s out from the tyre's outer face
    float burn_size = 1.6f;      // x a burnout puff's size
    float burn_opacity = 1.4f;   // x a burnout puff's alpha
    float burn_rate = 1.5f;      // x more puffs a second at full burnout
    float tyre_width = 0.25f;    // m across the tread the puffs come off
    // the wind (none in the game)
    float wind = 0.5f;           // x the city's average wind (10 m up; 0.5 about street level)
    float wind_gust = 0.4f;      // +- fraction in gusts
    float wind_dir = -1.0f;      // degrees (world x towards z) it blows towards; -1 random each race
};
Tune g_tune;
struct TuneField {
    const char* name;
    float Tune::*p;
};
const TuneField kTuneFields[] = {
    {"spawn_size", &Tune::spawn_size}, {"spawn_jitter", &Tune::spawn_jitter}, {"rise", &Tune::rise}, {"spread", &Tune::spread},
    {"drift", &Tune::drift}, {"opacity", &Tune::opacity}, {"hold", &Tune::hold}, {"fade", &Tune::fade}, {"tail", &Tune::tail},
    {"growth", &Tune::growth}, {"damping", &Tune::damping}, {"lift", &Tune::lift}, {"slip", &Tune::slip},
    {"slip_width", &Tune::slip_width}, {"slip_spread", &Tune::slip_spread}, {"wake_fade", &Tune::wake_fade},
    {"wake_life", &Tune::wake_life}, {"flank", &Tune::flank}, {"vortex_up", &Tune::vortex_up}, {"core", &Tune::core},
    {"nu", &Tune::nu}, {"circulation", &Tune::circulation}, {"vortex_length", &Tune::vortex_length},
    {"hull_long", &Tune::hull_long}, {"hull_wide", &Tune::hull_wide}, {"hull_tall", &Tune::hull_tall}, {"body", &Tune::body},
    {"turbulence", &Tune::turbulence}, {"turbulence_scale", &Tune::turbulence_scale}, {"ground", &Tune::ground},
    {"shade", &Tune::shade}, {"car_height", &Tune::car_height}, {"turbulence_floor", &Tune::turbulence_floor},
    {"image_vortices", &Tune::image_vortices}, {"own_wake", &Tune::own_wake}, {"size_var", &Tune::size_var},
    {"opacity_var", &Tune::opacity_var}, {"fade_in", &Tune::fade_in}, {"billow", &Tune::billow},
    {"billow_scale", &Tune::billow_scale}, {"billow_grow", &Tune::billow_grow}, {"shade_low", &Tune::shade_low},
    {"shade_height", &Tune::shade_height}, {"wake_follow", &Tune::wake_follow}, {"core_ref_speed", &Tune::core_ref_speed},
    {"core_min", &Tune::core_min}, {"burn_speed", &Tune::burn_speed}, {"wheel_swirl", &Tune::wheel_swirl},
    {"wheel_core", &Tune::wheel_core}, {"tyre_radius", &Tune::tyre_radius}, {"wheel_width", &Tune::wheel_width},
    {"ambient", &Tune::ambient}, {"tread_heat", &Tune::tread_heat}, {"tread_cool", &Tune::tread_cool},
    {"tread_cool_speed", &Tune::tread_cool_speed}, {"smoke_onset", &Tune::smoke_onset}, {"smoke_full", &Tune::smoke_full},
    {"smoke_min", &Tune::smoke_min},
    {"burn_arc", &Tune::burn_arc}, {"burn_eject", &Tune::burn_eject}, {"burn_spill", &Tune::burn_spill},
    {"burn_size", &Tune::burn_size}, {"burn_opacity", &Tune::burn_opacity}, {"burn_rate", &Tune::burn_rate},
    {"tyre_width", &Tune::tyre_width},
    {"wind", &Tune::wind}, {"wind_gust", &Tune::wind_gust}, {"wind_dir", &Tune::wind_dir},
};

// the tuning file, read again when it changes
void LoadTune() {
    static const std::string path = [] {
        const char* e = std::getenv("PGR2_SMOKE_TUNE");
        return e && *e ? std::string(e) : (app::ExeDir() / "smoke_tune.txt").string();
    }();
    static std::filesystem::file_time_type seen{};
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(path, ec);
    if (ec || when == seen) return;
    seen = when;
    std::ifstream in(path);
    Tune t;  // what the file leaves out is the default
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            const auto a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        for (const TuneField& f : kTuneFields)
            if (key == f.name) {
                char* end = nullptr;
                const float v = std::strtof(value.c_str(), &end);
                if (end != value.c_str() && std::isfinite(v)) t.*f.p = v, n++;
            }
    }
    t.wake_life = std::clamp(t.wake_life, 0.1f, float(kSamples - 1) * kSampleEvery);
    t.fade = std::max(t.fade, 0.0f), t.tail = std::max(t.tail, 0.1f), t.damping = std::clamp(t.damping, 0.0f, 30.0f);
    g_tune = t;
    LOGR("pgr2: smoke: %d tuning values from %s", n, path.c_str());
}
struct Sample {
    float at[3], v[3], dir[3], side[3], speed, born;
};
struct Wake {
    Sample s[kSamples];
    uint32_t head = 0, n = 0;
    float last = -1e9f, lo[3], hi[3], at[3], v[3], speed = 0;
    bool moving = false;
    uint32_t car = 0;  // its car record
};
Wake g_wakes[kMaxCars];
uint32_t g_wakes_used = 0;
float g_now = 0;

inline float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

// Burnouts: a tyre spinning on the spot drags the air round its axle, so its smoke wraps round the wheel and rolls off
// it in tight swirls; a skid's smoke trails into the car's wake instead. A smoking wheel counts as a burnout by how slow
// its car is (full at a standstill, none from burn_speed), times its intensity, and makes a Lamb-Oseen vortex about its
// axle (tyre_radius up, the tyre's top turning forward), with its image under the road, reaching wheel_width along it.
// Made each frame by the smoking wheels (0xF8D70's hook), cleared by SmokeFrame, read by every particle.
struct WheelVortex {
    float c[3], axis[3], road, gamma;
};
std::vector<WheelVortex> g_wheel_vortices;
void WheelVortexAir(const float p[3], float u[3]) {
    const Tune& t = g_tune;
    for (const WheelVortex& wv : g_wheel_vortices) {
        const float r[3] = {p[0] - wv.c[0], p[1] - wv.c[1], p[2] - wv.c[2]};
        const float along = Dot(r, wv.axis);
        if (std::fabs(along) > 3 * t.wheel_width) continue;
        for (int image = 0; image <= 1; image++) {
            const float cy = image ? 2 * wv.road - wv.c[1] : wv.c[1];
            const float q[3] = {r[0] - wv.axis[0] * along, p[1] - cy - wv.axis[1] * along, r[2] - wv.axis[2] * along};
            const float rr = std::sqrt(Dot(q, q));
            if (rr < 1e-3f || rr > 2.5f) continue;
            const float g = (image ? -1.0f : 1.0f) * wv.gamma;
            const float vt = g / (2 * kPi * rr) * (1 - std::exp(-rr * rr / (t.wheel_core * t.wheel_core))) *
                             std::exp(-along * along / (t.wheel_width * t.wheel_width));
            const float n[3] = {q[0] / rr, q[1] / rr, q[2] / rr};
            float tg[3];
            Cross(wv.axis, n, tg);  // the axle (up x forward) x out: the top turning forward, either handedness
            for (int k = 0; k < 3; k++) u[k] += tg[k] * vt;
        }
    }
}

// The wind: the game has none (no wind in its code, Frontend\Cities.Ini or the areas), so this is ours: each city's
// average wind (about 10 m up, from climate records, approximate) times tune.wind (0.5 about street level), blowing
// towards wind_dir (random each race when -1), gusting by +- wind_gust in gusts carried down the wind, weaker near the
// road ((height / 2 m)^0.3, at least 0.3). The city is the race's index at 0x38317C, Cities.Ini's order.
constexpr uint32_t kCity = 0x38317C;
constexpr float kCityWind[] = {5.0f /*Edinburgh*/, 3.2f /*Moscow*/, 3.4f /*Barcelona*/, 4.0f /*Washington DC*/,
                               4.6f /*Chicago*/,   2.2f /*Florence*/, 3.9f /*Stockholm*/, 3.3f /*Hong Kong*/,
                               3.8f /*Yokohama*/,  4.3f /*Sydney*/,   4.2f /*Nurburgring*/};
float g_wind_dir = 0;  // degrees, this race's
void WindAt(const float p[3], float ground, float u[3]) {
    const uint32_t city = R32(kCity);
    const float U = (city < std::size(kCityWind) ? kCityWind[city] : 0.0f) * g_tune.wind;
    if (U <= 0) return;
    const float dir = (g_tune.wind_dir >= 0 ? g_tune.wind_dir : g_wind_dir) * kPi / 180.0f;
    const float dx = std::cos(dir), dz = std::sin(dir);
    const float tt = g_now - (p[0] * dx + p[2] * dz) / std::max(U, 1.0f);  // gusts travel with the wind
    const float gust = 1 + g_tune.wind_gust * (0.6f * std::sin(0.21f * tt + 1.3f) + 0.4f * std::sin(0.53f * tt + 4.1f));
    const float turn = 0.25f * std::sin(0.13f * tt + 2.2f);
    const float prof = std::max(0.3f, std::pow(std::max(p[1] - ground, 0.01f) / 2.0f, 0.3f));
    u[0] += U * gust * prof * std::cos(dir + turn), u[2] += U * gust * prof * std::sin(dir + turn);
}

// the air's velocity at p, for a particle made by the car `owner` (0 none): its own car's air counts own_wake times
// (none by default: a drifting car's own wake and vortices sent its smoke shooting off every way, which looked wrong;
// the smoke still takes the car's drift as it is made, and every other car stirs it)
void AirAt(const float p[3], float u[3], uint32_t owner = 0) {
    u[0] = u[1] = u[2] = 0;
    WheelVortexAir(p, u);
    float stir = 0, stir_road = 0;  // the wake's speed here, for the turbulence, and the road under it
    for (uint32_t w = 0; w < g_wakes_used; w++) {
        const Wake& wk = g_wakes[w];
        if (!wk.n && !wk.moving) continue;
        const float weight = owner && wk.car == owner ? g_tune.own_wake : 1.0f;
        if (weight <= 0) continue;
        float uw[3] = {0, 0, 0};  // this car's air
        bool in = true;
        for (int k = 0; k < 3; k++) in &= p[k] >= wk.lo[k] - kReach && p[k] <= wk.hi[k] + kReach;
        if (!in) continue;
        // round the body as it is now: potential flow past a sphere, in the car's own proportions (a hull 2.2 x 0.9 x
        // 0.7 m half sizes, scaled to a unit sphere), and only ahead of the rear axle: behind a car the flow separates
        // into the wake (no potential flow there; taken as one, the wheels sat inside the sphere, where it carried the
        // air forward at more than the car's speed and the car's own smoke went with it rather than trailing)
        if (wk.moving && wk.speed > kMinSpeed) {
            const float r[3] = {p[0] - wk.at[0], p[1] - wk.at[1], p[2] - wk.at[2]};
            const float dir[3] = {wk.v[0] / wk.speed, wk.v[1] / wk.speed, wk.v[2] / wk.speed};
            const float along = Dot(r, dir);
            float side[3] = {dir[2], 0, -dir[0]};
            const float sl = std::sqrt(side[0] * side[0] + side[2] * side[2]);
            if (sl > 1e-3f) side[0] /= sl, side[2] /= sl;
            const float across = Dot(r, side), up = r[1] - dir[1] * along;
            const float q[3] = {along / g_tune.hull_long, across / g_tune.hull_wide, up / g_tune.hull_tall};
            const float d = std::sqrt(Dot(q, q));
            const float behind = std::clamp((along + 1.0f) / 1.0f, 0.0f, 1.0f);  // fades out from 0 to 1 m behind the middle
            if (d > 1.0f && d < 3.0f && behind > 0) {
                // the flow's direction from the scaled place, its strength 1/(2 d^3) of the car's speed
                const float m[3] = {q[0] * g_tune.hull_long, q[1] * g_tune.hull_wide, q[2] * g_tune.hull_tall};
                const float ml = std::sqrt(Dot(m, m));
                const float nl[3] = {m[0] / ml, m[1] / ml, m[2] / ml};
                const float n[3] = {nl[0] * dir[0] + nl[1] * side[0], nl[0] * dir[1] + nl[2], nl[0] * dir[2] + nl[1] * side[2]};
                const float f = g_tune.body * behind / (2 * d * d * d), un = Dot(wk.v, n);
                for (int k = 0; k < 3; k++) uw[k] += f * (3 * un * n[k] - wk.v[k]);
            }
        }
        // the wake: its nearest sample
        const Sample* best = nullptr;
        float best_d2 = kReach * kReach;
        for (uint32_t i = 0; i < wk.n; i++) {
            const Sample& sm = wk.s[(wk.head + kSamples - 1 - i) % kSamples];
            if (g_now - sm.born > g_tune.wake_life) break;
            const float r[3] = {p[0] - sm.at[0], p[1] - sm.at[1], p[2] - sm.at[2]};
            const float d2 = Dot(r, r);
            if (d2 < best_d2) best_d2 = d2, best = &sm;
        }
        if (!best) {
            for (int k = 0; k < 3; k++) u[k] += uw[k] * weight;
            continue;
        }
        const Sample& sm = *best;
        const float age = g_now - sm.born, fade = std::exp(-age / g_tune.wake_fade);
        const float width = g_tune.slip_width + g_tune.slip_spread * age, g = std::exp(-best_d2 / (width * width));
        for (int k = 0; k < 3; k++) uw[k] += sm.v[k] * g_tune.slip * fade * g;
        // a faster car's vortex is tighter: its core from full size at core_ref_speed down to core_min of it
        const float rc0 = g_tune.core * std::clamp(g_tune.core_ref_speed / std::max(sm.speed, 0.1f), g_tune.core_min, 1.0f);
        const float rc2 = rc0 * rc0 + 4 * g_tune.nu * age;
        const float road = sm.at[1] - g_tune.car_height;
        for (int side = -1; side <= 1; side += 2)
            for (int image = 0; image <= (g_tune.image_vortices != 0 ? 1 : 0); image++) {
            // the vortex, and its image under the road turning the other way: together no flow through the road, so
            // the downwash turns out along it instead of the turbulence and the road clamp ratcheting smoke upwards
            const float cy = sm.at[1] + sm.side[1] * g_tune.flank * float(side) + g_tune.vortex_up;
            const float c[3] = {sm.at[0] + sm.side[0] * g_tune.flank * float(side), image ? 2 * road - cy : cy,
                                sm.at[2] + sm.side[2] * g_tune.flank * float(side)};
            float r[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
            const float along = Dot(r, sm.dir);
            for (int k = 0; k < 3; k++) r[k] -= sm.dir[k] * along;
            const float rr = std::sqrt(Dot(r, r));
            if (rr < 1e-3f) continue;
            const float circulation = float(image ? side : -side) * g_tune.circulation * sm.speed * fade;  // down between them, out along the road
            const float vt = circulation / (2 * kPi * rr) * (1 - std::exp(-rr * rr / rc2)) * std::exp(-along * along / (g_tune.vortex_length * g_tune.vortex_length));
            const float n[3] = {r[0] / rr, r[1] / rr, r[2] / rr};
            float t[3];
            Cross(sm.dir, n, t);
            for (int k = 0; k < 3; k++) uw[k] += t[k] * vt;
        }
        if (weight * sm.speed * fade * g > stir) stir = weight * sm.speed * fade * g, stir_road = road;
        for (int k = 0; k < 3; k++) u[k] += uw[k] * weight;
    }
    // turbulence: the curl of psi = (sin(ky + a) cos(kz), sin(kz + b) cos(kx), sin(kx + c) cos(ky)), changing slowly
    if (stir > 0.05f) {
        const float k = g_tune.turbulence_scale, amp = g_tune.turbulence * stir / k, a = g_now * 0.7f, b = g_now * 0.9f + 1.7f, c = g_now * 1.1f + 3.1f;
        const float x = p[0] * k, y = p[1] * k, z = p[2] * k;
        // psi_x = sin(y+a)cos(z), psi_y = sin(z+b)cos(x), psi_z = sin(x+c)cos(y); u = curl psi (k folded into amp)
        const float dpz_dy = -std::sin(x + c) * std::sin(y), dpy_dz = std::cos(z + b) * std::cos(x);
        const float dpx_dz = -std::sin(y + a) * std::sin(z), dpz_dx = std::cos(x + c) * std::cos(y);
        const float dpy_dx = -std::sin(z + b) * std::sin(x), dpx_dy = std::cos(y + a) * std::cos(z);
        u[0] += amp * k * (dpz_dy - dpy_dz);
        u[1] += amp * k * (dpx_dz - dpz_dx) * std::clamp((p[1] - stir_road) / g_tune.turbulence_floor, 0.0f, 1.0f);  // the road damps it
        u[2] += amp * k * (dpy_dx - dpx_dy);
    }
}

uint32_t Density() { return std::clamp<uint32_t>(pgr2::smoke_density, 100, 400) / 100 * 100; }
float SizeScale() { return 1.0f / std::sqrt(float(Density()) / 100.0f); }

float Duration() { return std::clamp<uint32_t>(pgr2::smoke_duration, 50, 300) / 100.0f; }

// A full-intensity particle's life in seconds (held, faded from its start to 60, the tail's 60 steps) and the console's
// (2.59 s). The game makes a particle only to replace one that died (and, below its most, one half the frames), so
// particles born together die together and a wheel makes its smoke in waves with nothing between them, which shows
// once lives are longer. So each wheel also keeps a steady rate, the console's average (50 a console life) x the density,
// topping up each frame what the game's own left short, and its most is that rate x the life (and 30%).
float Life() {
    const Tune& t = g_tune;
    const float d = Duration();
    return t.hold + d / std::max(t.fade, 0.01f) * std::log(std::max(196.0f * t.opacity, 61.0f) / 60.0f) + 60.0f * d / std::max(t.tail, 0.1f);
}
const float kConsoleLife = std::log(196.0f / 60.0f) + 2.0f;
float WheelRate() { return 50.0f * float(Density()) / 100.0f / kConsoleLife; }
uint32_t WheelMax() {
    return std::min<uint32_t>(kListCap, uint32_t(std::lround(std::max(50.0f * float(Density()) / 100.0f, WheelRate() * (1 + g_tune.burn_rate) * Life() * 1.3f))));
}
uint32_t g_owner = 0;   // the car whose emitter is making particles now (0xF8D70)
// The car's frame (rows at +0x14, +0x24, +0x34 before its place at +0x44; the model's +z points back, its +y up), as
// three rows of 16 bytes or, failing that, 12: whichever reads as a rotation. false if neither.
bool CarFrame(uint32_t car, float up[3], float fwd[3]) {
    for (uint32_t stride : {16u, 12u}) {
        float r[3][3];
        for (uint32_t i = 0; i < 3; i++)
            for (uint32_t k = 0; k < 3; k++) r[i][k] = F(car + 0x44 - (3 - i) * stride + 4 * k);
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) ok = std::fabs(Dot(r[i], r[i]) - 1.0f) < 0.05f && std::fabs(Dot(r[i], r[(i + 1) % 3])) < 0.05f;
        if (!ok || r[1][1] < 0.5f) continue;
        for (int k = 0; k < 3; k++) up[k] = r[1][k], fwd[k] = -r[2][k];
        return true;
    }
    return false;
}
// A burning-out wheel (WheelVortexAir, and its puffs in 0xF8A50): set from its emitter while 0xF8D70 runs. A smoking
// wheel is a burnout by how slow its car is (its emitter's alpha the intensity x 196), the axle from the car's frame,
// out the side of the car the wheel is on.
// The tread's heat decides how much smoke there is (rubber breaks down into smoke from about 200 to 400 C; tyre wear
// studies find sliding, not road speed, drives what comes off): each smoking wheel's tread heats with its intensity (the
// game's slip measure) and cools towards the air, faster as the car moves; its smoke goes from smoke_min of full (scale,
// its alpha; a cold tyre) to full between smoke_onset and smoke_full, and a burnout's puffs off the tyre and its swirl
// with it, so a launch's brief wheelspin stays light, a burnout builds over seconds and a long drift settles between.
struct Burn {
    bool on = false;
    float b, scale = 1, up[3], fwd[3], axis[3], out[3];
} g_burn;
std::unordered_map<uint32_t, float> g_tread;  // each emitter's tread, degrees C
void SetBurn(uint32_t em) {
    const Tune& t = g_tune;
    g_burn.on = false, g_burn.scale = 1;
    // how much of a burnout this wheel is now: the car's speed and place (its dynamics' velocity, its +0x44)
    float b0 = 0, slide = 0, speed = 0, at[3] = {};
    const uint32_t dyn = g_owner ? R32(g_owner + kCarDynamics) : 0;
    if (dyn) {
        float v[3];
        for (int k = 0; k < 3; k++) v[k] = F(dyn + kDynVelocity + 4 * uint32_t(k)), at[k] = F(g_owner + kCarAt + 4 * uint32_t(k));
        speed = std::sqrt(Dot(v, v));
        if (!std::isfinite(speed) || !std::isfinite(at[0] + at[1] + at[2])) speed = 0;
        else if (!R32(em + kEmStopped)) {
            slide = std::min(1.0f, float(R32(em + 0x48) >> 24) / 196.0f);
            b0 = slide * std::clamp(1.0f - speed / std::max(t.burn_speed, 0.1f), 0.0f, 1.0f);
        }
    }
    const float dt = std::clamp(F(kFrameSeconds), 0.0f, 0.1f);
    auto [it, fresh] = g_tread.try_emplace(em, t.ambient);
    float& tread = it->second;
    tread += dt * (t.tread_heat * slide - (t.tread_cool + t.tread_cool_speed * speed) * (tread - t.ambient));
    if (!std::isfinite(tread)) tread = t.ambient;
    const float hot = std::clamp((tread - t.smoke_onset) / std::max(t.smoke_full - t.smoke_onset, 1.0f), 0.0f, 1.0f);
    g_burn.scale = t.smoke_min + (1 - t.smoke_min) * hot;
    const float b = b0 * hot;
    if (b <= 0.01f) return;
    if (!CarFrame(g_owner, g_burn.up, g_burn.fwd)) {
        static bool told = false;
        if (!told) told = true, LOGR("pgr2: smoke: no car frame at %08X+0x14 for the burnout", g_owner);
        return;
    }
    Cross(g_burn.up, g_burn.fwd, g_burn.axis);
    const float al = std::sqrt(Dot(g_burn.axis, g_burn.axis));
    if (al < 0.5f) return;
    float side = 0;
    for (int k = 0; k < 3; k++) g_burn.axis[k] /= al, side += (F(em + kEmPlace + 4 * uint32_t(k)) - at[k]) * g_burn.axis[k];
    for (int k = 0; k < 3; k++) g_burn.out[k] = g_burn.axis[k] * (side < 0 ? -1.0f : 1.0f);
    g_burn.b = b, g_burn.on = true;
}
// its vortex about the axle
void AddWheelVortex(uint32_t em) {
    const Tune& t = g_tune;
    if (!g_burn.on || !pgr2::smoke_physics || t.wheel_swirl <= 0) return;
    WheelVortex wv;
    for (int k = 0; k < 3; k++) wv.axis[k] = g_burn.axis[k], wv.c[k] = F(em + kEmPlace + 4 * uint32_t(k)) + g_burn.up[k] * t.tyre_radius;
    wv.road = F(em + kEmPlace + 4);
    wv.gamma = t.wheel_swirl * g_burn.b;
    if (g_wheel_vortices.size() < 64) g_wheel_vortices.push_back(wv);
}
uint32_t g_spawns = 0;  // particles made (0xF8A50), to count the game's own each frame
std::unordered_map<uint32_t, float> g_credit;  // each emitter's steady rate owed

// the records past the game's: made once, laid out as its own (0xF8BC0)
uint32_t g_extra = 0, g_next = 0;
uint32_t Extra() {
    if (g_extra) return g_extra;
    g_extra = mem::AllocSystem(kExtra * kRecord);
    if (!g_extra) return 0;
    std::memset(kernel::HostPtr(g_extra), 0, kExtra * kRecord);
    for (uint32_t i = 0; i < kExtra; i++) {
        W32(g_extra + i * kRecord, kClass);
        W32(g_extra + i * kRecord + kLink, kFree);
    }
    LOGR("pgr2: smoke: %u more particle records (%u in all)", kExtra, kAllRecords);
    return g_extra;
}

}  // namespace

// the pool's take (thiscall: the pool; a record or 0): past the game's 400, one of ours
X86_EXTERN_FUNC(__imp__sub_000F89B0);
X86_FUNC(sub_000F89B0) {
    const uint32_t pool = ctx.ecx;
    __imp__sub_000F89B0(ctx, base);
    if (ctx.eax || pool != kPool) return;
    const uint32_t extra = Extra();
    if (!extra) return;
    for (uint32_t n = 0; n < kExtra; n++) {
        const uint32_t i = (g_next + n) % kExtra, rec = extra + i * kRecord;
        if (R32(rec + kLink) != kFree) continue;
        W32(rec + kLink, 0);
        W32(kPoolUsed, R32(kPoolUsed) + 1);  // freed as the game's are, each a decrement of +4
        g_next = (i + 1) % kExtra;
        ctx.eax = rec;
        return;
    }
    static bool told = false;
    if (!told) told = true, LOGR("pgr2: smoke: all %u particle records in use", kAllRecords);
}


// Smoke in the sun and in shade (smoke_lighting): a wheel's smoke colour (0xF87F0, thiscall on the emitter at the wheel's
// +0x134: place, last place, colour, intensity, type; ret 0x14; the colour kept at the emitter's +0x48) is the base colour
// times the sun's (0x23620, 0x49EEA8), the same in a building's shade as in the sun. The car's own lighting takes its sun
// share from its brightness (+0x150C: 0.25 for each wheel on a triangle not flagged as shade, eased; 0x1EF90), the city's
// baked shadows; the smoke now does too, from 45% in full shade to all of it in the sun, as each particle is made.
constexpr uint32_t kEmitterColour = 0x48, kWheelEmitter = 0x134, kWheelCar = 0xD4, kCarBrightness = 0x150C;
X86_EXTERN_FUNC(__imp__sub_000F87F0);
X86_FUNC(sub_000F87F0) {
    const uint32_t emitter = ctx.ecx;
    __imp__sub_000F87F0(ctx, base);
    if (!pgr2::smoke_lighting) return;
    const uint32_t car = R32(emitter - kWheelEmitter + kWheelCar);
    if (!car) return;
    const float b = F(car + kCarBrightness);
    if (!(b >= 0.0f && b <= 1.5f)) return;
    const float k = g_tune.shade + (1.0f - g_tune.shade) * std::min(b, 1.0f);
    const uint32_t c = R32(emitter + kEmitterColour);
    uint32_t out = c & 0xFF000000u;
    for (int i = 0; i < 3; i++) out |= uint32_t(std::lround(float(c >> (8 * i) & 0xFF) * k)) << (8 * i);
    W32(emitter + kEmitterColour, out);
}


// Beside each record: the road under it (its height as it was made at the wheel; the wake's downwash pushes smoke down,
// the road stops it, so it spreads along the road instead of sinking out of sight) and its age (for tune.hold, lift).
struct Side {
    float ground, age;
    uint32_t car;  // the car whose wheel made it (0 not known)
    uint32_t colour;  // its colour as made; the record's +0x10 holds what is drawn (shaded, faded in, varied)
    uint32_t alpha;   // its life's alpha (the game's: from intensity x 196 down; 0 dead)
    float av, seed;   // its alpha's variation, a random number for its billow
    bool burnt;       // a burnout's puff off the tyre (no fade-in)
};
std::unordered_map<uint32_t, Side> g_side;

// A new particle (thiscall on the record: place, drift, colour, type, seconds already lived; the record in eax, ret
// 0x14): the game's 0xF8A50, rand() called in the same order (the place's x and z jitter but for type 2, then the rise,
// then the x and z spread), with the tuning's sizes and speeds, then a first update of the seconds given (0xF8630).
X86_FUNC(sub_000F8A50) {
    const uint32_t p = ctx.ecx, at = R32(ctx.esp + 4), drift = R32(ctx.esp + 8), colour = R32(ctx.esp + 12);
    const uint32_t type = R32(ctx.esp + 16), lived = R32(ctx.esp + 20);
    const Tune& t = g_tune;
    auto rnd = [&] { return float(kernel::CallGuest(ctx, kRand, {}) & 0x7FFF); };
    float pos[3] = {F(at), F(at + 4), F(at + 8)}, vel[3];
    for (int i = 0; i < 3; i++) vel[i] = F(drift + 4 * uint32_t(i)) * t.drift;
    float size;
    if (type == 2) {
        size = 0.1f;
    } else {
        size = t.spawn_size;
        pos[0] += (rnd() - 16383.0f) / 16383.0f * t.spawn_jitter;
        pos[2] += (rnd() - 16383.0f) / 16383.0f * t.spawn_jitter;
    }
    vel[1] += rnd() / 32767.0f * t.rise;
    vel[0] += (rnd() - 16383.0f) / 16383.0f * t.spread;
    vel[2] += (rnd() - 16383.0f) / 16383.0f * t.spread;
    for (int i = 0; i < 3; i++) PutF(p + 4 + 4 * uint32_t(i), pos[i]), PutF(p + 0x1C + 4 * uint32_t(i), vel[i]);
    // host random numbers for what the game never drew, so rand()'s sequence is the game's
    static uint32_t hseed = 0x2545F491u;
    auto hrand = [] { hseed = hseed * 1664525u + 1013904223u; return float(hseed >> 8) * (1.0f / 16777216.0f); };
    float sz = size * SizeScale() * (1 + (hrand() * 2 - 1) * t.size_var);  // more particles, each smaller (smoke_density)
    W32(p + 0x18, type);
    uint32_t c = R32(colour);
    uint32_t a = std::min<uint32_t>(uint32_t(std::lround(float(c >> 24) * std::max(t.opacity, 0.0f) * g_burn.scale)), 255);
    // A burnout's puffs (that share of them) come off the tyre itself: anywhere on its tread from the road up the back
    // (burn_arc), across its width, leaving along the tread (the tyre's top turning forward: back along the road, then up
    // the back of it) and spilling out of its outer face, bigger and denser, thick at once.
    bool burnt = false;
    if (g_burn.on && hrand() < g_burn.b) {
        const float th = hrand() * t.burn_arc * kPi / 180.0f, R = t.tyre_radius, w = (hrand() - 0.5f) * t.tyre_width;
        const float cs = std::cos(th), sn = std::sin(th), spill = t.burn_spill * (0.5f + hrand());
        for (int k = 0; k < 3; k++) {
            const float place = F(at + 4 * uint32_t(k)) + g_burn.up[k] * (R - R * cs) - g_burn.fwd[k] * R * sn + g_burn.axis[k] * w;
            const float tangent = -g_burn.fwd[k] * cs + g_burn.up[k] * sn;
            PutF(p + 4 + 4 * uint32_t(k), place);
            PutF(p + 0x1C + 4 * uint32_t(k), tangent * t.burn_eject * g_burn.b + g_burn.out[k] * spill + F(drift + 4 * uint32_t(k)) * t.drift);
        }
        sz *= t.burn_size;
        a = std::min<uint32_t>(uint32_t(std::lround(float(a) * t.burn_opacity)), 255);
        burnt = true;
    }
    PutF(p + 0x14, sz);
    W32(p + 0x10, (c & 0xFFFFFFu) | a << 24);
    g_side[p] = {F(at + 4) - t.ground, 0.0f, g_owner, (c & 0xFFFFFFu) | a << 24, a,
                 std::clamp(1 + (hrand() * 2 - 1) * t.opacity_var, 0.0f, 2.0f), hrand(), burnt};
    W32(p + 0x10, c & 0xFFFFFFu);  // drawn clear until its first frame (fade_in)
    g_spawns++;
    const uint32_t ecx = ctx.ecx;
    ctx.ecx = p;
    kernel::CallGuest(ctx, kUpdate, {lived});
    ctx.ecx = ecx;
    ctx.eax = p;
    ctx.esp += 0x18;  // ret 0x14
}

// More particles a wheel (smoke_density, percent of the console's 50), each smaller, so the same smoke is made of finer
// puffs that the air can pull apart: an emitter asks for 50 x the density (0xF87C0 from 0x232C0), its list holds up to
// kListCap (0xF8C00, the game's 0x32), it grows density times as fast (0xF8D70: the game adds one, half the frames, while
// it has fewer than it asked for; the extra are made between the wheel's last and current places, as replacements are,
// with the game's rand() state put back after, so the game's own draws from it are unchanged), and the draw (0xF8F50,
// whose stack holds 50) is called once for each 48 of the list.
X86_EXTERN_FUNC(__imp__sub_000F87C0);
X86_FUNC(sub_000F87C0) {
    const uint32_t em = ctx.ecx;
    __imp__sub_000F87C0(ctx, base);
    W32(em + kEmMax, std::min(R32(em + kEmMax) * Density() / 100, kListCap));
}

// the list's add (thiscall: the list, a record; al added, ret 4): the game's with the cap raised
X86_FUNC(sub_000F8C00) {
    const uint32_t list = ctx.ecx, rec = R32(ctx.esp + 4);
    uint32_t ok = 0;
    if (rec) {
        if (R32(list + kListCount) < kListCap) {
            if (!R32(list + kListHead)) W32(list + kListHead, rec);
            else W32(R32(list + kListTail) + kLink, rec);
            W32(list + kListTail, rec);
            W32(list + kListCount, R32(list + kListCount) + 1);
            ok = 1;
        } else {
            W32(rec + kLink, kFree);
            W32(kPoolUsed, R32(kPoolUsed) - 1);
        }
    }
    ctx.eax = (ctx.eax & ~0xFFu) | ok;
    ctx.esp += 8;  // ret 4
}

// the emitter's frame (thiscall: the emitter; al 1): the game's, then the extra particles
X86_EXTERN_FUNC(__imp__sub_000F8D70);
X86_FUNC(sub_000F8D70) {
    const uint32_t em = ctx.ecx;
    g_owner = R32(em - kWheelEmitter + kWheelCar);  // the wheel's car
    struct Clear {
        ~Clear() { g_owner = 0, g_burn.on = false, g_burn.scale = 1; }
    } clear;
    SetBurn(em);
    // PGR2_SMOKE_LOG: is the wheel's car one of the race cars the wakes follow?
    static const bool log = std::getenv("PGR2_SMOKE_LOG") != nullptr;
    if (log && g_owner && !R32(em + kEmStopped)) {
        static uint32_t checked = 0, matched = 0;
        for (uint32_t w = 0; w < g_wakes_used; w++) matched += g_wakes[w].car == g_owner;
        if (++checked == 2000) LOGR("smoke: %u of %u smoking wheels' cars (+0xD4) are race cars (e.g. %08X; the first %08X)", matched, checked, g_owner, g_wakes[0].car);
    }
    W32(em + kEmMax, WheelMax());
    const uint32_t made_before = g_spawns;
    __imp__sub_000F8D70(ctx, base);
    const uint32_t made = g_spawns - made_before;  // the game's own this frame
    if (R32(em + kEmStopped)) {
        g_credit.erase(em);
        return;
    }
    const float dt = F(kFrameSeconds);
    AddWheelVortex(em);
    float& credit = g_credit[em];
    credit = std::clamp(credit + WheelRate() * (1 + g_tune.burn_rate * (g_burn.on ? g_burn.b : 0.0f)) * std::clamp(dt, 0.0f, 0.1f) - float(made), -3.0f, 3.0f);
    if (credit < 1.0f || R32(em + kEmDelay)) return;
    static uint32_t seed = 0x9E3779B9u;
    auto host_rand = [] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) * (1.0f / 16777216.0f); };
    // rand()'s state put back after, so the game's own draws from it are unchanged
    const uint32_t eax = ctx.eax, tid = kernel::CallGuest(ctx, kTidData, {}), state = R32(tid + kRandState);
    while (credit >= 1.0f && R32(em + kEmCount) < R32(em + kEmMax)) {
        const uint32_t before = R32(em + kEmCount), ecx = ctx.ecx;
        ctx.ecx = em;
        kernel::CallGuest(ctx, kEmSpawn, {R32(em + kEmType)});
        ctx.ecx = ecx;
        if (R32(em + kEmCount) == before) break;
        credit -= 1.0f;
        // between the last place and this one
        const uint32_t p = R32(em + 0x50 + kListTail);
        const float r = host_rand();
        for (int c = 0; c < 3; c++)
            PutF(p + 4 + 4 * c, F(p + 4 + 4 * c) + (F(em + kEmLast + 4 * c) - F(em + kEmPlace + 4 * c)) * r);
    }
    W32(tid + kRandState, state);
    ctx.eax = eax;
}

// the screen the particles from rec on (count of them, 0 all) cover, as the draw's first pass sums it: each one's
// projected size (0x15D70 on the camera 0x3CD804: out, place, 0; the out's +0xC its scale), size x camera +0x130 x
// width x scale, at most the width, squared
float Coverage(X86Context& ctx, uint32_t rec, uint32_t count) {
    const uint32_t cam = R32(kCamera);
    if (!cam) return 0;
    const float w = float(int32_t(R32(cam + 0x30))), k = F(cam + 0x130);
    const uint32_t esp = ctx.esp, ecx = ctx.ecx, eax = ctx.eax;
    ctx.esp -= 0x40;
    const uint32_t out = ctx.esp + 0x10;
    float sum = 0;
    for (uint32_t i = 0; rec && (!count || i < count); i++, rec = R32(rec + kLink)) {
        ctx.ecx = cam;
        if (!(kernel::CallGuest(ctx, kProject, {out, rec + 4, 0}) & 0xFF)) continue;
        const float a = std::min(w, F(rec + 0x14) * k * w * F(out + 0x0C));
        sum += a * a;
    }
    ctx.esp = esp, ctx.ecx = ecx, ctx.eax = eax;
    return sum;
}

// the smoke draw (thiscall: the emitter): its stack array holds 50 particles, so a longer list is drawn 48 at a time.
// The draw fades the emitter's smoke when it covers much of the screen (alpha x min(1, 2 sqrt(screen / the sum of the
// squared projected sizes))); a part of the list sums less than the whole, so each part's alphas are scaled by the
// whole list's fade over the part's while it is drawn, and put back after.
X86_EXTERN_FUNC(__imp__sub_000F8F50);
X86_FUNC(sub_000F8F50) {
    const uint32_t em = ctx.ecx, list = em + 0x50;
    const uint32_t n = R32(list + kListCount);
    if (n <= kDrawChunk) {
        __imp__sub_000F8F50(ctx, base);
        return;
    }
    const uint32_t head = R32(list + kListHead), tail = R32(list + kListTail), esp = ctx.esp;
    const uint32_t cam = R32(kCamera);
    const float screen = cam ? float(int32_t(R32(cam + 0x30)) * int32_t(R32(cam + 0x34))) : 0;
    auto fade = [&](float sum) { return sum > 0 ? std::min(1.0f, 2 * std::sqrt(screen / sum)) : 1.0f; };
    const float whole = fade(Coverage(ctx, head, 0));
    uint32_t alpha[kDrawChunk];
    for (uint32_t start = head; start;) {
        uint32_t last = start, c = 1;
        while (c < kDrawChunk && R32(last + kLink)) last = R32(last + kLink), c++;
        const uint32_t next = R32(last + kLink);
        W32(last + kLink, 0);
        W32(list + kListHead, start), W32(list + kListTail, last), W32(list + kListCount, c);
        static bool told = false;
        if (!told) told = true, LOGR("pgr2: smoke: an emitter of %u particles drawn %u at a time", n, kDrawChunk);
        const float part = fade(Coverage(ctx, start, c)), scale = part > 0 ? whole / part : 1.0f;
        if (scale < 0.999f) {
            uint32_t i = 0;
            for (uint32_t q = start; q && i < c; q = R32(q + kLink), i++) {
                alpha[i] = R32(q + 0x10);
                W32(q + 0x10, (alpha[i] & 0xFFFFFFu) | uint32_t(std::lround(float(alpha[i] >> 24) * scale)) << 24);
            }
        }
        ctx.esp = esp;  // the same return address each time
        ctx.ecx = em;
        __imp__sub_000F8F50(ctx, base);
        if (scale < 0.999f) {
            uint32_t i = 0;
            for (uint32_t q = start; q && i < c; q = R32(q + kLink), i++) W32(q + 0x10, alpha[i]);
        }
        W32(last + kLink, next);
        start = next;
    }
    W32(list + kListHead, head), W32(list + kListTail, tail), W32(list + kListCount, n), W32(list + kListCursor, 0);
}

// a smoke particle's frame (thiscall: the particle, seconds; al alive, ret 4): the game's 0xF8630 but for the alpha's
// fade, Duration() times slower and its tail counted in time
X86_FUNC(sub_000F8630) {
    const uint32_t p = ctx.ecx;
    float t = F(ctx.esp + 4);
    const float dt = t;
    const uint32_t type = R32(p + 0x18);
    // the air's velocity here (still, but for the cars' wakes)
    float air[3] = {0, 0, 0};
    float car_air = 0;  // how strongly the cars move the air here (tune.wake_follow)
    if (pgr2::smoke_physics && (g_wakes_used || !g_wheel_vortices.empty())) {
        const float at[3] = {F(p + 0x04), F(p + 0x08), F(p + 0x0C)};
        const auto own = g_side.find(p);
        AirAt(at, air, own != g_side.end() ? own->second.car : 0);
        car_air = std::sqrt(Dot(air, air));
        WindAt(at, own != g_side.end() ? own->second.ground : at[1], air);
        // PGR2_SMOKE_LOG=1: what the air model does to the particles, every 10 s
        static const bool log = std::getenv("PGR2_SMOKE_LOG") != nullptr;
        if (log) {
            static uint64_t n = 0, moved = 0, near2 = 0;
            static double dsum = 0, airsum = 0;
            static float last_log = 0;
            float best = 1e30f, bc[3] = {};
            for (uint32_t w = 0; w < g_wakes_used; w++) {
                const float r[3] = {at[0] - g_wakes[w].at[0], at[1] - g_wakes[w].at[1], at[2] - g_wakes[w].at[2]};
                const float d2 = Dot(r, r);
                if (d2 < best) best = d2, std::memcpy(bc, g_wakes[w].at, sizeof bc);
            }
            const float a = std::sqrt(Dot(air, air));
            n++, dsum += std::sqrt(best), airsum += a, moved += a > 0.5f, near2 += best < 16.0f;
            if (g_now - last_log > 10.0f) {
                uint32_t moving = 0, samples = 0;
                float top = 0;
                for (uint32_t w = 0; w < g_wakes_used; w++) moving += g_wakes[w].moving, samples += g_wakes[w].n, top = std::max(top, g_wakes[w].speed);
                LOGR("smoke physics: %u particles alive, %llu updates, %llu within 4 m of a car, %llu with air over 0.5 m/s (mean %.2f), mean distance to the "
                     "nearest car %.1f m; %u cars, %u moving (top %.1f m/s), %u wake samples; a particle at %.1f %.1f %.1f, its car at %.1f %.1f %.1f",
                     R32(kPoolUsed), (unsigned long long)n, (unsigned long long)near2, (unsigned long long)moved, n ? airsum / double(n) : 0.0,
                     n ? dsum / double(n) : 0.0, g_wakes_used, moving, top, samples, at[0], at[1], at[2], bc[0], bc[1], bc[2]);
                n = moved = near2 = 0, dsum = airsum = 0, last_log = g_now;
            }
        }
    }
    // PGR2_SMOKE_WIND="x y z": a steady wind on every particle (a test that the air reaches what is drawn)
    static float wind[3] = {0, 0, 0};
    static const bool windy = [] {
        const char* w = std::getenv("PGR2_SMOKE_WIND");
        return w && std::sscanf(w, "%f %f %f", &wind[0], &wind[1], &wind[2]) == 3;
    }();
    if (windy) for (int k = 0; k < 3; k++) air[k] += wind[k];
    // place by velocity
    PutF(p + 0x04, F(p + 0x04) + F(p + 0x1C) * t);
    PutF(p + 0x08, F(p + 0x08) + F(p + 0x20) * t);
    PutF(p + 0x0C, F(p + 0x0C) + F(p + 0x24) * t);
    // not below the road it was made on
    const Tune& tn = g_tune;
    const auto side = g_side.find(p);
    float age = 0;
    if (side != g_side.end()) {
        age = side->second.age += dt;
        if (pgr2::smoke_physics && F(p + 0x08) < side->second.ground) {
            PutF(p + 0x08, side->second.ground);
            if (F(p + 0x20) < 0) PutF(p + 0x20, 0.0f);
        }
    }
    air[1] += tn.lift * std::exp(-age);  // hot smoke rising, cooling
    // the plume's own slow eddies, growing with its age (air it takes in), damped near the road as the wake's turbulence
    if (tn.billow > 0 && side != g_side.end()) {
        const float k = tn.billow_scale, amp = tn.billow * std::min(1.0f, age / std::max(tn.billow_grow, 0.01f));
        const float tt = g_now * 0.35f, a = tt * 0.7f, b = tt * 0.9f + 1.7f, c = tt * 1.1f + 3.1f;
        const float x = (F(p + 0x04) + side->second.seed * 7) * k, y = F(p + 0x08) * k, z = F(p + 0x0C) * k;
        const float floor = std::clamp((F(p + 0x08) - side->second.ground) / tn.turbulence_floor, 0.0f, 1.0f);
        air[0] += amp * (-std::sin(x + c) * std::sin(y) - std::cos(z + b) * std::cos(x));
        air[1] += amp * (-std::sin(y + a) * std::sin(z) - std::cos(x + c) * std::cos(y)) * floor;
        air[2] += amp * (-std::sin(z + b) * std::sin(x) - std::cos(y + a) * std::cos(z));
    }
    // types 3 and 4 lose upward speed; type 3 lives at twice the pace
    if (type == 3 || type == 4) {
        PutF(p + 0x20, F(p + 0x20) - t * 4.0f);
        if (type == 3) t *= 2.0f;
    }
    // towards the air's velocity at the game's rate (towards still air, as the game has it, where no car has passed)
    // towards the air at the damping rate, faster where the cars' air is strong, so smoke follows a tight vortex
    const float damp = std::max(0.0f, 1.0f - t * (tn.damping + tn.wake_follow * std::min(1.0f, car_air / 5.0f)));
    PutF(p + 0x1C, air[0] + (F(p + 0x1C) - air[0]) * damp);
    PutF(p + 0x20, air[1] + (F(p + 0x20) - air[1]) * damp);
    PutF(p + 0x24, air[2] + (F(p + 0x24) - air[2]) * damp);
    // growth: type 1 slowing as it grows
    const float size = F(p + 0x14);
    const float ss = SizeScale();  // growth scaled with the size it started at
    PutF(p + 0x14, type == 1 ? size + t * ss * ss * tn.growth / size : size + t * ss);
    if (type == 3) t *= 2.0f;
    // alpha
    const float d = Duration();
    uint32_t colour = R32(p + 0x10), a = side != g_side.end() ? side->second.alpha : colour >> 24;
    if (a < 60) {
        // the game's rand(), as it was called; the tail one step each 1/30 s (the console's frame), times the duration
        // (CallGuest returns rand()'s eax and puts the caller's registers back; reading ctx.eax after it gave the
        // caller's eax, the particle's address, so each particle's 'random' number was fixed and, where it was above
        // 30 dt / duration, its tail never fell and it never died)
        const float r = float(kernel::CallGuest(ctx, kRand, {}) & 0x7FFF) * (1.0f / 32768.0f);
        if (tn.tail * dt / d > r && a) a--;
    } else if (age >= tn.hold) {
        const float k = std::max(0.0f, 1.0f - t * tn.fade / d);
        a = std::min<uint32_t>(uint32_t(std::max(0.0f, k * float(a))), 255);
    }
    if (side != g_side.end()) {
        // what is drawn: faded in as it leaves the tyre, varied per puff, darker near the road
        Side& sd = side->second;
        sd.alpha = a;
        const float in = tn.fade_in > 0 && !sd.burnt ? std::min(1.0f, age / tn.fade_in) : 1.0f;
        const uint32_t shown = std::min<uint32_t>(uint32_t(std::lround(float(a) * in * sd.av)), 255);
        const float lit = tn.shade_low + (1 - tn.shade_low) * std::clamp((F(p + 0x08) - sd.ground) / std::max(tn.shade_height, 0.01f), 0.0f, 1.0f);
        colour = shown << 24;
        for (int i = 0; i < 3; i++) colour |= uint32_t(std::lround(float(sd.colour >> (8 * i) & 0xFF) * lit)) << (8 * i);
    } else {
        colour = (colour & 0xFFFFFFu) | a << 24;
    }
    W32(p + 0x10, colour);
    ctx.eax = (ctx.eax & ~0xFFu) | (a ? 1u : 0u);
    ctx.esp += 8;  // ret 4
}

namespace pgr2 {
// before each frame step, on the game thread: each race car's wake for the smoke (AirAt)
void SmokeFrame() {
    static auto checked = std::chrono::steady_clock::time_point{};
    if (const auto now = std::chrono::steady_clock::now(); now - checked > std::chrono::seconds(1)) checked = now, LoadTune();
    g_wheel_vortices.clear();
    static bool racing = false;
    if (!smoke_physics || !kernel::G<uint8_t>(0x49E110)) {
        for (Wake& w : g_wakes) w.n = 0, w.moving = false, w.last = -1e9f;
        g_wakes_used = 0;
        racing = false;
        return;
    }
    if (!racing) {  // a race starts: its wind's direction
        racing = true;
        static uint32_t seed = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
        seed = seed * 1664525u + 1013904223u;
        g_wind_dir = float(seed >> 8) * (360.0f / 16777216.0f);
    }
    const float step = F(kFrameSeconds);
    if (step > 0 && step < 0.5f) g_now += step;
    const uint32_t count = std::min<uint32_t>(R32(kCars + kCarCount), kMaxCars);
    g_wakes_used = count;
    const float up[3] = {0, 1, 0};
    for (uint32_t k = 0; k < count; k++) {
        Wake& w = g_wakes[k];
        const uint32_t car = kCars + kCarFirst + k * kCarStride, dyn = R32(car + kCarDynamics);
        w.car = car;
        w.moving = false;
        if (!dyn) continue;
        float at[3], v[3];
        for (int i = 0; i < 3; i++) at[i] = F(car + kCarAt + uint32_t(i) * 4), v[i] = F(dyn + kDynVelocity + uint32_t(i) * 4);
        const float speed = std::sqrt(Dot(v, v));
        if (!std::isfinite(at[0] + at[1] + at[2]) || !(speed < 200.0f)) continue;
        // a car moved far at once (a reset onto the road) starts a new wake
        if (w.n) {
            const Sample& last = w.s[(w.head + kSamples - 1) % kSamples];
            const float r[3] = {at[0] - last.at[0], at[1] - last.at[1], at[2] - last.at[2]};
            if (Dot(r, r) > 30.0f * 30.0f) w.n = 0;
        }
        std::memcpy(w.at, at, sizeof at), std::memcpy(w.v, v, sizeof v), w.speed = speed;
        w.moving = speed >= kMinSpeed;
        if (w.moving && g_now - w.last >= kSampleEvery) {
            Sample& sm = w.s[w.head];
            std::memcpy(sm.at, at, sizeof at), std::memcpy(sm.v, v, sizeof v);
            for (int i = 0; i < 3; i++) sm.dir[i] = v[i] / speed;
            Cross(sm.dir, up, sm.side);
            const float sl = std::sqrt(Dot(sm.side, sm.side));
            for (int i = 0; i < 3; i++) sm.side[i] = sl > 1e-3f ? sm.side[i] / sl : 0.0f;
            sm.speed = speed, sm.born = g_now;
            w.head = (w.head + 1) % kSamples, w.n = std::min(w.n + 1, kSamples), w.last = g_now;
        }
        // the wake's bounds, for a quick test per particle
        for (int i = 0; i < 3; i++) w.lo[i] = w.hi[i] = at[i];
        for (uint32_t i = 0; i < w.n; i++) {
            const Sample& sm = w.s[(w.head + kSamples - 1 - i) % kSamples];
            if (g_now - sm.born > g_tune.wake_life) break;
            for (int c = 0; c < 3; c++) w.lo[c] = std::min(w.lo[c], sm.at[c]), w.hi[c] = std::max(w.hi[c], sm.at[c]);
        }
    }
}
}  // namespace pgr2
