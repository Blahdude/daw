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
#include <vector>

#include <ytkmm/box.h>
#include <ytkmm/entry.h>
#include <ytkmm/label.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/textview.h>

#include "pbd/signals.h"

#include "ardour/ardour.h"
#include "ardour/session_handle.h"

#include "widgets/ardour_button.h"

#include "ardour_window.h"
#include "copilot_api.h"
#include "copilot_executor.h"

class CopilotWindow :
	public ArdourWindow,
	public PBD::ScopedConnectionList
{
public:
	static CopilotWindow* instance ();
	~CopilotWindow ();

	void set_session (ARDOUR::Session* s);

private:
	CopilotWindow ();
	static CopilotWindow* _instance;

	/* UI widgets */
	Gtk::TextView   _chat_view;
	Gtk::ScrolledWindow _chat_scroll;
	Gtk::Entry      _input_entry;
	ArdourWidgets::ArdourButton _send_button;
	Gtk::Label      _status_label;
	Gtk::HBox       _input_box;
	Gtk::VBox       _main_vbox;

	/* conversation history for multi-turn */
	struct Message {
		std::string role;    /* "user" or "assistant" */
		std::string content;
	};
	std::vector<Message> _conversation;

	/* components */
	CopilotApi      _api;
	CopilotExecutor _executor;

	/* state */
	bool _waiting_for_response;
	int  _retry_count;
	std::string _last_lua_code;
	std::string _last_user_message;

	/* methods */
	void session_going_away ();
	void update_title ();

	void on_send_clicked ();
	void on_entry_activated ();
	void send_message (const std::string& text);

	void on_api_response (const std::string& response);
	void on_api_error (const std::string& error);

	void append_chat (const std::string& sender, const std::string& text);
	void append_system (const std::string& text);
	void scroll_to_bottom ();
	void set_status (const std::string& status);

	void set_input_sensitive (bool sensitive);
};
