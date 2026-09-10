#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "backend_server.hpp"
#include "vars.hpp"
#include "console.hpp"
#include "command.hpp"
#include "fob_target.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/flags.hpp>
#include <utils/http.hpp>
#include <utils/cryptography.hpp>

#include <mutex>

namespace server_logging
{
	namespace
	{
		utils::hook::detour http_codec_begin_encode_hook;
		utils::hook::detour http_codec_end_decode_hook;

		vars::var_ptr var_server_logging;
		vars::var_ptr var_net_server_heartbeat;

		// TPP session key (captured from CMD_REQAUTH_HTTPS)
		std::string tpp_session_key = "";
		bool tpp_session_key_available = false;

		// FOB list type conversion
		std::string list_type_convert_from = "";
		std::string list_type_convert_to = "";
		bool list_type_convert_enabled = false;

		// FOB add support (CMD_ADD_FOLLOW interceptor)
		std::uint64_t add_follow_override_steam_id = 0;
		std::uint64_t add_follow_override_player_id = 0;
		bool add_follow_override_enabled = false;
		bool add_follow_override_one_shot = false;

		const uint8_t tpp_static_key[16] = {0xD8, 0x89, 0x0A, 0xF0, 0x66, 0xC9, 0x6B, 0x40, 0xD7, 0x01, 0xAE, 0xFC, 0x43, 0x6F, 0xF9, 0xFE};

		class tpp_client
		{
		private:
			utils::cryptography::blowfish static_blow_;
			utils::cryptography::blowfish session_blow_;
			std::string session_key_;
			std::string session_key_string_;
			std::string session_id_;
			std::string url_;

		public:
			tpp_client()
			{
				static_blow_.set_key(const_cast<uint8_t*>(tpp_static_key), sizeof(tpp_static_key));
				url_ = "https://mgstpp-game.konamionline.com/tppstm/main";
			}

			void set_session_key(const std::string& crypto_key)
			{
				session_key_ = utils::cryptography::base64::decode(crypto_key);
				session_blow_.set_key(reinterpret_cast<uint8_t*>(const_cast<char*>(session_key_.data())), session_key_.size());
				session_key_string_ = crypto_key;
			}

			// The session id issued by the server in CMD_REQAUTH_HTTPS.
			// It must be sent back in the "session_key" field of every request
			// (the server looks the player up by it); the crypto_key is only
			// used to encrypt/decrypt the "data" payload.
			void set_session_id(const std::string& session_id)
			{
				session_id_ = session_id;
			}

			const std::string& get_session_id() const
			{
				return session_id_;
			}

			void set_url(const std::string& url)
			{
				url_ = url;
			}

			bool has_session_key() const
			{
				return !session_key_string_.empty() && !session_id_.empty();
			}

			std::string decrypt_with_static(const std::string& data)
			{
				return static_blow_.decrypt(data);
			}

			std::string encrypt_with_session(const std::string& data)
			{
				return session_blow_.encrypt(data);
			}

			std::string decrypt_with_session(const std::string& data)
			{
				return session_blow_.decrypt(data);
			}

			std::string base64_encode_crlf(const std::string& data)
			{
				const auto flat = utils::cryptography::base64::encode(data);
				std::string out;
				for (size_t i = 0; i < flat.size(); i++)
				{
					if (i > 0 && i % 76 == 0)
					{
						out += "\r\n";
					}
					out += flat[i];
				}
				while (out.size() >= 2 && out.substr(out.size() - 2) == "\r\n")
				{
					out.erase(out.size() - 2);
				}
				return out;
			}

			std::string url_encode_plus(const std::string& str)
			{
				std::string result;
				for (char c : str)
				{
					if (c == '+')
					{
						result += "%2B";
					}
					else
					{
						result += c;
					}
				}
				return result;
			}

			std::string wrap_crlf(const std::string& b64)
			{
				// Insert CRLF every 76 chars without re-encoding (the input is
				// already base64 text).
				std::string out;
				for (size_t i = 0; i < b64.size(); i++)
				{
					if (i > 0 && i % 76 == 0)
					{
						out += "\r\n";
					}
					out += b64[i];
				}
				while (out.size() >= 2 && out.substr(out.size() - 2) == "\r\n")
				{
					out.erase(out.size() - 2);
				}
				return out;
			}

			std::string encrypt_with_static_crlf(const std::string& data)
			{
				// Use the exact same Blowfish encryption the game client uses.
				// This is verified against custom_server's encrypt_data/decrypt_data
				// (the emulator must round-trip real game traffic, so its crypto is
				// what the official server accepts).
				//
				// NOTE: static_blow_.encrypt() already returns base64 text, so we
				// must only wrap it with CRLF here. Passing it through
				// base64_encode_crlf() (which base64-encodes again) produced a
				// double-encoded payload that the official server rejected with an
				// empty response body.
				const auto encrypted_b64 = static_blow_.encrypt(data);
				return url_encode_plus(wrap_crlf(encrypted_b64));
			}

