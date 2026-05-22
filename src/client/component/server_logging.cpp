#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "custom_server.hpp"
#include "fob_target.hpp"
#include "vars.hpp"
#include "console.hpp"
#include "command.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/flags.hpp>
#include <utils/cryptography.hpp>
#include <utils/http.hpp>

#ifdef _WIN32
#define BSWAP32(x) _byteswap_ulong(x)
#else
#define BSWAP32(x) __builtin_bswap32(x)
#endif

namespace server_logging
{
	namespace
	{
		utils::hook::detour http_codec_begin_encode_hook;
		utils::hook::detour http_codec_end_decode_hook;

		vars::var_ptr var_server_logging;
		vars::var_ptr var_net_server_heartbeat;

		std::uint64_t add_follow_override_steam_id = 0;
		std::uint64_t add_follow_override_player_id = 0;
		bool add_follow_override_enabled = false;
		bool add_follow_override_one_shot = false;

		std::string tpp_session_key = "";
		bool tpp_session_key_available = false;

		std::string list_type_convert_from = "";
		std::string list_type_convert_to = "";
		bool list_type_convert_enabled = false;

		const uint8_t tpp_static_key[16] = {0xD8, 0x89, 0x0A, 0xF0, 0x66, 0xC9, 0x6B, 0x40, 0xD7, 0x01, 0xAE, 0xFC, 0x43, 0x6F, 0xF9, 0xFE};

		class tpp_client
		{
		private:
			utils::cryptography::blowfish static_blow_;
			utils::cryptography::blowfish session_blow_;
			std::string session_key_;
			std::string session_key_string_;
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

			bool has_session_key() const
			{
				return !session_key_string_.empty();
			}

