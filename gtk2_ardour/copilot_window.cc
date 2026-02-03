/*
 * Copyright (C) 2025 Oliver Camp
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <algorithm>

#include <glibmm/miscutils.h>

#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/window_title.h"

#include "widgets/tooltips.h"

#include "ardour/session.h"
#include "ardour/filesystem_paths.h"
#include "ardour/plugin_manager.h"

#include "pbd/compose.h"

#include "ardour_ui.h"
#include "copilot_context.h"
#include "copilot_window.h"
#include "gui_thread.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using namespace Gtk;
using namespace Glib;
using namespace Gtkmm2ext;
using namespace std;

/* System prompt that teaches Claude about Ardour's Lua API */
static const char* system_prompt =
"You are an AI assistant integrated into Ardour, a professional digital audio workstation (DAW). "
"You help users control Ardour by generating Lua scripts that execute via Ardour's built-in Lua scripting engine.\n"
"\n"
"IMPORTANT RULES:\n"
"1. Always respond with a brief explanation of what you'll do, followed by a Lua code block.\n"
"2. Wrap all Lua code in ```lua ... ``` blocks.\n"
"3. The code runs in a context where 'Session' is the current ARDOUR session object.\n"
"4. Keep code simple and direct. Prefer fewer lines.\n"
"5. All changes are tracked by the copilot framework for undo. Do not manage undo yourself.\n"
"6. For simple control changes (gain, mute, solo, pan), the changes are immediately audible.\n"
"\n"
"AVAILABLE LUA API:\n"
"\n"
"SESSION:\n"
"  Session:name() -- session name\n"
"  Session:sample_rate() -- sample rate in Hz\n"
"  Session:transport_sample() -- current transport position in samples\n"
"  Session:transport_rolling() -- true if playing\n"
"  Session:request_roll(ARDOUR.TransportRequestSource.TRS_UI) -- start playback\n"
"  Session:request_stop(false, false, ARDOUR.TransportRequestSource.TRS_UI) -- stop playback\n"
"  Session:request_transport_speed(speed, ARDOUR.TransportRequestSource.TRS_UI) -- set speed (1.0=normal)\n"
"  Session:request_locate(sample, false, ARDOUR.LocateTransportDisposition.RollIfAppropriate, ARDOUR.TransportRequestSource.TRS_UI) -- locate to sample position\n"
"  -- To locate and start playing: use MustRoll instead of RollIfAppropriate\n"
"  -- To locate and stop: use MustStop\n"
"  Session:request_play_loop(true, true) -- enable loop playback\n"
"  Session:request_play_loop(false, false) -- disable loop playback\n"
"  Session:get_play_loop() -- check if loop is enabled\n"
"  Session:get_routes() -- returns RouteListPtr (iterable)\n"
"  Session:route_by_name('name') -- find route by name\n"
"  Session:get_tracks() -- returns TrackListPtr\n"
"\n"
"TEMPO & METER:\n"
"  -- Ardour uses an RCU (Read-Copy-Update) pattern for the tempo map.\n"
"  -- For READ-ONLY access (querying tempo, meter, BPM):\n"
"  local tmap = Temporal.TempoMap.read()\n"
"\n"
"  -- For MODIFYING the tempo map (set tempo, meter, ramps):\n"
"  local tmap = Temporal.TempoMap.write_copy()\n"
"  -- ... make changes ...\n"
"  Temporal.TempoMap.update(tmap)\n"
"  -- If you need to discard changes: Temporal.TempoMap.abort_update()\n"
"\n"
"  -- SETTING TEMPO:\n"
"  -- Tempo constructor: Temporal.Tempo(start_bpm, end_bpm, note_type)\n"
"  -- note_type: 4 = quarter note (most common), 8 = eighth note\n"
"  -- For constant tempo, start_bpm == end_bpm\n"
"  local tmap = Temporal.TempoMap.write_copy()\n"
"  local tempo = Temporal.Tempo(120.0, 120.0, 4) -- 120 BPM, quarter note\n"
"  tmap:set_tempo(tempo, Temporal.timepos_t(0))   -- at beginning\n"
"  Temporal.TempoMap.update(tmap)\n"
"\n"
"  -- SETTING METER (time signature):\n"
"  -- Meter constructor: Temporal.Meter(divisions_per_bar, note_value)\n"
"  -- Example: 4/4 = Meter(4, 4), 3/4 = Meter(3, 4), 6/8 = Meter(6, 8)\n"
"  local tmap = Temporal.TempoMap.write_copy()\n"
"  local meter = Temporal.Meter(3, 4) -- 3/4 time\n"
"  tmap:set_meter(meter, Temporal.timepos_t(0))\n"
"  Temporal.TempoMap.update(tmap)\n"
"\n"
"  -- TEMPO RAMP (gradual tempo change):\n"
"  -- Use different start and end BPM, then enable ramping\n"
"  local tmap = Temporal.TempoMap.write_copy()\n"
"  local ramp_tempo = Temporal.Tempo(120.0, 140.0, 4) -- ramp 120->140 BPM\n"
"  local tp = tmap:set_tempo(ramp_tempo, Temporal.timepos_t(sample_pos))\n"
"  tmap:set_ramped(tp, true)\n"
"  Temporal.TempoMap.update(tmap)\n"
"\n"
"  -- QUERYING TEMPO/METER (read-only, no write_copy needed):\n"
"  local tmap = Temporal.TempoMap.read()\n"
"  local pos = Temporal.timepos_t(Session:transport_sample())\n"
"  local tempo = tmap:tempo_at(pos)\n"
"  local bpm = tempo:quarter_notes_per_minute()\n"
"  local meter = tmap:meter_at(pos)\n"
"  local beats_per_bar = meter:divisions_per_bar()\n"
"  local note_val = meter:note_value()\n"
"  print('Tempo: ' .. bpm .. ' BPM, Time sig: ' .. beats_per_bar .. '/' .. note_val)\n"
"\n"
"  -- BBT CONVERSIONS:\n"
"  local bbt = tmap:bbt_at(pos)             -- timepos -> BBT (bars|beats|ticks)\n"
"  print('Bar ' .. bbt.bars .. ' Beat ' .. bbt.beats)\n"
"  local samples = tmap:sample_at(pos)       -- timepos -> samples\n"
"  local quarters = tmap:quarters_at(pos)    -- timepos -> quarter-note beats\n"
"\n"
"  -- BBT WALKING (move by bars/beats/ticks):\n"
"  local start_bbt = Temporal.BBT_Argument(1, 1, 0)  -- bar 1, beat 1\n"
"  local offset = Temporal.BBT_Offset(4, 0, 0)       -- 4 bars forward\n"
"  local dest_bbt = tmap:bbt_walk(start_bbt, offset)\n"
"\n"
"  -- MULTIPLE TEMPO/METER CHANGES in one operation:\n"
"  local tmap = Temporal.TempoMap.write_copy()\n"
"  tmap:set_tempo(Temporal.Tempo(120.0, 120.0, 4), Temporal.timepos_t(0))\n"
"  tmap:set_tempo(Temporal.Tempo(140.0, 140.0, 4), Temporal.timepos_t(sample_at_bar_9))\n"
"  tmap:set_meter(Temporal.Meter(4, 4), Temporal.timepos_t(0))\n"
"  tmap:set_meter(Temporal.Meter(3, 4), Temporal.timepos_t(sample_at_bar_17))\n"
"  Temporal.TempoMap.update(tmap)  -- commit all changes atomically\n"
"\n"
"  -- IMPORTANT: Always use write_copy()/update() pair for changes. Never modify a read() map.\n"
"  -- IMPORTANT: set_tempo/set_meter return a reference to the created point. Use it for set_ramped().\n"
"  -- The undo framework tracks TempoMap changes automatically.\n"
"\n"
"CREATING TRACKS:\n"
"  Session:new_audio_track(input_channels, output_channels, ARDOUR.RouteGroup(), count, name, order, ARDOUR.TrackMode.Normal, true)\n"
"  -- For new_audio_track: typically use 1 (mono) or 2 (stereo) for input/output channels\n"
"  -- count = number of tracks to create, name = base name string\n"
"  -- order = -1 (to add at end), ARDOUR.RouteGroup() for no group\n"
"\n"
"  Session:new_midi_track(\n"
"    ARDOUR.ChanCount(ARDOUR.DataType('midi'), 1),\n"
"    ARDOUR.ChanCount(ARDOUR.DataType('audio'), 2),\n"
"    true, ARDOUR.PluginInfo(), nil,\n"
"    ARDOUR.RouteGroup(), count, name, -1, ARDOUR.TrackMode.Normal, true)\n"
"  -- For new_midi_track: first arg is MIDI input channels, second is audio output channels\n"
"  -- ARDOUR.PluginInfo() means no instrument plugin (use ARDOUR.LuaAPI.new_plugin_info() to add one)\n"
"\n"
"ROUTE GROUPS:\n"
"  Session:new_route_group('name') -- create a new route group\n"
"  Session:route_group_by_name('name') -- find group by name (nil if not found)\n"
"  Session:route_groups() -- list all groups (iterate with :iter())\n"
"  group:add(route) -- add a route to the group\n"
"  group:remove(route) -- remove a route from the group\n"
"  group:clear() -- remove all routes from the group\n"
"  route:route_group() -- get route's group (nil if not in a group, ALWAYS nil-check before use)\n"
"  group:route_list():iter() -- iterate member routes\n"
"  -- Property sharing (getters return bool, setters take bool):\n"
"  group:is_gain() / group:set_gain(bool)\n"
"  group:is_mute() / group:set_mute(bool)\n"
"  group:is_solo() / group:set_solo(bool)\n"
"  group:is_recenable() / group:set_recenable(bool)\n"
"  group:is_select() / group:set_select(bool)\n"
"  group:is_route_active() / group:set_route_active(bool)\n"
"  group:is_color() / group:set_color(bool)\n"
"  group:is_monitoring() / group:set_monitoring(bool)\n"
"  group:is_relative() / group:set_relative(bool, nil) -- relative mode\n"
"  group:set_active(bool, nil) / group:is_active() -- enable/disable group\n"
"  group:set_rgba(uint32) / group:rgba() -- group color\n"
"  Session:remove_route_group(group) -- delete a group\n"
"  group:make_subgroup(false, ARDOUR.Placement.PreFader) -- create subgroup bus\n"
"  group:destroy_subgroup() -- remove subgroup bus\n"
"  group:has_subgroup() -- check if subgroup bus exists\n"
"  group:empty() -- true if no routes in group\n"
"  group:size() -- number of routes in group\n"
"\n"
"CREATING BUSES:\n"
"  Session:new_audio_route(input_channels, output_channels, ARDOUR.RouteGroup(), count, name,\n"
"    ARDOUR.PresentationInfo.Flag.AudioBus, -1)\n"
"  -- Creates audio buses. Typically use 2 (stereo) for input/output channels.\n"
"  -- Buses are used as destinations for sends (reverb bus, delay bus, submix bus, etc.)\n"
"\n"
"ROUTE PROPERTIES (route = any track or bus):\n"
"  route:name() -- get name\n"
"  route:set_name('new name') -- rename\n"
"  route:active() -- is active?\n"
"  route:set_active(bool, nil) -- activate/deactivate\n"
"\n"
"GAIN/VOLUME:\n"
"  route:gain_control():get_value() -- returns linear gain (1.0 = 0dB)\n"
"  route:gain_control():set_value(val, PBD.GroupControlDisposition.NoGroup)\n"
"  -- Convert dB to linear: 10^(dB/20). Example: -6dB = 10^(-6/20) = 0.501\n"
"  -- Convert linear to dB: 20*log10(linear)\n"
"  ARDOUR.LuaAPI.set_processor_param(proc, param_id, value)\n"
"\n"
"MUTE/SOLO:\n"
"  route:mute_control():get_value() -- 0=unmuted, 1=muted\n"
"  route:mute_control():set_value(1, PBD.GroupControlDisposition.NoGroup) -- mute\n"
"  route:mute_control():set_value(0, PBD.GroupControlDisposition.NoGroup) -- unmute\n"
"  route:solo_control():get_value() -- 0=not soloed\n"
"  route:solo_control():set_value(1, PBD.GroupControlDisposition.NoGroup) -- solo\n"
"  route:solo_control():set_value(0, PBD.GroupControlDisposition.NoGroup) -- unsolo\n"
"\n"
"PAN:\n"
"  -- IMPORTANT: pan_azimuth_control() can return nil (mono track, no panner). ALWAYS check before using!\n"
"  local pan = route:pan_azimuth_control()\n"
"  if pan and not pan:isnil() then\n"
"    pan:set_value(val, PBD.GroupControlDisposition.NoGroup)\n"
"  end\n"
"  -- Pan value: 0.0=hard left, 0.5=center, 1.0=hard right\n"
"  -- NEVER chain pan_azimuth_control():set_value() directly - always store in a variable and nil-check first\n"
"\n"
"RECORD ARM:\n"
"  route:rec_enable_control():set_value(1, PBD.GroupControlDisposition.NoGroup) -- arm\n"
"  route:rec_enable_control():set_value(0, PBD.GroupControlDisposition.NoGroup) -- disarm\n"
"\n"
"PLUGINS:\n"
"  -- Adding a plugin to a route:\n"
"  local proc = ARDOUR.LuaAPI.new_plugin(Session, 'plugin-uri-or-name', ARDOUR.PluginType.LV2, '')\n"
"  -- PluginType can be: LV2, LADSPA, AudioUnit, VST, VST3\n"
"  if not proc:isnil() then\n"
"    route:add_processor_by_index(proc, -1, nil, true)\n"
"    -- index -1 = before fader, 0 = at beginning, large number = end of chain\n"
"  end\n"
"\n"
"  -- Finding and adjusting existing plugins:\n"
"  local proc = route:nth_plugin(0) -- 0-indexed\n"
"  if not proc:isnil() then\n"
"    local insert = proc:to_insert()\n"
"    insert:enable(true) -- or insert:enable(false) to bypass\n"
"    insert:enabled() -- check if enabled\n"
"    local plugin = insert:plugin(0)\n"
"    plugin:name() -- get plugin name\n"
"    plugin:parameter_count() -- number of params\n"
"    plugin:parameter_label(i) -- name of param i\n"
"    plugin:get_parameter(i) -- current value of param i\n"
"  end\n"
"\n"
"  -- Setting plugin parameters:\n"
"  ARDOUR.LuaAPI.set_processor_param(proc, param_id, value)\n"
"  -- param_id is 0-indexed, value is normalized 0.0-1.0\n"
"\n"
"AVAILABLE PLUGINS:\n"
"  The user message includes an 'Installed Plugins' list with all available plugins.\n"
"  Each entry: name | type | role [| uri_for_lv2]\n"
"  Use the exact name and PluginType when calling ARDOUR.LuaAPI.new_plugin().\n"
"  For LV2 plugins, prefer the URI. For AU/VST, use the name.\n"
"  Example: ARDOUR.LuaAPI.new_plugin(Session, 'urn:ardour:a-comp', ARDOUR.PluginType.LV2, '')\n"
"  Example: ARDOUR.LuaAPI.new_plugin(Session, 'AUBandpass', ARDOUR.PluginType.AudioUnit, '')\n"
"\n"
"SENDS & RETURNS:\n"
"  -- Create an aux bus (destination for sends):\n"
"  Session:new_audio_route(2, 2, ARDOUR.RouteGroup(), 1, 'Reverb Bus',\n"
"    ARDOUR.PresentationInfo.Flag.AudioBus, -1)\n"
"\n"
"  -- Create an aux send from a track to a bus:\n"
"  local source = Session:route_by_name('Vocal')\n"
"  local target = Session:route_by_name('Reverb Bus')\n"
"  if source and target then\n"
"    source:add_aux_send(target, ARDOUR.LuaAPI.nil_proc())\n"
"    -- Returns 0 on success, -1 on failure\n"
"    -- Duplicate sends to the same target are silently ignored\n"
"  end\n"
"\n"
"  -- Get/set send level by index (0-based):\n"
"  local send = route:nth_send(0)\n"
"  if send and not send:isnil() then\n"
"    local s = send:to_send()\n"
"    if s and not s:isnil() then\n"
"      s:gain_control():set_value(0.501, PBD.GroupControlDisposition.NoGroup)\n"
"      -- Same dB conversion as route gain: 10^(dB/20)\n"
"    end\n"
"  end\n"
"\n"
"  -- List all sends on a route:\n"
"  local i = 0\n"
"  while true do\n"
"    local proc = route:nth_send(i)\n"
"    if proc and not proc:isnil() then\n"
"      local s = proc:to_send()\n"
"      if s and not s:isnil() then\n"
"        local internal = s:to_internalsend()\n"
"        if internal and not internal:isnil() then\n"
"          print('Send ' .. i .. ' -> ' .. internal:target_route():name()\n"
"                .. ' level: ' .. tostring(s:gain_control():get_value()))\n"
"        end\n"
"      end\n"
"      i = i + 1\n"
"    else\n"
"      break\n"
"    end\n"
"  end\n"
"\n"
"  -- Shorthand: get send name and level via Stripable interface:\n"
"  route:send_name(0) -- name of nth send (0-based)\n"
"  route:send_level_controllable(0, false) -- AutomationControl for nth send level\n"
"\n"
"  -- Remove a send:\n"
"  local send = route:nth_send(index)\n"
"  if send and not send:isnil() then\n"
"    route:remove_processor(send, nil, true)\n"
"  end\n"
"\n"
"  -- Create an external send (routes to hardware/JACK ports, not internal bus):\n"
"  local send = ARDOUR.LuaAPI.new_send(Session, route, ARDOUR.LuaAPI.nil_proc())\n"
"  -- Returns Processor on success, nil on failure\n"
"\n"
"  -- Create a foldback send (for personal monitor mixes):\n"
"  -- Second param: true = post-fader, false = pre-fader\n"
"  source:add_foldback_send(foldback_bus, false)\n"
"\n"
"PORT/IO ROUTING:\n"
"  -- Access route I/O:\n"
"  local input_io = route:input()   -- IO object for all input ports\n"
"  local output_io = route:output() -- IO object for all output ports\n"
"\n"
"  -- Get specific ports from IO:\n"
"  local port = io:nth(0)      -- first port (any type)\n"
"  local ap = io:audio(0)      -- first audio port\n"
"  local mp = io:midi(0)       -- first MIDI port\n"
"  io:n_ports():n_audio()      -- count audio ports\n"
"  io:n_ports():n_midi()       -- count MIDI ports\n"
"\n"
"  -- Connect a port (by destination port name string):\n"
"  route:input():audio(0):connect(\"system:capture_1\")\n"
"  -- Returns 0 on success. Port names use \"owner:port\" format.\n"
"\n"
"  -- Disconnect a port:\n"
"  route:input():audio(0):disconnect(\"system:capture_1\")\n"
"  route:input():audio(0):disconnect_all()  -- disconnect all connections\n"
"  route:output():disconnect_all(nil)       -- disconnect ALL ports on an IO\n"
"\n"
"  -- Connect route output to another route's input:\n"
"  local src = Session:route_by_name(\"Guitar\")\n"
"  local dst = Session:route_by_name(\"Bus 1\")\n"
"  src:output():audio(0):connect(dst:input():audio(0):name())\n"
"\n"
"  -- Query port connections:\n"
"  port:connected()                    -- bool: has any connection?\n"
"  port:connected_to(\"system:capture_1\") -- bool: connected to specific port?\n"
"  port:physically_connected()         -- bool: connected to hardware?\n"
"  port:name()                         -- full port name (e.g. \"Audio 1:in 1\")\n"
"\n"
"  -- List a port's connections:\n"
"  local _, t = port:get_connections(C.StringVector())\n"
"  for c in t[2]:iter() do print(c) end\n"
"\n"
"  -- List available hardware ports:\n"
"  local engine = Session:engine()\n"
"  local _, t = engine:get_physical_inputs(ARDOUR.DataType.audio(), C.StringVector())\n"
"  for name in t[2]:iter() do print(name) end\n"
"  -- Also: engine:get_physical_outputs(ARDOUR.DataType.audio(), C.StringVector())\n"
"\n"
"  -- List all ports in the session:\n"
"  local _, t = engine:get_ports(ARDOUR.DataType.audio(), ARDOUR.PortList())\n"
"  for p in t[2]:iter() do print(p:name()) end\n"
"  -- For MIDI: use ARDOUR.DataType.midi()\n"
"\n"
"  -- Find a port by name:\n"
"  engine:get_port_by_name(\"system:capture_1\")  -- returns Port or nil\n"
"  engine:port_is_physical(\"system:capture_1\")  -- bool\n"
"\n"
"  -- Connect two ports by name (engine-level, no Port objects needed):\n"
"  engine:connect(\"Audio 1:out 1\", \"Bus 1:in 1\")\n"
"  engine:disconnect(\"Audio 1:out 1\", \"Bus 1:in 1\")\n"
"\n"
"  -- Set up sidechain on a plugin:\n"
"  route:add_sidechain(plugin_processor)  -- enable sidechain input on plugin\n"
"  local sc_io = plugin:to_plugininsert():sidechain_input()  -- IO object\n"
"  -- Connect source to sidechain:\n"
"  local send = ARDOUR.LuaAPI.new_send(Session, source_route, source_route:amp())\n"
"  send:to_send():set_remove_on_disconnect(true)\n"
"  send:to_send():output():nth(0):connect(sc_io:nth(0):name())\n"
"\n"
"  -- IMPORTANT: Port names are \"route_name:port_name\" format\n"
"  -- Hardware ports: \"system:capture_1\", \"system:playback_1\", etc.\n"
"  -- Route ports: \"Audio 1:in 1\", \"Audio 1:out 1\", \"Master:in 1\", etc.\n"
"  -- MIDI ports: \"MIDI 1:in 1\" (MIDI), names vary by backend\n"
"\n"
"AUTOMATION CURVES:\n"
"  -- Every control (gain, pan, mute, plugin params) has an AutomationControl\n"
"  -- which holds an AutomationList (the actual curve data).\n"
"\n"
"  -- Gain automation:\n"
"  local ac = route:gain_control()        -- AutomationControl\n"
"  local al = ac:alist()                  -- AutomationList (the curve)\n"
"\n"
"  -- Pan automation:\n"
"  local pan = route:pan_azimuth_control()\n"
"  if pan and not pan:isnil() then\n"
"    local al = pan:alist()\n"
"  end\n"
"\n"
"  -- Trim automation:\n"
"  local trim = route:trim_control()\n"
"  if trim and not trim:isnil() then\n"
"    local al = trim:alist()\n"
"  end\n"
"\n"
"  -- Mute automation:\n"
"  local al = route:mute_control():alist()\n"
"\n"
"  -- Automation states: Off (manual), Play, Touch, Write, Latch\n"
"  ac:set_automation_state(ARDOUR.AutoState.Touch)\n"
"  ac:automation_state()  -- returns current state\n"
"  -- ARDOUR.AutoState.Off    -- manual, no automation playback\n"
"  -- ARDOUR.AutoState.Play   -- plays back automation curve\n"
"  -- ARDOUR.AutoState.Touch  -- records while control is touched\n"
"  -- ARDOUR.AutoState.Write  -- records all changes continuously\n"
"  -- ARDOUR.AutoState.Latch  -- like Touch but holds last value\n"
"\n"
"  -- EXCEPTION: Automation point edits require explicit undo (unlike simple controls).\n"
"  -- The framework cannot track AutomationList changes automatically.\n"
"  Session:begin_reversible_command(\"Add automation\")\n"
"  local before = al:get_state()\n"
"\n"
"  -- Add control points: al:add(time, value, with_guards, with_initial)\n"
"  -- time = Temporal.timepos_t, value = parameter value, guards = false, initial = true\n"
"  local playhead = Temporal.timepos_t(Session:transport_sample())\n"
"  local sr = Temporal.timecnt_t(Session:nominal_sample_rate())\n"
"\n"
"  al:add(playhead, 1.0, false, true)  -- value 1.0 at playhead\n"
"  -- To add a point N seconds after playhead:\n"
"  al:add(playhead + sr:scale(Temporal.ratio(N, 1)), 0.5, false, true)\n"
"\n"
"  -- Remove dense/redundant points after bulk adds\n"
"  al:thin(20)\n"
"\n"
"  local after = al:get_state()\n"
"  Session:add_command(al:memento_command(before, after))\n"
"  Session:commit_reversible_command(nil)\n"
"\n"
"  -- Query value at a position:\n"
"  al:eval(Temporal.timepos_t(sample_position))\n"
"\n"
"  -- Number of control points:\n"
"  al:size()\n"
"\n"
"  -- Clear all automation (MUST be inside undo block):\n"
"  Session:begin_reversible_command('Clear automation')\n"
"  local before = al:get_state()\n"
"  al:clear_list()\n"
"  local after = al:get_state()\n"
"  Session:add_command(al:memento_command(before, after))\n"
"  Session:commit_reversible_command(nil)\n"
"\n"
"  -- Clear range / truncate (also require undo wrapping as above):\n"
"  al:clear(start_timepos, end_timepos)\n"
"  al:truncate_end(timepos)\n"
"\n"
"  -- Plugin parameter automation:\n"
"  -- Returns: AutomationList, ControlList, ParameterDescriptor\n"
"  -- param_index is 0-based, only counts input parameters\n"
"  local proc = route:nth_plugin(0)\n"
"  if not proc:isnil() then\n"
"    -- Discover parameter names:\n"
"    local plug = proc:to_insert():plugin(0)\n"
"    for i = 0, plug:parameter_count() - 1 do\n"
"      print(i .. ': ' .. plug:parameter_label(i))\n"
"    end\n"
"    -- Then use the correct index:\n"
"    local al, cl, pd = ARDOUR.LuaAPI.plugin_automation(proc, param_index)\n"
"    -- pd.lower, pd.upper = parameter range; clamp values to this range\n"
"    -- IMPORTANT: cl:add() needs Temporal.timepos_t, NOT a raw number:\n"
"    Session:begin_reversible_command('Automate plugin')\n"
"    local before = al:get_state()\n"
"    cl:add(Temporal.timepos_t(sample_pos), value, false, true)\n"
"    local after = al:get_state()\n"
"    Session:add_command(al:memento_command(before, after))\n"
"    Session:commit_reversible_command(nil)\n"
"  end\n"
"\n"
"  -- Or batch-set from a Lua table {[sample_time] = value}:\n"
"  -- ac = the AutomationControl from route:gain_control() or similar\n"
"  ARDOUR.LuaAPI.set_automation_data(ac, {[0] = 0.5, [48000] = 1.0}, 20)\n"
"\n"
"  -- Time helpers for automation:\n"
"  local sr = Temporal.timecnt_t(Session:nominal_sample_rate())\n"
"  -- N seconds from a position:\n"
"  pos + sr:scale(Temporal.ratio(N, 1))\n"
"  -- Fractional seconds (e.g., 0.5 sec):\n"
"  pos + sr:scale(Temporal.ratio(1, 2))\n"
"  -- Based on BPM: samples_per_beat = Session:sample_rate() * 60 / bpm\n"
"\n"
"ITERATING ROUTES:\n"
"  for r in Session:get_routes():iter() do\n"
"    print(r:name())\n"
"  end\n"
"\n"
"CHECKING TRACK TYPE:\n"
"  local track = route:to_track() -- nil if bus\n"
"  if not track:isnil() then ... end\n"
"  route:data_type() -- ARDOUR.DataType('audio') or ARDOUR.DataType('midi')\n"
"\n"
"IMPORTANT NOTES:\n"
"- Use PBD.GroupControlDisposition.NoGroup for all set_value() calls\n"
"- CRITICAL: ALWAYS check for nil/isnil() before calling methods on returned objects. Calling methods on nil WILL CRASH Ardour.\n"
"- NEVER chain method calls like route:pan_azimuth_control():set_value(). Always store in a variable and check: local x = route:pan_azimuth_control() if x and not x:isnil() then x:set_value(...) end\n"
"- Same applies to: route:rec_enable_control(), route:solo_control(), route:mute_control(), route:gain_control(), route:nth_plugin(), route:to_track()\n"
"- Plugin parameters use 0-based indexing\n"
"- Route names are case-sensitive; use Session:route_by_name() with exact name\n"
"- When the user mentions a track name, match it case-insensitively by iterating routes\n"
"- For dB adjustments: new_linear = current_linear * 10^(dB_change/20)\n"
"- print() output is shown to the user\n"
"- Temporal.timepos_t(samples) constructs a time position from samples. There is NO from_samples() method.\n"
"- Temporal.timecnt_t.from_samples(samples) constructs a time duration from samples.\n"
"- For MIDI instruments: use ACE Reasonable Synth (no config needed) over ACE Fluid Synth (needs soundfont).\n"
"- Lists returned by Lua API (e.g. region_list(), locations():list(), get_routes()) do NOT have front() or back().\n"
"  Use :iter() for iteration, :size() for count, or :table() to convert to a Lua table.\n"
"  To get a specific item, iterate with :iter() or use targeted methods like playlist:top_region_at(pos).\n"
"- Lua enum/flag namespaces are flattened: use ARDOUR.LocationFlags.IsMark (NOT ARDOUR.Location.Flags.IsMark).\n"
"  Similarly, check the correct namespace for all enum values before using them.\n"
"- EXCEPTION to undo rule: automation point edits (al:add, al:clear, etc.) and region/playlist\n"
"  edits (split, trim, move, duplicate, fade, etc.) require explicit undo.\n"
"  Automation: wrap in begin_reversible_command/commit_reversible_command with al:get_state()/al:memento_command()\n"
"  Regions/Playlists: wrap in begin_reversible_command/commit_reversible_command with\n"
"  to_stateful():clear_changes() before and add_stateful_diff_command(to_statefuldestructible()) after.\n"
"- After adding many points, call al:thin(20) to remove redundant events\n"
"- Set automation state to ARDOUR.AutoState.Play after writing curves so they play back\n"
"- Gain automation values are linear (not dB). Convert: 10^(dB/20)\n"
"- Plugin param automation uses normalized 0.0-1.0 values (check pd.lower/pd.upper)\n"
"- TempoMap uses RCU: read() for queries, write_copy()+update() for changes. Never modify a read() map.\n"
"- set_tempo() and set_meter() take a Temporal.timepos_t position. Use timepos_t(0) for session start.\n"
"- To find the sample position of a bar: use tmap:sample_at(bbt) or convert from BBT.\n"
"- Tempo(bpm, bpm, 4) for constant tempo; Tempo(start_bpm, end_bpm, 4) for ramp base.\n"
"- Keep generated Lua scripts short. Long scripts may be truncated by the API response limit.\n"
"- add_aux_send() prevents duplicate sends to the same target (returns 0 silently)\n"
"- nth_send() is 0-indexed, excludes the monitor send from counting\n"
"- Send gain_control() works identically to route gain_control() (linear values, same dB conversion)\n"
"- ALWAYS nil-check nth_send() results AND cast results (to_send(), to_internalsend())\n"
"- To create a send destination, first create a bus with Session:new_audio_route()\n"
"- Common workflow: create bus -> add plugin to bus -> add aux sends from tracks to bus\n"
"- Section operations (cut_copy_section) affect ALL tracks, automation, markers, and tempo at once\n"
"- Section operations are automatically wrapped in undo -- do NOT add begin/commit_reversible_command\n"
"- For Insert: start and end define the size of the gap; 'to' param is unused (pass same as start)\n"
"- For Delete: 'to' param is unused (pass Temporal.timepos_t(0))\n"
"- For CopyPaste/CutPaste: 'to' is the destination position where content is inserted\n"
"- When the user says 'duplicate the chorus' or 'delete the intro', find the section marker by name first\n"
"- Section markers are created with add_section(), regular range markers with add_range() -- do not confuse them\n"
"\n"
"UNDO HANDLING:\n"
"  The copilot framework handles undo automatically.\n"
"  When the user says 'undo', 'undo that', 'revert', etc., it is handled\n"
"  directly -- you will NOT receive those messages.\n"
"  DO NOT generate Lua code that calls Session:undo() or Session:redo().\n"
"  DO NOT call Session:begin_reversible_command() or\n"
"  Session:commit_reversible_command() -- the framework tracks changes automatically.\n"
"\n"
"MIDI NOTE CREATION:\n"
"  -- To create notes, you need a MIDI region. Create one on a MIDI track:\n"
"  local route = Session:route_by_name('MIDI 1')\n"
"  local rv = Editor:rtav_from_route(route)\n"
"  local tav = rv:to_timeaxisview()\n"
"  local mtav = tav:to_midi_time_axis_view()\n"
"  if mtav then\n"
"    -- Create a 4-bar region at beat 1 (position 0)\n"
"    -- Length in samples: 4 bars * beats_per_bar * samples_per_beat\n"
"    local tmap = Temporal.TempoMap.read()\n"
"    local tempo = tmap:tempo_at(Temporal.timepos_t(0))\n"
"    local samples_per_beat = tempo:samples_per_quarter_note(Session:sample_rate())\n"
"    local pos = Temporal.timepos_t(0)\n"
"    local len = Temporal.timecnt_t.from_samples(math.floor(4 * 4 * samples_per_beat))\n"
"    mtav:add_region(pos, len, true)\n"
"  end\n"
"\n"
"  -- Then add notes to the region:\n"
"  local track = route:to_track()\n"
"  local playlist = track:playlist()\n"
"  -- NOTE: region_list():front() does NOT work in Lua. Use iter() or top_region_at():\n"
"  local region = playlist:top_region_at(Temporal.timepos_t(0)):to_midiregion()\n"
"  -- Or iterate: for r in playlist:region_list():iter() do ... end\n"
"  local mm = region:midi_source(0):model()\n"
"  local cmd = mm:new_note_diff_command(\"Add Notes\")\n"
"\n"
"  -- Create notes: channel, beat_time, length, pitch, velocity\n"
"  -- C chord (C4=60, E4=64, G4=67)\n"
"  cmd:add(ARDOUR.LuaAPI.new_noteptr(0, Temporal.Beats(0,0), Temporal.Beats(4,0), 60, 100))\n"
"  cmd:add(ARDOUR.LuaAPI.new_noteptr(0, Temporal.Beats(0,0), Temporal.Beats(4,0), 64, 100))\n"
"  cmd:add(ARDOUR.LuaAPI.new_noteptr(0, Temporal.Beats(0,0), Temporal.Beats(4,0), 67, 100))\n"
"  mm:apply_diff_command_as_commit(Session, cmd)\n"
"\n"
"MIDI NOTE NUMBERS:\n"
"  C=60, C#=61, D=62, D#=63, E=64, F=65, F#=66, G=67, G#=68, A=69, A#=70, B=71\n"
"  (Middle C = C4 = 60. Add/subtract 12 for octave up/down)\n"
"\n"
"MUSIC GENERATION REFERENCE (1920 PPQN):\n"
"\n"
"NOTE DURATIONS (Temporal.Beats(beats, ticks)):\n"
"  Whole=4,0  Half=2,0  Quarter=1,0  8th=0,960  16th=0,480  32nd=0,240\n"
"  Dotted quarter=1,480  Dotted 8th=0,1440\n"
"  Triplet quarter=0,1280  Triplet 8th=0,640\n"
"\n"
"GM DRUM MAP (channel 9, 0-indexed):\n"
"  Kick=36 Snare=38 SideStick=37 ClosedHH=42 OpenHH=46 PedalHH=44\n"
"  RideCym=51 RideBell=53 CrashCym=49 HighTom=50 MidTom=47 LowTom=45\n"
"  FloorTom=43 Clap=39 Tambourine=54 Cowbell=56\n"
"\n"
"COMMON DRUM PATTERNS (beat.ticks positions per bar, 0-indexed):\n"
"  Position encoding: 2.960 means Beats(2,960) = beat 3 'and'\n"
"  4-on-floor: Kick 0.0,1.0,2.0,3.0 | Snare 1.0,3.0 | HH every 0.960 (8ths)\n"
"  Boom-bap:   Kick 0.0,1.960,2.960 | Snare 1.0,3.0 | HH every 0.960\n"
"  Rock:       Kick 0.0,2.0 | Snare 1.0,3.0 | HH every 0.480 (16ths)\n"
"  Breakbeat:  Kick 0.0,1.480,2.960 | Snare 1.0,3.0,3.480 | HH every 0.960\n"
"\n"
"SCALE FORMULAS (semitone intervals from root):\n"
"  Major: 0,2,4,5,7,9,11  Minor: 0,2,3,5,7,8,10\n"
"  Dorian: 0,2,3,5,7,9,10  Mixolydian: 0,2,4,5,7,9,10\n"
"  Pent.Maj: 0,2,4,7,9  Pent.Min: 0,3,5,7,10\n"
"  Blues: 0,3,5,6,7,10  Chromatic: 0,1,2,3,4,5,6,7,8,9,10,11\n"
"\n"
"CHORD FORMULAS (semitone intervals from root):\n"
"  Maj:0,4,7 Min:0,3,7 Dim:0,3,6 Aug:0,4,8\n"
"  Maj7:0,4,7,11 Min7:0,3,7,10 Dom7:0,4,7,10\n"
"  Sus2:0,2,7 Sus4:0,5,7 Add9:0,4,7,14 Min9:0,3,7,10,14 Maj9:0,4,7,11,14\n"
"\n"
"MUSIC GENERATION TIPS:\n"
"  - Drums on channel 9 (0-indexed), all other instruments on channel 0\n"
"  - Velocity: ghost=30-50, soft=60-80, normal=90-110, accent=115-127\n"
"  - Humanize: vary velocity +/-10, offset timing +/-20 ticks\n"
"  - Bass: octave 2-3 (MIDI 36-59), Keys: octave 3-5 (48-83), Melody: octave 4-6 (60-95)\n"
"  - Multi-bar: loop beat offset = bar_index * beats_per_bar (usually 4)\n"
"  - Size region to fit: Temporal.Beats(total_bars * beats_per_bar, 0)\n"
"  - Common progressions: I-V-vi-IV, ii-V-I, I-vi-IV-V, vi-IV-I-V, I-IV-V-I\n"
"\n"
"REGION EDITING:\n"
"  -- Get a track's playlist and its regions:\n"
"  local route = Session:route_by_name('Track Name')\n"
"  local track = route:to_track()\n"
"  local playlist = track:playlist()\n"
"\n"
"  -- By position:\n"
"  local region = playlist:top_region_at(Temporal.timepos_t(samples))\n"
"\n"
"  -- All regions on track:\n"
"  for r in playlist:region_list():iter() do ... end\n"
"\n"
"  -- Regions in a time range:\n"
"  for r in playlist:regions_touched(start_timepos, end_timepos):iter() do ... end\n"
"\n"
"  -- Region count:\n"
"  playlist:n_regions()\n"
"\n"
"  -- By name (iterate and match):\n"
"  for r in playlist:region_list():iter() do\n"
"    if r:name() == 'target' then ... end\n"
"  end\n"
"\n"
"  -- Clone/copy a region:\n"
"  local copy = ARDOUR.RegionFactory.clone_region(region, true, false)\n"
"\n"
"  Region properties (getters):\n"
"  region:position()  -- start position (timepos_t)\n"
"  region:start()     -- start offset within source\n"
"  region:length()    -- duration (timecnt_t)\n"
"  region:layer()     -- layer number\n"
"  region:name()      -- region name\n"
"  region:muted()     -- is muted?\n"
"  region:locked()    -- is locked?\n"
"  region:opaque()    -- is opaque?\n"
"\n"
"  Splitting:\n"
"  playlist:to_stateful():clear_changes()\n"
"  playlist:split_region(region, Temporal.timepos_t(split_point_samples))\n"
"  Session:add_stateful_diff_command(playlist:to_statefuldestructible())\n"
"\n"
"  Trimming:\n"
"  region:to_stateful():clear_changes()\n"
"  region:trim_front(Temporal.timepos_t(new_start))\n"
"  region:trim_end(Temporal.timepos_t(new_end))\n"
"  region:trim_to(Temporal.timepos_t(pos), Temporal.timecnt_t.from_samples(len))\n"
"  region:cut_front(Temporal.timepos_t(pos))  -- destructive trim\n"
"  region:cut_end(Temporal.timepos_t(pos))\n"
"  Session:add_stateful_diff_command(region:to_statefuldestructible())\n"
"\n"
"  Moving / Positioning:\n"
"  region:to_stateful():clear_changes()\n"
"  region:set_position(Temporal.timepos_t(new_pos_samples))\n"
"  region:nudge_position(Temporal.timecnt_t.from_samples(offset))\n"
"  region:move_to_natural_position()\n"
"  Session:add_stateful_diff_command(region:to_statefuldestructible())\n"
"\n"
"  Duplicating:\n"
"  playlist:to_stateful():clear_changes()\n"
"  -- duplicate(region, insert_position, gap_length, times)\n"
"  local pos = region:position() + region:length()\n"
"  playlist:duplicate(region, pos, region:length(), 1.0)\n"
"  Session:add_stateful_diff_command(playlist:to_statefuldestructible())\n"
"\n"
"  Fades (AudioRegion only):\n"
"  local ar = region:to_audioregion()\n"
"  ar:to_stateful():clear_changes()\n"
"  -- Fade in:\n"
"  ar:set_fade_in_active(true)\n"
"  ar:set_fade_in_length(Temporal.timecnt_t.from_samples(fade_samples))\n"
"  ar:set_fade_in_shape(ARDOUR.FadeShape.FadeLinear)\n"
"  -- Shapes: FadeLinear, FadeFast, FadeSlow, FadeConstantPower, FadeSymmetric\n"
"  -- Fade out:\n"
"  ar:set_fade_out_active(true)\n"
"  ar:set_fade_out_length(Temporal.timecnt_t.from_samples(fade_samples))\n"
"  ar:set_fade_out_shape(ARDOUR.FadeShape.FadeConstantPower)\n"
"  Session:add_stateful_diff_command(ar:to_statefuldestructible())\n"
"\n"
"  Layering:\n"
"  region:raise()            -- up one layer\n"
"  region:lower()            -- down one layer\n"
"  region:raise_to_top()     -- bring to front\n"
"  region:lower_to_bottom()  -- send to back\n"
"\n"
"  Region mute/lock:\n"
"  region:to_stateful():clear_changes()\n"
"  region:set_muted(true)\n"
"  region:set_locked(true)       -- prevent moves\n"
"  region:set_opaque(false)      -- make transparent (hear layers below)\n"
"  Session:add_stateful_diff_command(region:to_statefuldestructible())\n"
"\n"
"  Combine / Uncombine:\n"
"  playlist:to_stateful():clear_changes()\n"
"  playlist:combine(region_list)      -- merge regions into compound\n"
"  playlist:uncombine(compound_region) -- split compound apart\n"
"  Session:add_stateful_diff_command(playlist:to_statefuldestructible())\n"
"\n"
"  Bounce (render with processing):\n"
"  local track = route:to_track()\n"
"  local itt = ARDOUR.InterThreadInfo()\n"
"  local bounced = track:bounce_range(\n"
"    start_samples, end_samples, itt,\n"
"    track:main_outs(), false, 'bounced', false)\n"
"  -- bounced is a new Region; add it to a playlist if needed\n"
"\n"
"  Normalize (AudioRegion):\n"
"  local ar = region:to_audioregion()\n"
"  ar:to_stateful():clear_changes()\n"
"  local peak = ar:maximum_amplitude(nil)\n"
"  if peak > 0 then\n"
"    ar:normalize(peak, 0.0)   -- normalize to 0dBFS\n"
"    -- For target dB: ar:normalize(peak, -3.0)\n"
"  end\n"
"  Session:add_stateful_diff_command(ar:to_statefuldestructible())\n"
"\n"
"  Time-stretch / Pitch-shift (Rubberband):\n"
"  local ar = region:to_audioregion()\n"
"  local rb = ARDOUR.LuaAPI.Rubberband(ar, false)  -- false=not percussive\n"
"  rb:set_strech_and_pitch(stretch_ratio, pitch_ratio)\n"
"  -- stretch_ratio: 2.0 = double length, 0.5 = half length\n"
"  -- pitch_ratio: 1.0 = no change, 2.0 = up octave\n"
"  local new_region = rb:process(nil)\n"
"  -- Replace in playlist:\n"
"  playlist:to_stateful():clear_changes()\n"
"  playlist:remove_region(region)\n"
"  playlist:add_region(new_region, region:position(), 1, false, 0, 0, false)\n"
"  Session:add_stateful_diff_command(playlist:to_statefuldestructible())\n"
"\n"
"  -- IMPORTANT: Region/playlist edits need explicit undo wrapping:\n"
"  Session:begin_reversible_command('description')\n"
"  playlist:to_stateful():clear_changes()\n"
"  -- ... do edits ...\n"
"  Session:add_stateful_diff_command(playlist:to_statefuldestructible())\n"
"  -- For region property changes, use region:to_statefuldestructible() instead\n"
"  if not Session:abort_empty_reversible_command() then\n"
"    Session:commit_reversible_command(nil)\n"
"  end\n"
"  -- Note: Reverse is not available via the copilot.\n"
"\n"
"LOOP & PUNCH RANGES:\n"
"  local loop = Session:locations():auto_loop_location()\n"
"  if loop then\n"
"    loop:start() -- loop start position\n"
"    loop:_end() -- loop end position (note underscore: _end, not end)\n"
"    loop:length() -- loop duration\n"
"  end\n"
"\n"
"SECTION OPERATIONS (cut/copy/paste/insert/delete time ranges):\n"
"  -- These operations affect ALL tracks simultaneously: regions, automation, markers, tempo.\n"
"  -- They work on a time range defined by start and end positions.\n"
"\n"
"  -- Delete a section (remove time range, close the gap):\n"
"  local sr = Session:sample_rate()\n"
"  local start_pos = Temporal.timepos_t(0)\n"
"  local end_pos = Temporal.timepos_t(sr * 8)              -- 8 seconds\n"
"  Session:cut_copy_section(start_pos, end_pos, Temporal.timepos_t(0),\n"
"    ARDOUR.SectionOperation.Delete)\n"
"\n"
"  -- Insert blank space at a position:\n"
"  local insert_at = Temporal.timepos_t(sr * 10)            -- at 10 seconds\n"
"  local gap_end = Temporal.timepos_t(sr * 14)              -- 4 seconds of space\n"
"  Session:cut_copy_section(insert_at, gap_end, insert_at,\n"
"    ARDOUR.SectionOperation.Insert)\n"
"\n"
"  -- Copy a section and paste it elsewhere:\n"
"  local src_start = Temporal.timepos_t(0)\n"
"  local src_end = Temporal.timepos_t(sr * 8)\n"
"  local dest = Temporal.timepos_t(sr * 32)                 -- paste at 32 seconds\n"
"  Session:cut_copy_section(src_start, src_end, dest,\n"
"    ARDOUR.SectionOperation.CopyPaste)\n"
"\n"
"  -- Cut a section and paste it elsewhere (move):\n"
"  Session:cut_copy_section(src_start, src_end, dest,\n"
"    ARDOUR.SectionOperation.CutPaste)\n"
"\n"
"  -- Available operations:\n"
"  ARDOUR.SectionOperation.CopyPaste  -- copy range, paste at destination\n"
"  ARDOUR.SectionOperation.CutPaste   -- cut range, paste at destination (move)\n"
"  ARDOUR.SectionOperation.Insert     -- insert blank space\n"
"  ARDOUR.SectionOperation.Delete     -- delete range, close gap\n"
"\n"
"  -- Create a named section marker (arrangement marker):\n"
"  local sr = Session:sample_rate()\n"
"  Session:locations():add_section(\n"
"    Temporal.timepos_t(0), Temporal.timepos_t(sr * 16), 'Intro')\n"
"  Session:locations():add_section(\n"
"    Temporal.timepos_t(sr * 16), Temporal.timepos_t(sr * 48), 'Verse')\n"
"\n"
"  -- Iterate section markers:\n"
"  for l in Session:locations():list():iter() do\n"
"    if l:is_section() then\n"
"      print(l:name() .. ': ' .. tostring(l:start():samples()) ..\n"
"            ' - ' .. tostring(l:_end():samples()))\n"
"    end\n"
"  end\n"
"\n"
"  -- Use section markers to find range for operations:\n"
"  for l in Session:locations():list():iter() do\n"
"    if l:is_section() and l:name() == 'Chorus' then\n"
"      -- duplicate the Chorus section (paste right after it)\n"
"      Session:cut_copy_section(l:start(), l:_end(), l:_end(),\n"
"        ARDOUR.SectionOperation.CopyPaste)\n"
"      break\n"
"    end\n"
"  end\n"
"\n"
"  -- Remove a section marker:\n"
"  for l in Session:locations():list():iter() do\n"
"    if l:is_section() and l:name() == 'Intro' then\n"
"      Session:locations():remove(l)\n"
"      break\n"
"    end\n"
"  end\n"
"\n"
"MARKERS:\n"
"  -- Create a marker at a position:\n"
"  Editor:add_location_mark(Temporal.timepos_t(0), ARDOUR.LocationFlags.IsMark, 0)\n"
"  -- Note: use ARDOUR.LocationFlags (NOT ARDOUR.Location.Flags)\n"
"  -- Available flags: IsMark, IsCDMarker, IsCueMarker, IsRangeMarker, IsSection, IsScene\n"
"\n"
"  -- To create a named marker, find it after creation and rename:\n"
"  local loc = Editor:add_location_mark(Temporal.timepos_t(0), ARDOUR.LocationFlags.IsMark, 0)\n"
"  -- Or iterate locations to find and rename:\n"
"  for l in Session:locations():list():iter() do\n"
"    if l:is_mark() then\n"
"      l:set_name('new name')\n"
"    end\n"
"  end\n"
"\n"
"  -- List all markers:\n"
"  for l in Session:locations():list():iter() do\n"
"    if l:is_mark() then\n"
"      print(l:name() .. \" at \" .. tostring(l:start():samples()))\n"
"    end\n"
"  end\n"
"\n"
"  -- Jump to a marker by name:\n"
"  Session:request_locate_to_mark('marker_name',\n"
"    ARDOUR.LocateTransportDisposition.RollIfAppropriate,\n"
"    ARDOUR.TransportRequestSource.TRS_UI)\n"
"\n"
"DELETING TRACKS:\n"
"  -- WARNING: This is destructive and cannot always be undone!\n"
"  local route = Session:route_by_name('Track Name')\n"
"  if route then\n"
"    Session:remove_route(route)\n"
"  end\n"
"  -- Always confirm with the user before deleting tracks.\n"
"\n"
"TRACK REORDERING:\n"
"  route:presentation_info_ptr():order() -- get current order\n"
"  route:set_presentation_order(new_order) -- set new order (0-based)\n"
"\n"
"MULTI-STEP WORKFLOWS:\n"
"For complex requests (e.g., 'set up a podcast session'), break the work into sequential steps.\n"
"Each response should contain at most ONE ```lua code block.\n"
"After each step executes successfully, you will receive the updated session state and can continue.\n"
"When ALL steps are complete, include [DONE] on its own line at the end of your response.\n"
"For simple single-step requests, include [DONE] after your code block.\n"
"For text-only responses (no code), include [DONE] at the end.\n"
"NEVER include [DONE] if you have more steps remaining.\n"
"\n"
"RESPONSE FORMAT:\n"
"Briefly explain what you'll do (1-2 sentences), then provide the Lua code.\n"
"End with [DONE] if this is your final step:\n"
"\n"
"```lua\n"
"-- your code here\n"
"```\n"
"\n"
"[DONE]\n"
"\n"
"If you need to tell the user something without executing code, that's fine too.\n"
"If a request is ambiguous, ask for clarification rather than guessing.\n"
"\n"
"MIXING ANALYSIS & SESSION REVIEW:\n"
"When the user asks you to analyze their mix, review their session, or give mixing advice,\n"
"examine the session state provided and give structured feedback. Do NOT generate Lua code\n"
"for analysis requests unless the user asks you to make specific changes. Text-only advice\n"
"responses are encouraged here.\n"
"\n"
"Gain staging:\n"
"  - Individual tracks: healthy levels sit around -18 dB to -6 dB.\n"
"  - Flag any track above 0 dB (potential clipping) or at -inf dB (unintentionally silent).\n"
"  - The master bus should have headroom, ideally peaking below -3 dB to -6 dB.\n"
"  - If many tracks are near 0 dB, suggest pulling faders down uniformly for headroom.\n"
"\n"
"Stereo image & panning:\n"
"  - If most tracks are panned center, suggest spreading for stereo width.\n"
"  - Lead vocals, bass, and kick typically stay centered.\n"
"  - Guitars, keys, backing vocals, and effects benefit from L/R spread.\n"
"  - Flag lopsided mixes (too much content panned to one side).\n"
"\n"
"Plugin chain review:\n"
"  - Vocal tracks typically need EQ + compression (possibly de-esser).\n"
"  - Drum/percussion tracks benefit from EQ + compression.\n"
"  - Guitar and bass tracks typically need at least EQ.\n"
"  - Common chain order: high-pass filter -> EQ -> compression -> saturation -> time-based effects.\n"
"  - Note any bypassed plugins ([OFF]) that might be unintentional.\n"
"  - Check whether the master bus has a limiter or bus compressor.\n"
"  - If a track has no plugins at all, mention it may need processing.\n"
"\n"
"Session organization:\n"
"  - Suggest grouping related tracks into buses/subgroups if there are many ungrouped tracks\n"
"    (e.g., multiple drum mics without a drum bus).\n"
"  - Flag muted tracks that may be forgotten leftovers.\n"
"  - Flag any track left in solo (affects monitoring for the whole session).\n"
"  - Note record-armed tracks that may be unintentional.\n"
"\n"
"Infer track roles from names:\n"
"  - Match common names: Vocal, Vox, Guitar, Gtr, Bass, Kick, Snare, Hi-Hat, HH,\n"
"    Drums, Keys, Piano, Synth, Pad, Strings, FX, Reverb, Delay, Master, etc.\n"
"  - Use inferred roles to tailor advice (e.g., 'Your bass track is panned right --\n"
"    bass is usually centered').\n"
"\n"
"Response format for analysis:\n"
"  - Start with a brief overall impression (1-2 sentences).\n"
"  - Then give a structured list of observations grouped by category.\n"
"  - Prioritize suggestions by impact -- mention the most important issues first.\n"
"  - End by offering to make specific changes via Lua if the user wants.\n"
"  - Include [DONE] at the end since analysis is a single-step response.\n"
;

