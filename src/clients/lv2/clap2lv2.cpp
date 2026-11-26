/*
 * clap2lv2.cpp - the Shortcircuit XT LV2 plugin + LV2 X11 UI.
 *
 * Part of the maarthome/shortcircuit-xt "my_own" branch LV2 wrapper.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 *
 * Shortcircuit XT's own plugin code is linked into this binary and driven
 * through its plugin interface in the same process (like the upstream VST3,
 * which clap-wrapper builds the same way). The bundle holds this binary and
 * two .ttl files written by scxt-lv2-ttl. Port layout (must match clap2lv2-ttl.cpp):
 *   0        atom input  (MIDI + time:Position)
 *   1, 2     audio out L / R (the CLAP "Main Out")
 *   3 + i    control input for CLAP parameter index i
 *
 * The whole plugin state (patch, samples paths, macros...) is stored as one
 * LV2 state property holding the CLAP state blob.
 */

#include "clap_host.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <X11/Xlib.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#ifndef CLAP2LV2_URI
#define CLAP2LV2_URI "https://github.com/maarthome/shortcircuit-xt#lv2"
#endif
// Shortcircuit XT's plugin code (the scxt-plugin library) is compiled into
// this binary, exactly as clap-wrapper does for the upstream VST3: these are
// its factory functions, called directly, no .clap file is loaded.
namespace scxt::clap_first
{
extern const void *get_factory(const char *factory_id);
extern bool clap_init(const char *p);
extern void clap_deinit();
} // namespace scxt::clap_first

static const clap_plugin_entry_t scxtEntry = {CLAP_VERSION, scxt::clap_first::clap_init,
                                              scxt::clap_first::clap_deinit,
                                              scxt::clap_first::get_factory};

#define CLAP2LV2_UI_URI CLAP2LV2_URI "-ui"
#define CLAP2LV2_STATE_KEY CLAP2LV2_URI "-clapstate"

namespace clap2lv2
{

enum PortIndex : uint32_t
{
    PORT_EVENTS_IN = 0,
    PORT_OUT_L = 1,
    PORT_OUT_R = 2,
    PORT_FIRST_PARAM = 3
};

// ---------------------------------------------------------------------------
// CLAP event lists backed by a fixed buffer (no allocation in run())
// ---------------------------------------------------------------------------
union AnyEvent
{
    clap_event_header_t header;
    clap_event_midi_t midi;
    clap_event_param_value_t param;
};

struct InputEvents
{
    static constexpr uint32_t capacity = 2048;
    AnyEvent events[capacity];
    uint32_t count{0};
    clap_input_events_t list{this, &InputEvents::size, &InputEvents::get};

    void clear() { count = 0; }
    AnyEvent *add()
    {
        if (count >= capacity)
            return nullptr;
        return &events[count++];
    }
    static uint32_t size(const clap_input_events_t *l) { return ((InputEvents *)l->ctx)->count; }
    static const clap_event_header_t *get(const clap_input_events_t *l, uint32_t i)
    {
        auto *s = (InputEvents *)l->ctx;
        return i < s->count ? &s->events[i].header : nullptr;
    }
};

struct OutputEvents
{
    clap_output_events_t list{this, [](const clap_output_events_t *, const clap_event_header_t *) {
                                  return true; // GUI-driven param changes: nothing to forward
                              }};
};

// ---------------------------------------------------------------------------
// The LV2 plugin instance
// ---------------------------------------------------------------------------
struct Plugin
{
    Host host;
    ClapModule *module{nullptr};
    const clap_plugin_t *clap{nullptr};
    const clap_plugin_params_t *params{nullptr};
    const clap_plugin_state_t *state{nullptr};
    const clap_plugin_gui_t *gui{nullptr};
    const clap_plugin_audio_ports_t *audioPorts{nullptr};

    LV2_URID_Map *map{nullptr};
    struct
    {
        LV2_URID midiEvent, atomObject, atomBlank, atomChunk, atomFloat, atomDouble, atomInt,
            atomLong, timePosition, timeBpm, timeSpeed, timeBar, timeBarBeat, timeBeatsPerBar,
            timeBeatUnit, stateKey, bufMaxBlock, bufNominalBlock;
    } u{};