			std::string encrypt_with_static(const std::string& data)
			{
				return static_blow_.encrypt(data);
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

			std::string encrypt_with_static_crlf(const std::string& data)
			{
				std::string padded = data;
				const auto mod = (padded.size() % 8);
				if (mod != 0)
				{
					const auto byte_count = 8 - mod;
					for (auto i = 0ull; i < byte_count; i++)
					{
						padded += static_cast<char>(byte_count);
					}
				}

				std::string encrypted_bytes;
				for (auto offset = 0u; offset < padded.size(); offset += 8)
				{
					const auto chunk = padded.substr(offset, 8);
					auto chunk_l = chunk.substr(0, 4);
					auto chunk_r = chunk.substr(4, 4);

					auto xl = static_cast<std::uint32_t>(BSWAP32(*reinterpret_cast<std::uint32_t*>(chunk_l.data())));
					auto xr = static_cast<std::uint32_t>(BSWAP32(*reinterpret_cast<std::uint32_t*>(chunk_r.data())));

					static_blow_.encrypt_single(xl, xr);

					xl = BSWAP32(xl);
					xr = BSWAP32(xr);

					encrypted_bytes.append(reinterpret_cast<char*>(&xl), 4);
					encrypted_bytes.append(reinterpret_cast<char*>(&xr), 4);
				}

				std::string base64_crlf = base64_encode_crlf(encrypted_bytes);
				return url_encode_plus(base64_crlf);
			}

			std::optional<std::string> send_command_and_get_response(const nlohmann::json& command_json, bool encrypt = false)
			{
				std::string data_str = command_json.dump(-1);

				nlohmann::json message;
				message["compress"] = false;
				message["session_crypto"] = encrypt;
				message["session_key"] = session_key_string_;
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

				utils::http::headers headers;
				headers["Connection"] = "Keep-Alive";
				headers["Content-Type"] = "application/x-www-form-urlencoded";

				console::info("[TPP Client] Sending command: %s", command_json["msgid"].get<std::string>().data());

				auto response = utils::http::post_data(url_, post_body, headers);

				if (response.has_value())
				{
					console::info("[TPP Client] Response received!");

					const auto& raw = response.value();
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
					console::error("[TPP Client] Request failed!");
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
				wormhole_json["flag"] = "FRIENDLY";
				wormhole_json["is_open"] = 1;
				wormhole_json["msgid"] = "CMD_OPEN_WORMHOLE";
				wormhole_json["player_id"] = my_player_id;
				wormhole_json["retaliate_score"] = 252;
				wormhole_json["rqid"] = 0;
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
				json["is_event"] = 0;
				json["is_plus"] = 0;
				json["is_sneak"] = 1;
				json["is_steal"] = 0;
				json["mgo_id"] = 0;
				json["mode"] = "actual";
				json["mother_base_id"] = mother_base_id;
				json["msgid"] = "CMD_GET_FOB_TARGET_DETAIL";
				json["platform"] = 0;
				json["rqid"] = 0;

				console::info("[TPP Client] Getting FOB target detail:");
				console::info("[TPP Client]   mother_base_id: %llu", mother_base_id);
				console::info("[TPP Client]   mode: actual");
				console::info("[TPP Client]   is_sneak: 1");

				return send_command(json, false);
			}
		};

		tpp_client tpp_client_instance_;

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

		std::uint64_t get_my_player_id_from_cache_impl()
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

		std::string get_dump_path(const std::string cmd_name, const bool request)
		{
			static const auto game_name = SELECT_VALUE_NOLANG("tpp", "mgo");
			static const auto folder = custom_server::is_using_custom_server() ? "server_dump/custom" : "server_dump/konami";

			const auto request_folder = request ? "requests" : "responses";
			const auto name = utils::string::va("tpp-mod/%s/%s/%s/%s/%lli.json", folder, game_name, request_folder,
				cmd_name.data(), GetTickCount64());

			return name;
		}

		std::string get_raw_dump_path(const std::string cmd_name, const bool request)
		{
			static const auto game_name = SELECT_VALUE_NOLANG("tpp", "mgo");
			static const auto folder = custom_server::is_using_custom_server() ? "server_dump/custom" : "server_dump/konami";

			const auto request_folder = request ? "requests_raw" : "responses_raw";
			const auto name = utils::string::va("tpp-mod/%s/%s/%s/%s/%lli.bin", folder, game_name, request_folder,
				cmd_name.data(), GetTickCount64());

			return name;
		}

		std::string get_encrypted_dump_path(const std::string cmd_name, const bool request)
		{
			static const auto game_name = SELECT_VALUE_NOLANG("tpp", "mgo");
			static const auto folder = custom_server::is_using_custom_server() ? "server_dump/custom" : "server_dump/konami";

			const auto request_folder = request ? "requests_encrypted" : "responses_encrypted";
			const auto name = utils::string::va("tpp-mod/%s/%s/%s/%s/%lli.bin", folder, game_name, request_folder,
				cmd_name.data(), GetTickCount64());

			return name;
		}

		std::string get_fox_buffer(game::fox::Buffer* buffer)
		{
			const auto buf = game::fox::Buffer_::GetBuffer(buffer);
			const auto buf_size = game::fox::Buffer_::GetSize(buffer);
			return std::string{buf, buf + buf_size};
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

					if (cmd == "CMD_REQAUTH_HTTPS" || cmd == "CMD_REQAUTH")
					{
						if (json.contains("crypto_key") && json["crypto_key"].is_string())
						{
							const auto crypto_key = json["crypto_key"].get<std::string>();
							tpp_session_key = crypto_key;
							tpp_session_key_available = true;
							tpp_client_instance_.set_session_key(crypto_key);

							console::info("[TPP Client] =============================================");
							console::info("[TPP Client] Session key captured from %s", cmd.data());
							console::info("[TPP Client] Session key: %s", crypto_key.substr(0, 20).c_str());
							console::info("[TPP Client] You can now use fob_open_wormhole directly!");
							console::info("[TPP Client] =============================================");
						}
					}


					if (var_server_logging->current.enabled())
					{
						const auto modified_data = json.dump(-1);

						if (modified_data.size() <= buffer->capacity)
						{
							std::memcpy(game::fox::Buffer_::GetBuffer(buffer), modified_data.data(), modified_data.size());
							buffer->size = modified_data.size();
						}
						else
						{
							console::error("[server logging] Modified data too large for buffer");
						}

						const auto path = get_dump_path(cmd, false);
						utils::io::write_file(path, json.dump(4));

						const auto raw_path = get_raw_dump_path(cmd, false);
						utils::io::write_file(raw_path, modified_data);
					}
				}

				if (var_server_logging->current.enabled())
				{
					try
					{
						const auto encrypted_json = nlohmann::json::parse(get_fox_buffer(buffer));
						const auto encrypted_cmd = encrypted_json.value("msgid", "unknown");

						const auto encrypted_path = get_encrypted_dump_path(encrypted_cmd, false);
						utils::io::write_file(encrypted_path, get_fox_buffer(buffer));
					}
					catch (...)
					{
					}
				}
			}

