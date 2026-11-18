/*
 * clap_host.h - minimal CLAP host used to run a CLAP plugin inside an LV2 shell.
 *
 * Part of the maarthome/shortcircuit-xt "my_own" branch LV2 wrapper.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 *
 * The host implements just enough of the CLAP host side for Shortcircuit XT:
 * log, thread-check, params, state, gui, timer-support, posix-fd-support,
 * audio-ports, note-ports and latency. Timers, fds and main-thread callbacks
 * are serviced from idle(), which the LV2 UI idle interface calls.
 */
#pragma once

#include <clap/clap.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace clap2lv2
{

// ---------------------------------------------------------------------------
// Loading the .clap module. A module is loaded and initialised once per
// process and never unloaded: plugin libraries (JUCE in particular) do not
// like being deinitialised and reloaded in the same process.
// ---------------------------------------------------------------------------
struct ClapModule
{
    void *lib{nullptr};
    const clap_plugin_entry_t *entry{nullptr};
    const clap_plugin_factory_t *factory{nullptr};
    std::string path;

    static ClapModule *get(const std::string &path)
    {
        static std::mutex m;
        static std::vector<ClapModule *> loaded;
        std::lock_guard<std::mutex> g(m);
        for (auto *mod : loaded)
            if (mod->path == path)
                return mod;

        auto *mod = new ClapModule();
        mod->path = path;
        mod->lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!mod->lib)
        {
            fprintf(stderr, "[clap2lv2] dlopen(%s) failed: %s\n", path.c_str(), dlerror());
            delete mod;
            return nullptr;
        }
        mod->entry = (const clap_plugin_entry_t *)dlsym(mod->lib, "clap_entry");
        if (!mod->entry || !mod->entry->init(path.c_str()))
        {
            fprintf(stderr, "[clap2lv2] clap_entry missing or init failed in %s\n", path.c_str());
            delete mod;
            return nullptr;
        }
        mod->factory = (const clap_plugin_factory_t *)mod->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        if (!mod->factory || mod->factory->get_plugin_count(mod->factory) == 0)
        {
            fprintf(stderr, "[clap2lv2] no plugin factory in %s\n", path.c_str());
            delete mod;
            return nullptr;
        }
        loaded.push_back(mod);
        return mod;
    }

    // The CLAP id of the first plugin in the module (Shortcircuit XT has one)
    std::string firstPluginId() const
    {
        auto *d = factory->get_plugin_descriptor(factory, 0);
        return d && d->id ? d->id : "";
    }
};

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------
struct Host
{
    clap_host_t clapHost{};
    const clap_plugin_t *plugin{nullptr};

    std::thread::id mainThread{std::this_thread::get_id()};
    std::atomic<std::thread::id> audioThread{};

    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> paramsFlushRequested{false};
    std::atomic<bool> restartRequested{false};

    // gui callbacks, set by the LV2 UI while it is open
    std::function<bool(uint32_t, uint32_t)> onGuiResize;
    std::function<void()> onGuiClosed;

    struct Timer
    {
        clap_id id;
        uint32_t periodMs;
        std::chrono::steady_clock::time_point next;
    };
    std::vector<Timer> timers;
    clap_id nextTimerId{1};

    struct Fd
    {
        int fd;
        clap_posix_fd_flags_t flags;
    };
    std::vector<Fd> fds;

    // plugin extensions, filled by attach()
    const clap_plugin_timer_support_t *pluginTimer{nullptr};
    const clap_plugin_posix_fd_support_t *pluginFd{nullptr};

    Host()
    {
        clapHost.clap_version = CLAP_VERSION;
        clapHost.host_data = this;
        clapHost.name = "clap2lv2 (Shortcircuit XT LV2)";
        clapHost.vendor = "maarthome";
        clapHost.url = "https://github.com/maarthome/shortcircuit-xt";
        clapHost.version = "1.0.0";
        clapHost.get_extension = &Host::getExtension;
        clapHost.request_restart = [](const clap_host_t *h) { self(h)->restartRequested = true; };
        clapHost.request_process = [](const clap_host_t *) {};
        clapHost.request_callback = [](const clap_host_t *h) { self(h)->callbackRequested = true; };
    }

    static Host *self(const clap_host_t *h) { return (Host *)h->host_data; }

    void attach(const clap_plugin_t *p)
    {
        plugin = p;
        pluginTimer = (const clap_plugin_timer_support_t *)p->get_extension(
            p, CLAP_EXT_TIMER_SUPPORT);
        pluginFd = (const clap_plugin_posix_fd_support_t *)p->get_extension(
            p, CLAP_EXT_POSIX_FD_SUPPORT);
    }

    bool isMainThread() const { return std::this_thread::get_id() == mainThread; }
    bool isAudioThread() const { return std::this_thread::get_id() == audioThread.load(); }

