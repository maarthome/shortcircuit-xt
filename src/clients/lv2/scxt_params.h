/*
 * scxt_params.h - the LV2 patch parameters of the Shortcircuit XT LV2 that the
 * MOD panel uses (group selection, file browser and loading, group settings, EG1).
 *
 * Part of the maarthome/shortcircuit-xt "My_Changes" branch.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 *
 * Shared by the plugin (clap2lv2.cpp / scxt_bridge.cpp), the .ttl generator
 * (clap2lv2_ttl.cpp) and, through the .ttl, the panel. Values are in the
 * engine's own units; the panel formats them.
 */
#pragma once

#include <cstdint>

namespace scxt_lv2
{

// URI of a parameter: CLAP2LV2_URI "-" key, e.g. ...#lv2-group_volume
enum ParamId : uint32_t
{
    P_GROUP = 0,      // which group of part 1 the panel edits (0-based)
    P_GROUP_COUNT,    // number of groups in part 1 (read only)
    P_LOAD_MODE,      // 0 = add to part 1, 1 = replace part 1
    P_LOADED,         // increases after each load, preset change or structure change (read only)

    // group settings (engine::Group::GroupOutputInfo)
    P_VOLUME,         // amplitude, cubic: dB = 60 log10(v), 0 .. +12 dB
    P_PAN,            // -1 .. 1
    P_TUNING,         // semitones
    P_PB_DOWN,        // semitones
    P_PB_UP,          // semitones
    P_GLIDE_TIME,     // 0..1, seconds = (exp(0.6931 + x * 9.4336) - 2) / 1000
    P_GLIDE_MODE,     // 0 = time, 1 = rate
    P_PLAY_MODE,      // 0 poly, 1 mono, 2 legato, 3 piano
    P_NOTE_PRIORITY,  // 0 latest, 1 highest, 2 lowest
    P_MIDI_CHANNEL,   // -1 = follow part, 0..15
    P_POLY_LIMIT,     // 0 = part's limit, else voices
    P_EXCLUSIVE,      // exclusive group, 0 = off
    P_VEL_SENS,       // velocity sensitivity 0..1

    // group EG1 (amp envelope), engine::Group::gegStorage[0]; times 0..1
    P_EG_DLY,
    P_EG_A,
    P_EG_H,
    P_EG_D,
    P_EG_S,
    P_EG_R,

    // text parameters (atom:String)
    P_BROWSE,         // panel -> plugin: list this folder (empty: the last one)
    P_LISTING,        // plugin -> panel: the folder listing, in chunks (see scxt_bridge.cpp)
    P_LOAD_PATH,      // panel -> plugin: load this file (sfz, sample, ...) with load_mode
    P_FOLDERS,        // plugin -> panel: saved folders, separated by '|'
    P_FOLDER_ADD,     // panel -> plugin: save this folder
    P_FOLDER_REMOVE,  // panel -> plugin: forget this folder

    P_NUM
};

constexpr uint32_t P_FIRST_STRING = P_BROWSE;
inline bool isStringParam(uint32_t p) { return p >= P_FIRST_STRING && p < P_NUM; }

struct ParamDef
{
    const char *key;
    const char *label;
    float min, max, def;
    bool writable; // false = output only (plugin -> panel)
    bool integer;
};

inline const ParamDef paramDefs[P_NUM] = {
    {"group", "Group", 0, 63, 0, true, true},
    {"group_count", "Group count", 0, 64, 0, false, true},
    {"load_mode", "Load mode", 0, 1, 0, true, true},
    {"loaded", "Loaded", 0, 1e9f, 0, false, true},

    {"group_volume", "Volume", 0, 1.5849f, 1, true, false},
    {"group_pan", "Pan", -1, 1, 0, true, false},
    {"group_tuning", "Tuning", -24, 24, 0, true, false},
    {"group_pb_down", "Bend down", -48, 48, -2, true, false},
    {"group_pb_up", "Bend up", -48, 48, 2, true, false},
    {"group_glide_time", "Glide", 0, 1, 0, true, false},
    {"group_glide_mode", "Glide mode", 0, 1, 0, true, true},
    {"group_play_mode", "Play mode", 0, 3, 0, true, true},
    {"group_note_priority", "Note priority", 0, 2, 0, true, true},
    {"group_midi_channel", "MIDI channel", -1, 15, -1, true, true},
    {"group_poly_limit", "Polyphony", 0, 128, 0, true, true},
    {"group_exclusive", "Exclusive group", 0, 32, 0, true, true},
    {"group_vel_sens", "Velocity", 0, 1, 0.6f, true, false},

    {"eg1_delay", "Dly", 0, 1, 0, true, false},
    {"eg1_attack", "A", 0, 1, 0, true, false},
    {"eg1_hold", "H", 0, 1, 0, true, false},
    {"eg1_decay", "D", 0, 1, 0, true, false},
    {"eg1_sustain", "S", 0, 1, 1, true, false},
    {"eg1_release", "R", 0, 1, 0.5f, true, false},

    {"browse", "Browse folder", 0, 0, 0, true, false},
    {"listing", "Folder listing", 0, 0, 0, false, false},
    {"load_path", "Load file", 0, 0, 0, true, false},
    {"folders", "Saved folders", 0, 0, 0, false, false},
    {"folder_add", "Save folder", 0, 0, 0, true, false},
    {"folder_remove", "Forget folder", 0, 0, 0, true, false},
};

} // namespace scxt_lv2