    double sampleRate{48000};
    uint32_t maxBlock{8192};
    bool activated{false};
    bool processing{false};
    int64_t steadyTime{0};

    const LV2_Atom_Sequence *eventsIn{nullptr};
    float *outL{nullptr}, *outR{nullptr};
    std::vector<float> scratchL, scratchR;

    std::vector<clap_id> paramIds;
    std::vector<const float *> paramPorts;
    std::vector<float> paramLast;

    uint32_t numOutputs{1};
    std::vector<clap_audio_buffer_t> outBuffers;
    float *mainData[2]{nullptr, nullptr};

    clap_event_transport_t transport{};
    InputEvents inEvents;
    OutputEvents outEvents;

    bool init(const char *bundlePath, const LV2_Feature *const *features)
    {
        for (int i = 0; features && features[i]; ++i)
        {
            if (!strcmp(features[i]->URI, LV2_URID__map))
                map = (LV2_URID_Map *)features[i]->data;
        }
        if (!map)
        {
            fprintf(stderr, "[clap2lv2] host does not provide urid:map\n");
            return false;
        }
        auto m = [this](const char *uri) { return map->map(map->handle, uri); };
        u.midiEvent = m(LV2_MIDI__MidiEvent);
        u.atomObject = m(LV2_ATOM__Object);
        u.atomBlank = m(LV2_ATOM__Blank);
        u.atomChunk = m(LV2_ATOM__Chunk);
        u.atomFloat = m(LV2_ATOM__Float);
        u.atomDouble = m(LV2_ATOM__Double);
        u.atomInt = m(LV2_ATOM__Int);
        u.atomLong = m(LV2_ATOM__Long);
        u.timePosition = m(LV2_TIME__Position);
        u.timeBpm = m(LV2_TIME__beatsPerMinute);
        u.timeSpeed = m(LV2_TIME__speed);
        u.timeBar = m(LV2_TIME__bar);
        u.timeBarBeat = m(LV2_TIME__barBeat);
        u.timeBeatsPerBar = m(LV2_TIME__beatsPerBar);
        u.timeBeatUnit = m(LV2_TIME__beatUnit);
        u.stateKey = m(CLAP2LV2_STATE_KEY);
        u.bufMaxBlock = m(LV2_BUF_SIZE__maxBlockLength);
        u.bufNominalBlock = m(LV2_BUF_SIZE__nominalBlockLength);

        for (int i = 0; features && features[i]; ++i)
        {
            if (!strcmp(features[i]->URI, LV2_OPTIONS__options))
            {
                for (auto *o = (const LV2_Options_Option *)features[i]->data; o && o->key; ++o)
                {
                    if (o->key == u.bufMaxBlock && o->type == u.atomInt)
                        maxBlock = (uint32_t) * (const int32_t *)o->value;
                }
            }
        }

        module = ClapModule::fromEntry(&scxtEntry, bundlePath);
        if (!module)
            return false;

        auto id = module->firstPluginId();
        clap = module->factory->create_plugin(module->factory, &host.clapHost, id.c_str());
        if (!clap)
        {
            fprintf(stderr, "[clap2lv2] could not create CLAP plugin %s\n", id.c_str());
            return false;
        }
        if (!clap->init(clap))
        {
            fprintf(stderr, "[clap2lv2] CLAP plugin init failed\n");
            clap->destroy(clap);
            clap = nullptr;
            return false;
        }
        host.attach(clap);

        params = (const clap_plugin_params_t *)clap->get_extension(clap, CLAP_EXT_PARAMS);
        state = (const clap_plugin_state_t *)clap->get_extension(clap, CLAP_EXT_STATE);
        gui = (const clap_plugin_gui_t *)clap->get_extension(clap, CLAP_EXT_GUI);
        audioPorts =
            (const clap_plugin_audio_ports_t *)clap->get_extension(clap, CLAP_EXT_AUDIO_PORTS);

        uint32_t np = params ? params->count(clap) : 0;
        paramIds.resize(np);
        paramPorts.assign(np, nullptr);
        paramLast.assign(np, NAN);
        for (uint32_t i = 0; i < np; ++i)
        {
            clap_param_info_t info{};
            params->get_info(clap, i, &info);
            paramIds[i] = info.id;
            paramLast[i] = (float)info.default_value;
        }

        // Every CLAP output port gets a buffer entry; only the main one gets
        // memory, the others get null data pointers (Shortcircuit XT skips them).
        numOutputs = audioPorts ? audioPorts->count(clap, false) : 1;
        if (numOutputs < 1)
            numOutputs = 1;
        outBuffers.assign(numOutputs, clap_audio_buffer_t{});
        for (auto &b : outBuffers)
        {
            b.channel_count = 2;
            b.data32 = nullptr;
        }
        outBuffers[0].data32 = mainData;

        transport.header.size = sizeof(transport);
        transport.header.type = CLAP_EVENT_TRANSPORT;
        transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE |
                          CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        transport.tempo = 120.0;
        transport.tsig_num = 4;
        transport.tsig_denom = 4;
        return true;
    }

