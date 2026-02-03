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

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <curl/curl.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <sigc++/sigc++.h>

#include "pbd/compose.h"
#include "pbd/error.h"
#include "pbd/ccurl.h"

#include "ardour/filesystem_paths.h"

#include "copilot_api.h"

using namespace std;

static string find_json_string (const char* json, size_t len, const string& key);

static size_t
write_callback (void* ptr, size_t size, size_t nmemb, void* data)
{
	string* response = static_cast<string*> (data);
	size_t realsize = size * nmemb;
	response->append (static_cast<char*> (ptr), realsize);
	return realsize;
}

CopilotApi::CopilotApi ()
	: _busy (false)
	, _cancel (false)
	, _thread_active (false)
	, _streaming (false)
{
}

CopilotApi::~CopilotApi ()
{
	cancel ();
	if (_thread_active) {
		pthread_join (_thread, NULL);
		_thread_active = false;
	}
}

bool
CopilotApi::load_api_key ()
{
	/* Try environment variable first */
	const char* env_key = getenv ("ANTHROPIC_API_KEY");
	if (env_key && strlen (env_key) > 0) {
		_api_key = env_key;
		return true;
	}

	/* Try config file: ~/.config/ardour9/anthropic_api_key */
	string config_dir = ARDOUR::user_config_directory ();
	string key_path = Glib::build_filename (config_dir, "anthropic_api_key");

	ifstream ifs (key_path.c_str ());
	if (ifs.is_open ()) {
		getline (ifs, _api_key);
		/* trim whitespace */
		while (!_api_key.empty () && (_api_key.back () == '\n' || _api_key.back () == '\r' || _api_key.back () == ' ')) {
			_api_key.pop_back ();
		}
		if (!_api_key.empty ()) {
			return true;
		}
	}

	return false;
}

void
CopilotApi::send_request (
	const string& system_prompt,
	const vector<CopilotMessage>& messages,
	function<void(const string&)> on_response,
	function<void(const string&)> on_error,
	function<void(const string&)> on_stream_delta)
{
	if (_busy.load ()) {
		if (on_error) {
			on_error ("A request is already in progress");
		}
		return;
	}

	/* Wait for any previous thread to finish */
	if (_thread_active) {
		pthread_join (_thread, NULL);
		_thread_active = false;
	}

	_request_system_prompt = system_prompt;
	_request_messages = messages;
	_on_response = on_response;
	_on_error = on_error;
	_on_stream_delta = on_stream_delta;
	_streaming = (on_stream_delta != nullptr);
	_response_text.clear ();
	_error_text.clear ();
	_stream_pending.clear ();
	_stream_accumulated.clear ();
	_stream_raw_response.clear ();
	_sse_line_buffer.clear ();
	_cancel.store (false);
	_busy.store (true);

	int rv = pthread_create (&_thread, NULL, thread_func, this);
	if (rv != 0) {
		_busy.store (false);
		if (on_error) {
			on_error ("Failed to create background thread");
		}
		return;
	}
	_thread_active = true;
}

void
CopilotApi::cancel ()
{
	_cancel.store (true);
}

void*
CopilotApi::thread_func (void* arg)
{
	CopilotApi* self = static_cast<CopilotApi*> (arg);
	self->do_request ();
	return NULL;
}

size_t
CopilotApi::stream_write_callback (void* ptr, size_t size, size_t nmemb, void* data)
{
	CopilotApi* self = static_cast<CopilotApi*> (data);
	size_t realsize = size * nmemb;

	if (self->_cancel.load ()) {
		return 0; /* abort transfer */
	}

	self->_stream_raw_response.append (static_cast<char*> (ptr), realsize);
	self->parse_sse_chunk (static_cast<char*> (ptr), realsize);

	return realsize;
}