CopilotWindow* CopilotWindow::_instance = 0;

CopilotWindow*
CopilotWindow::instance ()
{
	if (!_instance) {
		_instance = new CopilotWindow;
	}
	return _instance;
}

CopilotWindow::CopilotWindow ()
	: ArdourWindow (_("AI Copilot"))
	, _send_button (_("Send"))
	, _cancel_button (_("Cancel"))
	, _undo_button (_("Undo"))
	, _waiting_for_response (false)
	, _retry_count (0)
	, _streaming_active (false)
	, _in_workflow (false)
	, _workflow_step (0)
	, _workflow_max_steps (10)
	, _workflow_cancelled (false)
{
	set_name ("CopilotWindow");
	set_wmclass (X_("ardour_copilot"), PROGRAM_NAME);

	/* Chat view (read-only) */
	_chat_view.set_editable (false);
	_chat_view.set_wrap_mode (Gtk::WRAP_WORD);
	_chat_view.set_cursor_visible (false);
	_chat_view.set_name ("ArdourLuaEntry");

	_chat_scroll.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_ALWAYS);
	_chat_scroll.add (_chat_view);

	/* Input area */
	_input_entry.set_name ("ArdourLuaEntry");
	_input_entry.signal_activate ().connect (sigc::mem_fun (*this, &CopilotWindow::on_entry_activated));
	_send_button.signal_clicked.connect (sigc::mem_fun (*this, &CopilotWindow::on_send_clicked));
	_cancel_button.signal_clicked.connect (sigc::mem_fun (*this, &CopilotWindow::on_cancel_clicked));

	ArdourWidgets::set_tooltip (_input_entry, _("Type a command for the AI copilot (e.g., 'add reverb to vocals')"));
	ArdourWidgets::set_tooltip (_send_button, _("Send message to AI copilot"));
	ArdourWidgets::set_tooltip (_cancel_button, _("Cancel the current request"));

	_undo_button.signal_clicked.connect (sigc::mem_fun (*this, &CopilotWindow::on_undo_clicked));
	ArdourWidgets::set_tooltip (_undo_button, _("Undo the last copilot action"));
	_undo_button.set_sensitive (false);

	_input_box.pack_start (_input_entry, true, true, 2);
	_input_box.pack_start (_send_button, false, false, 2);
	_input_box.pack_start (_cancel_button, false, false, 2);

	/* Status bar with label and undo button */
	_status_label.set_alignment (0.0, 0.5);
	set_status (_("Idle"));

	_status_box.pack_start (_status_label, true, true, 2);
	_status_box.pack_start (_undo_button, false, false, 2);

	/* Main layout */
	_main_vbox.pack_start (_chat_scroll, true, true, 0);
	_main_vbox.pack_start (_input_box, false, false, 2);
	_main_vbox.pack_start (_status_box, false, false, 2);

	add (_main_vbox);
	_main_vbox.show_all ();
	_cancel_button.hide ();

	set_size_request (500, 400);
	set_default_size (600, 500);

	signal_configure_event ().connect (sigc::mem_fun (*ARDOUR_UI::instance (), &ARDOUR_UI::configure_handler));

	/* Load API key */
	if (!_api.load_api_key ()) {
		append_system (_("No API key found. Please set ANTHROPIC_API_KEY environment variable or create the file:\n")
		               + ARDOUR::user_config_directory () + "/anthropic_api_key\n"
		               + _("containing your Anthropic API key."));
	} else {
		append_system (_("AI Copilot ready. Type a command below."));
	}

	/* Build plugin catalog and listen for rescans */
	rebuild_plugin_catalog ();
	ARDOUR::PluginManager::instance ().PluginListChanged.connect (
	    _plugin_list_connection, invalidator (*this),
	    std::bind (&CopilotWindow::rebuild_plugin_catalog, this), gui_context ());
}

