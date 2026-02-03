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

#include "pbd/controllable.h"

#include "ardour/automation_control.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "copilot_undo_record.h"

using namespace std;
using namespace PBD;
using namespace ARDOUR;

CopilotUndoRecord::CopilotUndoRecord ()
	: native_undo_count (0)
	, _valid (false)
	, _undo_depth_before (0)
{
}

void
CopilotUndoRecord::snapshot (Session* session)
{
	clear ();

	if (!session) {
		return;
	}

	/* Capture controllable values (same pattern as MixerScene::snapshot) */
	for (auto const& c : Controllable::registered_controllables ()) {
		if (!std::dynamic_pointer_cast<AutomationControl> (c)) {
			continue;
		}
		if (c->flags () & Controllable::Flag (Controllable::HiddenControl | Controllable::MonitorControl)) {
			continue;
		}
		_ctrl_map[c->id ()] = c->get_save_value ();
	}

	/* Capture existing route IDs */
	auto routes = session->get_routes ();
	for (auto const& r : *routes) {
		_route_ids.insert (r->id ());
	}

	/* Capture undo stack depth */
	_undo_depth_before = session->undo_depth ();

	_valid = true;
}

bool
CopilotUndoRecord::restore (Session* session)
{
	if (!_valid || !session) {
		return false;
	}

	/* 1. Undo any native undo entries created during execution */
	for (uint32_t i = 0; i < native_undo_count; ++i) {
		if (session->undo_depth () > 0) {
			session->undo (1);
		}
	}

	/* 2. Restore controllable values that differ from snapshot */
	for (auto const& entry : _ctrl_map) {
		auto c = Controllable::by_id (entry.first);
		if (!c) {
			continue;
		}
		double current = c->get_value ();
		if (current != entry.second) {
			c->set_value (entry.second, Controllable::NoGroup);
		}
	}

	/* 3. Remove routes that were added during execution */
	auto routes = session->get_routes ();
	std::shared_ptr<RouteList> to_remove (new RouteList);
	for (auto const& r : *routes) {
		if (_route_ids.find (r->id ()) == _route_ids.end ()) {
			to_remove->push_back (r);
		}
	}
	if (!to_remove->empty ()) {
		session->remove_routes (to_remove);
	}

	clear ();
	return true;
}

void
CopilotUndoRecord::clear ()
{
	_valid = false;
	_ctrl_map.clear ();
	_route_ids.clear ();
	_undo_depth_before = 0;
	native_undo_count = 0;
	description.clear ();
}
