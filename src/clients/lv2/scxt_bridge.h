/*
 * scxt_bridge.h - lets the MOD panel edit Shortcircuit XT's engine.
 *
 * Part of the maarthome/shortcircuit-xt "my_own" branch.
 * Licensed under the MIT license, like the rest of src/ in this repository.
 *
 * The panel sets LV2 patch parameters (scxt_params.h). run() hands them to
 * the bridge without blocking; the bridge's own thread turns them into the
 * same engine messages Shortcircuit XT's editor sends (select group, group
 * output field edits, EG edits, clear part, add sample) and reports the
 * selected group's values back, which run() sends to the panel.
 */
#pragma once

#include "scxt_params.h"

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <string>

namespace scxt_lv2
{

class Bridge
{
  public:
    // plugin: the Shortcircuit XT CLAP plugin instance linked into this binary
    // bundlePath: the LV2 bundle (instruments.txt lives there)
    Bridge(const clap_plugin_t *plugin, const std::string &bundlePath);
    ~Bridge();

    // audio thread: a parameter value from the panel (never blocks)
    void post(uint32_t param, float value);
    // audio thread: the panel asked for values (patch:Get); all are re-sent
    void requestAll();
    // audio thread: next value to send to the panel; false when none
    bool popOut(uint32_t &param, float &value);

    // engine state was replaced (LV2 state restore): re-read everything
    void stateChanged();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace scxt_lv2