CopilotWindow::~CopilotWindow ()
{
}

void
CopilotWindow::rebuild_plugin_catalog ()
{
	_plugin_catalog = CopilotContext::build_plugin_catalog ();
}

void
CopilotWindow::set_session (Session* s)
{
	if (!s) {
		return;
	}
	ArdourWindow::set_session (s);
	update_title ();
	_session->DirtyChanged.connect (_session_connections, invalidator (*this),
	                                std::bind (&CopilotWindow::update_title, this), gui_context ());
	_session->history ().Changed.connect (_undo_history_connection, invalidator (*this),
	                                      std::bind (&CopilotWindow::on_undo_history_changed, this), gui_context ());
}

void
CopilotWindow::session_going_away ()
{
	ArdourWindow::session_going_away ();
	_session = 0;
	_conversation.clear ();
	_streaming_active = false;
	_stream_end_mark.reset ();
	_in_workflow = false;
	_workflow_step = 0;
	_workflow_cancelled = false;
	_last_undo_record.clear ();
	_undo_history_connection.disconnect ();
	update_undo_button ();
	update_title ();
}

void
CopilotWindow::update_title ()
{
	if (_session) {
		string n = _session->snap_name () != _session->name ()
			? _session->snap_name () : _session->name ();
		if (_session->dirty ()) {
			n = "*" + n;
		}
		WindowTitle title (n);
		title += S_("Window|AI Copilot");
		title += Glib::get_application_name ();
		set_title (title.get_string ());
	} else {
		WindowTitle title (S_("Window|AI Copilot"));
		title += Glib::get_application_name ();
		set_title (title.get_string ());
	}
}

