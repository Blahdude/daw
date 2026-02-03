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

#include <algorithm>

#include "ardour/audioengine.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin.h"
#include "ardour/plugin_manager.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/track.h"
#include "ardour/route_group.h"
#include "ardour/audio_track.h"
#include "ardour/midi_track.h"
#include "ardour/automation_control.h"
#include "ardour/send.h"
#include "ardour/internal_send.h"
#include "ardour/location.h"
#include "ardour/playlist.h"
#include "ardour/region.h"
#include "temporal/tempo.h"

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
auto_state_str (ARDOUR::AutoState s)
{
	switch (s) {
	case ARDOUR::Off:   return "Off";
	case ARDOUR::Play:  return "Play";
	case ARDOUR::Write: return "Write";
	case ARDOUR::Touch: return "Touch";
	case ARDOUR::Latch: return "Latch";
	default:            return "Off";
	}
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

	/* Tempo & Meter at playhead */
	auto tmap = Temporal::TempoMap::read ();
	if (tmap) {
		Temporal::timepos_t tp (pos);
		auto const& tempo = tmap->tempo_at (tp);
		auto const& meter = tmap->meter_at (tp);
		out << " | " << tempo.quarter_notes_per_minute () << " BPM | "
		    << meter.divisions_per_bar () << "/" << meter.note_value ();
	}

	/* Full tempo map listing */
	if (tmap) {
		out << "\n\nTempo Map:\n";

		auto const& tempos = tmap->tempos ();
		for (auto const& tp : tempos) {
			auto const& bbt = tp.bbt ();
			out << "  Tempo: " << tp.quarter_notes_per_minute () << " BPM";
			if (tp.end_note_types_per_minute () != tp.note_types_per_minute ()) {
				out << " -> " << (tp.end_note_types_per_minute () * 4.0 / tp.note_type ()) << " BPM (ramped)";
			}
			out << " at Bar " << bbt.bars << "|Beat " << bbt.beats << "\n";
		}

		auto const& meters = tmap->meters ();
		for (auto const& mp : meters) {
			auto const& bbt = mp.bbt ();
			out << "  Meter: " << mp.divisions_per_bar () << "/" << mp.note_value ()
			    << " at Bar " << bbt.bars << "|Beat " << bbt.beats << "\n";
		}
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

		/* Automation state (only show non-Off) */
		{
			string auto_info;
			auto gain_ctrl = r->gain_control ();
			if (gain_ctrl && gain_ctrl->automation_state () != ARDOUR::Off) {
				auto_info += " Gain:" + auto_state_str (gain_ctrl->automation_state ());
			}
			if (pan_ctrl && pan_ctrl->automation_state () != ARDOUR::Off) {
				if (!auto_info.empty ()) auto_info += ",";
				auto_info += " Pan:" + auto_state_str (pan_ctrl->automation_state ());
			}
			auto mute_ctrl = r->mute_control ();
			if (mute_ctrl && mute_ctrl->automation_state () != ARDOUR::Off) {
				if (!auto_info.empty ()) auto_info += ",";
				auto_info += " Mute:" + auto_state_str (mute_ctrl->automation_state ());
			}
			if (!auto_info.empty ()) {
				out << " | Auto:" << auto_info;
			}
		}

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

		auto rg = r->route_group ();
		if (rg) {
			out << " | Group: \"" << rg->name () << "\"";
		}

		out << "\n";

		/* I/O connections */
		{
			auto input_io = r->input ();
			auto output_io = r->output ();

			/* Input connections */
			string in_conns;
			for (uint32_t pi = 0; pi < input_io->n_ports ().n_total (); ++pi) {
				auto port = input_io->nth (pi);
				if (!port) continue;
				std::vector<std::string> connections;
				port->get_connections (connections);
				for (auto const& c : connections) {
					if (!in_conns.empty ()) in_conns += ", ";
					in_conns += c;
				}
			}

			/* Output connections */
			string out_conns;
			for (uint32_t pi = 0; pi < output_io->n_ports ().n_total (); ++pi) {
				auto port = output_io->nth (pi);
				if (!port) continue;
				std::vector<std::string> connections;
				port->get_connections (connections);
				for (auto const& c : connections) {
					if (!out_conns.empty ()) out_conns += ", ";
					out_conns += c;
				}
			}

			if (!in_conns.empty () || !out_conns.empty ()) {
				out << "   I/O: ";
				if (!in_conns.empty ()) {
					out << "In: " << in_conns;
				}
				if (!out_conns.empty ()) {
					if (!in_conns.empty ()) out << " | ";
					out << "Out: " << out_conns;
				}
				out << "\n";
			}
		}

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

		/* Sends */
		bool has_sends = false;
		for (uint32_t si = 0; ; ++si) {
			std::shared_ptr<Processor> send_proc = r->nth_send (si);
			if (!send_proc) {
				break;
			}

			std::shared_ptr<Send> send = std::dynamic_pointer_cast<Send> (send_proc);
			if (!send) {
				continue;
			}

			if (!has_sends) {
				out << "   Sends:\n";
				has_sends = true;
			}

			double send_gain = send->gain_control ()->get_value ();
			string send_gain_str = gain_to_db_str (send_gain);

			std::shared_ptr<InternalSend> int_send = std::dynamic_pointer_cast<InternalSend> (send_proc);
			if (int_send) {
				std::shared_ptr<Route> target = int_send->target_route ();
				if (target) {
					out << "   > Send " << si << " -> \"" << target->name () << "\" | " << send_gain_str;
					if (send->is_foldback ()) {
						out << " [Foldback]";
					}
					out << "\n";
				}
			} else {
				/* External send */
				out << "   > Send " << si << " (external) | " << send_gain_str << "\n";
			}
		}

		/* Regions (tracks only) */
		std::shared_ptr<Track> trk = std::dynamic_pointer_cast<Track> (r);
		if (trk) {
			std::shared_ptr<Playlist> pl = trk->playlist ();
			if (pl && pl->n_regions () > 0) {
				std::shared_ptr<RegionList> regions = pl->region_list ();
				if (regions) {
					out << "   Regions:";
					int rcount = 0;
					double sr = session->sample_rate ();
					for (auto const& reg : *regions) {
						if (rcount >= 10) {
							out << " | ...(" << (pl->n_regions () - rcount) << " more)";
							break;
						}
						samplepos_t rstart = reg->position ().samples ();
						samplepos_t rend = rstart + reg->length ().samples ();
						double s1 = (double)rstart / sr;
						double s2 = (double)rend / sr;
						int m1 = (int)(s1 / 60.0);
						double r1 = s1 - (m1 * 60.0);
						int m2 = (int)(s2 / 60.0);
						double r2 = s2 - (m2 * 60.0);
						char tbuf[64];
						snprintf (tbuf, sizeof(tbuf), "%d:%04.1f-%d:%04.1f", m1, r1, m2, r2);
						if (rcount > 0) {
							out << " |";
						}
						out << " \"" << reg->name () << "\" " << tbuf;
						++rcount;
					}
					out << "\n";
				}
			}
		}

		out << "\n";
	}

	/* Route Groups */
	{
		auto const& groups = session->route_groups ();
		if (!groups.empty ()) {
			out << "Route Groups:\n";
			for (auto const& g : groups) {
				out << "  \"" << g->name () << "\" ["
				    << (g->is_active () ? "ON" : "OFF") << "]";

				/* Shared properties */
				string shares;
				if (g->is_gain ())         { if (!shares.empty ()) shares += ","; shares += "gain"; }
				if (g->is_mute ())         { if (!shares.empty ()) shares += ","; shares += "mute"; }
				if (g->is_solo ())         { if (!shares.empty ()) shares += ","; shares += "solo"; }
				if (g->is_recenable ())    { if (!shares.empty ()) shares += ","; shares += "rec"; }
				if (g->is_select ())       { if (!shares.empty ()) shares += ","; shares += "sel"; }
				if (g->is_route_active ()) { if (!shares.empty ()) shares += ","; shares += "active"; }
				if (g->is_color ())        { if (!shares.empty ()) shares += ","; shares += "color"; }
				if (g->is_monitoring ())   { if (!shares.empty ()) shares += ","; shares += "mon"; }
				if (!shares.empty ()) {
					out << " | Shares: " << shares;
				}

				if (g->is_relative ()) {
					out << " | Relative";
				}

				/* Member routes */
				std::shared_ptr<RouteList> rl = g->route_list ();
				out << " | " << rl->size () << " routes:";
				bool first = true;
				for (auto const& mr : *rl) {
					if (!first) out << ",";
					out << " " << mr->name ();
					first = false;
				}
				out << "\n";
			}
			out << "\n";
		}
	}

	/* Locations / markers */
	Locations* locs = session->locations ();
	if (locs) {
		Locations::LocationList ll (locs->list ());

		/* Section markers first */
		bool has_sections = false;
		for (auto const& loc : ll) {
			if (!loc->is_section ()) {
				continue;
			}
			if (!has_sections) {
				out << "Sections:\n";
				has_sections = true;
			}
			double sec_start = (double)loc->start_sample () / session->sample_rate ();
			double sec_end = (double)loc->end_sample () / session->sample_rate ();
			int s_mins = (int)(sec_start / 60.0);
			double s_rem = sec_start - (s_mins * 60.0);
			int e_mins = (int)(sec_end / 60.0);
			double e_rem = sec_end - (e_mins * 60.0);
			char s_buf[32], e_buf[32];
			snprintf (s_buf, sizeof(s_buf), "%d:%04.1f", s_mins, s_rem);
			snprintf (e_buf, sizeof(e_buf), "%d:%04.1f", e_mins, e_rem);
			out << "  - \"" << loc->name () << "\" " << s_buf << " - " << e_buf << "\n";
		}

		/* Regular markers (skip sections) */
		bool has_markers = false;
		for (auto const& loc : ll) {
			if (loc->is_session_range () || loc->is_auto_loop () || loc->is_auto_punch ()) {
				continue;
			}
			if (loc->is_section ()) {
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

static string
plugin_role_str (PluginInfoPtr const& pi)
{
	if (pi->is_instrument ()) {
		return "instrument";
	} else if (pi->is_analyzer ()) {
		return "analyzer";
	} else if (pi->is_utility ()) {
		return "utility";
	}
	return "effect";
}

string
CopilotContext::build_plugin_catalog ()
{
	PluginManager& mgr = PluginManager::instance ();

	struct PluginEntry {
		string name;
		string type;
		string role;
		string uri; /* LV2 only */
	};

	vector<PluginEntry> entries;

	auto collect = [&] (PluginInfoList const& list) {
		for (auto const& pi : list) {
			if (pi->is_internal ()) {
				continue;
			}
			PluginManager::PluginStatusType st = mgr.get_status (pi);
			if (st == PluginManager::Hidden || st == PluginManager::Concealed) {
				continue;
			}

			PluginEntry e;
			e.name = pi->name;
			e.type = PluginManager::plugin_type_name (pi->type, false);
			e.role = plugin_role_str (pi);
			if (pi->type == LV2) {
				e.uri = pi->unique_id;
			}
			entries.push_back (e);
		}
	};

	/* Collect from all plugin formats */
	collect (mgr.lv2_plugin_info ());
	collect (mgr.ladspa_plugin_info ());
	collect (mgr.lua_plugin_info ());

#ifdef AUDIOUNIT_SUPPORT
	collect (mgr.au_plugin_info ());
#endif
#ifdef WINDOWS_VST_SUPPORT
	collect (mgr.windows_vst_plugin_info ());
#endif
#ifdef LXVST_SUPPORT
	collect (mgr.lxvst_plugin_info ());
#endif
#ifdef MACVST_SUPPORT
	collect (mgr.mac_vst_plugin_info ());
#endif
#ifdef VST3_SUPPORT
	collect (mgr.vst3_plugin_info ());
#endif

	/* Sort by type then name */
	sort (entries.begin (), entries.end (), [] (PluginEntry const& a, PluginEntry const& b) {
		if (a.type != b.type) {
			return a.type < b.type;
		}
		return a.name < b.name;
	});

	/* Truncate if needed */
	const size_t max_entries = 400;
	bool truncated = entries.size () > max_entries;
	if (truncated) {
		entries.resize (max_entries);
	}

	/* Format output */
	ostringstream out;
	out << "Installed Plugins (" << entries.size ();
	if (truncated) {
		out << "+, list truncated";
	}
	out << "):\n";

	for (auto const& e : entries) {
		out << "  " << e.name << " | " << e.type << " | " << e.role;
		if (!e.uri.empty ()) {
			out << " | " << e.uri;
		}
		out << "\n";
	}

	return out.str ();
}