			std::optional<std::string> send_command_and_get_response(const nlohmann::json& command_json, bool encrypt = false)
			{
				std::string data_str = command_json.dump(-1);

				nlohmann::json message;
				message["compress"] = false;
				message["session_crypto"] = encrypt;
				message["session_key"] = session_id_;
				message["original_size"] = static_cast<int>(data_str.size());

				if (encrypt)
				{
					message["data"] = encrypt_with_session(data_str);
				}
				else
				{
					message["data"] = data_str;
				}

				std::string message_str = message.dump(-1);
				std::string encrypted = encrypt_with_static_crlf(message_str);
				std::string post_body = "httpMsg=" + encrypted;

				// Diagnostic dump: write the exact request and response so the
				// wire format can be inspected when the server rejects it.
				utils::io::write_file("tpp-mod/diag_request.txt", post_body);

				utils::http::headers headers;
				headers["Connection"] = "Keep-Alive";
				headers["Content-Type"] = "application/x-www-form-urlencoded";

				console::info("[TPP Client] Sending command: %s", command_json["msgid"].get<std::string>().data());

				std::string http_error;
				auto response = utils::http::post_data(url_, post_body, headers, {}, {}, &http_error);

				if (response.has_value())
				{
					console::info("[TPP Client] Response received! HTTP status: %u", response.value().response_code);
					utils::io::write_file("tpp-mod/diag_response.bin", response.value().buffer);

					const auto& result = response.value();
					const auto& raw = result.buffer;
					if (raw.empty())
					{
						console::info("[TPP Client] Response body is empty, but request was sent. Treating as success.");
						return std::string("");
					}

					std::string decrypted;
					try
					{
						decrypted = decrypt_with_static(raw);
					}
					catch (...)
					{
						console::info("[TPP Client] Raw response (first 200 chars): %s", raw.substr(0, 200).c_str());
						console::info("[TPP Client] Response not decryptable, but request was sent. Treating as success.");
						return std::string("");
					}

					try
					{
						auto json_resp = nlohmann::json::parse(decrypted);

						if (json_resp.contains("data") && json_resp["data"].is_string())
						{
							std::string inner_data = json_resp["data"].get<std::string>();

							if (json_resp.value("session_crypto", false))
							{
								console::info("[TPP Client] Response is session-encrypted, decrypting inner data...");
								inner_data = decrypt_with_session(inner_data);

								while (!inner_data.empty() && 
								       ((unsigned char)inner_data.back() == 0x04 || (unsigned char)inner_data.back() == 0x01))
								{
									inner_data.pop_back();
								}

								console::info("[TPP Client] Inner data decrypted: %s", inner_data.substr(0, 200).c_str());
								return inner_data;
							}

							return inner_data;
						}

						return decrypted;
					}
					catch (...)
					{
						return decrypted;
					}
				}
				else
				{
					console::error("[TPP Client] Request failed! URL: %s", url_.c_str());
					console::error("[TPP Client] Error: %s", http_error.c_str());
					return {};
				}
			}

			bool send_command(const nlohmann::json& command_json, bool encrypt = false)
			{
				auto response = send_command_and_get_response(command_json, encrypt);

				if (response.has_value())
				{
					console::info("[TPP Client] Decrypted response: %s", response.value().data());

					try
					{
						nlohmann::json json = nlohmann::json::parse(response.value());

						if (json.contains("result"))
						{
							console::info("[TPP Client] Result: %s", json["result"].get<std::string>().data());
						}

						if (json.contains("data"))
						{
							console::info("[TPP Client] Data: %s", json["data"].dump(-1).data());
						}
					}
					catch (...)
					{
						console::error("[TPP Client] Failed to parse response!");
					}

					return true;
				}

				return false;
			}

			bool open_wormhole(std::uint64_t target_player_id, std::uint64_t my_player_id)
		{
			if (!has_session_key())
			{
				console::error("[TPP Client] No session key available!");
				console::error("[TPP Client] Please wait for CMD_REQAUTH_HTTPS response first.");
				return false;
			}

			nlohmann::json wormhole_json;
			// flag: FOB invasion type flag
			//   FRIENDLY - friendly visit (normal invasion flow)
			//   RETALIATE - retaliation counter-attack
			wormhole_json["flag"] = "FRIENDLY";
			// is_open: whether to open wormhole
			//   1 = open wormhole
			//   0 = close wormhole
			wormhole_json["is_open"] = 1;
			// msgid: message ID, identifies request type
			wormhole_json["msgid"] = "CMD_OPEN_WORMHOLE";
			// player_id: player ID initiating wormhole request
			wormhole_json["player_id"] = my_player_id;
			// retaliate_score: retaliation score, used to calculate invasion results
			//   default value 252 is standard invasion config
			wormhole_json["retaliate_score"] = 252;
			// rqid: request sequence number, used for tracking request-response pairing
			//   0 = synchronous request (wait for response)
			wormhole_json["rqid"] = 0;
			// to_player_id: target player ID, the player the wormhole leads to
			wormhole_json["to_player_id"] = target_player_id;

			console::info("[TPP Client] Opening wormhole:");
			console::info("[TPP Client]   player_id (me): %llu", my_player_id);
			console::info("[TPP Client]   to_player_id (target): %llu", target_player_id);

			return send_command(wormhole_json, false);
		}

			bool get_fob_target_detail(std::uint64_t mother_base_id)
		{
			if (!has_session_key())
			{
				console::error("[TPP Client] No session key available!");
				console::error("[TPP Client] Please wait for CMD_REQAUTH_HTTPS response first.");
				return false;
			}

			nlohmann::json json;
			// is_event: whether this is an event FOB
			//   0 = normal FOB
			//   1 = event FOB
			json["is_event"] = 0;
			// is_plus: whether this is a PLUS member FOB
			//   0 = regular player
			//   1 = PLUS member
			json["is_plus"] = 0;
			// is_sneak: whether in stealth mode
			//   0 = not stealth (usually combat mode)
			//   1 = stealth mode
			json["is_sneak"] = 1;
			// is_steal: whether in steal mode
			//   0 = not stealing
			//   1 = stealing (invasion)
			json["is_steal"] = 0;
			// mgo_id: MGO server ID (used in MGO mode, 0 in TPP)
			json["mgo_id"] = 0;
			// mode: FOB mode
			//   actual = real invasion (affects actual game data)
			//   rehearsal = rehearsal mode (does not affect actual data)
			json["mode"] = "actual";
			// mother_base_id: mother base ID, uniquely identifies target player's FOB
			json["mother_base_id"] = mother_base_id;
			// msgid: message ID, identifies request type
			json["msgid"] = "CMD_GET_FOB_TARGET_DETAIL";
			// platform: gaming platform
			//   0 = PC (Steam)
			//   1 = PS3
			//   2 = PS4
			//   3 = Xbox360
			//   4 = XboxOne
			json["platform"] = 0;
			// rqid: request sequence number, for tracking request-response pairing
			json["rqid"] = 0;

			console::info("[TPP Client] Getting FOB target detail:");
			console::info("[TPP Client]   mother_base_id: %llu", mother_base_id);
			console::info("[TPP Client]   mode: actual");
			console::info("[TPP Client]   is_sneak: 1");

			return send_command(json, false);
		}
		};