void
CopilotApi::parse_sse_chunk (const char* data, size_t size)
{
	_sse_line_buffer.append (data, size);

	size_t pos = 0;
	while (pos < _sse_line_buffer.size ()) {
		size_t nl = _sse_line_buffer.find ('\n', pos);
		if (nl == string::npos) {
			break; /* incomplete line, wait for more data */
		}

		string line = _sse_line_buffer.substr (pos, nl - pos);
		pos = nl + 1;

		/* strip trailing \r */
		if (!line.empty () && line.back () == '\r') {
			line.pop_back ();
		}

		/* SSE format: lines starting with "data: " carry JSON payload */
		if (line.compare (0, 6, "data: ") == 0) {
			string json_str = line.substr (6);

			/* check for stream end marker */
			if (json_str == "[DONE]") {
				continue;
			}

			string delta = extract_sse_text_delta (json_str.c_str (), json_str.size ());
			if (!delta.empty ()) {
				Glib::Threads::Mutex::Lock lm (_stream_mutex);
				_stream_pending += delta;
				_stream_accumulated += delta;
			}
		} else if (line.compare (0, 7, "event: ") == 0) {
			string event_type = line.substr (7);
			if (event_type == "error") {
				/* next data line will contain error JSON */
			}
		}
	}

	/* keep any incomplete trailing line */
	if (pos < _sse_line_buffer.size ()) {
		_sse_line_buffer = _sse_line_buffer.substr (pos);
	} else {
		_sse_line_buffer.clear ();
	}
}

string
CopilotApi::extract_sse_text_delta (const char* json_data, size_t json_size)
{
	/* SSE data for text deltas:
	 * {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"token"}}
	 */
	string type = find_json_string (json_data, json_size, "type");
	if (type == "content_block_delta") {
		return find_json_string (json_data, json_size, "text");
	}
	return "";
}

bool
CopilotApi::on_stream_timer ()
{
	string chunk;
	{
		Glib::Threads::Mutex::Lock lm (_stream_mutex);
		chunk.swap (_stream_pending);
	}

	if (!chunk.empty () && _on_stream_delta) {
		_on_stream_delta (chunk);
	}

	return _busy.load (); /* auto-disconnect when request completes */
}

void
CopilotApi::do_request ()
{
	string json_payload = build_json_payload (_request_system_prompt, _request_messages);

	PBD::CCurl ccurl;
	CURL* curl = ccurl.curl ();

	if (!curl) {
		_error_text = "Failed to initialize curl";
		_busy.store (false);
		Glib::signal_idle ().connect (sigc::mem_fun (*this, &CopilotApi::on_idle_deliver_result));
		return;
	}

	string response_data;
	struct curl_slist* headers = NULL;

	headers = curl_slist_append (headers, ("x-api-key: " + _api_key).c_str ());
	headers = curl_slist_append (headers, "anthropic-version: 2023-06-01");
	headers = curl_slist_append (headers, "content-type: application/json");

	curl_easy_setopt (curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
	curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt (curl, CURLOPT_POSTFIELDS, json_payload.c_str ());
	curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, (long)json_payload.size ());

	if (_streaming) {
		curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
		curl_easy_setopt (curl, CURLOPT_WRITEDATA, this);
		curl_easy_setopt (curl, CURLOPT_TIMEOUT, 0L);
		curl_easy_setopt (curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt (curl, CURLOPT_LOW_SPEED_TIME, 30L);

		/* start GUI-thread timer for delivering stream deltas */
		_stream_timer_connection = Glib::signal_timeout ().connect (
			sigc::mem_fun (*this, &CopilotApi::on_stream_timer), 50);
	} else {
		curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt (curl, CURLOPT_WRITEDATA, &response_data);
		curl_easy_setopt (curl, CURLOPT_TIMEOUT, 60L);
	}

	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 10L);

	PBD::CCurl::ca_setopt (curl);

	CURLcode res = curl_easy_perform (curl);

	curl_slist_free_all (headers);

	if (_streaming) {
		_stream_timer_connection.disconnect ();
	}

	if (_cancel.load ()) {
		_error_text = "Request cancelled";
		_busy.store (false);
		Glib::signal_idle ().connect (sigc::mem_fun (*this, &CopilotApi::on_idle_deliver_result));
		return;
	}

	if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
		_error_text = string_compose ("Network error: %1", curl_easy_strerror (res));
		_busy.store (false);
		Glib::signal_idle ().connect (sigc::mem_fun (*this, &CopilotApi::on_idle_deliver_result));
		return;
	}

	/* CURLE_WRITE_ERROR may come from stream_write_callback returning 0 on cancel */
	if (res == CURLE_WRITE_ERROR && _cancel.load ()) {
		_error_text = "Request cancelled";
		_busy.store (false);
		Glib::signal_idle ().connect (sigc::mem_fun (*this, &CopilotApi::on_idle_deliver_result));
		return;
	}

	long http_status = 0;
	curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_status);

	if (http_status != 200) {
		const string& raw = _streaming ? _stream_raw_response : response_data;
		string api_err = extract_error_message (raw.c_str (), raw.size ());
		if (api_err.empty ()) {
			_error_text = string_compose ("API error (HTTP %1)", http_status);
		} else {
			_error_text = string_compose ("API error (HTTP %1): %2", http_status, api_err);
		}
		_busy.store (false);
		Glib::signal_idle ().connect (sigc::mem_fun (*this, &CopilotApi::on_idle_deliver_result));
		return;
	}

	if (_streaming) {
		_response_text = _stream_accumulated;
		if (_response_text.empty () && !_error_text.empty ()) {
			/* error was set during SSE parsing */
		} else if (_response_text.empty ()) {
			_error_text = "Failed to parse streaming API response";
		}
	} else {
		_response_text = extract_response_text (response_data.c_str (), response_data.size ());
		if (_response_text.empty ()) {
			_error_text = "Failed to parse API response";
		}
	}

	_busy.store (false);
	Glib::signal_idle ().connect (sigc::mem_fun (*this, &CopilotApi::on_idle_deliver_result));
}

