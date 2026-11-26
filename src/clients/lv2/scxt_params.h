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
    P_PROC_ROUTING,   // effect routing: 0 S1 (1>2>3>4), 1 S2, 2 S3, 3 P1, 4 P2, 5 P3, 6 bypass
    P_ROUTE_TO,       // output: -1 default (part), 0 main, 1..16 bus, 17..20 aux
    P_OVERSAMPLE,     // 0 off, 1 auto, 2 on

    // group EGs 1..5 (engine::Group::gegStorage); P_EG_INDEX picks the one the panel edits;
    // times 0..1
    P_EG_INDEX,
    P_EG_DLY,
    P_EG_A,
    P_EG_H,
    P_EG_D,
    P_EG_S,
    P_EG_R,
    P_EG_ASH,         // attack / decay / release shape -1..1
    P_EG_DSH,
    P_EG_RSH,
    P_EG_SYNC,        // tempo sync 0/1
    P_EG_GATE,        // 0 gated (DAHDSR), 1 no sustain, 2 oneshot
    P_EG_GATE_ANY,    // gate on voice sounding 0/1

    // group LFOs 1..4 (engine::Group::modulatorStorage); P_LFO_INDEX picks the one shown
    P_LFO_INDEX,
    P_LFO_SHAPE,      // ModulatorStorage::ModulatorShape (0 step .. 8 env)
    P_LFO_TRIGGER,    // 0 keytrig, 1 songpos, 2 random, 3 release, 4 oneshot
    P_LFO_RATE,       // -7..9 (2^x Hz)
    P_LFO_PHASE,      // 0..1
    P_LFO_DEFORM,     // -1..1
    P_LFO_ANGLE,      // -1..1
    P_LFO_SYNC,       // 0/1
    P_LFO_UNI,        // unipolar 0/1
    P_LFO_USEENV,     // curve LFO envelope (D A R) on 0/1
    P_LFO_DLY,        // curve LFO envelope times 0..1
    P_LFO_ATK,
    P_LFO_REL,
    P_LFOE_DLY,       // the ENV shape's DAHDSR
    P_LFOE_A,
    P_LFOE_H,
    P_LFOE_D,
    P_LFOE_S,
    P_LFOE_R,
    P_LFOE_LOOP,

    // part 1 (engine::Part::PartConfiguration) and its sends to the aux busses
    P_PART_VOLUME,    // level, cubic amplitude 0..1 (0 dB = 1)
    P_PART_PAN,       // -1 .. 1
    P_PART_TUNING,    // -36 .. 36 semitones (fine)
    P_PART_TRANSPOSE, // -36 .. 36 keys
    P_PART_CHANNEL,   // -1 omni, 0..15
    P_PART_POLY,      // 0 = no limit, else voices
    P_PART_MUTE,      // 0/1
    P_PART_SOLO,      // 0/1
    P_SEND1,          // send level to aux 1..4, 0..1
    P_SEND2,
    P_SEND3,
    P_SEND4,

    // text parameters (atom:String)
    P_BROWSE,         // panel -> plugin: list this folder (empty: the last one)
    P_LISTING,        // plugin -> panel: the folder listing, in chunks (see scxt_bridge.cpp)
    P_LOAD_PATH,      // panel -> plugin: load this file (sfz, sample, ...) with load_mode
    P_FOLDERS,        // plugin -> panel: saved folders, separated by '|'
    P_FOLDER_ADD,     // panel -> plugin: save this folder
    P_FOLDER_REMOVE,  // panel -> plugin: forget this folder
    P_FX_TYPES,       // plugin -> panel: the effect (processor) types, chunked
    P_FX_DESC,        // plugin -> panel: one slot's controls (names, ranges, choices), chunked
    P_FX_VALUES,      // plugin -> panel: one slot's values and their display texts, chunked
    P_FX_EDIT,        // panel -> plugin: "<slot>,<t|a|m|l|f|i>,<index>,<value>"
    P_MOD_META,       // plugin -> panel: mod matrix sources, targets and curves, chunked
    P_MOD_ROWS,       // plugin -> panel: the 18 mod matrix rows, chunked
    P_MOD_EDIT,       // panel -> plugin: "<row>,<a|s|v|c|t|d>,<value>"
    P_NAMES,          // plugin -> panel: part 1's groups "<name>^<zones>^<zone name>^<zone name>...~...", chunked
    P_MODS_TEXT,      // plugin -> panel: value texts "E|<6 EG times>|L|<rate phase deform angle d a r>|V|<6 env-LFO>|P|<part vol pan tuning>"
    P_PFX_TYPES,      // plugin -> panel: part effect types "<id>^<name>~..."
    P_PFX_DESC,       // plugin -> panel: one part effect slot's controls, chunked
    P_PFX_VALUES,     // plugin -> panel: one part effect slot's values and texts, chunked
    P_PFX_EDIT,       // panel -> plugin: "<slot>,<t|a|y|p>,<index>,<value>"
    P_GROUP_CMD,      // panel -> plugin: "r,<group>,<name>" rename, "d,<group>" delete,
                      //                  "m,<group>,<0|1>" mute, "s,<group>,<0|1>" solo
    P_SAVE_PART,      // panel -> plugin: "<0|1>,<name or path>" save part 1 as .scp (1 = with samples)
    P_SAVED,          // plugin -> panel: "ok|<path>" or "error|<text>"
    P_MISC_TEXT,      // plugin -> panel: phasors, randoms, envelope followers (see scxt_bridge.cpp)
    P_MISC_EDIT,      // panel -> plugin: "<p|r|f>,<index>,<field>,<value>"
    P_MACRO_NAMES,    // plugin -> panel: the 16 macro names joined with '~'
    P_MACRO_EDIT,     // panel -> plugin: "<macro>,<name>"
    P_PART_FILE,      // plugin -> panel: the .scp part 1 was loaded from or last saved to ("" = none)

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
    {"group", "Group (-1 = all)", -1, 63, -1, true, true},
    {"group_count", "Group count", 0, 64, 0, false, true},
    {"load_mode", "Load mode (0 add, 1 replace part, 2 replace group)", 0, 2, 0, true, true},
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

    {"group_proc_routing", "Effect routing", 0, 6, 0, true, true},
    {"group_route_to", "Output", -1, 20, -1, true, true},
    {"group_oversample", "Oversample", 0, 2, 1, true, true},
    {"eg_index", "Group EG (0..4 = EG 1..5)", 0, 4, 0, true, true},
    {"eg1_delay", "Dly", 0, 1, 0, true, false},
    {"eg1_attack", "A", 0, 1, 0, true, false},
    {"eg1_hold", "H", 0, 1, 0, true, false},
    {"eg1_decay", "D", 0, 1, 0, true, false},
    {"eg1_sustain", "S", 0, 1, 1, true, false},
    {"eg1_release", "R", 0, 1, 0.5f, true, false},
    {"eg_attack_shape", "Attack shape", -1, 1, 0, true, false},
    {"eg_decay_shape", "Decay shape", -1, 1, 0, true, false},
    {"eg_release_shape", "Release shape", -1, 1, 0, true, false},
    {"eg_temposync", "Tempo sync", 0, 1, 0, true, true},
    {"eg_gate_mode", "Gate mode", 0, 2, 0, true, true},
    {"eg_gate_any", "Gate on voice sounding", 0, 1, 0, true, true},

    {"lfo_index", "LFO", 0, 3, 0, true, true},
    {"lfo_shape", "LFO shape", 0, 8, 1, true, true},
    {"lfo_trigger", "LFO trigger", 0, 4, 0, true, true},
    {"lfo_rate", "LFO rate", -7, 9, 0, true, false},
    {"lfo_phase", "LFO phase", 0, 1, 0, true, false},
    {"lfo_deform", "LFO deform", -1, 1, 0, true, false},
    {"lfo_angle", "LFO angle", -1, 1, 0, true, false},
    {"lfo_temposync", "LFO tempo sync", 0, 1, 0, true, true},
    {"lfo_unipolar", "LFO unipolar", 0, 1, 0, true, true},
    {"lfo_useenv", "LFO envelope", 0, 1, 0, true, true},
    {"lfo_delay", "LFO delay", 0, 1, 0, true, false},
    {"lfo_attack", "LFO attack", 0, 1, 0, true, false},
    {"lfo_release", "LFO release", 0, 1, 1, true, false},
    {"lfoenv_delay", "LFO env delay", 0, 1, 0, true, false},
    {"lfoenv_attack", "LFO env attack", 0, 1, 0, true, false},
    {"lfoenv_hold", "LFO env hold", 0, 1, 0, true, false},
    {"lfoenv_decay", "LFO env decay", 0, 1, 0, true, false},
    {"lfoenv_sustain", "LFO env sustain", 0, 1, 1, true, false},
    {"lfoenv_release", "LFO env release", 0, 1, 1, true, false},
    {"lfoenv_loop", "LFO env loop", 0, 1, 0, true, true},

    {"part_volume", "Part volume", 0, 1, 1, true, false},
    {"part_pan", "Part pan", -1, 1, 0, true, false},
    {"part_tuning", "Part tuning", -36, 36, 0, true, false},
    {"part_transpose", "Part transpose", -36, 36, 0, true, true},
    {"part_channel", "Part MIDI channel (-1 = omni)", -1, 15, -1, true, true},
    {"part_poly", "Part polyphony (0 = no limit)", 0, 256, 0, true, true},
    {"part_mute", "Part mute", 0, 1, 0, true, true},
    {"part_solo", "Part solo", 0, 1, 0, true, true},
    {"part_send1", "Send to aux 1", 0, 1, 0, true, false},
    {"part_send2", "Send to aux 2", 0, 1, 0, true, false},
    {"part_send3", "Send to aux 3", 0, 1, 0, true, false},
    {"part_send4", "Send to aux 4", 0, 1, 0, true, false},

    {"browse", "Browse folder", 0, 0, 0, true, false},
    {"listing", "Folder listing", 0, 0, 0, false, false},
    {"load_path", "Load file", 0, 0, 0, true, false},
    {"folders", "Saved folders", 0, 0, 0, false, false},
    {"folder_add", "Save folder", 0, 0, 0, true, false},
    {"folder_remove", "Forget folder", 0, 0, 0, true, false},
    {"fx_types", "Effect types", 0, 0, 0, false, false},
    {"fx_desc", "Effect controls", 0, 0, 0, false, false},
    {"fx_values", "Effect values", 0, 0, 0, false, false},
    {"fx_edit", "Effect edit", 0, 0, 0, true, false},
    {"mod_meta", "Mod matrix lists", 0, 0, 0, false, false},
    {"mod_rows", "Mod matrix rows", 0, 0, 0, false, false},
    {"mod_edit", "Mod matrix edit", 0, 0, 0, true, false},
    {"names", "Loaded names", 0, 0, 0, false, false},
    {"mods_text", "Modulator value texts", 0, 0, 0, false, false},
    {"pfx_types", "Part effect types", 0, 0, 0, false, false},
    {"pfx_desc", "Part effect controls", 0, 0, 0, false, false},
    {"pfx_values", "Part effect values", 0, 0, 0, false, false},
    {"pfx_edit", "Part effect edit", 0, 0, 0, true, false},
    {"group_cmd", "Group command", 0, 0, 0, true, false},
    {"save_part", "Save part 1", 0, 0, 0, true, false},
    {"saved", "Save result", 0, 0, 0, false, false},
    {"misc_text", "Misc and audio modulators", 0, 0, 0, false, false},
    {"misc_edit", "Misc and audio modulator edit", 0, 0, 0, true, false},
    {"macro_names", "Macro names", 0, 0, 0, false, false},
    {"macro_edit", "Macro rename", 0, 0, 0, true, false},
    {"part_file", "Part 1 file", 0, 0, 0, false, false},
};

} // namespace scxt_lv2
