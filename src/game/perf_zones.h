// the game's functions timed by the performance recorder (app/perf.h), in the order src/game/draw_filter.cpp names them
#pragma once

namespace pgr2 {
enum ZoneId { kZGameView, kZWaterView, kZCubePass, kZCarCubes, kZFlush, kZQueueCars, kZWorldDraw, kZShadows };
}