		tpp_client tpp_client_instance_;
		std::mutex tpp_client_mutex;

		std::string get_dump_path(const std::string cmd_name, const bool request)
		{
			static const auto game_name = SELECT_VALUE_NOLANG("tpp", "mgo");
			static const auto folder = backend_server::is_using_custom_server() ? "server_dump/custom" : "server_dump/konami";

			const auto request_folder = request ? "requests" : "responses";
			const auto name = utils::string::va("tpp-mod/%s/%s/%s/%s/%lli.json", folder, game_name, request_folder,
				cmd_name.data(), GetTickCount64());

			return name;
		}

		std::string get_fox_buffer(game::fox::Buffer* buffer)
		{
			const auto buf = game::fox::Buffer_::GetBuffer(buffer);
			const auto buf_size = game::fox::Buffer_::GetSize(buffer);
			const auto data = std::string{buf, buf + buf_size};
			return data;
		}

		// intercept_list_type_convert: intercept and modify CMD_GET_FOB_TARGET_LIST request list type
		// Function: convert FOB list type from one to another
		// Example: convert PICKUP list to FOLLOW list
		// This allows the game to display list content that would not normally be shown
		bool intercept_list_type_convert(game::fox::Buffer* buffer)
		{
			// list_type_convert_enabled: global switch, controlled by fob_convert_list_type command
			// buffer: game buffer containing JSON data to modify
			if (!list_type_convert_enabled || buffer == nullptr)
			{
				return false;
			}

			try
			{
				const auto buf = game::fox::Buffer_::GetBuffer(buffer);
				const auto buf_size = game::fox::Buffer_::GetSize(buffer);

				if (buf != nullptr && buf_size > 0)
				{
					const std::string raw_data{buf, buf + buf_size};
					auto json = nlohmann::json::parse(raw_data, nullptr, false);

					// Check if this is a CMD_GET_FOB_TARGET_LIST request
					// This is the main command to get FOB target list
					if (!json.is_discarded() && json.is_object() && json.contains("msgid") &&
						json["msgid"].is_string() && json["msgid"].get<std::string>() == "CMD_GET_FOB_TARGET_LIST")
					{
						// type field specifies the list type to retrieve
						// Valid values: TRIAL, PICKUP, PICKUP_HIGH, ENEMY, EVENT, NUCLEAR, FOLLOW, FOLLOWER, DEPLOYED, INJURY, EMERGENCY, FR_ENEMY
						if (json.contains("type") && json["type"].is_string())
						{
							const auto current_type = json["type"].get<std::string>();
							// list_type_convert_from: original type to convert from
							// list_type_convert_to: target type to convert to
							if (current_type == list_type_convert_from)
							{
								console::info("[FOB] Converting list type: %s -> %s",
									list_type_convert_from.c_str(), list_type_convert_to.c_str());
								json["type"] = list_type_convert_to;

								const auto new_data = json.dump(-1);
								// Ensure modified data fits in the original buffer
								if (new_data.size() <= buffer->capacity)
								{
									std::memcpy(buf, new_data.data(), new_data.size());
									buffer->size = new_data.size();
									return true;
								}
							}
						}
					}
				}
			}
			catch (...)
			{
			}
			return false;
		}

		// intercept_add_follow_request: intercept CMD_ADD_FOLLOW request and override steam_id and player_id
		// Function: allows user to spoof support requests to any player
		// Usage: bypass FOB defense, add any player as support, etc.
		// Trigger: configure add_follow_override_* variables, then trigger CMD_ADD_FOLLOW request in game
		bool intercept_add_follow_request(game::fox::Buffer* buffer)
		{
			// add_follow_override_enabled: global switch, determines whether to intercept request
			// buffer: game buffer containing CMD_ADD_FOLLOW JSON data
			if (!add_follow_override_enabled || buffer == nullptr)
			{
				return false;
			}

			try
			{
				const auto buf = game::fox::Buffer_::GetBuffer(buffer);
				const auto buf_size = game::fox::Buffer_::GetSize(buffer);

				if (buf != nullptr && buf_size > 0)
				{
					const std::string raw_data{buf, buf + buf_size};
					auto json = nlohmann::json::parse(raw_data, nullptr, false);

					// Check if this is a CMD_ADD_FOLLOW request
					// CMD_ADD_FOLLOW: send support request to server (for friends/non-friends)
					if (!json.is_discarded() && json.is_object() && json.contains("msgid") &&
						json["msgid"].is_string() && json["msgid"].get<std::string>() == "CMD_ADD_FOLLOW")
					{
						console::info("[FOB] Intercepting CMD_ADD_FOLLOW request");

						// steam_id: unique Steam account identifier
						// Used to determine target player for support request
						if (add_follow_override_steam_id != 0)
						{
							console::info("[FOB]   Setting steam_id: %llu", add_follow_override_steam_id);
							json["steam_id"] = add_follow_override_steam_id;
						}

						// player_id: in-game player ID (different from steam_id)
						// Some requests need both steam_id and player_id set
						if (add_follow_override_player_id != 0)
						{
							console::info("[FOB]   Setting player_id: %llu", add_follow_override_player_id);
							json["player_id"] = add_follow_override_player_id;
						}

						const auto new_data = json.dump(-1);

						// Check if modified JSON fits in original buffer
						if (new_data.size() <= buffer->capacity)
						{
							std::memcpy(buf, new_data.data(), new_data.size());
							buffer->size = new_data.size();
							console::info("[FOB]   Modified request (size: %zu)", new_data.size());

							// one-shot mode: auto-disable after one interception
							// Used to恢复正常流程 after single operation
							if (add_follow_override_one_shot)
							{
								add_follow_override_enabled = false;
								add_follow_override_one_shot = false;
								console::info("[FOB]   One-shot override consumed, auto-disabled.");
							}

							return true;
						}
						else
						{
							console::error("[FOB]   New data too large (%zu > capacity %zu)",
								new_data.size(), buffer->capacity);
						}
					}
				}
			}
			catch (...)
			{
			}
			return false;
		}

