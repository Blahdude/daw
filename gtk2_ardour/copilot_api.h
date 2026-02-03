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
#include <functional>
#include <atomic>

#include <pthread.h>
#include <glibmm/threads.h>
#include <sigc++/connection.h>

struct CopilotMessage {
	std::string role;    /* "user" or "assistant" */
	std::string content;
};

class CopilotApi {
public:
	CopilotApi ();
	~CopilotApi ();

	/* Load API key from env var or config file.
	 * Returns true if a key was found.
	 */
	bool load_api_key ();
	bool has_api_key () const { return !_api_key.empty (); }

	/* Send a request to the Anthropic API in a background thread.
	 * system_prompt: the system prompt text
	 * messages: conversation history
	 * The on_response / on_error callbacks are called in the GUI thread.
	 */
	void send_request (
		const std::string& system_prompt,
		const std::vector<CopilotMessage>& messages,
		std::function<void(const std::string&)> on_response,
		std::function<void(const std::string&)> on_error,
		std::function<void(const std::string&)> on_stream_delta = nullptr
	);

	/* Cancel any in-flight request */
	void cancel ();

	/* Check if a request is in progress */
	bool busy () const { return _busy.load (); }

private:
	std::string _api_key;
	std::atomic<bool> _busy;
	std::atomic<bool> _cancel;

	/* background thread */
	pthread_t _thread;
	bool _thread_active;

	/* data passed to/from the background thread */
	std::string _request_system_prompt;
	std::vector<CopilotMessage> _request_messages;
	std::string _response_text;
	std::string _error_text;

	/* callbacks (stored, invoked on GUI thread) */
	std::function<void(const std::string&)> _on_response;
	std::function<void(const std::string&)> _on_error;
	std::function<void(const std::string&)> _on_stream_delta;

	/* streaming support */
	bool _streaming;

	Glib::Threads::Mutex _stream_mutex;
	std::string _stream_pending;        /* text waiting to be delivered to GUI */
	std::string _stream_accumulated;    /* full response text so far */
	std::string _stream_raw_response;   /* raw bytes for error extraction */
	std::string _sse_line_buffer;       /* partial SSE line buffer */

	sigc::connection _stream_timer_connection;
	bool on_stream_timer ();

	static size_t stream_write_callback (void* ptr, size_t size, size_t nmemb, void* data);
	void parse_sse_chunk (const char* data, size_t size);
	std::string extract_sse_text_delta (const char* json_data, size_t json_size);

	/* idle callback to deliver result on GUI thread */
	bool on_idle_deliver_result ();

	/* thread entry point */
	static void* thread_func (void* arg);
	void do_request ();

	/* helpers */
	std::string build_json_payload (const std::string& system_prompt,
	                                const std::vector<CopilotMessage>& messages);
	std::string escape_json (const std::string& s);
	std::string extract_response_text (const char* json_data, size_t json_size);
	std::string extract_error_message (const char* json_data, size_t json_size);
};