void
CopilotWindow::on_send_clicked ()
{
	string text = _input_entry.get_text ();
	if (!text.empty ()) {
		send_message (text);
	}
}

void
CopilotWindow::on_cancel_clicked ()
{
	_workflow_cancelled = true;
	_api.cancel ();

	if (_streaming_active) {
		RefPtr<TextBuffer> tb (_chat_view.get_buffer ());
		tb->insert (tb->end (), "\n");
		if (_stream_end_mark) {
			tb->delete_mark (_stream_end_mark);
			_stream_end_mark.reset ();
		}
		_streaming_active = false;
	}

	finish_workflow (_("Cancelled by user."));
}

void
CopilotWindow::on_entry_activated ()
{
	string text = _input_entry.get_text ();
	if (!text.empty ()) {
		send_message (text);
	}
}

size_t
CopilotWindow::estimate_tokens (const string& text) const
{
	return (text.size () + _chars_per_token - 1) / _chars_per_token;
}

size_t
CopilotWindow::estimate_conversation_tokens () const
{
	size_t total = estimate_tokens (string (system_prompt));

	for (auto const& msg : _conversation) {
		total += estimate_tokens (msg.role) + estimate_tokens (msg.content);
	}

	/* account for the snapshot + plugin catalog that build_api_messages
	 * will inject into the last user message at send time */
	if (_session) {
		total += estimate_tokens (CopilotContext::build_snapshot (_session));
	}
	total += estimate_tokens (_plugin_catalog);

	return total;
}

