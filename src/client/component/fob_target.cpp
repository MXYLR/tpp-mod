#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "fob_target.hpp"
#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "custom_server.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/concurrency.hpp>
#include <utils/memory.hpp>
#include <utils/http.hpp>
#include <utils/cryptography.hpp>

namespace fob_target
{
	namespace
	{
		struct target_data_t
		{
			std::vector<custom_target> targets;
			std::unordered_map<std::uint64_t, cached_player_info> cached_players;
		};

		utils::concurrency::container<target_data_t> g_data;

		const std::uint8_t tpp_static_key[16] = 
		{
			0xD8, 0x89, 0x0A, 0xF0, 0x66, 0xC9, 0x6B, 0x40, 
			0xD7, 0x01, 0xAE, 0xFC, 0x43, 0x6F, 0xF9, 0xFE
		};

		std::string url_encode(const std::string& str)
		{
			std::string result;
			result.reserve(str.size());

			for (unsigned char c : str)
			{
				if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				{
					result += c;
				}
				else if (c == ' ')
				{
					result += '+';
				}
				else
				{
					static const char hex_chars[] = "0123456789ABCDEF";
					result += '%';
					result += hex_chars[c >> 4];
					result += hex_chars[c & 0x0F];
				}
			}

			return result;
		}

		std::string build_tpp_request(const nlohmann::json& command_json, const std::string& session_key)
		{
			nlohmann::json wrapper;
			wrapper["data"] = command_json.dump();
			wrapper["compress"] = false;
			wrapper["session_crypto"] = false;
			wrapper["session_key"] = session_key;

			const auto wrapper_str = wrapper.dump();

			utils::cryptography::blowfish blow;
			blow.set_key(const_cast<std::uint8_t*>(tpp_static_key), sizeof(tpp_static_key));

			const auto encrypted = blow.encrypt(wrapper_str);
			const auto url_encoded = url_encode(encrypted);

			return "httpMsg=" + url_encoded;
		}

		std::optional<std::string> send_tpp_command(const nlohmann::json& command_json, const std::string& session_key)
		{
			if (!custom_server::is_using_custom_server())
			{
				console::error("Error: Custom server is not configured");
				console::error("Please set net_custom_server to your server URL first");
				console::error("Example: net_custom_server http://127.0.0.1:30000");
				return {};
			}

			if (session_key.empty())
			{
				console::error("Error: Session key is required");
				console::error("You must be logged in to the server first");
				console::error("Start the game, go to FOB menu, then use this command");
				return {};
			}

			const auto server_url = std::string(custom_server::get_custom_url());
			const auto endpoint = server_url + "/tppstm";

			const auto body = build_tpp_request(command_json, session_key);

			utils::http::headers headers;
			headers["Content-Type"] = "application/x-www-form-urlencoded";

			return utils::http::post_data(endpoint, body, headers);
		}
	}

	void add_target(const std::uint64_t steam_id, const std::uint32_t player_id,
		const std::uint32_t mother_base_id, const std::string& name)
	{
		g_data.access([&](target_data_t& data)
		{
			std::uint32_t final_player_id = player_id;
			std::uint32_t final_mother_base_id = mother_base_id;
			std::string final_name = name;
			bool has_cached = false;
			game::tpp::mbm::PlayerBasicInfo cached_info{};

			const auto cached_iter = data.cached_players.find(steam_id);
			if (cached_iter != data.cached_players.end())
			{
				has_cached = true;
				cached_info = cached_iter->second.full_info;

				if (player_id == 0)
				{
					final_player_id = cached_iter->second.player_id;
				}
				if (mother_base_id == 0)
				{
					final_mother_base_id = cached_iter->second.mother_base_id;
				}
				if (final_name.empty())
				{
					final_name = cached_iter->second.name;
				}
			}

			for (auto& target : data.targets)
			{
				if (target.steam_id == steam_id)
				{
					target.player_id = final_player_id;
					target.mother_base_id = final_mother_base_id;
					target.name = final_name.empty() ? std::to_string(steam_id) : final_name;
					target.has_cached_info = has_cached;
					if (has_cached)
					{
						target.cached_info = cached_info;
					}
					return;
				}
			}

			custom_target target{};
			target.steam_id = steam_id;
			target.player_id = final_player_id;
			target.mother_base_id = final_mother_base_id;
			target.name = final_name.empty() ? std::to_string(steam_id) : final_name;
			target.has_cached_info = has_cached;
			if (has_cached)
			{
				target.cached_info = cached_info;
			}
			data.targets.push_back(target);
		});
	}

