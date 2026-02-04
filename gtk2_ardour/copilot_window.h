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

#include <fstream>
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
#include "copilot_undo_record.h"

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
	ArdourWidgets::ArdourButton _cancel_button;
	ArdourWidgets::ArdourButton _undo_button;
	Gtk::Label      _status_label;
	Gtk::HBox       _input_box;
	Gtk::HBox       _status_box;
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

	/* streaming display state */
	bool _streaming_active;
	Glib::RefPtr<Gtk::TextBuffer::Mark> _stream_end_mark;
	void on_stream_delta (const std::string& text);

	/* context window management */
	static const size_t _chars_per_token = 4;
	static const size_t _max_input_tokens = 100000;
	static const size_t _prune_target_tokens = 80000;
	static const size_t _min_keep_pairs = 2;

	size_t estimate_tokens (const std::string& text) const;
	size_t estimate_conversation_tokens () const;
	void   prune_conversation ();
	std::vector<CopilotMessage> build_api_messages () const;

	/* multi-step workflow state */
	bool        _in_workflow;
	int         _workflow_step;
	int         _workflow_max_steps;
	bool        _workflow_cancelled;

	/* plugin catalog */
	std::string _plugin_catalog;
	PBD::ScopedConnection _plugin_list_connection;
	void rebuild_plugin_catalog ();

	/* undo state */
	CopilotUndoRecord _last_undo_record;
	PBD::ScopedConnection _undo_history_connection;

	/* log file */
	std::ofstream _log_file;
	void open_log_file ();
	void log_write (const std::string& tag, const std::string& text);

	/* methods */
	void session_going_away ();
	void update_title ();

	void on_send_clicked ();
	void on_cancel_clicked ();
	void on_entry_activated ();
	void send_message (const std::string& text);

	void on_api_response (const std::string& response);
	void on_api_error (const std::string& error);

	void continue_workflow (bool success, const std::string& result_summary);
	void finish_workflow (const std::string& reason);

	void append_chat (const std::string& sender, const std::string& text);
	void append_system (const std::string& text);
	void scroll_to_bottom ();
	void set_status (const std::string& status);

	void set_input_sensitive (bool sensitive);

	/* undo methods */
	void on_undo_clicked ();
	void on_undo_history_changed ();
	void update_undo_button ();
	bool is_undo_request (const std::string& text) const;
	void perform_undo ();
};
