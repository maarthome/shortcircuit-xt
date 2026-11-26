/*
 * clap2lv2_ttl.cpp (scxt-lv2-ttl) - writes manifest.ttl and the plugin .ttl
 * of the Shortcircuit XT LV2 bundle.
 *
 * Part of the maarthome/shortcircuit-xt "my_own" branch LV2 wrapper.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 *
 * Usage: scxt-lv2-ttl [--state FILE] [--dump-state FILE] [--short-names]
 *                     <scxt_lv2.so> <bundle-dir> <binary-name> [max-params]
 *
 *   <scxt_lv2.so>      the built LV2 binary; the plugin is read from it
 *
 *   --state FILE       load this plugin state first, so the .ttl gets the
 *                      knob names from it (e.g. your own macro names)
 *   --dump-state FILE  write the plugin's (default or loaded) state to FILE
 *   --short-names      use only the part after " - " in names that have one
 *                      ("P1 M1 - Cutoff" becomes "Cutoff")
 *
 * Names of part 1 macros lose their "P1 " prefix when only part 1 is exposed.
 * MOD metadata (mod:brand, mod:label, version) is written for MOD / pi-Stomp.
 *
 * The plugin is loaded once to read its name and parameter list; port order
 * must match clap2lv2.cpp (events in, out L, out R, then one control per param).
 */

#include "clap_host.h"

#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>
#include <vector>

#ifndef CLAP2LV2_URI
#define CLAP2LV2_URI "https://github.com/maarthome/shortcircuit-xt#lv2"
#endif

static std::string esc(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            r += '\\';
        if (c == '\n')
        {
            r += "\\n";
            continue;
        }
        r += c;
    }
    return r;
}

static std::string symbolOf(const std::string &s)
{
    std::string r;
    for (char c : s)
        r += (isalnum((unsigned char)c) ? (char)tolower((unsigned char)c) : '_');
    if (r.empty() || isdigit((unsigned char)r[0]))
        r = "g_" + r;
    return r;
}

static std::string displayName(const std::string &name, bool shortNames)
{
    if (!shortNames)
        return name;
    auto pos = name.find(" - ");
    if (pos == std::string::npos || pos + 3 >= name.size())
        return name;
    return name.substr(pos + 3);
}

// "P1 Macro 3" -> "Macro 3" when only part 1 is exposed
static std::string stripPart1(const std::string &name, bool onlyPart1)
{
    if (onlyPart1 && name.rfind("P1 ", 0) == 0)
        return name.substr(3);
    return name;
}