    ~Plugin()
    {
        if (clap)
        {
            if (activated)
            {
                if (processing)
                    stopProcessing();
                clap->deactivate(clap);
            }
            clap->destroy(clap);
        }
    }

    void connect(uint32_t port, void *data)
    {
        switch (port)
        {
        case PORT_EVENTS_IN:
            eventsIn = (const LV2_Atom_Sequence *)data;
            break;
        case PORT_OUT_L:
            outL = (float *)data;
            break;
        case PORT_OUT_R:
            outR = (float *)data;
            break;
        default:
        {
            uint32_t i = port - PORT_FIRST_PARAM;
            if (i < paramPorts.size())
                paramPorts[i] = (const float *)data;
        }
        }
    }

    // CLAP wants stop_processing() on the audio thread; LV2 deactivates from
    // a non-realtime thread after audio has stopped, so act as audio thread
    void stopProcessing()
    {
        host.audioThread = std::this_thread::get_id();
        clap->stop_processing(clap);
        host.audioThread = std::thread::id();
        processing = false;
    }

    void activate()
    {
        if (!clap || activated)
            return;
        scratchL.assign(maxBlock, 0.f);
        scratchR.assign(maxBlock, 0.f);
        activated = clap->activate(clap, sampleRate, 1, maxBlock);
        if (!activated)
            fprintf(stderr, "[clap2lv2] CLAP activate failed\n");
    }

    void deactivate()
    {
        if (!clap || !activated)
            return;
        if (processing)
        {
            stopProcessing();
            processing = false;
        }
        clap->deactivate(clap);
        activated = false;
    }

    double atomNumber(const LV2_Atom *a, double def) const
    {
        if (!a)
            return def;
        if (a->type == u.atomFloat)
            return ((const LV2_Atom_Float *)a)->body;
        if (a->type == u.atomDouble)
            return ((const LV2_Atom_Double *)a)->body;
        if (a->type == u.atomInt)
            return ((const LV2_Atom_Int *)a)->body;
        if (a->type == u.atomLong)
            return (double)((const LV2_Atom_Long *)a)->body;
        return def;
    }

    void readPosition(const LV2_Atom_Object *obj)
    {
        const LV2_Atom *bpm = nullptr, *speed = nullptr, *bar = nullptr, *barBeat = nullptr,
                       *bpb = nullptr, *unit = nullptr;
        lv2_atom_object_get(obj, u.timeBpm, &bpm, u.timeSpeed, &speed, u.timeBar, &bar,
                            u.timeBarBeat, &barBeat, u.timeBeatsPerBar, &bpb, u.timeBeatUnit,
                            &unit, 0);
        if (bpm)
            transport.tempo = atomNumber(bpm, transport.tempo);
        if (bpb)
            transport.tsig_num = (uint16_t)atomNumber(bpb, transport.tsig_num);
        if (unit)
            transport.tsig_denom = (uint16_t)atomNumber(unit, transport.tsig_denom);
        if (speed)
        {
            if (atomNumber(speed, 0) > 0)
                transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
            else
                transport.flags &= ~CLAP_TRANSPORT_IS_PLAYING;
        }
        if (bar || barBeat)
        {
            double b = atomNumber(bar, 0), bb = atomNumber(barBeat, 0);
            double beats = b * transport.tsig_num + bb;
            transport.bar_number = (int32_t)b;
            transport.bar_start = (clap_beattime)std::llround(b * transport.tsig_num *
                                                              CLAP_BEATTIME_FACTOR);
            transport.song_pos_beats = (clap_beattime)std::llround(beats * CLAP_BEATTIME_FACTOR);
        }
    }

