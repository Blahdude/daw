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
#pragma once

#include <map>
#include <set>
#include <string>
#include <cstdint>

#include "pbd/id.h"

namespace ARDOUR {
	class Session;
}

class CopilotUndoRecord {
public:
	CopilotUndoRecord ();

	/** Capture controllable values, route IDs, and undo depth before execution */
	void snapshot (ARDOUR::Session*);

	/** Revert to captured state: undo native entries, restore controllable values, remove added routes */
	bool restore (ARDOUR::Session*);

	bool valid () const { return _valid; }
	void clear ();

	uint32_t undo_depth_before () const { return _undo_depth_before; }

	std::string description;
	uint32_t    native_undo_count;

private:
	bool _valid;
	std::map<PBD::ID, double> _ctrl_map;
	std::set<PBD::ID>         _route_ids;
	uint32_t                  _undo_depth_before;
};