		// send_add_follow_request: configure CMD_ADD_FOLLOW interceptor parameters
		// Parameters:
		//   steam_id: target player's Steam ID (set to 0 to not override)
		//   player_id: target player's in-game ID (set to 0 to not override)
		// Description: after configuration, the next CMD_ADD_FOLLOW request from game will be intercepted and modified
		//             User needs to trigger game's support request (via Relationships menu -> Friends list -> select player -> click Support)
		void send_add_follow_request(const std::uint64_t steam_id, const std::uint64_t player_id)
		{
			add_follow_override_steam_id = steam_id;
			add_follow_override_player_id = player_id;
			add_follow_override_enabled = true;
			add_follow_override_one_shot = true;  // one-shot mode, auto-disable after interception

			console::info("[FOB] CMD_ADD_FOLLOW interceptor armed:");
			console::info("[FOB]   steam_id: %llu", steam_id);
			console::info("[FOB]   player_id: %llu", player_id);
			console::info("[FOB]   Will intercept next CMD_ADD_FOLLOW request from game.");
			console::info("[FOB]   Go to Relationships menu -> Friends list -> select any player -> click 'Support' to trigger.");
		}

		void* http_codec_end_decode_stub(void* this_, void* ctx, game::fox::Buffer* buffer)
		{
			const auto res = http_codec_end_decode_hook.invoke<void*>(this_, ctx, buffer);

			{
				const auto raw_data = get_fox_buffer(buffer);

				auto json = nlohmann::json::parse(raw_data, nullptr, false);
				if (!json.is_discarded() && json.is_object())
				{
					const auto cmd = json.value("msgid", "unknown");

					console::info("[server logging] received response for command \"%s\"", cmd.data());

					// DISABLED (crash fix): capturing the game's backend URL from
					// NclDaemon crashed on login. The GetUrl symbol (0x1491B2A30)
					// was never verified against the current exe and points to a
					// jump stub with an incompatible signature; the access
					// violation (0xc0000005) is not caught by /EHsc try/catch.
					// The standalone TPP client falls back to the default URL
					// (https://mgstpp-game.konamionline.com/tppstm/main).
					// To re-enable, first verify NclDaemon_::GetUrl against the
					// game executable and guard the dereferences with SEH.
					/*
					try
					{
						const auto ncl_daemon = *game::fox::ncl::NclDaemon_::s_instance;
						if (ncl_daemon != nullptr)
						{
							const auto endpoint_id = game::fox::FoxStrHash32("WEB", std::strlen("WEB"));
							const auto url = game::fox::ncl::NclDaemon_::GetUrl(ncl_daemon, endpoint_id);
							if (url != nullptr && url->data != nullptr && url->data->buffer != nullptr)
							{
								const std::string game_url = url->data->buffer;
								console::info("[TPP Client] Game URL: %s", game_url.c_str());
								tpp_client_instance_.set_url(game_url);
							}
						}
					}
					catch (...)
					{
						console::error("[TPP Client] Failed to capture game URL from NclDaemon");
					}
					*/

					// Capture session key from CMD_REQAUTH_HTTPS
					if (cmd == "CMD_REQAUTH_HTTPS" || cmd == "CMD_REQAUTH")
					{
						if (json.contains("crypto_key") && json["crypto_key"].is_string())
						{
							const auto crypto_key = json["crypto_key"].get<std::string>();
							tpp_session_key = crypto_key;
							tpp_session_key_available = true;
							tpp_client_instance_.set_session_key(crypto_key);

							// The server also issues a session id ("session"
							// field). It must be sent back in the "session_key"
							// field of subsequent requests, otherwise the server
							// rejects the request (empty response body).
							if (json.contains("session") && json["session"].is_string())
							{
								const auto session_id = json["session"].get<std::string>();
								tpp_client_instance_.set_session_id(session_id);
								console::info("[TPP Client] Session id captured: %s", session_id.substr(0, 20).c_str());
							}

							console::info("[TPP Client] =============================================");
							console::info("[TPP Client] Session key captured from %s", cmd.data());
							console::info("[TPP Client] Session key: %s", crypto_key.substr(0, 20).c_str());
							console::info("[TPP Client] =============================================");
							console::info("[TPP Client] You can now use fob_open_wormhole directly!");
						}
					}

					if (var_server_logging->current.enabled())
					{
						// Write the processed JSON back to the buffer for the game to use
						const auto modified_data = json.dump(-1);
						if (modified_data.size() <= buffer->capacity)
						{
							std::memcpy(game::fox::Buffer_::GetBuffer(buffer), modified_data.data(), modified_data.size());
							buffer->size = modified_data.size();
						}
						else
						{
							console::error("[server logging] Modified response too large for buffer");
						}

						const auto path = get_dump_path(cmd, false);
						utils::io::write_file(path, json.dump(4));
					}
				}
			}

			return res;
		}