    void run(uint32_t n)
    {
        if (!clap)
            return;
        float *l = outL ? outL : scratchL.data();
        float *r = outR ? outR : scratchR.data();
        if (!activated || n == 0 || n > maxBlock)
        {
            if (l)
                memset(l, 0, sizeof(float) * n);
            if (r)
                memset(r, 0, sizeof(float) * n);
            return;
        }

        host.audioThread = std::this_thread::get_id();
        if (!processing)
            processing = clap->start_processing(clap);

        inEvents.clear();

        // control ports -> CLAP param events at frame 0
        for (size_t i = 0; i < paramPorts.size(); ++i)
        {
            if (!paramPorts[i])
                continue;
            float v = *paramPorts[i];
            if (v != paramLast[i])
            {
                paramLast[i] = v;
                if (auto *e = inEvents.add())
                {
                    auto &p = e->param;
                    p.header = {sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                                CLAP_EVENT_PARAM_VALUE, 0};
                    p.param_id = paramIds[i];
                    p.cookie = nullptr;
                    p.note_id = -1;
                    p.port_index = -1;
                    p.channel = -1;
                    p.key = -1;
                    p.value = v;
                }
            }
        }

        // MIDI + transport
        if (eventsIn)
        {
            LV2_ATOM_SEQUENCE_FOREACH(eventsIn, ev)
            {
                uint32_t t = (uint32_t)std::max<int64_t>(0, ev->time.frames);
                if (t >= n)
                    t = n - 1;
                if (ev->body.type == u.midiEvent)
                {
                    auto *msg = (const uint8_t *)LV2_ATOM_BODY_CONST(&ev->body);
                    uint32_t sz = ev->body.size;
                    if (sz < 1 || sz > 3 || msg[0] < 0x80 || msg[0] >= 0xF0)
                        continue; // only channel messages (no sysex / realtime)
                    if (auto *e = inEvents.add())
                    {
                        auto &mm = e->midi;
                        mm.header = {sizeof(clap_event_midi_t), t, CLAP_CORE_EVENT_SPACE_ID,
                                     CLAP_EVENT_MIDI, 0};
                        mm.port_index = 0;
                        mm.data[0] = msg[0];
                        mm.data[1] = sz > 1 ? msg[1] : 0;
                        mm.data[2] = sz > 2 ? msg[2] : 0;
                    }
                }
                else if (ev->body.type == u.atomObject || ev->body.type == u.atomBlank)
                {
                    auto *obj = (const LV2_Atom_Object *)&ev->body;
                    if (obj->body.otype == u.timePosition)
                        readPosition(obj);
                }
            }
        }

        mainData[0] = l;
        mainData[1] = r;

        clap_process_t proc{};
        proc.steady_time = steadyTime;
        proc.frames_count = n;
        proc.transport = &transport;
        proc.audio_inputs = nullptr;
        proc.audio_inputs_count = 0;
        proc.audio_outputs = outBuffers.data();
        proc.audio_outputs_count = numOutputs;
        proc.in_events = &inEvents.list;
        proc.out_events = &outEvents.list;

        clap->process(clap, &proc);
        host.audioThread = std::thread::id(); // outside run() no thread is "audio"
        steadyTime += n;

        if (transport.flags & CLAP_TRANSPORT_IS_PLAYING)
        {
            double beats = (double)n * transport.tempo / (60.0 * sampleRate);
            transport.song_pos_beats += (clap_beattime)std::llround(beats * CLAP_BEATTIME_FACTOR);
        }
    }

