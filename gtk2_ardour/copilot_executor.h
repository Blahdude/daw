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

#include <string>
#include <functional>

namespace ARDOUR {
	class Session;
}

class CopilotUndoRecord;

class CopilotExecutor {
public:
	CopilotExecutor ();

	/* Extract lua code blocks from a Claude response.
	 * Looks for ```lua ... ``` blocks.
	 * Returns concatenated code, or empty string if none found.
	 */
	std::string extract_lua_code (const std::string& response);

	/* Extract the explanation text (everything outside code blocks) */
	std::string extract_explanation (const std::string& response);

	/* Execute Lua code via LuaInstance's interactive state.
	 * Wraps in begin/commit_reversible_command for undo.
	 * on_output is called with any print() output from the script.
	 * Returns true on success, false on error (error_msg is set).
	 */
	bool execute (ARDOUR::Session* session,
	              const std::string& lua_code,
	              std::string& error_msg,
	              std::function<void(const std::string&)> on_output);

	/** Execute with undo record: snapshots before, detects native undo entries after.
	 *  On failure, restores session state immediately.
	 */
	bool execute (ARDOUR::Session* session,
	              const std::string& lua_code,
	              std::string& error_msg,
	              std::function<void(const std::string&)> on_output,
	              CopilotUndoRecord& undo_record);
};