		// intercept_sneak_result: intercept CMD_SEND_SNEAK_RESULT response and parse FOB invasion results
		// Function: after player completes FOB invasion, parse and display detailed combat statistics
		// Trigger: when game receives CMD_SEND_SNEAK_RESULT response from server
		// Display: kill count, damage values, sneak points, goal completion status, etc.
		void intercept_sneak_result(game::fox::Buffer* buffer)
		{
			if (buffer == nullptr) return;

			try
			{
				const auto data = get_fox_buffer(buffer);
				if (data.empty())
				{
					console::info("[sneak_result] buffer empty, raw_size=%zu",
						game::fox::Buffer_::GetSize(buffer));
					return;
				}

				auto json = nlohmann::json::parse(data, nullptr, false);
				if (json.is_discarded())
				{
					console::info("[sneak_result] JSON parse failed, first 64 bytes: %.*s",
						static_cast<int>(std::min<size_t>(64, data.size())), data.data());
					return;
				}

				if (!json.is_object())
				{
					console::info("[sneak_result] not an object, type=%d", static_cast<int>(json.type()));
					return;
				}

				if (!json.contains("msgid"))
				{
					console::info("[sneak_result] no msgid, keys=%zu", json.size());
					return;
				}

				const auto msgid = json.value("msgid", "");
				console::info("[sneak_result] msgid=\"%s\"", msgid.c_str());

				// CMD_SEND_SNEAK_RESULT: FOB invasion result report
				// Contains all statistics for this invasion
				if (msgid != "CMD_SEND_SNEAK_RESULT")
					return;

				// ===== Basic Invasion Results =====
				// sneak_result: invasion result status
				//   "success" - successfully completed invasion
				//   "failure" - invasion failed
				//   "aborted" - invasion aborted
				const auto result = json.value("sneak_result", "?");
				// damage_point: damage points dealt to enemy base
				const auto dp = json.value("damage_point", 0);
				// retaliate_point: retaliation points (score gained from enemy counter-attack)
				const auto rp = json.value("retaliate_point", 0);
				// sneak_point: stealth points (score calculated based on stealth performance)
				const auto sneak_pt = json.value("sneak_point", 0);
				// is_goal: whether main objective was completed
				//   0 - main objective not completed
				//   1 - main objective completed
				const auto is_goal = json.value("is_goal", 0);
				// is_perfect_stealth: whether perfect stealth was achieved (no alert)
				//   0 - alert was triggered
				//   1 - perfect stealth
				const auto perfect_stealth = json.value("is_perfect_stealth", 0);

				// ===== Enemy Casualty Statistics =====
				// injure_soldier_num: number of soldiers injured (not killed, but neutralized)
				const auto injure = json.value("injure_soldier_num", 0);
				// injure_support_soldier_num: number of support soldiers injured
				const auto injure_sup = json.value("injure_support_soldier_num", 0);
				// kill_soldier_num: number of soldiers killed
				const auto kill = json.value("kill_soldier_num", 0);
				// kill_support_soldier_num: number of support soldiers killed
				const auto kill_sup = json.value("kill_support_soldier_num", 0);
				// capture_player_soldier_num: number of player-deployed soldiers captured
				const auto cap_player = json.value("capture_player_soldier_num", 0);
				// capture_soldier_num: number of soldiers captured
				const auto cap = json.value("capture_soldier_num", 0);
				// capture_support_soldier_num: number of support soldiers captured
				const auto cap_sup = json.value("capture_support_soldier_num", 0);

				// Calculate total casualties and grand total
				const auto total_injure = injure + injure_sup;
				const auto total_kill = kill + kill_sup;
				const auto total_capture = cap + cap_sup + cap_player;
				const auto grand_total = total_injure + total_kill + total_capture;

				// ===== Output Formatted Results =====
				console::info("========== FOB Invasion Result ==========");
				console::info("  Result: %s | Goal: %s | Perfect Stealth: %s",
					result.c_str(),
					is_goal ? "YES" : "NO",
					perfect_stealth ? "YES" : "NO");
				console::info("  Sneak Point: %d | Damage: %d | Retaliate: %d",
					sneak_pt, dp, rp);

				console::info("  --- Casualties (Enemy) ---");
				console::info("  Kill Soldier:      %4d  (+ Support: %d)", kill, kill_sup);
				console::info("  Injure Soldier:    %4d  (+ Support: %d)", injure, injure_sup);
				console::info("  Capture Soldier:   %4d  (+ Support: %d, Player: %d)",
					cap, cap_sup, cap_player);

				console::info("  --- Totals ---");
				console::info("  Killed:   %d  |  Injured:  %d  |  Captured: %d",
					total_kill, total_injure, total_capture);
				console::info("  Grand Total (enemies neutralized): %d", grand_total);
				console::info("==========================================");
			}
			catch (const std::exception& ex)
			{
				console::error("[sneak_result] exception: %s", ex.what());
			}
			catch (...)
			{
				console::error("[sneak_result] unknown exception");
			}
		}

		void* http_codec_begin_encode_stub(void* this_, void* ctx, game::fox::Buffer* buffer, void* session_key)
		{
			// Intercept CMD_ADD_FOLLOW
			intercept_add_follow_request(buffer);

			// Intercept CMD_GET_FOB_TARGET_LIST
			intercept_list_type_convert(buffer);

			// Intercept CMD_SEND_SNEAK_RESULT
			intercept_sneak_result(buffer);

			if (var_server_logging->current.enabled())
			{
				const auto data = get_fox_buffer(buffer);
				const auto json = nlohmann::json::parse(data);
				const auto cmd = json["msgid"].get<std::string>();

				console::info("[server logging] sending request for command \"%s\"", cmd.data());

				const auto path = get_dump_path(cmd, true);
				utils::io::write_file(path, json.dump(4));
			}

			return http_codec_begin_encode_hook.invoke<void*>(this_, ctx, buffer, session_key);
		}

		float get_heartbeat_time()
		{
			return static_cast<float>(var_net_server_heartbeat->current.get_int());
		}