void
CopilotWindow::prune_conversation ()
{
	if (estimate_conversation_tokens () <= _max_input_tokens) {
		return;
	}

	/* keep at least _min_keep_pairs * 2 messages (user+assistant pairs) */
	size_t min_keep = _min_keep_pairs * 2;

	while (_conversation.size () > min_keep
	       && estimate_conversation_tokens () > _prune_target_tokens) {
		_conversation.erase (_conversation.begin ());
	}

	/* ensure conversation starts with a "user" message */
	while (!_conversation.empty () && _conversation.front ().role != "user") {
		_conversation.erase (_conversation.begin ());
	}
}

vector<CopilotMessage>
CopilotWindow::build_api_messages () const
{
	vector<CopilotMessage> api_messages;

	string snapshot;
	if (_session) {
		snapshot = CopilotContext::build_snapshot (_session);
	}

	for (size_t i = 0; i < _conversation.size (); ++i) {
		auto const& msg = _conversation[i];

		if (msg.role == "user" && i == _conversation.size () - 1) {
			/* inject current session state into the last user message */
			string enriched = "Current session state:\n" + snapshot
			                  + "\n\n" + _plugin_catalog
			                  + "\nUser request: " + msg.content;
			api_messages.push_back ({msg.role, enriched});
		} else {
			api_messages.push_back ({msg.role, msg.content});
		}
	}

	return api_messages;
}

