/*
 * scxt_bridge.cpp - see scxt_bridge.h
 *
 * Part of the maarthome/shortcircuit-xt "My_Changes" branch.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 */
#include "scxt_bridge.h"

#include "clap/scxt-plugin.h"
#include "messaging/messaging.h"
#include "messaging/client/client_messages.h"
#include "messaging/client/client_serial.h"
#include "browser/browser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <pwd.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace scxt_lv2
{
namespace cmsg = scxt::messaging::client;
using GroupOutputInfo = scxt::engine::Group::GroupOutputInfo;
using AdsrStorage = scxt::modulation::modulators::AdsrStorage;
using ModStorage = scxt::modulation::ModulatorStorage;
using ProcStorage = scxt::dsp::processor::ProcessorStorage;
using ProcDesc = scxt::dsp::processor::ProcessorControlDescription;
static constexpr int numFx = scxt::processorsPerZoneAndGroup;
static constexpr int numModRows = scxt::modMatrixRowsPerGroup;
using GMRouting = scxt::modulation::GroupMatrix::RoutingTable::Routing;
using GMSource = scxt::modulation::GroupMatrixConfig::SourceIdentifier;
using GMTarget = scxt::modulation::GroupMatrixConfig::TargetIdentifier;

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

// Single producer / single consumer ring of (param, text) slots
struct StrRing
{
    static constexpr size_t N = 64, LEN = 2048;
    struct Item
    {
        uint32_t p;
        uint32_t len;
        char text[LEN];
    };
    std::array<Item, N> items{};
    std::atomic<size_t> head{0}, tail{0};
    bool push(uint32_t p, const char *t, uint32_t len)
    {
        auto h = head.load(std::memory_order_relaxed);
        auto n = (h + 1) % N;
        if (n == tail.load(std::memory_order_acquire) || len >= LEN)
            return false;
        items[h].p = p;
        items[h].len = len;
        memcpy(items[h].text, t, len);
        items[h].text[len] = 0;
        head.store(n, std::memory_order_release);
        return true;
    }
    bool pop(uint32_t &p, std::string &t)
    {
        auto tl = tail.load(std::memory_order_relaxed);
        if (tl == head.load(std::memory_order_acquire))
            return false;
        p = items[tl].p;
        t.assign(items[tl].text, items[tl].len);
        tail.store((tl + 1) % N, std::memory_order_release);
        return true;
    }
};

namespace fs = std::filesystem;

static bool natLess(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j]))
        {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2]))
                ++i2;
            while (j2 < b.size() && isdigit((unsigned char)b[j2]))
                ++j2;
            auto na = std::stoull("0" + a.substr(i, std::min<size_t>(i2 - i, 18)));
            auto nb = std::stoull("0" + b.substr(j, std::min<size_t>(j2 - j, 18)));
            if (na != nb)
                return na < nb;
            i = i2;
            j = j2;
            continue;
        }
        auto ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[j]);
        if (ca != cb)
            return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

// Shortcircuit XT's own lists (browser.cpp), so the panel shows what the engine loads:
// samples .wav .flac .mp3 .opus .aif .aiff, instruments .sf2 .sfz .multisample .exs .gig .akp,
// and Shortcircuit's own .scm (multi) and .scp (part)
static bool loadable(const fs::path &p) { return scxt::browser::Browser::isLoadableFile(p); }

struct Bridge::Impl
{
    scxt::clap_first::scxt_plugin::SCXTPlugin *plugin{nullptr};
    scxt::engine::Engine *engine{nullptr};
    std::string bundlePath;

    Ring<1024> in, out;
    StrRing strIn;
    std::mutex strOutMutex;
    std::deque<std::pair<uint32_t, std::string>> strOut;
    std::atomic<bool> wantStrings{true};
    std::string homeDir, defaultDir, configFile, curDir;
    std::vector<std::string> favourites;
    std::atomic<bool> running{true}, wantAll{true}, structureChanged{true};
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::thread worker;

    // effect descriptions: the worker asks (descRequest = group), the audio thread
    // copies them between process calls and sets descReady
    std::array<ProcDesc, numFx> descSnap;
    scxt::modulation::groupMatrixMetadata_t metaSnap;
    // names of part 1's groups and zones, made on the audio thread on request
    std::atomic<bool> namesRequest{false}, namesReady{false};
    std::string namesSnap, sentNames;
    float namesForLoaded{-1};
    int namesTick{0};
    std::atomic<int> descRequest{-1};
    std::atomic<bool> descReady{false};

    // worker-thread state
    std::array<ProcDesc, numFx> desc;
    std::array<int, numFx> descType{}, descKeytrack{};
    int descGroup{-1}, descRequestedGroup{-1};
    bool descValid{false};
    std::array<std::string, numFx> sentFxValues;
    std::string sentModRows;
    // rows as last edited here: the engine applies edits asynchronously, so several edits
    // in a row must build on each other, not on the engine's not-yet-updated copy
    std::array<GMRouting, numModRows> rowShadow{};
    std::array<std::chrono::steady_clock::time_point, numModRows> rowShadowTime{};
    int rowShadowGroup{-1};
    int group{0};          // the group shown (with allGroups: the lead, group 1)
    bool allGroups{true};  // edits go to every group of part 1 (the panel's "All groups")
    int loadMode{0};
    int egIndex{0}, lfoIndex{0}; // group EG 1..5 (-1, the zones' amp EG, is no longer offered)
    // the EG as last edited here (see rowShadow): a gate mode change sends the whole EG
    AdsrStorage egShadow{};
    int egShadowGroup{-1}, egShadowIndex{-1};
    std::chrono::steady_clock::time_point egShadowTime{};
    AdsrStorage &currentEgShadow()
    {
        auto now = std::chrono::steady_clock::now();
        if (egShadowGroup != group || egShadowIndex != egIndex || now - egShadowTime > std::chrono::seconds(2))
        {
            auto &patch = engine->getPatch();
            auto *part = patch ? patch->getPart(0).get() : nullptr;
            (void)part;
            auto *src = shownEg();
            egShadow = src ? *src : AdsrStorage{};
            egShadowGroup = group;
            egShadowIndex = egIndex;
        }
        egShadowTime = now;
        return egShadow;
    }
    std::string sentModsText, modsText, partText;
    float loadedCounter{0};

    // part 1: settings, sends, effects (edits build on shadows, like rowShadow)
    using PartConfig = scxt::engine::Part::PartConfiguration;
    using SendStorage = scxt::engine::Bus::BusSendStorage;
    using BusFxStorage = scxt::engine::BusEffectStorage;
    PartConfig partShadow{};
    std::chrono::steady_clock::time_point partShadowTime{};
    SendStorage sendShadow{};
    std::chrono::steady_clock::time_point sendShadowTime{};
    struct PfxDesc
    {
        int type{-1};
        int n{0};
        std::array<scxt::datamodel::pmd, scxt::maxBusEffectParams> m{};
    };
    std::array<PfxDesc, scxt::maxEffectsPerPart> pfxSnap{}, pfxDesc{};
    std::atomic<bool> pfxRequest{false}, pfxReady{false};
    bool pfxValid{false};
    std::array<std::string, scxt::maxEffectsPerPart> sentPfxValues;
    std::array<BusFxStorage, scxt::maxEffectsPerPart> pfxShadow{};
    std::array<std::chrono::steady_clock::time_point, scxt::maxEffectsPerPart> pfxShadowTime{};
    // misc (phasors, randoms) and audio (envelope followers) modulators of the group
    scxt::modulation::MiscSourceStorage miscShadow{};
    scxt::modulation::AudioSourceStorage audioShadow{};
    std::chrono::steady_clock::time_point miscShadowTime{}, audioShadowTime{};
    int miscShadowGroup{-1}, audioShadowGroup{-1};
    std::string sentMiscText;
    // macro names, copied on the audio thread with the group names
    std::string macroSnap, sentMacroNames;
    // a part save in progress
    std::string savePath;
    std::chrono::steady_clock::time_point saveDeadline{};
    fs::file_time_type saveOldTime{};
    bool saveExisted{false};
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