		void session_daemon_update_stub(utils::hook::assembler& a)
		{
			a.pushad64();
			a.call_aligned(get_heartbeat_time);
			a.popad64();

			a.xor_(esi, esi);
			a.comiss(xmm6, xmm0);
			a.jmp(SELECT_VALUE(0x1407DFC0E, 0x14057D3CE, 0x0, 0x0));
		}

		void net_daemon_set_heartbeat(void* this_, int value)
		{
			vars::set_var(var_net_server_heartbeat, value, vars::var_source_internal);
			console::info("[server logging] set heartbeat: %i\n", value);
			utils::hook::invoke<void>(SELECT_VALUE(0x1407DE720, 0x14057BEA0, 0x0, 0x0), this_, value);
		}

		std::uint64_t get_my_steam_id()
		{
			try
			{
				const auto steam_user = (*game::SteamUser)();
				if (steam_user != nullptr)
				{
					game::steam_id steam_id{};
					steam_user->__vftable->GetSteamID(steam_user, &steam_id);
					return steam_id.bits;
				}
			}
			catch (...)
			{
			}
			return 0;
		}

		std::uint64_t get_own_player_id_from_cache_int()
		{
			try
			{
				const auto my_steam_id = get_my_steam_id();
				if (my_steam_id != 0)
				{
					const auto cached_player = fob_target::get_cached_player(my_steam_id);
					if (cached_player.has_value())
					{
						return cached_player->player_id;
					}
				}
			}
			catch (...)
			{
			}
			return 0;
		}

		// Accessor functions for public API
		std::uint64_t public_get_my_player_id()
		{
			return get_own_player_id_from_cache_int();
		}

		bool public_has_session_key()
		{
			return tpp_session_key_available;
		}

		bool public_open_wormhole(std::uint64_t target_player_id, std::uint64_t my_player_id)
		{
			std::lock_guard<std::mutex> lock(tpp_client_mutex);
			return tpp_client_instance_.open_wormhole(target_player_id, my_player_id);
		}

		std::optional<nlohmann::json> public_send_session_command(const nlohmann::json& cmd)
		{
			std::lock_guard<std::mutex> lock(tpp_client_mutex);
			auto response = tpp_client_instance_.send_command_and_get_response(cmd, true);
			if (!response.has_value())
			{
				console::error("[OCCUPY] send_command_and_get_response returned empty (HTTP/post failure or session key missing)");
				return {};
			}

			const auto& raw = response.value();
			console::info("[OCCUPY] Response (first 300 chars): %s", raw.substr(0, 300).c_str());

			try
			{
				return nlohmann::json::parse(raw);
			}
			catch (const std::exception& e)
			{
				console::error("[OCCUPY] Failed to parse response JSON: %s", e.what());
				return {};
			}
		}
	}

	bool has_session_key()
	{
		return public_has_session_key();
	}

	bool open_wormhole(std::uint64_t target_player_id, std::uint64_t my_player_id)
	{
		return public_open_wormhole(target_player_id, my_player_id);
	}

	std::uint64_t get_my_player_id_from_cache()
	{
		return public_get_my_player_id();
	}

	std::optional<nlohmann::json> send_session_command(const nlohmann::json& cmd)
	{
		return public_send_session_command(cmd);
	}

	class component final : public component_interface
	{
	public:
		void pre_load() override
		{
			var_server_logging = vars::register_bool("net_server_logging", false, vars::var_flag_saved, "enable server logging");
			var_net_server_heartbeat = vars::register_int("net_server_heartbeat", 0, 0, std::numeric_limits<int>::max(), 0, "backend server heartbeat interval");
		}

		void start() override
		{
			http_codec_begin_encode_hook.create(SELECT_VALUE(0x141CE2DC0, 0x140C41750, 0x0, 0x0), http_codec_begin_encode_stub);
			http_codec_end_decode_hook.create(SELECT_VALUE(0x141CE35E0, 0x140C41F70, 0x0, 0x0), http_codec_end_decode_stub);

			// MGO-only session daemon hooks (kept behind is_mgo() to match the
			// original behavior; the unconditional variant could hit unverified
			// jump/invoke targets in TPP mode and crash on login).
			if (game::environment::is_mgo())
			{
				utils::hook::far_jump<BASE_ADDRESS>(SELECT_VALUE(0x1407DFC08, 0x14057D3C8, 0x0, 0x0), utils::hook::assemble(session_daemon_update_stub));
				utils::hook::call(SELECT_VALUE(0x1407D2736, 0x140572156, 0x0, 0x0), net_daemon_set_heartbeat);
			}

			if (!game::environment::is_tpp())
			{
				return;
			}

			// fob_open_wormhole command
			command::add("fob_open_wormhole", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_open_wormhole <target_steam_id>");
					console::info("Example: fob_open_wormhole 76561199505076493");
					console::info("");
					console::info("Opens a wormhole to the target player's FOB directly.");
					console::info("Requires session key from CMD_REQAUTH_HTTPS.");
					console::info("");
					console::info("Special values:");
					console::info("  fob_open_wormhole status   - Show TPP client status");
					return;
				}

				const auto first_arg = params.get(1);

				if (first_arg == "status")
				{
					console::info("TPP Client status:");
					console::info("  session_key: %s", tpp_session_key_available ? "AVAILABLE" : "NOT AVAILABLE");
					console::info("  my_player_id: %llu", get_my_player_id_from_cache());
					console::info("  my_steam_id: %llu", get_my_steam_id());
					return;
				}

				const auto target_steam_id = params.get_uint64(1);

				if (target_steam_id == 0)
				{
					console::info("Invalid steam_id.");
					return;
				}

				console::info("[FOB] fob_open_wormhole called:");
				console::info("[FOB]   target_steam_id: %llu", target_steam_id);

				// Get cached info for target
				const auto cached_target = fob_target::get_cached_player(target_steam_id);
				std::uint64_t target_player_id = 0;

				if (cached_target.has_value())
				{
					target_player_id = cached_target->player_id;
					console::info("[FOB]   target_player_id (from cache): %llu", target_player_id);
				}
				else
				{
					console::info("[FOB]   target not in cache, using steam_id as player_id");
					target_player_id = target_steam_id;  // Fallback, may not work
				}

				// Get our player_id from cache
				const auto my_player_id = get_my_player_id_from_cache();
				if (my_player_id == 0)
				{
					console::error("[FOB] Cannot determine your player_id!");
					console::error("[FOB] Browse the FOB list first so the mod can cache your info.");
					return;
				}

				console::info("[FOB]   my_player_id: %llu", my_player_id);

				console::info("[FOB] Sending CMD_OPEN_WORMHOLE via TPP client...");
				const bool success = tpp_client_instance_.open_wormhole(target_player_id, my_player_id);

				if (success)
				{
					console::info("[FOB] Wormhole command sent successfully!");
					console::info("[FOB] The server should open the wormhole momentarily.");
				}
				else
				{
					console::error("[FOB] Failed to send wormhole command!");
				}
			});