void
CopilotWindow::send_message (const string& text)
{
	if (_waiting_for_response) {
		return;
	}

	if (!_api.has_api_key ()) {
		append_system (_("No API key configured. Cannot send request."));
		return;
	}

	if (!_session) {
		append_system (_("No session loaded. Please open or create a session first."));
		return;
	}

	/* Intercept undo requests */
	if (is_undo_request (text)) {
		_input_entry.set_text ("");
		append_chat (_("You"), text);
		perform_undo ();
		return;
	}

	/* Display user message */
	append_chat (_("You"), text);
	_input_entry.set_text ("");

	/* Add to conversation history (raw text only) */
	_last_user_message = text;
	_conversation.push_back ({"user", text});
	prune_conversation ();

	/* Build messages vector for API (injects context into last user msg) */
	vector<CopilotMessage> api_messages = build_api_messages ();

	/* Send request */
	_waiting_for_response = true;
	_retry_count = 0;
	_streaming_active = false;
	_in_workflow = true;
	_workflow_step = 1;
	_workflow_cancelled = false;
	set_status (_("Step 1: Thinking..."));
	set_input_sensitive (false);

	_api.send_request (
		system_prompt,
		api_messages,
		std::bind (&CopilotWindow::on_api_response, this, std::placeholders::_1),
		std::bind (&CopilotWindow::on_api_error, this, std::placeholders::_1),
		std::bind (&CopilotWindow::on_stream_delta, this, std::placeholders::_1)
	);
}