    // --- state ------------------------------------------------------------
    bool saveBlob(std::vector<uint8_t> &blob)
    {
        if (!state)
            return false;
        blob.clear();
        clap_ostream_t os{&blob, [](const clap_ostream_t *s, const void *buf, uint64_t sz) {
                              auto *v = (std::vector<uint8_t> *)s->ctx;
                              v->insert(v->end(), (const uint8_t *)buf, (const uint8_t *)buf + sz);
                              return (int64_t)sz;
                          }};
        return state->save(clap, &os);
    }

    // Also keep the latest state in a file, so generate_ttl.sh can read the
    // macro names from it. Default: ~/.local/share/shortcircuit-xt-lv2/last-state.scxtstate
    static std::string lastStatePath()
    {
        if (const char *p = getenv("SCXT_LV2_STATE_FILE"))
            return p;
        const char *home = getenv("HOME");
        std::string dir = std::string(home ? home : "/tmp") + "/.local";
        mkdir(dir.c_str(), 0755);
        dir += "/share";
        mkdir(dir.c_str(), 0755);
        dir += "/shortcircuit-xt-lv2";
        mkdir(dir.c_str(), 0755);
        return dir + "/last-state.scxtstate";
    }

    void writeLastState(const std::vector<uint8_t> &blob)
    {
        if (blob.empty())
            return;
        auto path = lastStatePath();
        auto tmp = path + ".tmp";
        if (FILE *f = fopen(tmp.c_str(), "wb"))
        {
            bool ok = fwrite(blob.data(), 1, blob.size(), f) == blob.size();
            ok = (fclose(f) == 0) && ok;
            if (ok)
                rename(tmp.c_str(), path.c_str());
            else
                remove(tmp.c_str());
        }
    }

    void dumpLastState()
    {
        std::vector<uint8_t> blob;
        if (saveBlob(blob))
            writeLastState(blob);
    }

    LV2_State_Status save(LV2_State_Store_Function store, LV2_State_Handle h)
    {
        if (!state)
            return LV2_STATE_ERR_NO_FEATURE;
        std::vector<uint8_t> blob;
        if (!saveBlob(blob))
            return LV2_STATE_ERR_UNKNOWN;
        writeLastState(blob);
        return store(h, u.stateKey, blob.data(), blob.size(), u.atomChunk,
                     LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }

    LV2_State_Status restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle h)
    {
        if (!state)
            return LV2_STATE_ERR_NO_FEATURE;
        size_t size = 0;
        uint32_t type = 0, flags = 0;
        auto *data = (const uint8_t *)retrieve(h, u.stateKey, &size, &type, &flags);
        if (!data || size == 0)
            return LV2_STATE_ERR_NO_PROPERTY;

        struct Reader
        {
            const uint8_t *d;
            size_t size, pos;
        } rd{data, size, 0};
        clap_istream_t is{&rd, [](const clap_istream_t *s, void *buf, uint64_t sz) -> int64_t {
                              auto *r = (Reader *)s->ctx;
                              uint64_t left = r->size - r->pos;
                              if (sz > left)
                                  sz = left;
                              memcpy(buf, r->d + r->pos, sz);
                              r->pos += sz;
                              return (int64_t)sz;
                          }};
        if (!state->load(clap, &is))
            return LV2_STATE_ERR_UNKNOWN;

        // pick up the restored parameter values so they are not overwritten by
        // stale port values; the host sends new values afterwards if it has them
        if (params)
            for (size_t i = 0; i < paramIds.size(); ++i)
            {
                double v;
                if (params->get_value(clap, paramIds[i], &v))
                    paramLast[i] = (float)v;
            }
        return LV2_STATE_SUCCESS;
    }
};

// ---------------------------------------------------------------------------
// LV2 plugin descriptor
// ---------------------------------------------------------------------------
static LV2_Handle instantiate(const LV2_Descriptor *, double rate, const char *bundlePath,
                              const LV2_Feature *const *features)
{
    auto p = std::make_unique<Plugin>();
    p->sampleRate = rate;
    if (!p->init(bundlePath, features))
        return nullptr;
    return p.release();
}

static void connectPort(LV2_Handle h, uint32_t port, void *data)
{
    ((Plugin *)h)->connect(port, data);
}
static void activate(LV2_Handle h) { ((Plugin *)h)->activate(); }
static void run(LV2_Handle h, uint32_t n) { ((Plugin *)h)->run(n); }
static void deactivate(LV2_Handle h) { ((Plugin *)h)->deactivate(); }
static void cleanup(LV2_Handle h) { delete (Plugin *)h; }

static LV2_State_Status stateSave(LV2_Handle h, LV2_State_Store_Function store,
                                  LV2_State_Handle sh, uint32_t, const LV2_Feature *const *)
{
    return ((Plugin *)h)->save(store, sh);
}
static LV2_State_Status stateRestore(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                                     LV2_State_Handle sh, uint32_t, const LV2_Feature *const *)
{
    return ((Plugin *)h)->restore(retrieve, sh);
}

static const void *extensionData(const char *uri)
{
    static const LV2_State_Interface stateIface{stateSave, stateRestore};
    if (!strcmp(uri, LV2_STATE__interface))
        return &stateIface;
    return nullptr;
}

static const LV2_Descriptor pluginDescriptor{CLAP2LV2_URI, instantiate, connectPort, activate,
                                             run,          deactivate,  cleanup,     extensionData};

// ---------------------------------------------------------------------------
// LV2 X11 UI: embeds the CLAP editor in the host's window
// ---------------------------------------------------------------------------
struct UI
{
    Plugin *plugin{nullptr};
    LV2UI_Resize *resize{nullptr};
    Window parent{0};
    bool created{false};
    bool closed{false};

