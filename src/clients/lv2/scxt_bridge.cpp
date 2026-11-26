/*
 * scxt_bridge.cpp - see scxt_bridge.h
 *
 * Part of the maarthome/shortcircuit-xt "my_own" branch.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 */
#include "scxt_bridge.h"

#include "clap/scxt-plugin.h"
#include "messaging/messaging.h"
#include "messaging/client/client_messages.h"
#include "messaging/client/client_serial.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace scxt_lv2
{
namespace cmsg = scxt::messaging::client;
using GroupOutputInfo = scxt::engine::Group::GroupOutputInfo;
using AdsrStorage = scxt::modulation::modulators::AdsrStorage;

// Single producer / single consumer ring of (param, value) pairs
template <size_t N> struct Ring
{
    struct Item
    {
        uint32_t p;
        float v;
    };
    std::array<Item, N> items{};
    std::atomic<size_t> head{0}, tail{0};

    bool push(uint32_t p, float v)
    {
        auto h = head.load(std::memory_order_relaxed);
        auto n = (h + 1) % N;
        if (n == tail.load(std::memory_order_acquire))
            return false; // full: drop
        items[h] = {p, v};
        head.store(n, std::memory_order_release);
        return true;
    }
    bool pop(uint32_t &p, float &v)
    {
        auto t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire))
            return false;
        p = items[t].p;
        v = items[t].v;
        tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }
};

struct Bridge::Impl
{
    scxt::clap_first::scxt_plugin::SCXTPlugin *plugin{nullptr};
    scxt::engine::Engine *engine{nullptr};
    std::string bundlePath;

    Ring<1024> in, out;
    std::atomic<bool> running{true}, wantAll{true}, structureChanged{true};
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::thread worker;

    // worker-thread state
    int group{0};
    int loadMode{0};
    float loadedCounter{0};
    std::array<float, P_NUM> sent{};
    std::array<bool, P_NUM> sentValid{};
    std::array<float, P_NUM> pending{};
    std::array<bool, P_NUM> pendingValid{};
    std::chrono::steady_clock::time_point holdReadback{};

    // ---------------------------------------------------------------------
    template <typename M, typename P> void send(const P &payload)
    {
        auto &mc = *engine->getMessageController();
        auto bypass = mc.threadingChecker.bypassChecksInScope();
        cmsg::clientSendToSerialization(M(payload), mc);
    }

    void selectGroup()
    {
        send<cmsg::SelectPart>((int16_t)0);
        scxt::selection::SelectionManager::SelectActionContents sac(0, group, -1, true, true, true);
        sac.forZone = false;
        send<cmsg::ApplySelectActions>(
            std::vector<scxt::selection::SelectionManager::SelectActionContents>{sac});
    }

    template <typename T> void groupField(T GroupOutputInfo::*member, T value)
    {
        GroupOutputInfo dummy{};
        auto off = (ptrdiff_t)((uint8_t *)&(dummy.*member) - (uint8_t *)&dummy);
        if constexpr (std::is_same_v<T, float>)
            send<cmsg::UpdateGroupOutputFloatValue>(cmsg::detail::diffMsg_t<float>{off, value});
        else if constexpr (std::is_same_v<T, bool>)
            send<cmsg::UpdateGroupOutputBoolValue>(cmsg::detail::diffMsg_t<bool>{off, value});
        else if constexpr (std::is_same_v<T, int16_t>)
            send<cmsg::UpdateGroupOutputInt16TValue>(cmsg::detail::diffMsg_t<int16_t>{off, value});
        else
            send<cmsg::UpdateGroupOutputInt32TValue>(
                cmsg::detail::diffMsg_t<int32_t>{off, (int32_t)value});
    }

    void egField(float AdsrStorage::*member, float value)
    {
        AdsrStorage dummy{};
        auto off = (ptrdiff_t)((uint8_t *)&(dummy.*member) - (uint8_t *)&dummy);
        // (forZone = false: group EG; index 0 = EG1)
        send<cmsg::UpdateZoneOrGroupEGFloatValue>(
            cmsg::detail::indexedZoneOrGroupDiffMsg_t<float>{false, (size_t)0, off, value});
    }

    std::string instrumentPath(int index)
    {
        // instruments.txt in the bundle: one path per line, relative to the
        // instruments folder (written by build_scxt.sh, same order as the panel)
        std::ifstream f(bundlePath + "instruments.txt");
        std::string line, base;
        int i = 0;
        while (std::getline(f, line))
        {
            if (line.rfind("#base ", 0) == 0)
            {
                base = line.substr(6);
                continue;
            }
            if (line.empty() || line[0] == '#')
                continue;
            if (i++ == index)
            {
                if (!line.empty() && line[0] == '/')
                    return line;
                const char *home = getenv("HOME");
                std::vector<std::string> bases;
                if (!base.empty())
                    bases.push_back(base);
                if (home)
                    bases.push_back(std::string(home) + "/.local/share/shortcircuit-xt/instruments");
                bases.push_back("/usr/share/shortcircuit-xt/instruments");
                for (auto &b : bases)
                {
                    auto p = b + "/" + line;
                    if (std::ifstream(p).good())
                        return p;
                }
                fprintf(stderr, "[scxt-lv2] instrument not found: %s\n", line.c_str());
                return "";
            }
        }
        fprintf(stderr, "[scxt-lv2] no instrument %d in instruments.txt\n", index);
        return "";
    }

