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

#include <glibmm/miscutils.h>

#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/window_title.h"

#include "widgets/tooltips.h"

#include "ardour/session.h"
#include "ardour/filesystem_paths.h"

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
"5. Most operations (track creation, plugin adding, etc.) handle their own undo automatically.\n"
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
"  Session:get_routes() -- returns RouteListPtr (iterable)\n"
"  Session:route_by_name('name') -- find route by name\n"
"  Session:get_tracks() -- returns TrackListPtr\n"
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
"COMMON BUILT-IN PLUGINS (LV2 URIs):\n"
"  'urn:ardour:a-comp' -- ACE Compressor\n"
"  'urn:ardour:a-reverb' -- ACE Reverb\n"
"  'urn:ardour:a-eq' -- ACE Equalizer\n"
"  'urn:ardour:a-delay' -- ACE Delay\n"
"  'urn:ardour:a-fluidsynth' -- ACE Fluid Synth\n"
"  'http://gareus.org/oss/lv2/fil4#stereo' -- x42 Parametric EQ\n"
"  'http://gareus.org/oss/lv2/dpl#stereo' -- x42 Digital Peak Limiter (use on master)\n"
"  'http://gareus.org/oss/lv2/dpl#mono' -- x42 Digital Peak Limiter Mono\n"
"\n"
"  -- For AudioUnit (macOS):\n"
"  ARDOUR.LuaAPI.new_plugin(Session, 'plugin-name', ARDOUR.PluginType.AudioUnit, '')\n"
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
"\n"
"RESPONSE FORMAT:\n"
"Briefly explain what you'll do (1-2 sentences), then provide the Lua code:\n"
"\n"
"```lua\n"
"-- your code here\n"
"```\n"
"\n"
"If you need to tell the user something without executing code, that's fine too.\n"
"If a request is ambiguous, ask for clarification rather than guessing.\n"
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
	, _waiting_for_response (false)
	, _retry_count (0)
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

	ArdourWidgets::set_tooltip (_input_entry, _("Type a command for the AI copilot (e.g., 'add reverb to vocals')"));
	ArdourWidgets::set_tooltip (_send_button, _("Send message to AI copilot"));

	_input_box.pack_start (_input_entry, true, true, 2);
	_input_box.pack_start (_send_button, false, false, 2);

	/* Status label */
	_status_label.set_alignment (0.0, 0.5);
	set_status (_("Idle"));

	/* Main layout */
	_main_vbox.pack_start (_chat_scroll, true, true, 0);
	_main_vbox.pack_start (_input_box, false, false, 2);
	_main_vbox.pack_start (_status_label, false, false, 2);

	add (_main_vbox);
	_main_vbox.show_all ();

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
}

CopilotWindow::~CopilotWindow ()
{
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
}

void
CopilotWindow::session_going_away ()
{
	ArdourWindow::session_going_away ();
	_session = 0;
	_conversation.clear ();
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
CopilotWindow::on_entry_activated ()
{
	string text = _input_entry.get_text ();
	if (!text.empty ()) {
		send_message (text);
	}
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

	/* Display user message */
	append_chat (_("You"), text);
	_input_entry.set_text ("");

	/* Build session context */
	string context = CopilotContext::build_snapshot (_session);

	/* Build the user message with context */
	string full_message = "Current session state:\n" + context + "\n\nUser request: " + text;

	/* Add to conversation history */
	_last_user_message = text;
	_conversation.push_back ({"user", full_message});

	/* Build messages vector for API */
	vector<CopilotMessage> api_messages;
	for (auto const& msg : _conversation) {
		api_messages.push_back ({msg.role, msg.content});
	}

	/* Send request */
	_waiting_for_response = true;
	_retry_count = 0;
	set_status (_("Thinking..."));
	set_input_sensitive (false);

	_api.send_request (
		system_prompt,
		api_messages,
		std::bind (&CopilotWindow::on_api_response, this, std::placeholders::_1),
		std::bind (&CopilotWindow::on_api_error, this, std::placeholders::_1)
	);
}

void
CopilotWindow::on_api_response (const string& response)
{
	/* Add assistant response to conversation */
	_conversation.push_back ({"assistant", response});

	/* Extract explanation and code */
	string explanation = _executor.extract_explanation (response);
	string lua_code = _executor.extract_lua_code (response);

	/* Display explanation */
	if (!explanation.empty ()) {
		append_chat (_("Copilot"), explanation);
	}

	/* Execute Lua code if present */
	if (!lua_code.empty ()) {
		append_system (_("Executing Lua code:"));

		/* Show the code in the chat */
		append_system (lua_code);

		string error_msg;
		bool success = _executor.execute (
			_session, lua_code, error_msg,
			[this] (const string& output) {
				append_system (string("> ") + output);
			}
		);

		_last_lua_code = lua_code;

		if (success) {
			append_system (_("Done. (Ctrl+Z to undo)"));
		} else {
			append_system (string (_("Execution error: ")) + error_msg);

			/* Auto-retry once: send the error back to Claude */
			if (_retry_count < 1) {
				_retry_count++;
				string retry_msg = "The Lua code failed with this error: " + error_msg
				                   + "\n\nPlease fix the code and try again.";
				_conversation.push_back ({"user", retry_msg});

				vector<CopilotMessage> api_messages;
				for (auto const& msg : _conversation) {
					api_messages.push_back ({msg.role, msg.content});
				}

				set_status (_("Retrying..."));
				_api.send_request (
					system_prompt,
					api_messages,
					std::bind (&CopilotWindow::on_api_response, this, std::placeholders::_1),
					std::bind (&CopilotWindow::on_api_error, this, std::placeholders::_1)
				);
				return;
			}
		}
	} else if (explanation.empty ()) {
		/* No explanation and no code - show raw response */
		append_chat (_("Copilot"), response);
	}

	_waiting_for_response = false;
	set_status (_("Idle"));
	set_input_sensitive (true);
	_input_entry.grab_focus ();
}

void
CopilotWindow::on_api_error (const string& error)
{
	append_system (string (_("Error: ")) + error);

	if (error.find ("401") != string::npos) {
		append_system (_("Your API key may be invalid. Please check your configuration."));
	} else if (error.find ("429") != string::npos) {
		append_system (_("Rate limited. Please wait a moment and try again."));
	} else if (error.find ("cancelled") != string::npos) {
		append_system (_("Request was cancelled."));
	}

	_waiting_for_response = false;
	set_status (_("Idle"));
	set_input_sensitive (true);
	_input_entry.grab_focus ();
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
}