    ~UI()
    {
        if (plugin)
        {
            plugin->host.onGuiResize = nullptr;
            plugin->host.onGuiClosed = nullptr;
            if (created && plugin->gui)
            {
                plugin->dumpLastState(); // macro names edited in the window
                plugin->gui->hide(plugin->clap);
                plugin->gui->destroy(plugin->clap);
            }
        }
    }

    // JUCE looks these atoms up "only if they exist" and then uses them without
    // checking; on an X server without a window manager that is a fatal
    // BadAtom error. Creating them first is harmless everywhere else.
    static void ensureWmAtoms()
    {
        Display *d = XOpenDisplay(nullptr);
        if (!d)
            return;
        static const char *names[] = {"WM_PROTOCOLS",
                                      "WM_TAKE_FOCUS",
                                      "WM_DELETE_WINDOW",
                                      "_NET_WM_PING",
                                      "WM_CHANGE_STATE",
                                      "WM_STATE",
                                      "_NET_WM_WINDOW_TYPE",
                                      "_NET_WM_STATE",
                                      "_NET_WM_STATE_HIDDEN",
                                      "_NET_WM_STATE_MAXIMIZED_HORZ",
                                      "_NET_WM_STATE_MAXIMIZED_VERT",
                                      "_NET_WM_STATE_FULLSCREEN",
                                      "_NET_WM_MOVERESIZE",
                                      "_NET_FRAME_EXTENTS",
                                      "_NET_WORKAREA",
                                      "_MOTIF_WM_HINTS",
                                      "_WIN_HINTS",
                                      "KWM_WIN_DECORATION"};
        for (auto *n : names)
            XInternAtom(d, n, False);
        XSync(d, False);
        XCloseDisplay(d);
    }