    // Run everything the plugin asked to happen on the main thread.
    // Called from the LV2 UI idle callback (main thread of the host).
    void idle()
    {
        if (!plugin)
            return;

        if (callbackRequested.exchange(false))
            plugin->on_main_thread(plugin);

        if (pluginFd && !fds.empty())
        {
            std::vector<pollfd> pfds;
            for (auto &f : fds)
            {
                short ev = 0;
                if (f.flags & CLAP_POSIX_FD_READ)
                    ev |= POLLIN;
                if (f.flags & CLAP_POSIX_FD_WRITE)
                    ev |= POLLOUT;
                pfds.push_back({f.fd, ev, 0});
            }
            if (poll(pfds.data(), pfds.size(), 0) > 0)
            {
                for (auto &p : pfds)
                {
                    clap_posix_fd_flags_t fl = 0;
                    if (p.revents & POLLIN)
                        fl |= CLAP_POSIX_FD_READ;
                    if (p.revents & POLLOUT)
                        fl |= CLAP_POSIX_FD_WRITE;
                    if (p.revents & (POLLERR | POLLHUP))
                        fl |= CLAP_POSIX_FD_ERROR;
                    if (fl)
                        pluginFd->on_fd(plugin, p.fd, fl);
                }
            }
        }

        if (pluginTimer && !timers.empty())
        {
            auto now = std::chrono::steady_clock::now();
            // copy: a timer callback may register or unregister timers
            auto due = timers;
            for (auto &t : due)
            {
                if (t.next <= now)
                {
                    for (auto &orig : timers)
                        if (orig.id == t.id)
                            orig.next = now + std::chrono::milliseconds(t.periodMs);
                    pluginTimer->on_timer(plugin, t.id);
                }
            }
        }
    }

    // --- host extensions -------------------------------------------------
    static const void *getExtension(const clap_host_t *h, const char *id)
    {
        static const clap_host_log_t log{[](const clap_host_t *, clap_log_severity s,
                                            const char *msg) {
            if (s >= CLAP_LOG_WARNING || getenv("CLAP2LV2_DEBUG"))
                fprintf(stderr, "[clap2lv2 plugin log %d] %s\n", (int)s, msg ? msg : "");
        }};
        static const clap_host_thread_check_t threadCheck{
            [](const clap_host_t *h) { return self(h)->isMainThread(); },
            [](const clap_host_t *h) { return self(h)->isAudioThread(); }};
        static const clap_host_params_t params{
            [](const clap_host_t *, clap_param_rescan_flags) {},
            [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
            [](const clap_host_t *h) { self(h)->paramsFlushRequested = true; }};
        static const clap_host_state_t state{[](const clap_host_t *) {}};
        static const clap_host_latency_t latency{[](const clap_host_t *) {}};
        static const clap_host_audio_ports_t audioPorts{
            [](const clap_host_t *, uint32_t) { return false; },
            [](const clap_host_t *, uint32_t) {}};
        static const clap_host_note_ports_t notePorts{
            [](const clap_host_t *) -> uint32_t { return CLAP_NOTE_DIALECT_MIDI; },
            [](const clap_host_t *, uint32_t) {}};
        static const clap_host_gui_t gui{
            [](const clap_host_t *) {},
            [](const clap_host_t *h, uint32_t w, uint32_t hh) {
                auto *s = self(h);
                return s->onGuiResize ? s->onGuiResize(w, hh) : false;
            },
            [](const clap_host_t *) { return false; },
            [](const clap_host_t *) { return false; },
            [](const clap_host_t *h, bool) {
                auto *s = self(h);
                if (s->onGuiClosed)
                    s->onGuiClosed();
            }};
        static const clap_host_timer_support_t timer{
            [](const clap_host_t *h, uint32_t ms, clap_id *tid) {
                auto *s = self(h);
                if (ms < 1)
                    ms = 1;
                Timer t{s->nextTimerId++, ms,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)};
                s->timers.push_back(t);
                *tid = t.id;
                return true;
            },
            [](const clap_host_t *h, clap_id tid) {
                auto &v = self(h)->timers;
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (it->id == tid)
                    {
                        v.erase(it);
                        return true;
                    }
                return false;
            }};
        static const clap_host_posix_fd_support_t posixFd{
            [](const clap_host_t *h, int fd, clap_posix_fd_flags_t fl) {
                self(h)->fds.push_back({fd, fl});
                return true;
            },
            [](const clap_host_t *h, int fd, clap_posix_fd_flags_t fl) {
                for (auto &f : self(h)->fds)
                    if (f.fd == fd)
                    {
                        f.flags = fl;
                        return true;
                    }
                return false;
            },
            [](const clap_host_t *h, int fd) {
                auto &v = self(h)->fds;
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (it->fd == fd)
                    {
                        v.erase(it);
                        return true;
                    }
                return false;
            }};

        if (!strcmp(id, CLAP_EXT_LOG))
            return &log;
        if (!strcmp(id, CLAP_EXT_THREAD_CHECK))
            return &threadCheck;
        if (!strcmp(id, CLAP_EXT_PARAMS))
            return &params;
        if (!strcmp(id, CLAP_EXT_STATE))
            return &state;
        if (!strcmp(id, CLAP_EXT_LATENCY))
            return &latency;
        if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &audioPorts;
        if (!strcmp(id, CLAP_EXT_NOTE_PORTS))
            return &notePorts;
        if (!strcmp(id, CLAP_EXT_GUI))
            return &gui;
        if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
            return &timer;
        if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
            return &posixFd;
        return nullptr;
    }
};

// Directory part of a path, with trailing slash
inline std::string dirOf(const std::string &p)
{
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? std::string("./") : p.substr(0, pos + 1);
}

} // namespace clap2lv2
