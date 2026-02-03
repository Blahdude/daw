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

#include <string>

#include "ardour/session.h"

#include "lua/luastate.h"
#include "LuaBridge/LuaBridge.h"

#include "ardour/luabindings.h"

#include "luainstance.h"
#include "public_editor.h"
#include "copilot_executor.h"
#include "ui_config.h"

using namespace std;
using namespace ARDOUR;

CopilotExecutor::CopilotExecutor ()
{
}

string
CopilotExecutor::extract_lua_code (const string& response)
{
	string code;
	string marker_start_lua = "```lua";
	string marker_start_plain = "```\n";
	string marker_end = "```";

	size_t pos = 0;
	while (pos < response.size ()) {
		/* Look for ```lua or ``` code block */
		size_t block_start = response.find (marker_start_lua, pos);
		bool found_lua = true;

		if (block_start == string::npos) {
			/* Try plain code block only if we haven't found any lua blocks yet */
			if (code.empty ()) {
				block_start = response.find (marker_start_plain, pos);
				found_lua = false;
			}
			if (block_start == string::npos) {
				break;
			}
		}

		/* Skip past the opening marker */
		size_t code_start;
		if (found_lua) {
			code_start = block_start + marker_start_lua.size ();
		} else {
			code_start = block_start + marker_start_plain.size ();
		}

		/* Skip to next line if there's content on the marker line */
		size_t nl = response.find ('\n', code_start);
		if (nl != string::npos && nl < code_start + 2) {
			code_start = nl + 1;
		} else if (nl != string::npos) {
			/* There may be language identifier after ```, skip the whole line */
			code_start = nl + 1;
		}

		/* Find closing ``` */
		size_t code_end = response.find (marker_end, code_start);
		if (code_end == string::npos) {
			/* Unclosed code block - take everything */
			code_end = response.size ();
		}

		string block = response.substr (code_start, code_end - code_start);

		/* Trim trailing whitespace */
		while (!block.empty () && (block.back () == '\n' || block.back () == '\r' || block.back () == ' ')) {
			block.pop_back ();
		}

		if (!block.empty ()) {
			if (!code.empty ()) {
				code += "\n\n";
			}
			code += block;
		}

		pos = code_end + marker_end.size ();
	}

	return code;
}

string
CopilotExecutor::extract_explanation (const string& response)
{
	string explanation;
	string marker_start_lua = "```lua";
	string marker_start_plain = "```";
	string marker_end = "```";

	size_t pos = 0;
	while (pos < response.size ()) {
		/* Find next code block */
		size_t block_start = response.find (marker_start_plain, pos);
		if (block_start == string::npos) {
			/* No more code blocks - rest is explanation */
			explanation += response.substr (pos);
			break;
		}

		/* Text before code block is explanation */
		if (block_start > pos) {
			explanation += response.substr (pos, block_start - pos);
		}

		/* Find end of code block */
		size_t inner_start = response.find ('\n', block_start);
		if (inner_start == string::npos) {
			break;
		}
		size_t block_end = response.find (marker_end, inner_start + 1);
		if (block_end == string::npos) {
			break;
		}

		pos = block_end + marker_end.size ();
	}

	/* Trim */
	while (!explanation.empty () && (explanation.front () == '\n' || explanation.front () == '\r')) {
		explanation.erase (explanation.begin ());
	}
	while (!explanation.empty () && (explanation.back () == '\n' || explanation.back () == '\r' || explanation.back () == ' ')) {
		explanation.pop_back ();
	}

	return explanation;
}

bool
CopilotExecutor::execute (Session* session,
                          const string& lua_code,
                          string& error_msg,
                          function<void(const string&)> on_output)
{
	if (!session) {
		error_msg = "No session loaded";
		return false;
	}

	if (lua_code.empty ()) {
		error_msg = "No Lua code to execute";
		return false;
	}

	/* Create a fresh LuaState for execution */
	LuaState lua (true, UIConfiguration::instance().get_sandbox_all_lua_scripts ());

	/* Connect print output */
	if (on_output) {
		lua.Print.connect (on_output);
	}

	/* Register Ardour classes */
	lua_State* L = lua.getState ();
	LuaInstance::register_classes (L, UIConfiguration::instance().get_sandbox_all_lua_scripts ());

	/* Set up session binding */
	LuaBindings::set_session (L, session);

	/* Push Editor reference */
	luabridge::push<PublicEditor*> (L, &PublicEditor::instance ());
	lua_setglobal (L, "Editor");

	try {
		lua.do_command ("function ardour () end");
		int result = lua.do_command (lua_code);
		if (result != 0) {
			error_msg = "Lua execution failed";
			return false;
		}
	} catch (luabridge::LuaException const& e) {
		error_msg = string ("Lua error: ") + e.what ();
		return false;
	} catch (std::exception const& e) {
		error_msg = string ("Error: ") + e.what ();
		return false;
	} catch (...) {
		error_msg = "Unknown error during Lua execution";
		return false;
	}

	return true;
}