    void apply(uint32_t p, float v)
    {
        auto iv = (int)std::lround(v);
        switch (p)
        {
        case P_GROUP:
            group = std::max(0, iv);
            selectGroup();
            wantAll = true;
            return;
        case P_LOAD_MODE:
            loadMode = iv;
            return;
        case P_INSTRUMENT:
        {
            if (iv < 0)
                return;
            auto path = instrumentPath(iv);
            if (path.empty())
                return;
            send<cmsg::SelectPart>((int16_t)0);
            bool isSfz = path.size() > 4 && (path.compare(path.size() - 4, 4, ".sfz") == 0 ||
                                             path.compare(path.size() - 4, 4, ".SFZ") == 0);
            if (loadMode == 1)
            {
                send<cmsg::ClearPart>(0);
                group = 0;
                if (isSfz)
                    send<cmsg::AddSample>(path);
                else
                    send<cmsg::AddSampleToGroup>(cmsg::addSampleToGroupPayload_t{path, 0, 0});
            }
            else
            {
                // add: into a new group after the existing ones (an sfz makes its own groups)
                auto &part = engine->getPatch()->getPart(0);
                int n = part ? (int)part->getGroups().size() : 0;
                if (isSfz)
                    send<cmsg::AddSample>(path);
                else
                    send<cmsg::AddSampleToGroup>(cmsg::addSampleToGroupPayload_t{path, 0, n});
                group = n;
            }
            fprintf(stderr, "[scxt-lv2] %s instrument %s\n", loadMode == 1 ? "replace with" : "add",
                    path.c_str());
            // the load runs on Shortcircuit's own thread; read back when it is done
            holdReadback = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            structureChanged = true;
            wantAll = true;
            return;
        }
        default:
            break;
        }

        // everything below edits the selected group
        selectGroup();
        switch (p)
        {
        case P_VOLUME: groupField(&GroupOutputInfo::amplitude, v); break;
        case P_PAN: groupField(&GroupOutputInfo::pan, v); break;
        case P_TUNING: groupField(&GroupOutputInfo::tuning, v); break;
        case P_PB_DOWN: groupField(&GroupOutputInfo::pbDown, v); break;
        case P_PB_UP: groupField(&GroupOutputInfo::pbUp, v); break;
        case P_GLIDE_TIME: groupField(&GroupOutputInfo::glideTime, v); break;
        case P_VEL_SENS: groupField(&GroupOutputInfo::velocitySensitivity, v); break;
        case P_GLIDE_MODE:
            groupField(&GroupOutputInfo::glideRateMode, (scxt::engine::Group::GlideRateMode)iv);
            break;
        case P_PLAY_MODE:
            groupField(&GroupOutputInfo::playMode, (scxt::engine::Group::PlayMode)iv);
            break;
        case P_NOTE_PRIORITY:
            groupField(&GroupOutputInfo::notePriority, (scxt::engine::Group::NotePriority)iv);
            break;
        case P_MIDI_CHANNEL: groupField(&GroupOutputInfo::midiChannel, (int16_t)iv); break;
        case P_EXCLUSIVE: groupField(&GroupOutputInfo::exclusiveGroup, (int32_t)iv); break;
        case P_POLY_LIMIT:
            if (iv > 0)
                groupField(&GroupOutputInfo::polyLimit, (int32_t)iv);
            groupField(&GroupOutputInfo::hasIndependentPolyLimit, iv > 0);
            break;
        case P_EG_DLY: egField(&AdsrStorage::dly, v); break;
        case P_EG_A: egField(&AdsrStorage::a, v); break;
        case P_EG_H: egField(&AdsrStorage::h, v); break;
        case P_EG_D: egField(&AdsrStorage::d, v); break;
        case P_EG_S: egField(&AdsrStorage::s, v); break;
        case P_EG_R: egField(&AdsrStorage::r, v); break;
        default: break;
        }
    }

