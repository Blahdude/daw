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

#include <cmath>
#include <cstdio>
#include <sstream>

#include "ardour/audioengine.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/track.h"
#include "ardour/audio_track.h"
#include "ardour/midi_track.h"
#include "ardour/location.h"

#include "copilot_context.h"

using namespace ARDOUR;
using namespace std;

static string
gain_to_db_str (double gain)
{
	if (gain == 0.0) {
		return "-inf dB";
	}
	char buf[32];
	snprintf (buf, sizeof(buf), "%.1f dB", 20.0 * log10 (gain));
	return buf;
}

static string
pan_to_str (double pan)
{
	if (pan < 0.01) {
		return "L";
	} else if (pan > 0.99) {
		return "R";
	} else if (fabs (pan - 0.5) < 0.01) {
		return "C";
	}
	char buf[32];
	if (pan < 0.5) {
		snprintf (buf, sizeof(buf), "%.0f%% L", (0.5 - pan) * 200.0);
	} else {
		snprintf (buf, sizeof(buf), "%.0f%% R", (pan - 0.5) * 200.0);
	}
	return buf;
}

static string
route_type_str (std::shared_ptr<Route> r)
{
	if (r->is_master ()) {
		return "Master Bus";
	}
	if (r->is_monitor ()) {
		return "Monitor Bus";
	}

	std::shared_ptr<MidiTrack> mt = std::dynamic_pointer_cast<MidiTrack> (r);
	if (mt) {
		return "MIDI Track";
	}

	std::shared_ptr<AudioTrack> at = std::dynamic_pointer_cast<AudioTrack> (r);
	if (at) {
		return "Audio Track";
	}

	/* It's a bus */
	return "Bus";
}

static string
plugin_params_str (std::shared_ptr<Plugin> plugin, int max_params)
{
	string result;
	int count = 0;
	uint32_t n_params = plugin->parameter_count ();

	for (uint32_t i = 0; i < n_params && count < max_params; ++i) {
		if (!plugin->parameter_is_control (i)) {
			continue;
		}
		if (!plugin->parameter_is_input (i)) {
			continue;
		}

		string label = plugin->parameter_label (i);
		if (label.empty ()) {
			continue;
		}

		float val = plugin->get_parameter (i);

		char buf[64];
		snprintf (buf, sizeof(buf), "%.3g", val);

		if (!result.empty ()) {
			result += " ";
		}
		result += label + "=" + buf;
		++count;
	}

	return result;
}

string
CopilotContext::build_snapshot (Session* session)
{
	if (!session) {
		return "";
	}

	ostringstream out;

	/* Session header */
	out << "Session: \"" << session->name () << "\" | "
	    << session->sample_rate () << " Hz | ";

	if (session->transport_rolling ()) {
		out << "Playing";
	} else {
		out << "Stopped";
	}

	samplepos_t pos = session->transport_sample ();
	double secs = (double)pos / session->sample_rate ();
	int mins = (int)(secs / 60.0);
	double remaining_secs = secs - (mins * 60.0);
	char time_buf[32];
	snprintf (time_buf, sizeof(time_buf), "%d:%04.1f", mins, remaining_secs);
	out << " at " << time_buf;

	RecordState rs = session->record_status ();
	if (rs == Recording) {
		out << " [RECORDING]";
	} else if (rs == Enabled) {
		out << " [Rec Armed]";
	}

	out << "\n\nTracks:\n";

	/* Routes */
	std::shared_ptr<RouteList const> routes = session->get_routes ();
	int route_num = 0;

	for (auto const& r : *routes) {
		if (r->is_auditioner ()) {
			continue;
		}

		++route_num;

		string rtype = route_type_str (r);

		/* Gain */
		double gain = r->gain_control ()->get_value ();
		string gain_str = gain_to_db_str (gain);

		/* Pan */
		string pan_str = "C";
		auto pan_ctrl = r->pan_azimuth_control ();
		if (pan_ctrl) {
			pan_str = pan_to_str (pan_ctrl->get_value ());
		}

		out << route_num << ". " << r->name () << " (" << rtype << ") | "
		    << gain_str << " | Pan: " << pan_str;

		/* Mute / Solo / Rec arm */
		if (r->muted ()) {
			out << " | Muted";
		}
		if (r->soloed ()) {
			out << " | Solo";
		}
		auto rec_ctrl = r->rec_enable_control ();
		if (rec_ctrl && rec_ctrl->get_value () > 0) {
			out << " | Rec";
		}

		out << "\n";

		/* Plugins */
		bool has_plugins = false;
		for (uint32_t pi = 0; ; ++pi) {
			std::shared_ptr<Processor> proc = r->nth_plugin (pi);
			if (!proc) {
				break;
			}

			std::shared_ptr<PluginInsert> insert = std::dynamic_pointer_cast<PluginInsert> (proc);
			if (!insert) {
				continue;
			}

			std::shared_ptr<Plugin> plugin = insert->plugin (0);
			if (!plugin) {
				continue;
			}

			has_plugins = true;
			out << "   - " << plugin->name ()
			    << " [" << (insert->enabled () ? "ON" : "OFF") << "]";

			string params = plugin_params_str (plugin, 8);
			if (!params.empty ()) {
				out << " " << params;
			}
			out << "\n";
		}

		if (!has_plugins) {
			out << "   - (no plugins)\n";
		}

		out << "\n";
	}

	/* Locations / markers */
	Locations* locs = session->locations ();
	if (locs) {
		Locations::LocationList ll (locs->list ());
		bool has_markers = false;
		for (auto const& loc : ll) {
			if (loc->is_session_range () || loc->is_auto_loop () || loc->is_auto_punch ()) {
				continue;
			}
			if (!has_markers) {
				out << "Markers:\n";
				has_markers = true;
			}
			double loc_secs = (double)loc->start_sample () / session->sample_rate ();
			int loc_mins = (int)(loc_secs / 60.0);
			double loc_rem = loc_secs - (loc_mins * 60.0);
			char loc_buf[32];
			snprintf (loc_buf, sizeof(loc_buf), "%d:%04.1f", loc_mins, loc_rem);
			out << "  - \"" << loc->name () << "\" at " << loc_buf << "\n";
		}
	}

	return out.str ();
}