static bool readFile(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

int main(int argc, char **argv)
{
    std::string statePath, dumpPath;
    bool shortNames = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--state" && i + 1 < argc)
            statePath = argv[++i];
        else if (a == "--dump-state" && i + 1 < argc)
            dumpPath = argv[++i];
        else if (a == "--short-names")
            shortNames = true;
        else
            pos.push_back(a);
    }
    if (pos.size() < 3)
    {
        fprintf(stderr,
                "usage: %s [--state FILE] [--dump-state FILE] [--short-names] <scxt_lv2.so> <bundle-dir> "
                "<binary-name> [max-params]\n",
                argv[0]);
        return 1;
    }
    std::string soPath = pos[0], bundle = pos[1], binary = pos[2];
    long maxParams = pos.size() > 3 ? atol(pos[3].c_str()) : -1;
    if (!bundle.empty() && bundle.back() != '/')
        bundle += '/';

    void *lib = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        fprintf(stderr, "dlopen(%s) failed: %s\n", soPath.c_str(), dlerror());
        return 2;
    }
    using EntryFn = const clap_plugin_entry_t *(*)();
    auto entryFn = (EntryFn)dlsym(lib, "scxt_lv2_plugin_entry");
    auto *mod = entryFn ? clap2lv2::ClapModule::fromEntry(entryFn(), clap2lv2::dirOf(soPath)) : nullptr;
    if (!mod)
    {
        fprintf(stderr, "%s has no Shortcircuit XT plugin entry\n", soPath.c_str());
        return 2;
    }

    clap2lv2::Host host;
    auto id = mod->firstPluginId();
    auto *desc = mod->factory->get_plugin_descriptor(mod->factory, 0);
    auto *plug = mod->factory->create_plugin(mod->factory, &host.clapHost, id.c_str());
    if (!plug || !plug->init(plug))
    {
        fprintf(stderr, "could not create/init %s\n", id.c_str());
        return 3;
    }
    host.attach(plug);

    auto *stateExt = (const clap_plugin_state_t *)plug->get_extension(plug, CLAP_EXT_STATE);
    auto pump = [&](int ms) {
        // let the plugin finish asynchronous work (Shortcircuit XT loads
        // state on its own thread and then asks for a main-thread callback)
        for (int t = 0; t < ms; t += 20)
        {
            host.idle();
            usleep(20000);
        }
    };

    if (!statePath.empty())
    {
        std::vector<uint8_t> blob;
        if (!stateExt || !readFile(statePath, blob))
        {
            fprintf(stderr, "could not read state file %s\n", statePath.c_str());
            return 4;
        }
        struct Reader
        {
            const std::vector<uint8_t> *d;
            size_t pos;
        } rd{&blob, 0};
        clap_istream_t is{&rd, [](const clap_istream_t *s, void *buf, uint64_t sz) -> int64_t {
                              auto *r = (Reader *)s->ctx;
                              uint64_t left = r->d->size() - r->pos;
                              if (sz > left)
                                  sz = left;
                              memcpy(buf, r->d->data() + r->pos, sz);
                              r->pos += sz;
                              return (int64_t)sz;
                          }};
        if (!stateExt->load(plug, &is))
        {
            fprintf(stderr, "plugin refused state file %s\n", statePath.c_str());
            return 5;
        }
        pump(2000);
        printf("loaded state %s\n", statePath.c_str());
    }
    else
        pump(200);

    if (!dumpPath.empty() && stateExt)
    {
        std::vector<uint8_t> blob;
        clap_ostream_t os{&blob, [](const clap_ostream_t *s, const void *buf, uint64_t sz) {
                              auto *v = (std::vector<uint8_t> *)s->ctx;
                              v->insert(v->end(), (const uint8_t *)buf, (const uint8_t *)buf + sz);
                              return (int64_t)sz;
                          }};
        if (stateExt->save(plug, &os))
        {
            std::ofstream(dumpPath, std::ios::binary)
                .write((const char *)blob.data(), (std::streamsize)blob.size());
            printf("wrote state to %s (%zu bytes)\n", dumpPath.c_str(), blob.size());
        }
    }

    auto *params = (const clap_plugin_params_t *)plug->get_extension(plug, CLAP_EXT_PARAMS);
    auto *gui = (const clap_plugin_gui_t *)plug->get_extension(plug, CLAP_EXT_GUI);
    bool hasUi = gui && gui->is_api_supported(plug, CLAP_WINDOW_API_X11, false);

    uint32_t np = params ? params->count(plug) : 0;
    if (maxParams >= 0 && (uint32_t)maxParams < np)
        np = (uint32_t)maxParams;

    std::ostringstream ports, groups;
    std::map<std::string, std::string> groupSyms;

    ports << "    lv2:port [\n"
             "        a lv2:InputPort , atom:AtomPort ;\n"
             "        atom:bufferType atom:Sequence ;\n"
             "        atom:supports midi:MidiEvent , time:Position ;\n"
             "        lv2:designation lv2:control ;\n"
             "        lv2:index 0 ;\n"
             "        lv2:symbol \"events_in\" ;\n"
             "        lv2:name \"MIDI In\"\n"
             "    ] , [\n"
             "        a lv2:OutputPort , lv2:AudioPort ;\n"
             "        lv2:index 1 ;\n"
             "        lv2:symbol \"out_l\" ;\n"
             "        lv2:name \"Out L\"\n"
             "    ] , [\n"
             "        a lv2:OutputPort , lv2:AudioPort ;\n"
             "        lv2:index 2 ;\n"
             "        lv2:symbol \"out_r\" ;\n"
             "        lv2:name \"Out R\"\n"
             "    ]";

    for (uint32_t i = 0; i < np; ++i)
    {
        clap_param_info_t info{};
        if (!params->get_info(plug, i, &info))
            continue;
        double val = info.default_value;
        params->get_value(plug, info.id, &val);

        std::string module = info.module;
        std::string gsym;
        if (!module.empty())
        {
            auto it = groupSyms.find(module);
            if (it == groupSyms.end())
            {
                gsym = symbolOf(module);
                groupSyms[module] = gsym;
                groups << "<" << CLAP2LV2_URI << "-" << gsym << ">\n"
                       << "    a pg:Group ;\n"
                       << "    lv2:symbol \"" << gsym << "\" ;\n"
                       << "    lv2:name \"" << esc(module) << "\" .\n\n";
            }
            else
                gsym = it->second;
        }

        bool toggled = (info.flags & CLAP_PARAM_IS_STEPPED) && info.min_value == 0 &&
                       info.max_value == 1;
        bool stepped = (info.flags & CLAP_PARAM_IS_STEPPED) && !toggled;

        ports << " , [\n"
              << "        a lv2:InputPort , lv2:ControlPort ;\n"
              << "        lv2:index " << (3 + i) << " ;\n"
              << "        lv2:symbol \"p" << i << "\" ;\n"
              << "        lv2:name \"" << esc(stripPart1(displayName(info.name, shortNames), np <= 16)) << "\" ;\n"
              << "        lv2:default " << info.default_value << " ;\n"
              << "        lv2:minimum " << info.min_value << " ;\n"
              << "        lv2:maximum " << info.max_value;
        if (toggled)
            ports << " ;\n        lv2:portProperty lv2:toggled";
        else if (stepped)
            ports << " ;\n        lv2:portProperty lv2:integer";
        if (!(info.flags & CLAP_PARAM_IS_AUTOMATABLE))
            ports << " ;\n        lv2:portProperty <http://lv2plug.in/ns/ext/port-props#notAutomatic>";
        if (!gsym.empty())
            ports << " ;\n        pg:group <" << CLAP2LV2_URI << "-" << gsym << ">";
        ports << "\n    ]";
    }

    std::string name = desc && desc->name ? desc->name : "CLAP plugin";
    // LV2 versions: minor/micro from the plugin's "major.minor.micro" version
    int majorV = 0, minorV = 0, microV = 0;
    if (desc && desc->version)
        sscanf(desc->version, "%d.%d.%d", &majorV, &minorV, &microV);
    // LV2 rule: an odd minorVersion, or 0, means "unstable"; keep it even and > 0
    if (minorV % 2)
        minorV += 1;
    if (minorV == 0)
        minorV = 2;
    std::string vendor = desc && desc->vendor ? desc->vendor : "";
    std::string ttlName = symbolOf(name) + ".ttl";

    std::ofstream man(bundle + "manifest.ttl");
    man << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
           "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
           "@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .\n\n"
        << "<" << CLAP2LV2_URI << ">\n"
        << "    a lv2:Plugin , lv2:InstrumentPlugin ;\n"
        << "    lv2:binary <" << binary << "> ;\n"
        << "    rdfs:seeAlso <" << ttlName << "> .\n";
    if (hasUi)
        man << "\n<" << CLAP2LV2_URI << "-ui>\n"
            << "    a ui:X11UI ;\n"
            << "    ui:binary <" << binary << "> ;\n"
            << "    rdfs:seeAlso <" << ttlName << "> .\n";
    man.close();

    std::ofstream ttl(bundle + ttlName);
    ttl << "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n"
           "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
           "@prefix foaf: <http://xmlns.com/foaf/0.1/> .\n"
           "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
           "@prefix midi: <http://lv2plug.in/ns/ext/midi#> .\n"
           "@prefix opts: <http://lv2plug.in/ns/ext/options#> .\n"
           "@prefix bufsz: <http://lv2plug.in/ns/ext/buf-size#> .\n"
           "@prefix pg:   <http://lv2plug.in/ns/ext/port-groups#> .\n"
           "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
           "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
           "@prefix time: <http://lv2plug.in/ns/ext/time#> .\n"
           "@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .\n"
           "@prefix mod:  <http://moddevices.com/ns/mod#> .\n"
           "@prefix urid: <http://lv2plug.in/ns/ext/urid#> .\n\n"
        << groups.str() << "<" << CLAP2LV2_URI << ">\n"
        << "    a lv2:Plugin , lv2:InstrumentPlugin ;\n"
        << "    doap:name \"" << esc(name) << "\" ;\n"
        << "    doap:license <https://www.gnu.org/licenses/gpl-3.0> ;\n"
        << "    lv2:minorVersion " << minorV << " ;\n"
        << "    lv2:microVersion " << microV << " ;\n"
        << "    mod:brand \"Surge Synth Team\" ;\n"
        << "    mod:label \"Shortcircuit XT\" ;\n"
        << "    rdfs:comment \"Shortcircuit XT sampler, built as LV2 from source (maarthome/shortcircuit-xt my_own)\" ;\n"
        << "    doap:maintainer [ foaf:name \"" << esc(vendor) << " (LV2 wrapper: maarthome)\" ] ;\n"
        << "    lv2:requiredFeature urid:map ;\n"
        << "    lv2:optionalFeature lv2:hardRTCapable , opts:options , bufsz:boundedBlockLength ;\n"
        << "    lv2:extensionData state:interface ;\n";
    if (hasUi)
        ttl << "    ui:ui <" << CLAP2LV2_URI << "-ui> ;\n";
    ttl << ports.str() << " .\n";
    if (hasUi)
        ttl << "\n<" << CLAP2LV2_URI << "-ui>\n"
            << "    a ui:X11UI ;\n"
            << "    lv2:requiredFeature ui:parent , ui:idleInterface ,\n"
            << "        <http://lv2plug.in/ns/ext/instance-access> ;\n"
            << "    lv2:optionalFeature ui:resize ;\n"
            << "    lv2:extensionData ui:idleInterface .\n";
    ttl.close();

    printf("wrote %smanifest.ttl and %s%s: %u parameters, UI %s\n", bundle.c_str(),
           bundle.c_str(), ttlName.c_str(), np, hasUi ? "yes" : "no");

    plug->destroy(plug);
    // like the LV2 side, the module is not deinitialised; exit directly so
    // plugin-side static destructors (JUCE) don't run in an odd order
    fflush(stdout);
    _exit(0);
}