	void remove_target(const std::uint64_t steam_id)
	{
		g_data.access([&](target_data_t& data)
		{
			for (auto it = data.targets.begin(); it != data.targets.end(); ++it)
			{
				if (it->steam_id == steam_id)
				{
					data.targets.erase(it);
					return;
				}
			}
		});
	}

	std::vector<custom_target> get_targets()
	{
		return g_data.access<std::vector<custom_target>>([&](const target_data_t& data)
		{
			return data.targets;
		});
	}

	bool has_custom_targets()
	{
		return !get_targets().empty();
	}

	void cache_player_info(const game::tpp::mbm::PlayerBasicInfo& player_info)
	{
		const auto steam_id = static_cast<std::uint64_t>(player_info.owner_account.id);
		if (steam_id == 0)
		{
			return;
		}

		g_data.access([&](target_data_t& data)
		{
			cached_player_info info{};
			info.steam_id = steam_id;
			info.player_id = player_info.owner_player_id;
			info.mother_base_id = player_info.mother_base_num > 0 ? player_info.mother_base_id[0] : 0;
			info.name = std::to_string(steam_id);
			info.espionage_score = player_info.espionage_score;
			info.espionage_win = player_info.espionage_win;
			info.espionage_total = player_info.espionage_total;
			info.full_info = player_info;

			data.cached_players[steam_id] = info;

			for (auto& target : data.targets)
			{
				if (target.steam_id == steam_id)
				{
					target.has_cached_info = true;
					target.cached_info = player_info;
					if (target.player_id == 0)
					{
						target.player_id = player_info.owner_player_id;
					}
					if (target.mother_base_id == 0 && player_info.mother_base_num > 0)
					{
						target.mother_base_id = player_info.mother_base_id[0];
					}
				}
			}
		});
	}

	std::vector<cached_player_info> get_cached_players()
	{
		return g_data.access<std::vector<cached_player_info>>([&](const target_data_t& data)
		{
			std::vector<cached_player_info> result;
			result.reserve(data.cached_players.size());
			for (const auto& [steam_id, info] : data.cached_players)
			{
				result.push_back(info);
			}
			return result;
		});
	}

	bool has_cached_player(const std::uint64_t steam_id)
	{
		return g_data.access<bool>([&](const target_data_t& data)
		{
			return data.cached_players.find(steam_id) != data.cached_players.end();
		});
	}

	std::optional<cached_player_info> get_cached_player(const std::uint64_t steam_id)
	{
		return g_data.access<std::optional<cached_player_info>>([&](const target_data_t& data)
		{
			const auto iter = data.cached_players.find(steam_id);
			if (iter != data.cached_players.end())
			{
				return std::optional<cached_player_info>{iter->second};
			}
			return std::optional<cached_player_info>{};
		});
	}