    // select every zone of the edited group(s): the zones' amp envelope edits go to all of them
    void selectZones()
    {
        using SAC = scxt::selection::SelectionManager::SelectActionContents;
        auto &patch = engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        if (!part)
            return;
        std::vector<SAC> acts;
        int n = (int)part->getGroups().size();
        for (int gi = 0; gi < n; ++gi)
        {
            if (!allGroups && gi != group)
                continue;
            int nz = (int)part->getGroup(gi)->getZones().size();
            for (int zi = 0; zi < nz; ++zi)
            {
                bool first = acts.empty();
                acts.push_back(SAC(0, gi, zi, true, first, first)); // forZone defaults to true
            }
        }
        if (!acts.empty())
            send<cmsg::ApplySelectActions>(acts);
    }
    bool hasZones()
    {
        auto &patch = engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        return part && group < (int)part->getGroups().size() && !part->getGroup(group)->getZones().empty();
    }
    const AdsrStorage *shownEg()
    {
        auto &patch = engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        if (!part || group >= (int)part->getGroups().size())
            return nullptr;
        auto &g = part->getGroup(group);
        if (egIndex >= 0)
            return &g->gegStorage[egIndex];
        if (g->getZones().empty())
            return nullptr;
        return &g->getZone(0)->egStorage[0];
    }

