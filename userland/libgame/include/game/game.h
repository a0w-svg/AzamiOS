/* ============================================================================
 * AzamiOS Game Framework — Master Include Header
 * File: userland/libgame/include/game/game.h
 *
 * Single header to pull in the entire AzamiOS game development framework:
 *
 *   • game_math.h    — 2D vectors, rects, fixed-point math, LUTs, RNG
 *   • game_input.h   — Keyboard, mouse, and action binding abstraction
 *   • game_ecs.h     — Entity-Component-System architecture
 *   • game_physics.h — 2D rigid bodies, spatial hash broadphase, colliders
 *   • game_render.h  — Sprites, animations, tilemaps, 2D camera, particles
 *   • game_audio.h   — Retro waveform synthesis, sound effects mixer
 *   • game_asset.h   — Asset manager (images, PPM loader, procedural textures)
 *   • game_scene.h   — Scene / state machine stack with transitions
 *   • game_core.h    — Window bootstrap, autonomous timer loop, game_run()
 * ============================================================================ */
#pragma once

/* Windowing & toolkit dependencies */
#if __has_include(<ui_kit.h>)
#include <ui_kit.h>
#elif __has_include("ui_kit.h")
#include "ui_kit.h"
#elif __has_include("../apps/shared/ui_kit.h")
#include "../apps/shared/ui_kit.h"
#elif __has_include("../../apps/shared/ui_kit.h")
#include "../../apps/shared/ui_kit.h"
#endif

/* Game subsystems in dependency order */
#include "game_math.h"
#include "game_math3d.h"
#include "game_input.h"
#include "game_ecs.h"
#include "game_physics.h"
#include "game_render.h"
#include "game_render3d.h"
#include "game_audio.h"
#include "game_asset.h"
#include "game_scene.h"
#include "game_core.h"