	class component final : public component_interface
	{
	public:
		void start() override
		{
			if (!game::environment::is_tpp())
			{
				return;
			}

			command::add("fob_connect", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_connect <steam_id>");
					console::info("Example: fob_connect 76561198000000000");
					console::info("");
					console::info("Important: You must be in the FOB menu for this to work!");
					console::info("1. Go to 'FOB Missions' -> 'Select Target'");
					console::info("2. Wait for the FOB list to load");
					console::info("3. Then use this command");
					return;
				}

				const auto steam_id = params.get_uint64(1);

				const auto server_manager = *game::tpp::net::ServerManager_::s_instance;
				if (!server_manager)
				{
					console::error("Error: ServerManager is not initialized");
					console::error("Make sure you are in the FOB menu and the list has loaded");
					return;
				}

				const auto fob_target_instance = game::tpp::net::ServerManager_::GetFobTarget(server_manager);
				if (!fob_target_instance)
				{
					console::error("Error: FobTarget is not initialized");
					console::error("Make sure you are in the FOB menu and the list has loaded");
					return;
				}

				// Check if sessionConnectInfo exists
				if (!fob_target_instance->sessionConnectInfo)
				{
					console::info("sessionConnectInfo is null, trying to initialize it by requesting target details...");

					// Try to find the target in the player list to call RequestDetail
					game::tpp::mbm::PlayerBasicInfo* target_player = nullptr;
					
					for (short i = 0; i < fob_target_instance->maxPlayers; i++)
					{
						if (static_cast<std::uint64_t>(fob_target_instance->playerInfos[i].owner_account.id) == steam_id)
						{
							target_player = &fob_target_instance->playerInfos[i];
							console::info("Found target in player list, requesting details...");
							
							// Call RequestDetail to initialize sessionConnectInfo
							game::tpp::net::FobTarget_::RequestDetail(
								fob_target_instance,
								target_player,
								0, // int param
								static_cast<unsigned int>(i), // unsigned int param (index)
								0, // char param
								0  // char param
							);
							
							break;
						}
					}

					// If target wasn't in the list, try to create a minimal PlayerBasicInfo to trigger initialization
					if (!target_player)
					{
						console::info("Target not found in current list, attempting to initialize connection info with dummy data...");
						
						// Create a temporary PlayerBasicInfo with the target's Steam ID
						game::tpp::mbm::PlayerBasicInfo dummy_player_info{};
						dummy_player_info.owner_account.id = steam_id;
						dummy_player_info.owner_player_id = 0; // We don't know the actual player ID
						
						// Try calling RequestDetail with the dummy player info to initialize sessionConnectInfo
						game::tpp::net::FobTarget_::RequestDetail(
							fob_target_instance,
							&dummy_player_info,
							0, // int param
							0, // unsigned int param (index)
							0, // char param
							0  // char param
						);
					}
				}

				// Now try to connect
				if (fob_target_instance->sessionConnectInfo)
				{
					console::debug("Using existing sessionConnectInfo");
					fob_target_instance->sessionConnectInfo->hostParam = steam_id;
					fob_target_instance->sessionConnectInfo->a1 = 0;

					console::info("Connecting to FOB target: %llu", steam_id);

					const auto result = game::tpp::net::FobTarget_::CreateClientSession(
						fob_target_instance,
						fob_target_instance->sessionConnectInfo
					);

					console::info("CreateClientSession returned: %d", result);
					console::info("Connection initiated. If successful, you should see the loading screen.");
				}
				else
				{
					console::info("sessionConnectInfo is still null, attempting direct connection with temporary object...");
					
					// Create a temporary SessionConnectInfo object to attempt direct connection
					game::tpp::net::SessionConnectInfo temp_session_info{};
					temp_session_info.hostParam = steam_id;
					temp_session_info.a1 = 0;
					
					console::info("Connecting to FOB target: %llu", steam_id);

					const auto result = game::tpp::net::FobTarget_::CreateClientSession(
						fob_target_instance,
						&temp_session_info
					);

					console::info("CreateClientSession returned: %d", result);
					console::info("Direct connection attempted. If successful, you should see the loading screen.");
				}
			},
			"Connect directly to a FOB by Steam ID (must be in FOB menu)",
			"fob_connect <steam_id>");

			command::add("fob_add_target", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_add_target <steam_id> [player_id] [mother_base_id] [name]");
					console::info("Example: fob_add_target 76561198000000000 12345 67890 MyFriend");
					console::info("Note: If player_id and mother_base_id are not provided, they will be auto-detected from cache");
					return;
				}

				const auto steam_id = params.get_uint64(1);
				std::uint32_t player_id = 0;
				std::uint32_t mother_base_id = 0;
				std::string name;

				if (params.size() >= 3)
				{
					player_id = static_cast<std::uint32_t>(params.get_int(2));
				}
				if (params.size() >= 4)
				{
					mother_base_id = static_cast<std::uint32_t>(params.get_int(3));
				}
				if (params.size() >= 5)
				{
					name = params.join(4);
				}

				const auto cached = get_cached_player(steam_id);
				if (cached.has_value())
				{
					const auto& info = cached.value();
					if (player_id == 0)
					{
						player_id = info.player_id;
					}
					if (mother_base_id == 0)
					{
						mother_base_id = info.mother_base_id;
					}
					if (name.empty())
					{
						name = info.name;
					}
					console::info("Using cached info for %llu: player_id=%u, mother_base_id=%u, name=%s",
						steam_id, player_id, mother_base_id, name.c_str());
				}
				else if (player_id == 0 || mother_base_id == 0)
				{
					console::warn("Player %llu not found in cache. Player may not be in any official FOB list yet.", steam_id);
					console::warn("Browse FOB lists first to cache player info, or provide player_id and mother_base_id manually.");
				}