			// fob_get_target_detail command - Send CMD_GET_FOB_TARGET_DETAIL to get FOB information
			// After opening a wormhole with fob_open_wormhole, use this command
			// to fetch the updated FOB details from the server.
			// Then go back to FOB menu (ESC) and re-select the target to see the changes.
			command::add("fob_get_target_detail", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_get_target_detail <target_steam_id>");
					console::info("Example: fob_get_target_detail 76561199505076493");
					console::info("");
					console::info("Sends CMD_GET_FOB_TARGET_DETAIL directly to Konami servers.");
					console::info("Use this AFTER running fob_open_wormhole to get target FOB details.");
					console::info("");
					console::info("Mother base ID is automatically retrieved from cache.");
					console::info("");
					console::info("Requires:");
					console::info("  - Session key from CMD_REQAUTH_HTTPS");
					console::info("  - Target player in FOB cache");
					return;
				}

				const auto target_steam_id = params.get_uint64(1);

				if (!tpp_client_instance_.has_session_key())
				{
					console::error("[FOB Detail] No session key available!");
					console::error("[FOB Detail] Please wait for CMD_REQAUTH_HTTPS response first.");
					return;
				}

				const auto target_cached = fob_target::get_cached_player(target_steam_id);
				if (!target_cached.has_value())
				{
					console::error("[FOB Detail] Could not find target in cache.");
					console::error("[FOB Detail]   target_steam_id: %llu", target_steam_id);
					console::error("[FOB Detail] Please access the FOB menu first to populate the cache.");
					console::error("[FOB Detail] Or add the target using fob_add_target first.");
					return;
				}

				const auto mother_base_id = static_cast<std::uint64_t>(target_cached->mother_base_id);

				if (mother_base_id == 0)
				{
					console::error("[FOB Detail] Mother base ID not found in cache.");
					console::error("[FOB Detail]   target_steam_id: %llu", target_steam_id);
					console::error("[FOB Detail]   target_player_id: %llu", static_cast<std::uint64_t>(target_cached->player_id));
					console::error("[FOB Detail] Please try browsing FOB lists again.");
					return;
				}

				console::info("[FOB Detail] Found target in cache:");
				console::info("[FOB Detail]   steam_id:       %llu", target_steam_id);
				console::info("[FOB Detail]   player_id:      %llu", static_cast<std::uint64_t>(target_cached->player_id));
				console::info("[FOB Detail]   mother_base_id: %llu", mother_base_id);
				console::info("[FOB Detail]   name:           %s", target_cached->name.c_str());

				console::info("[FOB Detail] Sending CMD_GET_FOB_TARGET_DETAIL...");

				const bool success = tpp_client_instance_.get_fob_target_detail(mother_base_id);

