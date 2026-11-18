/*
 * clap2lv2_ttl.cpp - writes manifest.ttl and the plugin .ttl for an LV2 bundle
 * that wraps a CLAP plugin with clap2lv2.
 *
 * Part of the maarthome/shortcircuit-xt "my_own" branch LV2 wrapper.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 *
 * Usage: clap2lv2-ttl <file.clap> <bundle-dir> <binary.so> [max-params]
 *
 * The plugin is loaded once to read its name and parameter list; port order
 * must match clap2lv2.cpp (events in, out L, out R, then one control per param).
 */

#include "clap_host.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <file.clap> <bundle-dir> <binary.so> [max-params]\n", argv[0]);
        return 1;
    }
    std::string clapPath = argv[1], bundle = argv[2], binary = argv[3];
    long maxParams = argc > 4 ? atol(argv[4]) : -1;
    if (!bundle.empty() && bundle.back() != '/')
        bundle += '/';

    auto *mod = clap2lv2::ClapModule::get(clapPath);
    if (!mod)
        return 2;

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
              << "        lv2:name \"" << esc(info.name) << "\" ;\n"
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
           "@prefix urid: <http://lv2plug.in/ns/ext/urid#> .\n\n"
        << groups.str() << "<" << CLAP2LV2_URI << ">\n"
        << "    a lv2:Plugin , lv2:InstrumentPlugin ;\n"
        << "    doap:name \"" << esc(name) << "\" ;\n"
        << "    doap:license <https://www.gnu.org/licenses/gpl-3.0> ;\n"
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