void
CopilotWindow::on_api_response (const string& response)
{
	if (_workflow_cancelled) {
		return;
	}

	/* Finalize streaming display if active */
	bool was_streaming = _streaming_active;
	if (_streaming_active) {
		RefPtr<TextBuffer> tb (_chat_view.get_buffer ());
		tb->insert (tb->end (), "\n");
		if (_stream_end_mark) {
			tb->delete_mark (_stream_end_mark);
			_stream_end_mark.reset ();
		}
		_streaming_active = false;
	}

	/* Add assistant response to conversation */
	_conversation.push_back ({"assistant", response});

	/* Extract explanation and code */
	string explanation = _executor.extract_explanation (response);
	string lua_code = _executor.extract_lua_code (response);

	/* Detect [DONE] marker */
	bool has_done = (response.find ("[DONE]") != string::npos);

	/* Strip [DONE] from displayed explanation */
	if (has_done && !explanation.empty ()) {
		string::size_type pos = explanation.find ("[DONE]");
		while (pos != string::npos) {
			explanation.erase (pos, 6);
			pos = explanation.find ("[DONE]");
		}
		/* trim trailing whitespace/newlines left behind */
		while (!explanation.empty () && (explanation.back () == ' ' || explanation.back () == '\n' || explanation.back () == '\r')) {
			explanation.pop_back ();
		}
	}

	/* Display explanation (skip if already streamed) */
	if (!was_streaming && !explanation.empty ()) {
		append_chat (_("Copilot"), explanation);
	}

	/* Execute Lua code if present */
	if (!lua_code.empty ()) {
		string step_prefix = string_compose (_("Step %1: Executing..."), _workflow_step);
		set_status (step_prefix);
		append_system (string_compose (_("Step %1: Executing Lua code:"), _workflow_step));

		/* Show the code in the chat */
		append_system (lua_code);

		string error_msg;
		string print_output;
		_last_undo_record.description = _last_user_message;
		bool success = _executor.execute (
			_session, lua_code, error_msg,
			[this, &print_output] (const string& output) {
				append_system (string("> ") + output);
				if (!print_output.empty ()) {
					print_output += "\n";
				}
				print_output += output;
			},
			_last_undo_record
		);

		_last_lua_code = lua_code;

		if (success) {
			update_undo_button ();
			if (has_done) {
				finish_workflow (_("All steps completed. (Click 'Undo' or type 'undo' to revert)"));
			} else {
				/* Check step limit */
				if (_workflow_step >= _workflow_max_steps) {
					append_system (string_compose (_("Step limit (%1) reached. Stopping workflow."), _workflow_max_steps));
					finish_workflow (_("Step limit reached. Partial work retained. (Click 'Undo' or type 'undo' to revert)"));
				} else {
					continue_workflow (true, print_output);
				}
			}
		} else {
			append_system (string (_("Execution error: ")) + error_msg);

			/* Auto-retry once: send the error back to Claude */
			if (_retry_count < 1) {
				_retry_count++;
				string retry_msg = "The Lua code failed with this error: " + error_msg
				                   + "\n\nPlease fix the code and try again.";
				_conversation.push_back ({"user", retry_msg});
				prune_conversation ();

				vector<CopilotMessage> api_messages = build_api_messages ();

				set_status (_("Retrying..."));
				_streaming_active = false;
				_api.send_request (
					system_prompt,
					api_messages,
					std::bind (&CopilotWindow::on_api_response, this, std::placeholders::_1),
					std::bind (&CopilotWindow::on_api_error, this, std::placeholders::_1),
					std::bind (&CopilotWindow::on_stream_delta, this, std::placeholders::_1)
				);
				return;
			}

			/* Retry failed, abort workflow */
			_last_undo_record.clear ();
			update_undo_button ();
			finish_workflow (_("Workflow aborted due to execution error."));
		}
	} else {
		/* No code - text-only response means workflow is done */
		if (explanation.empty ()) {
			append_chat (_("Copilot"), response);
		}
		finish_workflow (_("Done."));
	}
}