				if (success)
				{
					console::info("==================================================");
					console::info("FOB DETAIL REQUEST SENT!");
					console::info("==================================================");
					console::info("Check the console above for server response.");
					console::info("Now go back to FOB menu (ESC) and re-select the target.");
					console::info("The blockade should now be bypassed.");
				}
				else
				{
					console::error("==================================================");
					console::error("FOB DETAIL REQUEST FAILED!");
					console::error("==================================================");
				}
			});

			// fob_add_support command
			command::add("fob_add_support", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_add_support <steam_id>");
					console::info("Example: fob_add_support 76561198000000000");
					console::info("");
					console::info("Sends a CMD_ADD_FOLLOW request directly to the server with the given steam_id.");
					console::info("This bypasses the game UI and sends the request immediately.");
					console::info("");
					console::info("Special values:");
					console::info("  fob_add_support 0         - Disable the interceptor");
					console::info("  fob_add_support status    - Show current status");
					return;
				}

				const auto first_arg = params.get(1);

				if (first_arg == "status")
				{
					console::info("fob_add_support status:");
					console::info("  enabled:  %s", add_follow_override_enabled ? "yes" : "no");
					console::info("  one_shot: %s", add_follow_override_one_shot ? "yes" : "no");
					console::info("  steam_id: %llu", add_follow_override_steam_id);
					console::info("  player_id: %llu", add_follow_override_player_id);
					return;
				}

				const auto steam_id = params.get_uint64(1);

				if (steam_id == 0)
				{
					add_follow_override_enabled = false;
					add_follow_override_one_shot = false;
					add_follow_override_steam_id = 0;
					add_follow_override_player_id = 0;
					console::info("fob_add_support: interceptor disabled.");
					return;
				}

				send_add_follow_request(steam_id, 0);
			});

			// fob_convert_list_type command
			command::add("fob_convert_list_type", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_convert_list_type <from> <to>");
					console::info("Example: fob_convert_list_type PICKUP FOLLOW");
					console::info("");
					console::info("Converts the type field in CMD_GET_FOB_TARGET_LIST requests.");
					console::info("Useful for changing what list the game requests from the server.");
					console::info("");
					console::info("Examples:");
					console::info("  fob_convert_list_type PICKUP FOLLOW");
					console::info("  fob_convert_list_type PICKUP PICKUP_HIGH");
					console::info("  fob_convert_list_type PICKUP NUCLEAR");
					console::info("  fob_convert_list_type 0         (disable)");
					console::info("");
					console::info("Available types: TRIAL, PICKUP, PICKUP_HIGH, ENEMY, EVENT,");
					console::info("  NUCLEAR, FOLLOW, FOLLOWER, DEPLOYED, INJURY, EMERGENCY, FR_ENEMY");
					return;
				}

				const auto from_type = params.get(1);

				if (from_type == "0" || from_type == "disable")
				{
					list_type_convert_enabled = false;
					list_type_convert_from = "";
					list_type_convert_to = "";
					console::info("[FOB] List type conversion disabled.");
					return;
				}

				if (params.size() < 3)
				{
					console::info("Usage: fob_convert_list_type <from> <to>");
					return;
				}

				const auto to_type = params.get(2);

				list_type_convert_from = from_type;
				list_type_convert_to = to_type;
				list_type_convert_enabled = true;

				console::info("[FOB] ================================================");
				console::info("[FOB] List type conversion enabled!");
				console::info("[FOB]   From: %s", from_type.c_str());
				console::info("[FOB]   To:   %s", to_type.c_str());
				console::info("[FOB] ================================================");
				console::info("[FOB] When the game requests '%s' list,", from_type.c_str());
				console::info("[FOB] it will be converted to '%s' list request.", to_type.c_str());
				console::info("[FOB] ================================================");
			});

			// fob_status command
			command::add("fob_status", [](const command::params& params)
			{
				console::info("========== FOB Status ==========");
				console::info("fob_convert_list_type:    %s",
					list_type_convert_enabled ? "ENABLED" : "DISABLED");
				if (list_type_convert_enabled)
				{
					console::info("  from: %s -> to: %s",
						list_type_convert_from.c_str(), list_type_convert_to.c_str());
				}
				console::info("fob_add_support:");
				console::info("  enabled:  %s", add_follow_override_enabled ? "yes" : "no");
				console::info("  one_shot: %s", add_follow_override_one_shot ? "yes" : "no");
				console::info("  steam_id: %llu", add_follow_override_steam_id);
				console::info("  player_id: %llu", add_follow_override_player_id);
				console::info("TPP Client:");
				console::info("  session_key: %s", tpp_session_key_available ? "AVAILABLE" : "NOT AVAILABLE");
				console::info("  my_player_id: %llu", get_my_player_id_from_cache());
				console::info("================================");
			});

			// fob_exchange_probe command - send a custom CMD_EXCHANGE_LEAGUE_POINT2
			// request straight to the official server. Used to probe the server's
			// exchange validation logic (type/unique_id matching, num_to_exchange
			// bounds, uint32 overflow behavior, is_event table switching, ...).
			command::add("fob_exchange_probe", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					console::info("Usage: fob_exchange_probe <type> <unique_id> [num_to_exchange] [common_value] [is_event]");
					console::info("Example: fob_exchange_probe 4 19092");
					console::info("Example: fob_exchange_probe 4 19092 1 0 1");
					console::info("Example: fob_exchange_probe 4 19092 4294967295 0 1   (uint32 overflow probe)");
					console::info("Example: fob_exchange_probe 1 1 0 9 1               (num_to_exchange = 0 probe)");
					console::info("");
					console::info("Sends CMD_EXCHANGE_LEAGUE_POINT2 directly to the official server");
					console::info("with the given exchange parameters and prints the raw response.");
					console::info("Defaults: num_to_exchange=1, common_value=0, is_event=1");
					console::info("Requires a session key from CMD_REQAUTH_HTTPS.");
					return;
				}

				if (!tpp_client_instance_.has_session_key())
				{
					console::error("[Exchange Probe] No session key available!");
					console::error("[Exchange Probe] Please login and visit the FOB menu first.");
					return;
				}

				const auto type = params.get_uint64(1);
				const auto unique_id = params.get_uint64(2);
				const auto num_to_exchange = params.size() >= 4 ? params.get_uint64(3) : 1;
				const auto common_value = params.size() >= 5 ? params.get_uint64(4) : 0;
				const auto is_event = params.size() >= 6 ? params.get_uint64(5) : 1;

				console::info("[Exchange Probe] Sending CMD_EXCHANGE_LEAGUE_POINT2:");
				console::info("[Exchange Probe]   type=%llu unique_id=%llu num_to_exchange=%llu common_value=%llu is_event=%llu",
					static_cast<unsigned long long>(type),
					static_cast<unsigned long long>(unique_id),
					static_cast<unsigned long long>(num_to_exchange),
					static_cast<unsigned long long>(common_value),
					static_cast<unsigned long long>(is_event));

				nlohmann::json req;
				req["msgid"] = "CMD_EXCHANGE_LEAGUE_POINT2";
				req["rqid"] = 0;
				req["type"] = static_cast<std::uint32_t>(type);
				req["unique_id"] = static_cast<std::uint32_t>(unique_id);
				req["num_to_exchange"] = static_cast<std::uint32_t>(num_to_exchange);
				req["common_value"] = static_cast<std::uint32_t>(common_value);
				req["is_event"] = static_cast<std::uint32_t>(is_event);

				std::lock_guard<std::mutex> lock(tpp_client_mutex);
				auto response = tpp_client_instance_.send_command_and_get_response(req, true);
				if (!response.has_value())
				{
					console::error("[Exchange Probe] No response (HTTP failure or session expired).");
					return;
				}

				console::info("[Exchange Probe] Raw response: %s", response->c_str());
			});
		}
	};
}

REGISTER_COMPONENT(server_logging::component)