    // The CLAP editor creates its own child window inside `parent`; hosts
    // expect that child as the LV2 widget.
    Window findChild() const
    {
        Display *d = XOpenDisplay(nullptr);
        if (!d)
            return parent;
        Window root, par, *children = nullptr, res = parent;
        unsigned int n = 0;
        if (XQueryTree(d, parent, &root, &par, &children, &n) && n > 0)
            res = children[n - 1];
        if (children)
            XFree(children);
        XCloseDisplay(d);
        return res;
    }
};

static LV2UI_Handle uiInstantiate(const LV2UI_Descriptor *, const char *, const char *,
                                  LV2UI_Write_Function, LV2UI_Controller, LV2UI_Widget *widget,
                                  const LV2_Feature *const *features)
{
    auto ui = std::make_unique<UI>();
    for (int i = 0; features && features[i]; ++i)
    {
        if (!strcmp(features[i]->URI, LV2_UI__parent))
            ui->parent = (Window)(uintptr_t)features[i]->data;
        else if (!strcmp(features[i]->URI, LV2_UI__resize))
            ui->resize = (LV2UI_Resize *)features[i]->data;
        else if (!strcmp(features[i]->URI, LV2_INSTANCE_ACCESS_URI))
            ui->plugin = (Plugin *)features[i]->data;
    }
    if (!ui->plugin || !ui->plugin->gui || !ui->parent)
    {
        fprintf(stderr, "[clap2lv2] UI needs instance-access and ui:parent from the host\n");
        return nullptr;
    }

    auto *p = ui->plugin;
    auto *gui = p->gui;
    // the UI runs on the host's GUI thread, which is the CLAP main thread
    p->host.mainThread = std::this_thread::get_id();
    UI::ensureWmAtoms();

    if (!gui->is_api_supported(p->clap, CLAP_WINDOW_API_X11, false) ||
        !gui->create(p->clap, CLAP_WINDOW_API_X11, false))
    {
        fprintf(stderr, "[clap2lv2] CLAP plugin has no embeddable X11 editor\n");
        return nullptr;
    }
    ui->created = true;

    if (const char *sc = getenv("SCXT_LV2_UI_SCALE"))
        gui->set_scale(p->clap, atof(sc));

    UI *raw = ui.get();
    p->host.onGuiResize = [raw](uint32_t w, uint32_t h) {
        if (raw->resize)
            raw->resize->ui_resize(raw->resize->handle, (int)w, (int)h);
        return raw->plugin->gui->set_size(raw->plugin->clap, w, h);
    };
    p->host.onGuiClosed = [raw]() { raw->closed = true; };

    clap_window_t win{};
    win.api = CLAP_WINDOW_API_X11;
    win.x11 = (clap_xwnd)raw->parent;
    if (!gui->set_parent(p->clap, &win))
    {
        fprintf(stderr, "[clap2lv2] CLAP set_parent failed\n");
        return nullptr;
    }

    uint32_t w = 0, h = 0;
    if (gui->get_size(p->clap, &w, &h) && ui->resize)
        ui->resize->ui_resize(ui->resize->handle, (int)w, (int)h);

    gui->show(p->clap);
    p->host.idle();

    *widget = (LV2UI_Widget)(uintptr_t)ui->findChild();
    return ui.release();
}

static void uiCleanup(LV2UI_Handle h) { delete (UI *)h; }
static void uiPortEvent(LV2UI_Handle, uint32_t, uint32_t, uint32_t, const void *) {}

static int uiIdle(LV2UI_Handle h)
{
    auto *ui = (UI *)h;
    ui->plugin->host.idle();
    return ui->closed ? 1 : 0;
}

static const void *uiExtensionData(const char *uri)
{
    static const LV2UI_Idle_Interface idleIface{uiIdle};
    if (!strcmp(uri, LV2_UI__idleInterface))
        return &idleIface;
    return nullptr;
}

static const LV2UI_Descriptor uiDescriptor{CLAP2LV2_UI_URI, uiInstantiate, uiCleanup, uiPortEvent,
                                           uiExtensionData};

} // namespace clap2lv2

extern "C"
{
    // used by scxt-lv2-ttl to read the plugin's knob list at build time
    LV2_SYMBOL_EXPORT const clap_plugin_entry_t *scxt_lv2_plugin_entry() { return &scxtEntry; }

    LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
    {
        return index == 0 ? &clap2lv2::pluginDescriptor : nullptr;
    }

    LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
    {
        return index == 0 ? &clap2lv2::uiDescriptor : nullptr;
    }
}