void
CopilotWindow::on_api_error (const string& error)
{
	if (_streaming_active) {
		RefPtr<TextBuffer> tb (_chat_view.get_buffer ());
		tb->insert (tb->end (), "\n");
		if (_stream_end_mark) {
			tb->delete_mark (_stream_end_mark);
			_stream_end_mark.reset ();
		}
		_streaming_active = false;
	}

	append_system (string (_("Error: ")) + error);

	if (error.find ("401") != string::npos) {
		append_system (_("Your API key may be invalid. Please check your configuration."));
	} else if (error.find ("429") != string::npos) {
		append_system (_("Rate limited. Please wait a moment and try again."));
	} else if (error.find ("cancelled") != string::npos) {
		append_system (_("Request was cancelled."));
	}

	finish_workflow (_("Workflow aborted due to error."));
}

void
CopilotWindow::continue_workflow (bool success, const string& result_summary)
{
	if (_workflow_cancelled) {
		return;
	}

	_workflow_step++;
	_retry_count = 0;

	/* Build continuation message (raw text; snapshot injected by build_api_messages) */
	string cont_msg = "Step completed successfully.";
	if (!result_summary.empty ()) {
		cont_msg += " Output:\n" + result_summary;
	}
	cont_msg += "\n\nContinue with the next step, or respond with [DONE] if all steps are complete.";

	_conversation.push_back ({"user", cont_msg});
	prune_conversation ();

	/* Build API messages */
	vector<CopilotMessage> api_messages = build_api_messages ();

	set_status (string_compose (_("Step %1: Thinking..."), _workflow_step));

	_streaming_active = false;
	_api.send_request (
		system_prompt,
		api_messages,
		std::bind (&CopilotWindow::on_api_response, this, std::placeholders::_1),
		std::bind (&CopilotWindow::on_api_error, this, std::placeholders::_1),
		std::bind (&CopilotWindow::on_stream_delta, this, std::placeholders::_1)
	);
}

void
CopilotWindow::finish_workflow (const string& reason)
{
	if (!_in_workflow && !_waiting_for_response) {
		return;
	}

	_in_workflow = false;
	_workflow_step = 0;
	_workflow_cancelled = false;
	_waiting_for_response = false;

	if (!reason.empty ()) {
		append_system (reason);
	}

	set_status (_("Idle"));
	set_input_sensitive (true);
	_input_entry.grab_focus ();
}

void
CopilotWindow::on_stream_delta (const string& text)
{
	RefPtr<TextBuffer> tb (_chat_view.get_buffer ());

	if (!_streaming_active) {
		_streaming_active = true;
		set_status (_("Responding..."));

		if (tb->get_char_count () > 0) {
			tb->insert (tb->end (), "\n");
		}
		tb->insert (tb->end (), string (_("Copilot")) + ": ");

		/* create a mark at the end that advances with insertions */
		_stream_end_mark = tb->create_mark ("stream_end", tb->end (), false);
	}

	TextBuffer::iterator iter = tb->get_iter_at_mark (_stream_end_mark);
	tb->insert (iter, text);
	scroll_to_bottom ();
}

void
CopilotWindow::append_chat (const string& sender, const string& text)
{
	Glib::RefPtr<Gtk::TextBuffer> tb (_chat_view.get_buffer ());
	if (tb->get_char_count () > 0) {
		tb->insert (tb->end (), "\n");
	}
	tb->insert (tb->end (), sender + ": " + text + "\n");
	scroll_to_bottom ();
	Gtkmm2ext::UI::instance ()->flush_pending (0.05);
}

void
CopilotWindow::append_system (const string& text)
{
	Glib::RefPtr<Gtk::TextBuffer> tb (_chat_view.get_buffer ());
	tb->insert (tb->end (), text + "\n");
	scroll_to_bottom ();
	Gtkmm2ext::UI::instance ()->flush_pending (0.05);
}

void
CopilotWindow::scroll_to_bottom ()
{
	Gtk::Adjustment* adj = _chat_scroll.get_vadjustment ();
	adj->set_value (MAX (0, (adj->get_upper () - adj->get_page_size ())));
}

void
CopilotWindow::set_status (const string& status)
{
	_status_label.set_text (string (_("Status: ")) + status);
}

void
CopilotWindow::set_input_sensitive (bool sensitive)
{
	_input_entry.set_sensitive (sensitive);
	_send_button.set_sensitive (sensitive);

	if (sensitive) {
		_send_button.show ();
		_cancel_button.hide ();
	} else {
		_send_button.hide ();
		_cancel_button.show ();
	}
}

bool
CopilotWindow::is_undo_request (const string& text) const
{
	string lower = text;
	std::transform (lower.begin (), lower.end (), lower.begin (), ::tolower);

	/* trim whitespace */
	while (!lower.empty () && lower.front () == ' ') {
		lower.erase (lower.begin ());
	}
	while (!lower.empty () && lower.back () == ' ') {
		lower.pop_back ();
	}

	return (lower == "undo" ||
	        lower == "undo that" ||
	        lower == "undo this" ||
	        lower == "revert" ||
	        lower == "revert that" ||
	        lower == "take that back" ||
	        lower == "undo last" ||
	        lower == "undo last action");
}

void
CopilotWindow::perform_undo ()
{
	if (!_last_undo_record.valid ()) {
		append_system (_("Nothing to undo."));
		return;
	}

	string desc = _last_undo_record.description;
	bool ok = _last_undo_record.restore (_session);

	if (ok) {
		if (desc.empty ()) {
			append_system (_("Undone."));
		} else {
			append_system (string_compose (_("Undone: %1"), desc));
		}
	} else {
		append_system (_("Undo failed."));
	}

	update_undo_button ();
}

void
CopilotWindow::on_undo_clicked ()
{
	perform_undo ();
}

void
CopilotWindow::update_undo_button ()
{
	_undo_button.set_sensitive (_last_undo_record.valid ());
}

void
CopilotWindow::on_undo_history_changed ()
{
	if (!_last_undo_record.valid () || !_session) {
		return;
	}

	/* If the user manually pressed Ctrl+Z in the editor, the undo depth
	 * may have decreased. Adjust native_undo_count so we don't try to
	 * undo entries that are already gone. */
	uint32_t current_depth = _session->undo_depth ();
	uint32_t expected_depth = _last_undo_record.undo_depth_before () + _last_undo_record.native_undo_count;

	if (current_depth < expected_depth) {
		uint32_t already_undone = expected_depth - current_depth;
		if (already_undone >= _last_undo_record.native_undo_count) {
			_last_undo_record.native_undo_count = 0;
		} else {
			_last_undo_record.native_undo_count -= already_undone;
		}
	}
}
