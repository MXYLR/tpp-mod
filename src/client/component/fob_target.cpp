#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "fob_target.hpp"
#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/concurrency.hpp>

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

		
		}
	};
}

REGISTER_COMPONENT(fob_target::component)