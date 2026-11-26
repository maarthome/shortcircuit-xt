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
#include "browser/browser.h"

#include <algorithm>
#include <array>
#include <cctype>
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
    static constexpr size_t N = 16, LEN = 2048;
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
            group = n;
        }
        fprintf(stderr, "[scxt-lv2] %s %s\n", loadMode == 1 ? "replace part 1 with" : "add to part 1:",
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
            group = std::max(0, iv);
            selectGroup();
            wantAll = true;
            return;
        case P_LOAD_MODE:
            loadMode = iv;
            return;
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

void Bridge::stateChanged()
{
    impl->structureChanged = true;
    impl->holdReadback = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

} // namespace scxt_lv2
