#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "custom_server.hpp"
#include "vars.hpp"
#include "console.hpp"
#include "command.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/flags.hpp>
#include <utils/cryptography.hpp>
#include <utils/http.hpp>

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
			if (var_server_logging->current.enabled())
			{
				const auto raw_data = get_fox_buffer(buffer);

				try
				{
					const auto json = nlohmann::json::parse(raw_data);
					const auto cmd = json.value("msgid", "unknown");

					console::info("[server logging] received response for command \"%s\"", cmd.data());

					const auto path = get_dump_path(cmd, false);
					utils::io::write_file(path, json.dump(4));

					const auto raw_path = get_raw_dump_path(cmd, false);
					utils::io::write_file(raw_path, raw_data);
				}
				catch (const std::exception& e)
				{
					console::error("[server logging] failed to parse response: %s", e.what());

					const auto raw_path = get_raw_dump_path("parse_error", false);
					utils::io::write_file(raw_path, raw_data);
				}
			}

			const auto res = http_codec_end_decode_hook.invoke<void*>(this_, ctx, buffer);

			if (var_server_logging->current.enabled())
			{
				try
				{
					const auto json = nlohmann::json::parse(get_fox_buffer(buffer));
					const auto cmd = json.value("msgid", "unknown");

					const auto encrypted_path = get_encrypted_dump_path(cmd, false);
					utils::io::write_file(encrypted_path, get_fox_buffer(buffer));
				}
				catch (...)
				{
				}
			}

			return res;
		}

		bool intercept_add_follow_request(game::fox::Buffer* buffer)
		{
			if (!add_follow_override_enabled || buffer == nullptr)
			{
				return false;
			}

			try
			{
				const auto buf = game::fox::Buffer_::GetBuffer(buffer);
				const auto buf_size = game::fox::Buffer_::GetSize(buffer);

				if (buf == nullptr || buf_size == 0)
				{
					return false;
				}

				const std::string raw_data{buf, buf + buf_size};
				auto json = nlohmann::json::parse(raw_data, nullptr, false);

				if (json.is_discarded() || !json.contains("msgid") ||
					!json["msgid"].is_string() || json["msgid"].get<std::string>() != "CMD_ADD_FOLLOW")
				{
					return false;
				}

				console::info("[FOB] Intercepting CMD_ADD_FOLLOW request");

				// Modify the top-level fields directly (based on actual captured data)
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

				const auto new_data = json.dump(-1); // Compact format without extra whitespace

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
					// Try a minimal version with only essential fields
					nlohmann::json minimal_json;
					minimal_json["msgid"] = "CMD_ADD_FOLLOW";
					minimal_json["steam_id"] = json["steam_id"];
					if (json.contains("player_id")) {
						minimal_json["player_id"] = json["player_id"];
					}
					if (json.contains("rqid")) {
						minimal_json["rqid"] = json["rqid"];
					}
					if (json.contains("xu_id")) {
						minimal_json["xu_id"] = json["xu_id"];
					}
					if (json.contains("np_id")) {
						minimal_json["np_id"] = json["np_id"];
					}

					const auto minimal_data = minimal_json.dump(-1);

					if (minimal_data.size() <= buffer->capacity)
					{
						std::memcpy(buf, minimal_data.data(), minimal_data.size());
						buffer->size = minimal_data.size();
						console::info("[FOB]   Modified request with minimal format (size: %zu)", minimal_data.size());

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
						console::error("[FOB]   Even minimal data too large (%zu > capacity %zu)",
							minimal_data.size(), buffer->capacity);
						return false;
					}
				}
			}
			catch (const std::exception& e)
			{
				console::error("[FOB] Failed to intercept CMD_ADD_FOLLOW: %s", e.what());
				return false;
			}
		}

		void send_add_follow_request(const std::uint64_t steam_id, const std::uint64_t player_id)
		{
			// Set the parameters for the next CMD_ADD_FOLLOW request
			add_follow_override_steam_id = steam_id;
			add_follow_override_player_id = player_id;
			add_follow_override_enabled = true;
			add_follow_override_one_shot = true; // One-shot: disable after first use
			
			console::info("[FOB] CMD_ADD_FOLLOW interceptor armed:");
			console::info("[FOB]   steam_id: %llu", steam_id);
			console::info("[FOB]   player_id: %llu", player_id);
			console::info("[FOB]   Will intercept next CMD_ADD_FOLLOW request from game.");
			console::info("[FOB]   Go to Relationships menu -> Friends list -> select any player -> click 'Support' to trigger.");
		}
		
		void* http_codec_begin_encode_stub(void* this_, void* ctx, game::fox::Buffer* buffer, void* session_key)
		{
			intercept_add_follow_request(buffer);

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
			var_server_logging = vars::register_bool("net_server_logging", false, vars::var_flag_saved, "enable server logging (logs all server requests/responses)");
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

				// Send request directly
				send_add_follow_request(steam_id, 0);
			},
			"Send CMD_ADD_FOLLOW with the given steam_id (direct send)",
			"fob_add_support <steam_id>");
		}
	};
}

REGISTER_COMPONENT(server_logging::component)