    // Read the selected group's values from the engine. Read-only snapshot of
    // plain fields, like the editor's own display; done on this thread.
    bool readValues(std::array<float, P_NUM> &vals)
    {
        auto &patch = engine->getPatch();
        if (!patch)
            return false;
        auto &part = patch->getPart(0);
        if (!part)
            return false;
        auto n = (int)part->getGroups().size();
        vals.fill(0);
        vals[P_GROUP_COUNT] = (float)n;
        if (group >= n)
            group = n > 0 ? n - 1 : 0;
        vals[P_GROUP] = (float)group;
        vals[P_LOAD_MODE] = (float)loadMode;
        vals[P_INSTRUMENT] = -1;
        vals[P_LOADED] = loadedCounter;
        if (n == 0)
            return true;
        auto &g = part->getGroup(group);
        if (!g)
            return true;
        const auto &o = g->outputInfo;
        vals[P_VOLUME] = o.amplitude;
        vals[P_PAN] = o.pan;
        vals[P_TUNING] = o.tuning;
        vals[P_PB_DOWN] = o.pbDown;
        vals[P_PB_UP] = o.pbUp;
        vals[P_GLIDE_TIME] = o.glideTime;
        vals[P_GLIDE_MODE] = (float)o.glideRateMode;
        vals[P_PLAY_MODE] = (float)o.playMode;
        vals[P_NOTE_PRIORITY] = (float)o.notePriority;
        vals[P_MIDI_CHANNEL] = (float)o.midiChannel;
        vals[P_POLY_LIMIT] = o.hasIndependentPolyLimit ? (float)o.polyLimit : 0.f;
        vals[P_EXCLUSIVE] = (float)o.exclusiveGroup;
        vals[P_VEL_SENS] = o.velocitySensitivity;
        const auto &eg = g->gegStorage[0];
        vals[P_EG_DLY] = eg.dly;
        vals[P_EG_A] = eg.a;
        vals[P_EG_H] = eg.h;
        vals[P_EG_D] = eg.d;
        vals[P_EG_S] = eg.s;
        vals[P_EG_R] = eg.r;
        return true;
    }

    void readback()
    {
        if (std::chrono::steady_clock::now() < holdReadback)
            return; // a load is still running on Shortcircuit's thread
        std::array<float, P_NUM> vals;
        if (!readValues(vals))
            return;
        bool all = wantAll.exchange(false);
        if (structureChanged.exchange(false) || (sentValid[P_GROUP_COUNT] && vals[P_GROUP_COUNT] != sent[P_GROUP_COUNT]))
        {
            loadedCounter += 1;
            vals[P_LOADED] = loadedCounter;
            all = true;
        }
        for (uint32_t i = 0; i < P_NUM; ++i)
        {
            if (i == P_INSTRUMENT)
                continue;
            if (all || !sentValid[i] || vals[i] != sent[i])
            {
                if (out.push(i, vals[i]))
                {
                    sent[i] = vals[i];
                    sentValid[i] = true;
                }
            }
        }
    }

    void loop()
    {
        while (running)
        {
            {
                std::unique_lock<std::mutex> lk(wakeMutex);
                wake.wait_for(lk, std::chrono::milliseconds(200));
            }
            if (!running)
                break;
            // collect the newest value per parameter, then apply in order
            uint32_t p;
            float v;
            pendingValid.fill(false);
            std::vector<uint32_t> order;
            while (in.pop(p, v))
            {
                if (p >= P_NUM)
                    continue;
                if (!pendingValid[p])
                    order.push_back(p);
                pending[p] = v;
                pendingValid[p] = true;
            }
            for (auto q : order)
            {
                apply(q, pending[q]);
                // the panel already shows what it sent
                if (q != P_INSTRUMENT && q != P_GROUP)
                {
                    sent[q] = pending[q];
                    sentValid[q] = true;
                }
            }
            if (!order.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(30)); // let it apply
            readback();
        }
    }
};

Bridge::Bridge(const clap_plugin_t *plugin, const std::string &bundlePath)
    : impl(std::make_unique<Impl>())
{
    // the clap-helpers plugin base keeps `this` in plugin_data
    impl->plugin = static_cast<scxt::clap_first::scxt_plugin::SCXTPlugin *>(
        (scxt::clap_first::plugHelper_t *)plugin->plugin_data);
    impl->engine = impl->plugin->engine.get();
    impl->bundlePath = bundlePath;
    if (!impl->bundlePath.empty() && impl->bundlePath.back() != '/')
        impl->bundlePath += '/';
    impl->worker = std::thread([this]() { impl->loop(); });
}

Bridge::~Bridge()
{
    impl->running = false;
    impl->wake.notify_all();
    if (impl->worker.joinable())
        impl->worker.join();
}

void Bridge::post(uint32_t param, float value)
{
    impl->in.push(param, value);
    impl->wake.notify_one(); // notify without the mutex: does not block
}

void Bridge::requestAll() { impl->wantAll = true; }

bool Bridge::popOut(uint32_t &param, float &value) { return impl->out.pop(param, value); }

void Bridge::stateChanged()
{
    impl->structureChanged = true;
    impl->holdReadback = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

} // namespace scxt_lv2