    void selectGroup()
    {
        using SAC = scxt::selection::SelectionManager::SelectActionContents;
        send<cmsg::SelectPart>((int16_t)0);
        std::vector<SAC> acts;
        SAC lead(0, group, -1, true, true, true);
        lead.forZone = false;
        acts.push_back(lead);
        if (allGroups)
        {
            // like a multi-selection in Shortcircuit's editor: group 1 leads, the rest join
            auto &patch = engine->getPatch();
            auto *part = patch ? patch->getPart(0).get() : nullptr;
            int n = part ? (int)part->getGroups().size() : 0;
            for (int i = 0; i < n; ++i)
            {
                if (i == group)
                    continue;
                SAC a(0, i, -1, true, false, false);
                a.forZone = false;
                acts.push_back(a);
            }
        }
        send<cmsg::ApplySelectActions>(acts);
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

    template <typename T> void groupFieldAs16(T GroupOutputInfo::*member, int16_t value)
    {
        static_assert(sizeof(T) == sizeof(int16_t));
        GroupOutputInfo dummy{};
        auto off = (ptrdiff_t)((uint8_t *)&(dummy.*member) - (uint8_t *)&dummy);
        send<cmsg::UpdateGroupOutputInt16TValue>(cmsg::detail::diffMsg_t<int16_t>{off, value});
    }

    template <typename T> void egField(T AdsrStorage::*member, T value)
    {
        AdsrStorage dummy{};
        auto off = (ptrdiff_t)((uint8_t *)&(dummy.*member) - (uint8_t *)&dummy);
        currentEgShadow().*member = value;
        // group EG egIndex (0..4), or with egIndex -1 the zones' amp EG (egStorage[0])
        bool forZone = egIndex < 0;
        if (forZone)
        {
            if (!hasZones())
                return;
            selectZones();
        }
        size_t idx = forZone ? 0 : (size_t)egIndex;
        if constexpr (std::is_same_v<T, bool>)
            send<cmsg::UpdateZoneOrGroupEGBoolValue>(
                cmsg::detail::indexedZoneOrGroupDiffMsg_t<bool>{forZone, idx, off, value});
        else
            send<cmsg::UpdateZoneOrGroupEGFloatValue>(
                cmsg::detail::indexedZoneOrGroupDiffMsg_t<float>{forZone, idx, off, value});
    }

    // a field of the selected LFO: offset in ModulatorStorage, typed by T
    template <typename T> void lfoAt(ptrdiff_t off, T value)
    {
        using namespace cmsg;
        if constexpr (std::is_same_v<T, bool>)
            send<UpdateZoneOrGroupModStorageBoolValue>(
                detail::indexedZoneOrGroupDiffMsg_t<bool>{false, (size_t)lfoIndex, off, value});
        else if constexpr (std::is_same_v<T, int16_t>)
            send<UpdateZoneOrGroupModStorageInt16TValue>(
                detail::indexedZoneOrGroupDiffMsg_t<int16_t>{false, (size_t)lfoIndex, off, value});
        else
            send<UpdateZoneOrGroupModStorageFloatValue>(
                detail::indexedZoneOrGroupDiffMsg_t<float>{false, (size_t)lfoIndex, off, value});
    }
#define LFO_OFF(field) ((ptrdiff_t)offsetof(ModStorage, field))
#define CLFO_OFF(field)                                                                            \
    ((ptrdiff_t)offsetof(ModStorage, curveLfoStorage) +                                            \
     (ptrdiff_t)offsetof(scxt::modulation::modulators::CurveLFOStorage, field))
#define ELFO_OFF(field)                                                                            \
    ((ptrdiff_t)offsetof(ModStorage, envLfoStorage) +                                              \
     (ptrdiff_t)offsetof(scxt::modulation::modulators::EnvLFOStorage, field))

    void setGateMode(int mode)
    {
        bool forZone = egIndex < 0;
        if (forZone)
        {
            if (!hasZones())
                return;
            selectZones();
        }
        auto &st = currentEgShadow();
        st.gateMode = mode == 2   ? AdsrStorage::GateMode::ONESHOT
                      : mode == 1 ? AdsrStorage::GateMode::SEMI_GATED
                                  : AdsrStorage::GateMode::GATED;
        send<cmsg::UpdateFullAdsrStorageForGroupsOrZones>(
            cmsg::fullAdsrStoragePayload_t{forZone, forZone ? 0 : egIndex, st});
    }

    // ---- effects (the group's 4 processor slots) -------------------------
    static std::string clean(std::string t)
    {
        for (auto &c : t)
            if (c == '|' || c == '~' || c == '^' || c == '"' || c == '\\' || (unsigned char)c < 32)
                c = c == '"' ? '\'' : ' ';
        return t;
    }
    static std::string num(float v)
    {
        char b[32];
        snprintf(b, sizeof(b), "%.6g", v);
        return b;
    }

    // Text longer than mod-host passes in one message goes out in pieces:
    // "<tag> <n>/<total>|<piece>"; the panel joins the pieces of a tag in order.
    void sendChunked(uint32_t p, const std::string &tag, const std::string &payload)
    {
        const size_t piece = 400;
        size_t total = std::max<size_t>(1, (payload.size() + piece - 1) / piece);
        for (size_t i = 0; i < total; ++i)
            queueString(p, tag + " " + std::to_string(i + 1) + "/" + std::to_string(total) + "|" +
                               payload.substr(i * piece, piece));
    }

    void sendFxTypes()
    {
        // "<id>^<group>^<name>~..." in Shortcircuit's own menu order
        std::string t;
        for (auto &d : scxt::dsp::processor::getAllProcessorDescriptions())
        {
            if (!t.empty())
                t += "~";
            t += std::to_string((int)d.id) + "^" + clean(d.displayGroup) + "^" + clean(d.displayName);
        }
        sendChunked(P_FX_TYPES, "T", t);
    }

    // "<slot>|<type>|<name>|<floats>|<ints>"; floats "name^min^max^default^F|I|B" joined
    // with '~'; ints "name^min^max^default^choice^choice..." joined with '~'
    std::string fxDescText(int s)
    {
        const auto &d = desc[s];
        std::string t = std::to_string(s) + "|" + std::to_string(descType[s]) + "|" +
                        clean(d.typeDisplayName) + "|";
        for (int i = 0; i < d.numFloatParams; ++i)
        {
            const auto &m = d.floatControlDescriptions[i];
            t += (i ? "~" : "") + clean(m.name) + "^" + num(m.minVal) + "^" + num(m.maxVal) + "^" +
                 num(m.defaultVal) + "^" +
                 (m.type == sst::basic_blocks::params::ParamMetaData::NONE   ? "N"
                  : m.type == sst::basic_blocks::params::ParamMetaData::INT  ? "I"
                  : m.type == sst::basic_blocks::params::ParamMetaData::BOOL ? "B"
                                                                             : "F");
        }
        t += "|";
        for (int i = 0; i < d.numIntParams; ++i)
        {
            const auto &m = d.intControlDescriptions[i];
            if (m.type == sst::basic_blocks::params::ParamMetaData::NONE)
            {
                t += (i ? "~" : "") + std::string("^0^0^0"); // unused: no name, the panel hides it
                continue;
            }
            t += (i ? "~" : "") + clean(m.name) + "^" + num(m.minVal) + "^" + num(m.maxVal) + "^" +
                 num(m.defaultVal);
            int lo = (int)std::round(m.minVal), hi = (int)std::round(m.maxVal);
            for (int v = lo; v <= hi && v - lo < 96; ++v)
                t += "^" + clean(m.valueToString((float)v).value_or(std::to_string(v)));
        }
        return t;
    }

    // "<slot>|<type>|<active>|<mix>|<level>|<floats ~>|<ints ~>|<float texts ~>|<mix text>~<level text>"
    std::string fxValuesText(int s, const ProcStorage &ps)
    {
        const auto &d = desc[s];
        std::string t = std::to_string(s) + "|" + std::to_string((int)ps.type) + "|" +
                        (ps.isActive ? "1" : "0") + "|" + num(ps.mix) + "|" + num(ps.outputCubAmp) + "|";
        for (int i = 0; i < d.numFloatParams; ++i)
            t += (i ? "~" : "") + num(ps.floatParams[i]);
        t += "|";
        for (int i = 0; i < d.numIntParams; ++i)
            t += (i ? "~" : "") + std::to_string(ps.intParams[i]);
        t += "|";
        auto fs = sst::basic_blocks::params::ParamMetaData::FeatureState().withTemposync(ps.isTemposynced);
        for (int i = 0; i < d.numFloatParams; ++i)
            t += (i ? "~" : "") +
                 clean(d.floatControlDescriptions[i].valueToString(ps.floatParams[i], fs).value_or(num(ps.floatParams[i])));
        t += "|" + std::to_string((int)std::round(ps.mix * 100)) + " %~";
        auto amp = ps.outputCubAmp;
        if (amp <= 0.00001f)
            t += "-inf dB";
        else
        {
            char b[32];
            auto db = 60.0 * std::log10(amp) + ProcStorage::maxOutputDB;
            snprintf(b, sizeof(b), "%.1f dB", std::fabs(db) < 0.05 ? 0.0 : db);
            t += b;
        }
        return t;
    }

    // Called from readback(): refresh the descriptions when the group or a slot's type
    // changed, then send the slots whose values changed.
    void readFx(const scxt::engine::Group &g, bool all)
    {
        bool need = !descValid || descGroup != group || all;
        for (int s = 0; s < numFx && !need; ++s)
            need = (int)g.processorStorage[s].type != descType[s] ||
                   (int)g.processorStorage[s].isKeytracked != descKeytrack[s];
        if (need)
        {
            descValid = false; // stays wanted until a snapshot arrives
            if (descRequest.load() < 0 && !descReady.load())
            {
                descRequestedGroup = group;
                descRequest = group;
                return; // run() copies them; picked up on the next pass
            }
            if (!descReady.load())
                return;
            desc = descSnap;
            descReady = false;
            if (descRequestedGroup != group)
                return; // another group was selected meanwhile: ask again next pass
            for (int s = 0; s < numFx; ++s)
            {
                descType[s] = (int)desc[s].type;
                descKeytrack[s] = (int)g.processorStorage[s].isKeytracked;
            }
            descGroup = group;
            descValid = true;
            sendChunked(P_MOD_META, "M", modMetaText(metaSnap));
            sentModRows.clear();
            for (int s = 0; s < numFx; ++s)
            {
                // the slot changed type again meanwhile: try again
                if ((int)g.processorStorage[s].type != descType[s])
                    descValid = false;
                sendChunked(P_FX_DESC, std::to_string(s), fxDescText(s));
                sentFxValues[s].clear();
            }
        }
        {
            auto rows = modRowsText(g);
            if (rows != sentModRows)
            {
                sentModRows = rows;
                sendChunked(P_MOD_ROWS, "R", rows);
            }
        }
        for (int s = 0; s < numFx; ++s)
        {
            if ((int)g.processorStorage[s].type != descType[s])
                continue;
            auto t = fxValuesText(s, g.processorStorage[s]);
            if (t != sentFxValues[s])
            {
                sentFxValues[s] = t;
                sendChunked(P_FX_VALUES, std::to_string(s), t);
            }
        }
    }

    void applyFxEdit(const std::string &t)
    {
        int s = -1, idx = 0;
        char kind = 0;
        float v = 0;
        if (sscanf(t.c_str(), "%d,%c,%d,%f", &s, &kind, &idx, &v) != 4 || s < 0 || s >= numFx)
            return;
        selectGroup();
        auto fieldOff = [](auto ProcStorage::*member) {
            ProcStorage dummy{};
            return (ptrdiff_t)((uint8_t *)&(dummy.*member) - (uint8_t *)&dummy);
        };
        using fmsg = cmsg::detail::indexedZoneOrGroupDiffMsg_t<float>;
        switch (kind)
        {
        case 't':
            if (v >= 0 && v < (float)(int)scxt::dsp::processor::proct_num_types)
                send<cmsg::SetSelectedProcessorType>(
                    cmsg::setProcessorPayload_t{false, (int32_t)s, (int32_t)std::lround(v)});
            break;
        case 'a':
            send<cmsg::UpdateZoneOrGroupProcessorBoolValue>(cmsg::detail::indexedZoneOrGroupDiffMsg_t<bool>{
                false, (size_t)s, fieldOff(&ProcStorage::isActive), v > 0.5f});
            break;
        case 'm':
            send<cmsg::UpdateZoneOrGroupProcessorFloatValue>(
                fmsg{false, (size_t)s, fieldOff(&ProcStorage::mix), v});
            break;
        case 'l':
            send<cmsg::UpdateZoneOrGroupProcessorFloatValue>(
                fmsg{false, (size_t)s, fieldOff(&ProcStorage::outputCubAmp), v});
            break;
        case 'f':
            if (idx >= 0 && idx < (int)scxt::maxProcessorFloatParams)
                send<cmsg::UpdateZoneOrGroupProcessorFloatValue>(fmsg{
                    false, (size_t)s,
                    fieldOff(&ProcStorage::floatParams) + (ptrdiff_t)(idx * sizeof(float)), v});
            break;
        case 'i':
            if (idx >= 0 && idx < (int)scxt::maxProcessorIntParams)
                send<cmsg::UpdateZoneOrGroupProcessorInt32TValue>(cmsg::processorIntValuePayload_t{
                    false, (size_t)s,
                    fieldOff(&ProcStorage::intParams) + (ptrdiff_t)(idx * sizeof(int32_t)),
                    (int32_t)std::lround(v)});
            break;
        default:
            break;
        }
    }

    // ---- group mod matrix --------------------------------------------------
    template <typename I> static std::string ident(const std::optional<I> &i)
    {
        if (!i.has_value())
            return "";
        return std::to_string(i->gid) + "." + std::to_string(i->tid) + "." + std::to_string(i->index);
    }
    template <typename I> static std::optional<I> parseIdent(const std::string &t)
    {
        unsigned g, ti, ix;
        if (sscanf(t.c_str(), "%u.%u.%u", &g, &ti, &ix) != 3)
            return std::nullopt;
        I r;
        r.gid = g;
        r.tid = ti;
        r.index = ix;
        return r;
    }

    // "S|<id>^<path>^<name>~...|T|<id>^<path>^<name>~...|C|<id>^<path>^<name>~..."
    std::string modMetaText(const scxt::modulation::groupMatrixMetadata_t &m)
    {
        std::string t = "S|";
        bool first = true;
        for (auto &[src, nm] : std::get<1>(m))
        {
            t += (first ? "" : "~") + ident(std::optional<GMSource>(src)) + "^" + clean(nm.first) +
                 "^" + clean(nm.second);
            first = false;
        }
        t += "|T|";
        first = true;
        for (auto &nt : std::get<2>(m))
        {
            if (!std::get<3>(nt))
                continue; // not enabled
            auto &dn = std::get<1>(nt);
            t += (first ? "" : "~") + ident(std::optional<GMTarget>(std::get<0>(nt))) + "^" +
                 clean(scxt::modulation::displayPath(dn)) + "^" + clean(std::get<1>(dn));
            first = false;
        }
        t += "|C|";
        first = true;
        for (auto &[c, nm] : std::get<3>(m))
        {
            t += (first ? "" : "~") + std::to_string(c) + "^" + clean(nm.first) + "^" +
                 clean(nm.second);
            first = false;
        }
        return t;
    }

    // rows "<active>^<source>^<via>^<curve>^<target>^<depth>" joined with '~'
    std::string modRowsText(const scxt::engine::Group &g)
    {
        std::string t;
        for (int i = 0; i < numModRows; ++i)
        {
            const auto &r = g.routingTable.routes[i];
            auto src = r.source;
            auto via = r.sourceVia;
            auto tgt = r.target;
            auto crv = r.curve;
            t += (i ? "~" : "") + std::string(r.active ? "1" : "0") + "^" + ident(src) + "^" +
                 ident(via) + "^" + (crv.has_value() ? std::to_string(*crv) : std::string()) +
                 "^" + ident(tgt) + "^" + num(r.depth);
        }
        return t;
    }

    void applyModEdit(const std::string &t)
    {
        // "<row>,<field>,<value>"
        auto c1 = t.find(','), c2 = c1 == std::string::npos ? c1 : t.find(',', c1 + 1);
        if (c2 == std::string::npos || c2 != c1 + 2)
            return;
        int row = atoi(t.substr(0, c1).c_str());
        char field = t[c1 + 1];
        auto val = t.substr(c2 + 1);
        if (row < 0 || row >= numModRows)
            return;
        auto &patch = engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        if (!part || group >= (int)part->getGroups().size())
            return;
        // start from the row as it is (read like the editor's display), change one field
        auto now = std::chrono::steady_clock::now();
        bool useShadow = rowShadowGroup == group && now - rowShadowTime[row] < std::chrono::seconds(2);
        const auto &cur = useShadow ? rowShadow[row] : part->getGroup(group)->routingTable.routes[row];
        GMRouting r;
        r.active = cur.active;
        r.source = cur.source;
        r.sourceVia = cur.sourceVia;
        r.target = cur.target;
        r.curve = cur.curve;
        r.sourceLagMS = cur.sourceLagMS;
        r.sourceViaLagMS = cur.sourceViaLagMS;
        r.sourceLagExp = cur.sourceLagExp;
        r.sourceViaLagExp = cur.sourceViaLagExp;
        r.applicationMode = cur.applicationMode;
        r.depth = cur.depth;
        switch (field)
        {
        case 'a': r.active = val == "1"; break;
        case 's': r.source = parseIdent<GMSource>(val); break;
        case 'v': r.sourceVia = parseIdent<GMSource>(val); break;
        case 't': r.target = parseIdent<GMTarget>(val); break;
        case 'c':
            if (val.empty())
                r.curve = std::nullopt;
            else
                r.curve = (scxt::modulation::GroupMatrixConfig::CurveIdentifier)strtoul(val.c_str(), nullptr, 10);
            break;
        case 'd': r.depth = std::clamp((float)atof(val.c_str()), -1.f, 1.f); break;
        default: return;
        }
        if (rowShadowGroup != group)
            rowShadowTime.fill({});
        rowShadowGroup = group;
        rowShadow[row] = r;
        rowShadowTime[row] = now;
        selectGroup();
        send<cmsg::UpdateGroupRoutingRow>(cmsg::updateGroupRoutingRowPayload_t{row, r, field != 'd'});
        sentModRows.clear();
    }

    // ---- part 1: settings, sends, effects -----------------------------------
    scxt::engine::Part *part0()
    {
        auto &patch = engine->getPatch();
        return patch ? patch->getPart(0).get() : nullptr;
    }
    PartConfig &currentPartShadow()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - partShadowTime > std::chrono::seconds(2))
            if (auto *pt = part0())
                partShadow = pt->configuration;
        partShadowTime = now;
        return partShadow;
    }
    void sendPartConfig() { send<cmsg::UpdatePartFullConfig>(cmsg::partConfigurationPayload_t{(int16_t)0, partShadow}); }
    SendStorage &currentSendShadow()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - sendShadowTime > std::chrono::seconds(2))
            if (auto &patch = engine->getPatch())
                sendShadow = patch->busses.partBusses[0].busSendStorage;
        sendShadowTime = now;
        return sendShadow;
    }
    void setSend(int i, float v)
    {
        auto &ss = currentSendShadow();
        ss.sendLevels[i] = std::clamp(v, 0.f, 1.f);
        send<cmsg::SetBusSendStorage>(cmsg::setBusSendStorage_t{(int)scxt::engine::BusAddress::PART_0, ss});
    }

    void sendPfxTypes()
    {
        std::string t;
        for (int i = 0; i <= (int)scxt::engine::LastAvailableBusEffect; ++i)
            t += (i ? "~" : "") + std::to_string(i) + "^" +
                 clean(i == 0 ? std::string("none") : scxt::engine::getBusEffectDisplayName((scxt::engine::AvailableBusEffects)i));
        queueString(P_PFX_TYPES, t);
    }
    static const char *pmdType(const scxt::datamodel::pmd &m)
    {
        using PMD = sst::basic_blocks::params::ParamMetaData;
        return m.type == PMD::NONE ? "N" : m.type == PMD::INT ? "I" : m.type == PMD::BOOL ? "B" : "F";
    }
    // "<slot>|<type>|<name^min^max^default^F|I|B|N^choice^choice...>~..."
    std::string pfxDescText(int s)
    {
        const auto &d = pfxDesc[s];
        std::string t = std::to_string(s) + "|" + std::to_string(d.type) + "|";
        for (int i = 0; i < d.n; ++i)
        {
            const auto &m = d.m[i];
            t += (i ? "~" : "") + clean(m.name) + "^" + num(m.minVal) + "^" + num(m.maxVal) + "^" +
                 num(m.defaultVal) + "^" + pmdType(m);
            if (m.type == sst::basic_blocks::params::ParamMetaData::INT)
            {
                int lo = (int)std::round(m.minVal), hi = (int)std::round(m.maxVal);
                for (int v = lo; v <= hi && v - lo < 64; ++v)
                    t += "^" + clean(m.valueToString((float)v).value_or(std::to_string(v)));
            }
        }
        return t;
    }
    // "<slot>|<type>|<active>|<sync>|<values ~>|<texts ~>"
    std::string pfxValuesText(int s, const BusFxStorage &st)
    {
        const auto &d = pfxDesc[s];
        std::string t = std::to_string(s) + "|" + std::to_string((int)st.type) + "|" +
                        (st.isActive ? "1" : "0") + "|" + (st.isTemposync ? "1" : "0") + "|";
        for (int i = 0; i < d.n; ++i)
            t += (i ? "~" : "") + num(st.params[i]);
        t += "|";
        auto fs = sst::basic_blocks::params::ParamMetaData::FeatureState().withTemposync(st.isTemposync);
        for (int i = 0; i < d.n; ++i)
            t += (i ? "~" : "") + clean(d.m[i].valueToString(st.params[i], fs).value_or(num(st.params[i])));
        return t;
    }
    void readPfx(scxt::engine::Part &pt, bool all)
    {
        bool need = !pfxValid || all;
        for (int s = 0; s < (int)scxt::maxEffectsPerPart && !need; ++s)
            need = (int)pt.partEffectStorage[s].type != pfxDesc[s].type;
        if (need)
        {
            pfxValid = false;
            if (!pfxRequest.load() && !pfxReady.load())
            {
                pfxRequest = true;
                return;
            }
            if (!pfxReady.load())
                return;
            pfxDesc = pfxSnap;
            pfxReady = false;
            pfxValid = true;
            for (int s = 0; s < (int)scxt::maxEffectsPerPart; ++s)
            {
                if ((int)pt.partEffectStorage[s].type != pfxDesc[s].type)
                    pfxValid = false;
                sendChunked(P_PFX_DESC, std::to_string(s), pfxDescText(s));
                sentPfxValues[s].clear();
            }
        }
        for (int s = 0; s < (int)scxt::maxEffectsPerPart; ++s)
        {
            if ((int)pt.partEffectStorage[s].type != pfxDesc[s].type)
                continue;
            auto t = pfxValuesText(s, pt.partEffectStorage[s]);
            if (t != sentPfxValues[s])
            {
                sentPfxValues[s] = t;
                sendChunked(P_PFX_VALUES, std::to_string(s), t);
            }
        }
    }
    void applyPfxEdit(const std::string &t)
    {
        int s = -1, idx = 0;
        char kind = 0;
        float v = 0;
        if (sscanf(t.c_str(), "%d,%c,%d,%f", &s, &kind, &idx, &v) != 4 || s < 0 ||
            s >= (int)scxt::maxEffectsPerPart)
            return;
        auto *pt = part0();
        if (!pt)
            return;
        auto now = std::chrono::steady_clock::now();
        if (kind == 't')
        {
            int ty = (int)std::lround(v);
            if (ty < 0 || ty > (int)scxt::engine::LastAvailableBusEffect)
                return;
            send<cmsg::SetBusEffectToType>(cmsg::setBusEffectToType_t{-1, 0, s, ty});
            pfxShadowTime[s] = {}; // the new effect sets its own defaults
            return;
        }
        if (now - pfxShadowTime[s] > std::chrono::seconds(2))
            pfxShadow[s] = pt->partEffectStorage[s];
        pfxShadowTime[s] = now;
        auto &st = pfxShadow[s];
        switch (kind)
        {
        case 'a': st.isActive = v > 0.5f; break;
        case 'y': st.isTemposync = v > 0.5f; break;
        case 'p':
            if (idx < 0 || idx >= (int)scxt::maxBusEffectParams)
                return;
            st.params[idx] = v;
            break;
        default: return;
        }
        send<cmsg::SetBusEffectStorage>(cmsg::setBusEffectStorage_t{-1, 0, s, st});
    }

    // ---- group management ------------------------------------------------
    void applyGroupCmd(const std::string &t)
    {
        // "r,<group>,<name>" "d,<group>" "m,<group>,<0|1>" "s,<group>,<0|1>"
        if (t.size() < 3 || t[1] != ',')
            return;
        char k = t[0];
        auto rest = t.substr(2);
        auto c = rest.find(',');
        int g = atoi(rest.substr(0, c).c_str());
        std::string arg = c == std::string::npos ? "" : rest.substr(c + 1);
        auto *pt = part0();
        if (!pt || g < 0 || g >= (int)pt->getGroups().size())
            return;
        using ZA = scxt::selection::SelectionManager::ZoneAddress;
        switch (k)
        {
        case 'r':
            if (!arg.empty())
                send<cmsg::RenameGroup>(cmsg::renameGroupZonePayload_t{ZA(0, g, -1), arg.substr(0, 120)});
            break;
        case 'd':
            send<cmsg::DeleteGroup>(ZA(0, g, -1));
            if (!allGroups && group >= g && group > 0)
                group -= 1;
            holdReadback = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            break;
        case 'm':
        case 's':
            send<cmsg::MuteOrSoloGroup>(cmsg::muteOrSoloGroup_t{0, g, k == 's', arg == "1", 0});
            break;
        default:
            return;
        }
        structureChanged = true;
        wantAll = true;
    }

    // ---- save part 1 as .scp ---------------------------------------------
    void savePart(const std::string &t)
    {
        // "<0|1>,<name or path>": 1 = with its samples in the file
        if (t.size() < 3 || t[1] != ',')
            return;
        bool monolith = t[0] == '1';
        std::string name = t.substr(2);
        while (!name.empty() && (name.back() == ' ' || name.back() == '/'))
            name.pop_back();
        if (name.empty())
        {
            queueString(P_SAVED, "error|no name");
            return;
        }
        fs::path pth = name[0] == '/' ? fs::path(name) : fs::path(defaultDir) / name;
        if (pth.extension() != ".scp")
            pth += ".scp";
        std::error_code ec;
        fs::create_directories(pth.parent_path(), ec);
        savePath = pth.string();
        saveExisted = fs::exists(pth);
        saveOldTime = saveExisted ? fs::last_write_time(pth, ec) : fs::file_time_type{};
        saveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        send<cmsg::SavePart>(cmsg::savePartPayload_t{
            savePath, 0,
            (int)(monolith ? scxt::patch_io::SaveStyles::AS_MONOLITH : scxt::patch_io::SaveStyles::NO_SAMPLES)});
        fprintf(stderr, "[scxt-lv2] save part 1 to %s\n", savePath.c_str());
    }
    void checkSave()
    {
        if (savePath.empty())
            return;
        std::error_code ec;
        bool done = fs::exists(savePath) && fs::file_size(savePath, ec) > 0 &&
                    (!saveExisted || fs::last_write_time(savePath, ec) != saveOldTime);
        if (done)
        {
            queueString(P_SAVED, "ok|" + savePath);
            if (fs::path(savePath).parent_path() == fs::path(curDir))
                sendListing(curDir);
            savePath.clear();
        }
        else if (std::chrono::steady_clock::now() > saveDeadline)
        {
            queueString(P_SAVED, "error|could not write " + savePath);
            savePath.clear();
        }
    }

    // ---- misc / audio modulators of the group ------------------------------
    // "P|<sync^division^direction^phase^numerator^denominator>~...|R|<style>~...|
    //  F|<attack^release^gain^link^source^attack text^release text^gain text>~..."
    std::string miscText(const scxt::engine::Group &g)
    {
        const auto &ms = g.miscSourceStorage;
        std::string t = "P|";
        for (int i = 0; i < (int)scxt::phasorsPerGroupOrZone; ++i)
        {
            const auto &p = ms.phasors[i];
            t += (i ? "~" : "") + std::to_string((int)p.syncMode) + "^" + std::to_string((int)p.division) +
                 "^" + std::to_string((int)p.direction) + "^" + num(p.phase) + "^" +
                 std::to_string(p.numerator) + "^" + std::to_string(p.denominator);
        }
        t += "|R|";
        for (int i = 0; i < (int)scxt::randomsPerGroupOrZone; ++i)
            t += (i ? "~" : "") + std::to_string((int)ms.randoms[i].style);
        t += "|F|";
        scxt::modulation::AudioSourceStorage as = g.audioSourceStorage;
        using FS = sst::basic_blocks::params::ParamMetaData::FeatureState;
        auto txt = [&](float &v) {
            return clean(scxt::datamodel::describeValue(as, v).valueToString(v, FS()).value_or(num(v)));
        };
        for (int i = 0; i < (int)scxt::envFollowersPerGroupOrZone; ++i)
        {
            auto &f = as.followers[i];
            t += (i ? "~" : "") + num(f.attack) + "^" + num(f.release) + "^" + num(f.gain) + "^" +
                 (f.stereoLink ? "1" : "0") + "^" + std::to_string((int)f.followSource) + "^" +
                 txt(f.attack) + "^" + txt(f.release) + "^" + txt(f.gain);
        }
        return t;
    }
    void applyMiscEdit(const std::string &t)
    {
        // "<p|r|f>,<index>,<field>,<value>"
        char k = 0, f = 0;
        int i = 0;
        float v = 0;
        if (sscanf(t.c_str(), "%c,%d,%c,%f", &k, &i, &f, &v) != 4 || i < 0)
            return;
        auto *pt = part0();
        if (!pt || group >= (int)pt->getGroups().size())
            return;
        auto &g = pt->getGroup(group);
        auto now = std::chrono::steady_clock::now();
        int iv = (int)std::lround(v);
        selectGroup();
        if (k == 'f')
        {
            if (i >= (int)scxt::envFollowersPerGroupOrZone)
                return;
            if (audioShadowGroup != group || now - audioShadowTime > std::chrono::seconds(2))
                audioShadow = g->audioSourceStorage;
            audioShadowGroup = group;
            audioShadowTime = now;
            auto &e = audioShadow.followers[i];
            using ES = scxt::modulation::modulators::EnvFollowerStorage;
            switch (f)
            {
            case 'a': e.attack = std::clamp(v, 0.000001f, 0.1f); break;
            case 'r': e.release = std::clamp(v, 0.01f, 0.5f); break;
            case 'g': e.gain = std::clamp(v, -36.f, 36.f); break;
            case 'l': e.stereoLink = iv > 0; break;
            case 's': e.followSource = iv ? ES::Source::POST_PROC : ES::Source::PRE_PROC; break;
            default: return;
            }
            send<cmsg::UpdateAudiomodStorageForSelectedGroupOrZone>(cmsg::gzAudioModStorageUpdate_t{false, audioShadow});
            return;
        }
        if (miscShadowGroup != group || now - miscShadowTime > std::chrono::seconds(2))
            miscShadow = g->miscSourceStorage;
        miscShadowGroup = group;
        miscShadowTime = now;
        if (k == 'p')
        {
            if (i >= (int)scxt::phasorsPerGroupOrZone)
                return;
            using PS = scxt::modulation::modulators::PhasorStorage;
            auto &p = miscShadow.phasors[i];
            switch (f)
            {
            case 's': p.syncMode = iv ? PS::SyncMode::VOICEPOS : PS::SyncMode::SONGPOS; break;
            case 'd': p.division = (PS::Division)std::clamp(iv, 0, 3); break;
            case 'r': p.direction = iv ? PS::Direction::DESCENDING : PS::Direction::ASCENDING; break;
            case 'h': p.phase = std::clamp(v, 0.f, 1.f); break;
            case 'n': p.numerator = std::clamp(iv, 1, 64); break;
            case 'm': p.denominator = std::clamp(iv, 1, 64); break;
            default: return;
            }
        }
        else if (k == 'r')
        {
            if (i >= (int)scxt::randomsPerGroupOrZone || f != 's')
                return;
            miscShadow.randoms[i].style = (scxt::modulation::modulators::RandomStorage::Style)std::clamp(iv, 0, 6);
        }
        else
            return;
        send<cmsg::UpdateMiscmodStorageForSelectedGroupOrZone>(cmsg::gzMiscStorageUpdate_t{false, miscShadow});
    }

    // ---- macro names -------------------------------------------------------
    void applyMacroEdit(const std::string &t)
    {
        auto c = t.find(',');
        if (c == std::string::npos)
            return;
        int i = atoi(t.substr(0, c).c_str());
        auto *pt = part0();
        if (!pt || i < 0 || i >= (int)scxt::macrosPerPart)
            return;
        scxt::engine::Macro m = pt->macros[i];
        m.name = t.substr(c + 1, 60);
        send<cmsg::SetMacroFullState>(cmsg::macroFullState_t{(int16_t)0, (int16_t)i, m});
        namesRequest = true; // read the names again
    }

    // ---- file browser ---------------------------------------------------
    void initDirs()
    {
        const char *home = getenv("HOME");
        if (!home || !*home)
        {
            // services (mod-host) may run without HOME
            if (auto *pw = getpwuid(getuid()))
                home = pw->pw_dir;
        }
        homeDir = home ? home : "/";
        defaultDir = homeDir + "/.local/share/ShortCircuit-XT";
        std::error_code ec;
        fs::create_directories(defaultDir, ec);
        if (!fs::is_directory(defaultDir) && fs::is_directory("/usr/share/ShortCircuit-XT"))
            defaultDir = "/usr/share/ShortCircuit-XT";
        configFile = homeDir + "/.config/ShortCircuit-XT/browser.txt";
        curDir = defaultDir;
        std::ifstream f(configFile);
        std::string line;
        while (std::getline(f, line))
        {
            if (line.rfind("last=", 0) == 0 && fs::is_directory(line.substr(5)))
                curDir = line.substr(5);
            else if (line.rfind("folder=", 0) == 0)
                favourites.push_back(line.substr(7));
        }
    }

    void saveConfig()
    {
        std::error_code ec;
        fs::create_directories(fs::path(configFile).parent_path(), ec);
        std::ofstream f(configFile);
        f << "# Shortcircuit XT LV2 panel: last folder and saved folders\n";
        f << "last=" << curDir << "\n";
        for (auto &d : favourites)
            f << "folder=" << d << "\n";
    }

    void queueString(uint32_t p, const std::string &t)
    {
        std::lock_guard<std::mutex> g(strOutMutex);
        strOut.emplace_back(p, t);
    }

    void sendFolders()
    {
        // "<default folder>|<saved>|<saved>..." - the first entry is the instruments folder
        std::string t = defaultDir;
        for (auto &d : favourites)
            t += "|" + d;
        if (t.size() > 450)
            t.resize(450);
        queueString(P_FOLDERS, t);
    }

    // The listing goes out in chunks short enough for mod-host's feedback
    // messages (about 500 bytes): "<n>/<total>|<folder>|<entries>", entries
    // separated by '|', each starting with D (folder) or F (file).
    void sendListing(const std::string &dir)
    {
        std::vector<std::string> dirs, files;
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            auto name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || name.find('|') != std::string::npos)
                continue;
            std::error_code e2;
            if (it->is_directory(e2))
                dirs.push_back(name);
            else if (loadable(it->path()))
                files.push_back(name);
        }
        std::sort(dirs.begin(), dirs.end(), natLess);
        std::sort(files.begin(), files.end(), natLess);
        std::vector<std::string> entries;
        for (auto &d : dirs)
            entries.push_back("D" + d);
        for (auto &f : files)
            entries.push_back("F" + f);

        const size_t maxChunk = 420;
        std::string shownDir = dir.size() > 200 ? dir.substr(dir.size() - 200) : dir;
        std::vector<std::string> bodies(1);
        for (auto &e : entries)
        {
            auto en = e.size() > 200 ? e.substr(0, 200) : e;
            if (!bodies.back().empty() && bodies.back().size() + en.size() + 1 + shownDir.size() + 16 > maxChunk)
                bodies.emplace_back();
            if (!bodies.back().empty())
                bodies.back() += "|";
            bodies.back() += en;
        }
        if (bodies.size() > 400)
            bodies.resize(400); // a few thousand entries at most
        for (size_t i = 0; i < bodies.size(); ++i)
            queueString(P_LISTING, std::to_string(i + 1) + "/" + std::to_string(bodies.size()) + "|" +
                                       shownDir + "|" + bodies[i]);
    }

    int part0Groups()
    {
        auto &patch = engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        return part ? (int)part->getGroups().size() : 0;
    }

    void load(const std::string &path)
    {
        if (!fs::is_regular_file(path))
        {
            fprintf(stderr, "[scxt-lv2] file not found: %s\n", path.c_str());
            return;
        }
        send<cmsg::SelectPart>((int16_t)0);
        // instruments make their own groups; .scm (whole multi) and .scp (part) replace
        bool ownGroups = scxt::browser::Browser::isLoadableMultiSample(path);
        if (scxt::browser::Browser::isShortCircuitFormatFile(path))
        {
            send<cmsg::AddSample>(path); // .scp loads into the selected part (1), .scm everything
            group = 0;
        }
        else if (loadMode == 2 && !allGroups && part0Groups() > group)
        {
            // replace the selected group: a wave goes into that group in place of its zones
            // (the group keeps its settings, effects, envelopes and matrix); an instrument
            // file takes the group's place with the groups it makes (added at the end)
            if (ownGroups)
            {
                send<cmsg::DeleteGroup>(scxt::selection::SelectionManager::ZoneAddress(0, group, -1));
                send<cmsg::AddSample>(path);
                group = std::max(0, part0Groups() - 1);
            }
            else
            {
                auto &part = engine->getPatch()->getPart(0);
                int nz = (int)part->getGroup(group)->getZones().size();
                for (int z = nz - 1; z >= 0; --z)
                    send<cmsg::DeleteZone>(scxt::selection::SelectionManager::ZoneAddress(0, group, z));
                send<cmsg::AddSampleToGroup>(cmsg::addSampleToGroupPayload_t{path, 0, group});
                // name the group after the wave it now plays
                auto stem = fs::path(path).stem().u8string();
                send<cmsg::RenameGroup>(cmsg::renameGroupZonePayload_t{
                    scxt::selection::SelectionManager::ZoneAddress(0, group, -1),
                    std::string(stem.begin(), stem.end())});
            }
        }
        else if (loadMode == 1)
        {
            send<cmsg::ClearPart>(0);
            group = 0;
            if (ownGroups)
                send<cmsg::AddSample>(path);
            else
                send<cmsg::AddSampleToGroup>(cmsg::addSampleToGroupPayload_t{path, 0, 0});
        }
        else
        {
            // add: into a new group after the existing ones (an sfz etc. makes its own groups)
            auto &part = engine->getPatch()->getPart(0);
            int n = part ? (int)part->getGroups().size() : 0;
            if (ownGroups)
                send<cmsg::AddSample>(path);
            else
                send<cmsg::AddSampleToGroup>(cmsg::addSampleToGroupPayload_t{path, 0, n});
            if (!allGroups)
                group = n;
        }
        fprintf(stderr, "[scxt-lv2] %s %s\n",
                loadMode == 1 ? "replace part 1 with" : loadMode == 2 ? "replace the group with" : "add to part 1:",
                path.c_str());
        // the load runs on Shortcircuit's own thread; read back when it is done
        holdReadback = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        structureChanged = true;
        wantAll = true;
    }

    void applyString(uint32_t p, const std::string &t)
    {
        switch (p)
        {
        case P_BROWSE:
        {
            std::string dir = t.empty() ? curDir : t;
            std::error_code ec;
            auto canon = fs::weakly_canonical(dir, ec).string();
            if (!ec && !canon.empty())
                dir = canon;
            if (!fs::is_directory(dir))
                dir = defaultDir;
            if (dir != curDir)
            {
                curDir = dir;
                saveConfig();
            }
            sendListing(curDir);
            break;
        }
        case P_LOAD_PATH:
            load(t);
            break;
        case P_FOLDER_ADD:
            if (fs::is_directory(t) && t != defaultDir &&
                std::find(favourites.begin(), favourites.end(), t) == favourites.end())
            {
                favourites.push_back(t);
                saveConfig();
            }
            sendFolders();
            break;
        case P_FX_EDIT:
            applyFxEdit(t);
            break;
        case P_MOD_EDIT:
            applyModEdit(t);
            break;
        case P_PFX_EDIT:
            applyPfxEdit(t);
            break;
        case P_GROUP_CMD:
            applyGroupCmd(t);
            break;
        case P_SAVE_PART:
            savePart(t);
            break;
        case P_MISC_EDIT:
            applyMiscEdit(t);
            break;
        case P_MACRO_EDIT:
            applyMacroEdit(t);
            break;
        case P_FOLDER_REMOVE:
            favourites.erase(std::remove(favourites.begin(), favourites.end(), t), favourites.end());
            saveConfig();
            sendFolders();
            break;
        default:
            break;
        }
    }

    void apply(uint32_t p, float v)
    {
        auto iv = (int)std::lround(v);
        switch (p)
        {
        case P_GROUP:
            allGroups = iv < 0;
            group = std::max(0, iv);
            selectGroup();
            wantAll = true;
            return;
        case P_LOAD_MODE:
            loadMode = iv;
            return;
        case P_EG_INDEX:
            // 0..4: group EG 1..5 (as in Shortcircuit's group view)
            egIndex = std::clamp(iv, 0, (int)scxt::egsPerGroup - 1);
            wantAll = true;
            return;
        case P_LFO_INDEX:
            lfoIndex = std::clamp(iv, 0, (int)scxt::lfosPerGroup - 1);
            wantAll = true;
            return;
        case P_PART_VOLUME: currentPartShadow().level = std::clamp(v, 0.f, 1.f); sendPartConfig(); return;
        case P_PART_PAN: currentPartShadow().pan = std::clamp(v, -1.f, 1.f); sendPartConfig(); return;
        case P_PART_TUNING: currentPartShadow().tuning = std::clamp(v, -36.f, 36.f); sendPartConfig(); return;
        case P_PART_TRANSPOSE: currentPartShadow().transpose = std::clamp(iv, -36, 36); sendPartConfig(); return;
        case P_PART_CHANNEL: currentPartShadow().channel = (int16_t)std::clamp(iv, -1, 15); sendPartConfig(); return;
        case P_PART_POLY: currentPartShadow().polyLimitVoices = std::clamp(iv, 0, 256); sendPartConfig(); return;
        case P_PART_MUTE: currentPartShadow().mute = iv > 0; sendPartConfig(); return;
        case P_PART_SOLO: currentPartShadow().solo = iv > 0; sendPartConfig(); return;
        case P_SEND1: case P_SEND2: case P_SEND3: case P_SEND4: setSend((int)(p - P_SEND1), v); return;
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
        case P_PROC_ROUTING: groupFieldAs16(&GroupOutputInfo::procRouting, (int16_t)iv); break;
        case P_ROUTE_TO: groupFieldAs16(&GroupOutputInfo::routeTo, (int16_t)iv); break;
        case P_OVERSAMPLE: groupFieldAs16(&GroupOutputInfo::oversample, (int16_t)iv); break;
        case P_EG_ASH: egField(&AdsrStorage::aShape, v); break;
        case P_EG_DSH: egField(&AdsrStorage::dShape, v); break;
        case P_EG_RSH: egField(&AdsrStorage::rShape, v); break;
        case P_EG_SYNC: egField(&AdsrStorage::isTemposync, iv > 0); break;
        case P_EG_GATE_ANY: egField(&AdsrStorage::gateGroupEGOnAnyPlaying, iv > 0); break;
        case P_EG_GATE: setGateMode(iv); break;
        case P_LFO_SHAPE: lfoAt<int16_t>(LFO_OFF(modulatorShape), (int16_t)iv); break;
        case P_LFO_TRIGGER: lfoAt<int16_t>(LFO_OFF(triggerMode), (int16_t)iv); break;
        case P_LFO_RATE: lfoAt<float>(LFO_OFF(rate), v); break;
        case P_LFO_PHASE: lfoAt<float>(LFO_OFF(start_phase), v); break;
        case P_LFO_SYNC: lfoAt<bool>(LFO_OFF(temposync), iv > 0); break;
        case P_LFO_DEFORM: lfoAt<float>(CLFO_OFF(deform), v); break;
        case P_LFO_ANGLE: lfoAt<float>(CLFO_OFF(angle), v); break;
        case P_LFO_UNI: lfoAt<bool>(CLFO_OFF(unipolar), iv > 0); break;
        case P_LFO_USEENV: lfoAt<bool>(CLFO_OFF(useenv), iv > 0); break;
        case P_LFO_DLY: lfoAt<float>(CLFO_OFF(delay), v); break;
        case P_LFO_ATK: lfoAt<float>(CLFO_OFF(attack), v); break;
        case P_LFO_REL: lfoAt<float>(CLFO_OFF(release), v); break;
        case P_LFOE_DLY: lfoAt<float>(ELFO_OFF(delay), v); break;
        case P_LFOE_A: lfoAt<float>(ELFO_OFF(attack), v); break;
        case P_LFOE_H: lfoAt<float>(ELFO_OFF(hold), v); break;
        case P_LFOE_D: lfoAt<float>(ELFO_OFF(decay), v); break;
        case P_LFOE_S: lfoAt<float>(ELFO_OFF(sustain), v); break;
        case P_LFOE_R: lfoAt<float>(ELFO_OFF(release), v); break;
        case P_LFOE_LOOP: lfoAt<bool>(ELFO_OFF(loop), iv > 0); break;
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
        vals[P_GROUP] = allGroups ? -1.f : (float)group;
        vals[P_LOAD_MODE] = (float)loadMode;
        vals[P_LOADED] = loadedCounter;
        {
            const auto &pc = part->configuration;
            vals[P_PART_VOLUME] = pc.level;
            vals[P_PART_PAN] = pc.pan;
            vals[P_PART_TUNING] = pc.tuning;
            vals[P_PART_TRANSPOSE] = (float)pc.transpose;
            vals[P_PART_CHANNEL] = (float)pc.channel;
            vals[P_PART_POLY] = (float)pc.polyLimitVoices;
            vals[P_PART_MUTE] = pc.mute ? 1.f : 0.f;
            vals[P_PART_SOLO] = pc.solo ? 1.f : 0.f;
            const auto &ss = patch->busses.partBusses[0].busSendStorage;
            for (int i = 0; i < 4; ++i)
                vals[P_SEND1 + i] = ss.sendLevels[i];
            using FS = sst::basic_blocks::params::ParamMetaData::FeatureState;
            PartConfig c = pc;
            auto txt = [&](float &v) {
                return clean(scxt::datamodel::describeValue(c, v).valueToString(v, FS()).value_or(num(v)));
            };
            partText = txt(c.level) + "~" + txt(c.pan) + "~" + txt(c.tuning);
        }
        if (n == 0)
        {
            modsText = "P|" + partText;
            return true;
        }
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
        vals[P_PROC_ROUTING] = (float)o.procRouting;
        vals[P_ROUTE_TO] = (float)o.routeTo;
        vals[P_OVERSAMPLE] = (float)o.oversample;
        vals[P_EG_INDEX] = (float)egIndex;
        vals[P_LFO_INDEX] = (float)lfoIndex;
        AdsrStorage egNone{};
        const AdsrStorage *egp = shownEg();
        const auto &eg = egp ? *egp : egNone;
        vals[P_EG_ASH] = eg.aShape;
        vals[P_EG_DSH] = eg.dShape;
        vals[P_EG_RSH] = eg.rShape;
        vals[P_EG_SYNC] = eg.isTemposync ? 1.f : 0.f;
        vals[P_EG_GATE] = eg.gateMode == AdsrStorage::GateMode::ONESHOT      ? 2.f
                          : eg.gateMode == AdsrStorage::GateMode::SEMI_GATED ? 1.f
                                                                             : 0.f;
        vals[P_EG_GATE_ANY] = eg.gateGroupEGOnAnyPlaying ? 1.f : 0.f;
        const auto &ms = g->modulatorStorage[lfoIndex];
        vals[P_LFO_SHAPE] = (float)ms.modulatorShape;
        vals[P_LFO_TRIGGER] = (float)ms.triggerMode;
        vals[P_LFO_RATE] = ms.rate;
        vals[P_LFO_PHASE] = ms.start_phase;
        vals[P_LFO_SYNC] = ms.temposync ? 1.f : 0.f;
        vals[P_LFO_DEFORM] = ms.curveLfoStorage.deform;
        vals[P_LFO_ANGLE] = ms.curveLfoStorage.angle;
        vals[P_LFO_UNI] = ms.curveLfoStorage.unipolar ? 1.f : 0.f;
        vals[P_LFO_USEENV] = ms.curveLfoStorage.useenv ? 1.f : 0.f;
        vals[P_LFO_DLY] = ms.curveLfoStorage.delay;
        vals[P_LFO_ATK] = ms.curveLfoStorage.attack;
        vals[P_LFO_REL] = ms.curveLfoStorage.release;
        const auto &el = ms.envLfoStorage;
        vals[P_LFOE_DLY] = el.delay;
        vals[P_LFOE_A] = el.attack;
        vals[P_LFOE_H] = el.hold;
        vals[P_LFOE_D] = el.decay;
        vals[P_LFOE_S] = el.sustain;
        vals[P_LFOE_R] = el.release;
        vals[P_LFOE_LOOP] = el.loop ? 1.f : 0.f;
        // Shortcircuit's own value texts (tempo synced times show as note lengths)
        {
            using FS = sst::basic_blocks::params::ParamMetaData::FeatureState;
            auto txt = [](const scxt::datamodel::pmd &m, float v, bool sync) {
                return clean(m.valueToString(v, FS().withTemposync(sync)).value_or(num(v)));
            };
            AdsrStorage e = eg;
            std::string t = "E|";
            const float *ev[6] = {&e.dly, &e.a, &e.h, &e.d, &e.s, &e.r};
            for (int i = 0; i < 6; ++i)
                t += (i ? "~" : "") + txt(scxt::datamodel::describeValue(e, *ev[i]), *ev[i], e.isTemposync && i != 4);
            ModStorage m = ms;
            t += "|L|";
            const float *lv[7] = {&m.rate, &m.start_phase, &m.curveLfoStorage.deform, &m.curveLfoStorage.angle,
                                  &m.curveLfoStorage.delay, &m.curveLfoStorage.attack, &m.curveLfoStorage.release};
            for (int i = 0; i < 7; ++i)
                t += (i ? "~" : "") + txt(scxt::datamodel::describeValue(m, *lv[i]), *lv[i], m.temposync && (i == 0 || i >= 4));
            t += "|V|";
            const float *vv[6] = {&m.envLfoStorage.delay, &m.envLfoStorage.attack, &m.envLfoStorage.hold,
                                  &m.envLfoStorage.decay, &m.envLfoStorage.sustain, &m.envLfoStorage.release};
            for (int i = 0; i < 6; ++i)
                t += (i ? "~" : "") + txt(scxt::datamodel::describeValue(m, *vv[i]), *vv[i], m.temposync && i != 4);
            t += "|P|" + partText;
            modsText = t;
        }
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
        if (auto &patch = engine->getPatch())
            if (auto &part = patch->getPart(0))
                if (group < (int)part->getGroups().size())
                    if (auto &g = part->getGroup(group))
                    {
                        readFx(*g, all);
                        auto mt = miscText(*g);
                        if (all || mt != sentMiscText)
                        {
                            sentMiscText = mt;
                            queueString(P_MISC_TEXT, mt);
                        }
                    }
        if (auto *pt = part0())
            readPfx(*pt, all);
        checkSave();
        // names: ask the audio thread after loads / structure changes, send when they changed
        if (namesReady.load())
        {
            std::string n = namesSnap;
            std::string mn = macroSnap;
            namesReady = false;
            if (all || mn != sentMacroNames)
            {
                sentMacroNames = mn;
                queueString(P_MACRO_NAMES, mn);
            }
            if (all || n != sentNames)
            {
                sentNames = n;
                sendChunked(P_NAMES, "N", n);
            }
        }
        else if ((all || vals[P_LOADED] != namesForLoaded || ++namesTick % 5 == 0) && !namesRequest.load())
        {
            namesForLoaded = vals[P_LOADED];
            if (all)
                sentNames.clear();
            namesRequest = true;
        }
        if (!modsText.empty() && (all || modsText != sentModsText))
        {
            sentModsText = modsText;
            queueString(P_MODS_TEXT, modsText);
        }
        for (uint32_t i = 0; i < P_FIRST_STRING; ++i)
        {
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
                if (q != P_GROUP)
                {
                    sent[q] = pending[q];
                    sentValid[q] = true;
                }
            }
            std::string text;
            while (strIn.pop(p, text))
                applyString(p, text);
            if (wantStrings.exchange(false))
            {
                sendFolders();
                sendListing(curDir);
                sendFxTypes();
                sendPfxTypes();
                descValid = false; // and every slot again
                pfxValid = false;
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
    impl->initDirs();
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

void Bridge::requestAll()
{
    impl->wantAll = true;
    impl->wantStrings = true;
}

void Bridge::postString(uint32_t param, const char *text, uint32_t len)
{
    impl->strIn.push(param, text, len);
    impl->wake.notify_one();
}

bool Bridge::popOutString(uint32_t &param, char *buf, uint32_t cap, uint32_t &len)
{
    std::unique_lock<std::mutex> lk(impl->strOutMutex, std::try_to_lock);
    if (!lk.owns_lock() || impl->strOut.empty())
        return false;
    auto &f = impl->strOut.front();
    param = f.first;
    len = (uint32_t)std::min<size_t>(f.second.size(), cap);
    memcpy(buf, f.second.data(), len);
    impl->strOut.pop_front();
    return true;
}

bool Bridge::popOut(uint32_t &param, float &value) { return impl->out.pop(param, value); }

void Bridge::audioTick()
{
    if (impl->namesRequest.load(std::memory_order_acquire) && !impl->namesReady.load(std::memory_order_acquire))
    {
        // (rare: after loads and structure changes)
        std::string t;
        auto &patch = impl->engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        if (part)
        {
            bool firstG = true;
            for (auto &g : part->getGroups())
            {
                t += (firstG ? "" : "~") + Impl::clean(g->getName()) + "^" + std::to_string(g->getZones().size()) +
                     "^" + (g->outputInfo.muted ? "m" : "") + (g->outputInfo.soloed ? "s" : "");
                firstG = false;
                int k = 0;
                for (auto &z : g->getZones())
                {
                    if (++k > 24)
                        break; // enough to recognise it
                    t += "^" + Impl::clean(z->getName());
                }
            }
        }
        impl->namesSnap = t;
        std::string mn;
        if (part)
            for (int i = 0; i < (int)scxt::macrosPerPart; ++i)
                mn += (i ? "~" : "") + Impl::clean(part->macros[i].name);
        impl->macroSnap = mn;
        impl->namesRequest.store(false, std::memory_order_release);
        impl->namesReady.store(true, std::memory_order_release);
    }
    if (impl->pfxRequest.load(std::memory_order_acquire) && !impl->pfxReady.load(std::memory_order_acquire))
    {
        auto &patch = impl->engine->getPatch();
        auto *part = patch ? patch->getPart(0).get() : nullptr;
        for (int s = 0; s < (int)scxt::maxEffectsPerPart; ++s)
        {
            auto &d = impl->pfxSnap[s];
            d = {};
            if (!part)
                continue;
            d.type = (int)part->partEffectStorage[s].type;
            if (auto &fx = part->partEffects[s])
            {
                d.n = std::min(fx->numParams(), (int)scxt::maxBusEffectParams);
                for (int i = 0; i < d.n; ++i)
                    d.m[i] = fx->paramAt(i);
            }
        }
        impl->pfxRequest.store(false, std::memory_order_release);
        impl->pfxReady.store(true, std::memory_order_release);
    }
    auto g = impl->descRequest.load(std::memory_order_acquire);
    if (g < 0 || impl->descReady.load(std::memory_order_acquire))
        return;
    auto &patch = impl->engine->getPatch();
    auto *part = patch ? patch->getPart(0).get() : nullptr;
    for (int s = 0; s < numFx; ++s)
        impl->descSnap[s] = {};
    impl->metaSnap = {};
    if (part && g < (int)part->getGroups().size())
    {
        // (rare: only when the group or an effect type changes)
        auto &grp = part->getGroup(g);
        for (int s = 0; s < numFx; ++s)
            impl->descSnap[s] = grp->processorDescription[s];
        impl->metaSnap = scxt::modulation::getGroupMatrixMetadata(*grp);
    }
    impl->descRequest.store(-1, std::memory_order_release);
    impl->descReady.store(true, std::memory_order_release);
}

void Bridge::stateChanged()
{
    impl->structureChanged = true;
    impl->holdReadback = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

} // namespace scxt_lv2