bool
CopilotApi::on_idle_deliver_result ()
{
	if (!_error_text.empty ()) {
		if (_on_error) {
			_on_error (_error_text);
		}
	} else {
		if (_on_response) {
			_on_response (_response_text);
		}
	}
	return false; /* don't call again */
}

string
CopilotApi::escape_json (const string& s)
{
	string result;
	result.reserve (s.size () + 16);
	for (size_t i = 0; i < s.size (); ++i) {
		char c = s[i];
		switch (c) {
		case '"':  result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		case '\b': result += "\\b"; break;
		case '\f': result += "\\f"; break;
		default:
			if ((unsigned char)c < 0x20) {
				char buf[8];
				snprintf (buf, sizeof(buf), "\\u%04x", (unsigned int)(unsigned char)c);
				result += buf;
			} else {
				result += c;
			}
			break;
		}
	}
	return result;
}

string
CopilotApi::build_json_payload (const string& system_prompt,
                                const vector<CopilotMessage>& messages)
{
	ostringstream json;
	json << "{";
	json << "\"model\":\"claude-sonnet-4-20250514\",";
	json << "\"max_tokens\":4096,";
	if (_streaming) {
		json << "\"stream\":true,";
	}
	json << "\"system\":\"" << escape_json (system_prompt) << "\",";
	json << "\"messages\":[";

	for (size_t i = 0; i < messages.size (); ++i) {
		if (i > 0) {
			json << ",";
		}
		json << "{\"role\":\"" << escape_json (messages[i].role)
		     << "\",\"content\":\"" << escape_json (messages[i].content) << "\"}";
	}

	json << "]}";
	return json.str ();
}

/* Simple JSON value extractor - finds a string value for a given key.
 * This is intentionally simple and avoids a JSON library dependency.
 */
static string
find_json_string (const char* json, size_t len, const string& key)
{
	string search = "\"" + key + "\"";
	const char* pos = json;
	const char* end = json + len;

	while (pos < end) {
		const char* found = strstr (pos, search.c_str ());
		if (!found) {
			break;
		}

		/* Skip past key and find colon */
		const char* p = found + search.size ();
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
		if (p >= end || *p != ':') {
			pos = found + 1;
			continue;
		}
		++p;

		/* Skip whitespace */
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
		if (p >= end || *p != '"') {
			pos = found + 1;
			continue;
		}
		++p; /* skip opening quote */

		/* Extract string value */
		string value;
		while (p < end && *p != '"') {
			if (*p == '\\' && (p + 1) < end) {
				++p;
				switch (*p) {
				case '"':  value += '"'; break;
				case '\\': value += '\\'; break;
				case 'n':  value += '\n'; break;
				case 'r':  value += '\r'; break;
				case 't':  value += '\t'; break;
				case '/':  value += '/'; break;
				default:   value += '\\'; value += *p; break;
				}
			} else {
				value += *p;
			}
			++p;
		}
		return value;
	}
	return "";
}

string
CopilotApi::extract_response_text (const char* json_data, size_t json_size)
{
	/* The Anthropic Messages API returns:
	 * { "content": [ { "type": "text", "text": "..." } ] }
	 * We need to find the "text" field inside the content array.
	 */
	return find_json_string (json_data, json_size, "text");
}

string
CopilotApi::extract_error_message (const char* json_data, size_t json_size)
{
	/* Error responses have: { "error": { "message": "..." } } */
	return find_json_string (json_data, json_size, "message");
}