				add_target(steam_id, player_id, mother_base_id, name);

				console::info("Added FOB target: %llu (player_id: %u, mother_base_id: %u, name: %s)",
					steam_id, player_id, mother_base_id, name.empty() ? std::to_string(steam_id).c_str() : name.c_str());
			},
			"Add a custom FOB target to the list (auto-detects from cache if possible)",
			"fob_add_target <steam_id> [player_id] [mother_base_id] [name]");

			command::add("fob_remove_target", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_remove_target <steam_id>");
					return;
				}

				const auto steam_id = params.get_uint64(1);
				remove_target(steam_id);

				console::info("Removed FOB target: %llu", steam_id);
			},
			"Remove a custom FOB target from the list",
			"fob_remove_target <steam_id>");

			command::add("fob_clear_targets", []()
			{
				g_data.access([](target_data_t& data)
				{
					data.targets.clear();
				});

				console::info("Cleared all custom FOB targets");
			},
			"Clear all custom FOB targets",
			"fob_clear_targets");

			command::add("fob_target_list", []()
			{
				const auto targets = get_targets();

				if (targets.empty())
				{
					console::info("No custom FOB targets added");
					return;
				}

				console::info("Custom FOB targets:");
				console::info("----------------------------------------");

				for (auto i = 0u; i < targets.size(); i++)
				{
					const auto& target = targets[i];
					console::info("[%u] %s", i, target.name.c_str());
					console::info("     Steam ID: %llu", target.steam_id);
					console::info("     Player ID: %u", target.player_id);
					console::info("     Mother Base ID: %u", target.mother_base_id);
					console::info("----------------------------------------");
				}
			},
			"List all custom FOB targets",
			"fob_target_list");

			command::add("fob_cache_list", []()
			{
				const auto players = get_cached_players();

				if (players.empty())
				{
					console::info("No cached players. Browse FOB lists to auto-cache players.");
					return;
				}

				console::info("Cached players (%d):", players.size());
				console::info("----------------------------------------");

				for (auto i = 0u; i < players.size(); i++)
				{
					const auto& player = players[i];
					console::info("[%u] %s", i, player.name.c_str());
					console::info("     Steam ID: %llu", player.steam_id);
					console::info("     Player ID: %u", player.player_id);
					console::info("     Mother Base ID: %u", player.mother_base_id);
					console::info("     Espionage: %d points, %d wins, %d total",
						player.espionage_score, player.espionage_win, player.espionage_total);
					console::info("----------------------------------------");
				}
			},
			"List all cached players from FOB lists",
			"fob_cache_list");

			command::add("fob_cache_clear", []()
			{
				g_data.access([](target_data_t& data)
				{
					data.cached_players.clear();
				});

				console::info("Cleared all cached players");
			},
			"Clear all cached players",
			"fob_cache_clear");

			command::add("fob_query", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_query <steam_id>");
					console::info("Example: fob_query 76561198000000000");
					console::info("");
					console::info("This command attempts to query FOB information for a specific player.");
					console::info("Note: This may only work if the player is accessible through the game's FOB system.");
					return;
				}

				const auto steam_id = params.get_uint64(1);

				const auto server_manager = *game::tpp::net::ServerManager_::s_instance;
				if (!server_manager)
				{
					console::error("Error: ServerManager is not initialized");
					console::error("Make sure you are in the FOB menu and the list has loaded");
					return;
				}

				const auto fob_target_instance = game::tpp::net::ServerManager_::GetFobTarget(server_manager);
				if (!fob_target_instance)
				{
					console::error("Error: FobTarget is not initialized");
					console::error("Make sure you are in the FOB menu and the list has loaded");
					return;
				}

				// Try to find the target in the player list first
				game::tpp::mbm::PlayerBasicInfo* target_player = nullptr;
				
				for (short i = 0; i < fob_target_instance->maxPlayers; i++)
				{
					if (static_cast<std::uint64_t>(fob_target_instance->playerInfos[i].owner_account.id) == steam_id)
					{
						target_player = &fob_target_instance->playerInfos[i];
						console::info("Found target in current list, requesting details...");
						
						// Call RequestDetail to get the target's FOB information
						const auto result = game::tpp::net::FobTarget_::RequestDetail(
							fob_target_instance,
							target_player,
							0, // int param
							static_cast<unsigned int>(i), // unsigned int param (index)
							0, // char param
							0  // char param
						);
						
						console::info("RequestDetail returned: %d", result);
						console::info("FOB information query initiated for Steam ID: %llu", steam_id);
						return;
					}
				}

				// If target wasn't in the current list, try with a dummy player info
				console::info("Target not found in current list, attempting query with dummy data...");
				
				// Create a temporary PlayerBasicInfo with the target's Steam ID
				game::tpp::mbm::PlayerBasicInfo dummy_player_info{};
				dummy_player_info.owner_account.id = steam_id;
				dummy_player_info.owner_player_id = 0; // We don't know the actual player ID
				
				// Try calling RequestDetail with the dummy player info
				const auto result = game::tpp::net::FobTarget_::RequestDetail(
					fob_target_instance,
					&dummy_player_info,
					0, // int param
					0, // unsigned int param (index)
					0, // char param
					0  // char param
				);
				
				console::info("RequestDetail returned: %d", result);
				console::info("FOB information query initiated for Steam ID: %llu (may not be available if player is not in system)", steam_id);
			},
			"Query FOB information for a specific player by Steam ID",
			"fob_query <steam_id>");

			command::add("fob_follow", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("Usage: fob_follow <steam_id> [session_key]");
					console::info("Example: fob_follow 76561198000000000 my_session_key");
					console::info("");
					console::info("This command sends CMD_ADD_FOLLOW to the custom server.");
					console::info("The target player will appear in your FOLLOW list, allowing you to");
					console::info("get their player_id and mother_base_id for FOB invasion.");
					console::info("");
					console::info("Session key is required. You can find your session key in:");
					console::info("  tpp-mod/steam_storage/server-<hash>/TPP_GAME_DATA");
					console::info("");
					console::info("Note: This requires net_custom_server to be configured.");
					return;
				}

				const auto steam_id = params.get_uint64(1);
				std::string session_key;
				
				if (params.size() >= 3)
				{
					session_key = params.get(2);
				}
				else
				{
					console::error("Error: Session key is required");
					console::error("Find it in tpp-mod/steam_storage/server-<hash>/TPP_GAME_DATA");
					console::error("Usage: fob_follow <steam_id> <session_key>");
					return;
				}
				
				console::info("Sending CMD_ADD_FOLLOW for Steam ID: %llu", steam_id);

				nlohmann::json command_json;
				command_json["msgid"] = "CMD_ADD_FOLLOW";
				command_json["rqid"] = 1;
				command_json["data"] = nlohmann::json::object();
				command_json["data"]["steam_id"] = steam_id;
				command_json["data"]["player_id"] = 0;

				const auto result = send_tpp_command(command_json, session_key);

				if (!result.has_value())
				{
					console::error("Failed to send command to server");
					return;
				}

				console::info("Server response received");
				console::info("Response: %s", result.value().c_str());
				console::info("");
				console::info("If successful, the target should appear in your FOLLOW list.");
				console::info("Go to FOB Missions -> Select Target -> FOLLOW to see the target.");
			},
			"Add a player to FOLLOW list via Steam ID (requires custom server)",
			"fob_follow <steam_id> <session_key>");

			command::add("fob_unfollow", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					console::info("Usage: fob_unfollow <player_id> <session_key>");
					console::info("Example: fob_unfollow 12345 my_session_key");
					console::info("");
					console::info("This command sends CMD_DELETE_FOLLOW to the custom server.");
					console::info("Note: This command requires player_id, not steam_id.");
					console::info("Use fob_cache_list or fob_target_list to find the player_id.");
					return;
				}

				const auto player_id = static_cast<std::uint64_t>(params.get_int(1));
				const auto session_key = params.get(2);
				
				console::info("Sending CMD_DELETE_FOLLOW for Player ID: %llu", player_id);

				nlohmann::json command_json;
				command_json["msgid"] = "CMD_DELETE_FOLLOW";
				command_json["rqid"] = 1;
				command_json["data"] = nlohmann::json::object();
				command_json["data"]["player_id"] = player_id;

				const auto result = send_tpp_command(command_json, session_key);

				if (!result.has_value())
				{
					console::error("Failed to send command to server");
					return;
				}

				console::info("Server response received");
				console::info("Response: %s", result.value().c_str());
			},
			"Remove a player from FOLLOW list via Player ID (requires custom server)",
			"fob_unfollow <player_id> <session_key>");
		}
	};
}

REGISTER_COMPONENT(fob_target::component)