			return res;
		}

		bool intercept_add_follow_request(game::fox::Buffer* buffer)
		{
			if (add_follow_override_enabled && buffer != nullptr)
			{
				try
				{
					const auto buf = game::fox::Buffer_::GetBuffer(buffer);
					const auto buf_size = game::fox::Buffer_::GetSize(buffer);

					if (buf != nullptr && buf_size > 0)
					{
						const std::string raw_data{buf, buf + buf_size};
						auto json = nlohmann::json::parse(raw_data, nullptr, false);

						if (!json.is_discarded() && json.is_object() && json.contains("msgid") &&
							json["msgid"].is_string() && json["msgid"].get<std::string>() == "CMD_ADD_FOLLOW")
						{
							console::info("[FOB] Intercepting CMD_ADD_FOLLOW request");

							if (add_follow_override_steam_id != 0)
							{
								console::info("[FOB]   Setting steam_id: %llu", add_follow_override_steam_id);
								json["steam_id"] = add_follow_override_steam_id;
							}

							if (add_follow_override_player_id != 0)
							{
								console::info("[FOB]   Setting player_id: %llu", add_follow_override_player_id);
								json["player_id"] = add_follow_override_player_id;
							}

							const auto new_data = json.dump(-1);

							if (new_data.size() <= buffer->capacity)
							{
								std::memcpy(buf, new_data.data(), new_data.size());
								buffer->size = new_data.size();
								console::info("[FOB]   Modified request (size: %zu)", new_data.size());

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
				catch (const std::exception& e)
				{
					console::error("[FOB] Failed to intercept CMD_ADD_FOLLOW: %s", e.what());
				}
			}

			return false;
		}

		void send_add_follow_request(const std::uint64_t steam_id, const std::uint64_t player_id)
		{
			add_follow_override_steam_id = steam_id;
			add_follow_override_player_id = player_id;
			add_follow_override_enabled = true;
			add_follow_override_one_shot = true;

			console::info("[FOB] CMD_ADD_FOLLOW interceptor armed:");
			console::info("[FOB]   steam_id: %llu", steam_id);
			console::info("[FOB]   player_id: %llu", player_id);
			console::info("[FOB]   Will intercept next CMD_ADD_FOLLOW request from game.");
			console::info("[FOB]   Go to Relationships menu -> Friends list -> select any player -> click 'Support' to trigger.");
		}

		void* http_codec_begin_encode_stub(void* this_, void* ctx, game::fox::Buffer* buffer, void* session_key)
		{
			intercept_add_follow_request(buffer);

			if (buffer != nullptr)
			{
				try
				{
					const auto buf = game::fox::Buffer_::GetBuffer(buffer);
					const auto buf_size = game::fox::Buffer_::GetSize(buffer);

					if (buf != nullptr && buf_size > 0)
					{
						const std::string raw{buf, buf + buf_size};
						auto json = nlohmann::json::parse(raw, nullptr, false);
						if (!json.is_discarded() && json.is_object() && json.contains("msgid") &&
							json["msgid"].is_string() && json["msgid"].get<std::string>() == "CMD_SEND_SNEAK_RESULT")
						{
							const auto dp = json.value("damage_point", 0);
							const auto rp = json.value("retaliate_point", 0);
							const auto result = json.value("sneak_result", "?");
							console::info("[FOB] CMD_SEND_SNEAK_RESULT: result=%s, damage_point=%d, retaliate_point=%d",
								result.c_str(), dp, rp);
						}
					}
				}
				catch (...)
				{
				}
			}

			if (list_type_convert_enabled && buffer != nullptr)
			{
				try
				{
					const auto buf = game::fox::Buffer_::GetBuffer(buffer);
					const auto buf_size = game::fox::Buffer_::GetSize(buffer);

					if (buf != nullptr && buf_size > 0)
					{
						const std::string raw_data{buf, buf + buf_size};
						auto json = nlohmann::json::parse(raw_data, nullptr, false);

						if (!json.is_discarded() && json.is_object() && json.contains("msgid") &&
							json["msgid"].is_string() && json["msgid"].get<std::string>() == "CMD_GET_FOB_TARGET_LIST")
						{
							if (json.contains("type") && json["type"].is_string())
							{
								const auto current_type = json["type"].get<std::string>();
								if (current_type == list_type_convert_from)
								{
									console::info("[FOB] Converting list type: %s -> %s",
										list_type_convert_from.c_str(), list_type_convert_to.c_str());
									json["type"] = list_type_convert_to;

									const auto new_data = json.dump(-1);
									if (new_data.size() <= buffer->capacity)
									{
										std::memcpy(buf, new_data.data(), new_data.size());
										buffer->size = new_data.size();
										console::info("[FOB] List type converted successfully!");
									}
									else
									{
										console::error("[FOB] Modified request too large for buffer!");
									}
								}
							}
						}
					}
				}
				catch (const std::exception& e)
				{
					console::error("[FOB] Failed to convert list type: %s", e.what());
				}
			}

			if (var_server_logging->current.enabled())
			{
				const auto raw_data = get_fox_buffer(buffer);

				try
				{
					const auto json = nlohmann::json::parse(raw_data);
					const auto cmd = json.value("msgid", "unknown");

					console::info("[server logging] sending request for command \"%s\"", cmd.data());

					const auto path = get_dump_path(cmd, true);
					utils::io::write_file(path, json.dump(4));

					const auto raw_path = get_raw_dump_path(cmd, true);
					utils::io::write_file(raw_path, raw_data);
				}
				catch (const std::exception& e)
				{
					console::error("[server logging] failed to parse request: %s", e.what());

					const auto raw_path = get_raw_dump_path("parse_error", true);
					utils::io::write_file(raw_path, raw_data);
				}
			}

			const auto res = http_codec_begin_encode_hook.invoke<void*>(this_, ctx, buffer, session_key);

			if (var_server_logging->current.enabled())
			{
				try
				{
					const auto json = nlohmann::json::parse(get_fox_buffer(buffer));
					const auto cmd = json.value("msgid", "unknown");

					const auto encrypted_path = get_encrypted_dump_path(cmd, true);
					utils::io::write_file(encrypted_path, get_fox_buffer(buffer));
				}
				catch (...)
				{
				}
			}

			return res;
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
			a.jmp(SELECT_VALUE(0x1407DF0CE, 0x14057D1BE, 0x1407DED0E, 0x14057CB8E));
		}

		void net_daemon_set_heartbeat(void* this_, int value)
		{
			vars::set_var(var_net_server_heartbeat, value, vars::var_source_internal);
			console::info("[server logging] set heartbeat: %i\n", value);
			utils::hook::invoke<void>(SELECT_VALUE(0x1407DDBE0, 0x14057BC90, 0x1407DD820, 0x14057B660), this_, value);
		}
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
			http_codec_begin_encode_hook.create(SELECT_VALUE(0x14D343690, 0x14A4E7640, 0x14D88F960, 0x1494F5CD0), http_codec_begin_encode_stub);
			http_codec_end_decode_hook.create(SELECT_VALUE(0x141CE3210, 0x140C42A20, 0x141CE3090, 0x140C42520), http_codec_end_decode_stub);

			if (game::environment::is_mgo())
			{
				utils::hook::far_jump<BASE_ADDRESS>(SELECT_VALUE(0x1407DF0C8, 0x14057D1B8, 0x1407DED08, 0x14057CB88), utils::hook::assemble(session_daemon_update_stub));
				utils::hook::call(SELECT_VALUE(0x1407D1A76, 0x144DA6856, 0x1407D16B6, 0x14651E946), net_daemon_set_heartbeat);
			}

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
			},
			"Send CMD_ADD_FOLLOW with the given steam_id (direct send)",
			"fob_add_support <steam_id>");

			command::add("fob_open_wormhole", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_open_wormhole <target_steam_id>");
					console::info("Example: fob_open_wormhole 76561199505076493");
					console::info("");
					console::info("Directly sends CMD_OPEN_WORMHOLE to Konami server,");
					console::info("like mgsv-client-tools.");
					console::info("");
					console::info("After success:");
					console::info("  Go back to FOB menu (ESC) and re-select the target.");
					console::info("");
					console::info("Requirements:");
					console::info("  - Session key from CMD_REQAUTH_HTTPS");
					console::info("  - Target player in FOB cache");
					console::info("  - Your player_id in FOB cache");
					return;
				}

				const auto target_steam_id = params.get_uint64(1);

				if (target_steam_id == 0)
				{
					console::info("Usage: fob_open_wormhole <target_steam_id>");
					return;
				}

				if (!tpp_client_instance_.has_session_key())
				{
					console::error("[FOB Wormhole] No session key available!");
					console::error("[FOB Wormhole] Please login and visit FOB menu first.");
					return;
				}

				auto my_player_id = get_my_player_id_from_cache_impl();
				if (my_player_id == 0)
				{
					console::error("[FOB Wormhole] Could not find your player_id in cache.");
					console::error("[FOB Wormhole] Please access the FOB menu first.");
					return;
				}

				const auto target_cached = fob_target::get_cached_player(target_steam_id);
				if (!target_cached.has_value())
				{
					console::error("[FOB Wormhole] Could not find target player_id in cache.");
					console::error("[FOB Wormhole]   target_steam_id: %llu", target_steam_id);
					console::error("[FOB Wormhole] Please access the FOB menu first.");
					return;
				}

				const auto target_player_id = target_cached->player_id;

				console::info("[FOB Wormhole] Sending CMD_OPEN_WORMHOLE...");
				console::info("[FOB Wormhole]   target_player_id: %u", target_player_id);
				console::info("[FOB Wormhole]   my_player_id:     %llu", (unsigned long long)my_player_id);

				const bool success = tpp_client_instance_.open_wormhole(target_player_id, my_player_id);

				if (success)
				{
					console::info("==================================================");
					console::info("WORMHOLE OPENED SUCCESSFULLY!");
					console::info("==================================================");
					console::info("Go back to FOB menu and re-select the target.");
					console::info("The blockade should now be bypassed.");
				}
				else
				{
					console::error("==================================================");
					console::error("WORMHOLE REQUEST FAILED!");
					console::error("==================================================");
				}
			},
			"Open wormhole to bypass FOB blockade (direct send)",
			"fob_open_wormhole <target_steam_id>");

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
					console::info("==================================================");
				}
				else
				{
					console::error("==================================================");
					console::error("FOB DETAIL REQUEST FAILED!");
					console::error("==================================================");
				}
			},
			"Send CMD_GET_FOB_TARGET_DETAIL to get FOB information",
			"fob_get_target_detail <target_steam_id>");


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
				console::info("[FOB] When the game requests '%s' list,",
					from_type.c_str());
				console::info("[FOB] it will be converted to '%s' list request.",
					to_type.c_str());
				console::info("[FOB] ================================================");
			},
			"Convert FOB list type in requests (e.g. PICKUP -> FOLLOW)",
			"fob_convert_list_type <from> <to>");


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
				console::info("fob_add_target count:    %zu",
					fob_target::get_targets().size());
				console::info("fob_target cache count:  %zu",
					fob_target::get_cached_players().size());
				console::info("================================");
			},
			"Show FOB status and configuration",
			"fob_status");


		}
	};
}


REGISTER_COMPONENT(server_logging::component)

namespace server_logging
{
	bool open_wormhole(std::uint64_t target_player_id, std::uint64_t my_player_id)
	{
		if (!tpp_client_instance_.has_session_key())
		{
			console::error("[FOB Wormhole] No session key available!");
			return false;
		}
		return tpp_client_instance_.open_wormhole(target_player_id, my_player_id);
	}

	bool has_session_key()
	{
		return tpp_client_instance_.has_session_key();
	}

	std::uint64_t get_my_player_id_from_cache()
	{
		return get_my_player_id_from_cache_impl();
	}
}
