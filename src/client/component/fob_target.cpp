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
			for (auto& target : data.targets)
			{
				if (target.steam_id == steam_id)
				{
					target.player_id = player_id;
					target.mother_base_id = mother_base_id;
					target.name = name.empty() ? std::to_string(steam_id) : name;
					return;
				}
			}

			custom_target target{};
			target.steam_id = steam_id;
			target.player_id = player_id;
			target.mother_base_id = mother_base_id;
			target.name = name.empty() ? std::to_string(steam_id) : name;
			target.has_cached_info = false;
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
		return g_data.access<std::vector<custom_target>>([&](target_data_t& data)
		{
			return data.targets;
		});
	}

	bool has_custom_targets()
	{
		return g_data.access<bool>([&](target_data_t& data)
		{
			return !data.targets.empty();
		});
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
			info.mother_base_id = player_info.mother_base_id[0];
			info.name = std::string(player_info.owner_name, strnlen(player_info.owner_name, sizeof(player_info.owner_name)));
			info.espionage_score = player_info.espionage_score;
			info.espionage_win = player_info.espionage_win;
			info.espionage_total = player_info.espionage_total;

			data.cached_players[steam_id] = info;

			for (auto& target : data.targets)
			{
				if (target.steam_id == steam_id)
				{
					target.has_cached_info = true;
					if (target.player_id == 0)
					{
						target.player_id = info.player_id;
					}
					if (target.mother_base_id == 0)
					{
						target.mother_base_id = info.mother_base_id;
					}
					if (target.name.empty() || target.name == std::to_string(steam_id))
					{
						target.name = info.name;
					}
					break;
				}
			}
		});
	}

	std::vector<cached_player_info> get_cached_players()
	{
		return g_data.access<std::vector<cached_player_info>>([&](target_data_t& data)
		{
			std::vector<cached_player_info> result;
			for (const auto& pair : data.cached_players)
			{
				result.push_back(pair.second);
			}
			return result;
		});
	}

	bool has_cached_player(std::uint64_t steam_id)
	{
		return g_data.access<bool>([&](target_data_t& data)
		{
			return data.cached_players.find(steam_id) != data.cached_players.end();
		});
	}

	std::optional<cached_player_info> get_cached_player(std::uint64_t steam_id)
	{
		return g_data.access<std::optional<cached_player_info>>([&](target_data_t& data) -> std::optional<cached_player_info>
		{
			const auto it = data.cached_players.find(steam_id);
			if (it != data.cached_players.end())
			{
				return it->second;
			}
			return std::nullopt;
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

				// Try to auto-detect from cache
				if (player_id == 0 || mother_base_id == 0)
				{
					const auto cached = get_cached_player(steam_id);
					if (cached.has_value())
					{
						if (player_id == 0) player_id = cached->player_id;
						if (mother_base_id == 0) mother_base_id = cached->mother_base_id;
						if (name.empty()) name = cached->name;
						console::info("Auto-detected from cache: player_id=%u, mother_base_id=%u, name=%s",
							player_id, mother_base_id, name.c_str());
					}
				}

				add_target(steam_id, player_id, mother_base_id, name);

				console::info("Added FOB target: %llu (player_id: %u, mother_base_id: %u, name: %s)",
					steam_id, player_id, mother_base_id, name.empty() ? std::to_string(steam_id).c_str() : name.c_str());
			});

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
			});

			command::add("fob_clear_targets", []()
			{
				g_data.access([&](target_data_t& data)
				{
					data.targets.clear();
				});
				console::info("All FOB targets cleared.");
			});

			command::add("fob_target_list", []()
			{
				const auto targets = get_targets();
				if (targets.empty())
				{
					console::info("No custom FOB targets.");
					return;
				}

				console::info("Custom FOB targets (%zu):", targets.size());
				for (const auto& target : targets)
				{
					console::info("  %llu: player_id=%u, mother_base_id=%u, name=%s%s",
						target.steam_id, target.player_id, target.mother_base_id, target.name.c_str(),
						target.has_cached_info ? " (cached)" : "");
				}
			});

			command::add("fob_cache_list", []()
			{
				const auto cached = get_cached_players();
				if (cached.empty())
				{
					console::info("No cached player info.");
					console::info("Browse FOB lists in game first to populate cache.");
					return;
				}

				console::info("Cached player info (%zu):", cached.size());
				for (const auto& info : cached)
				{
					console::info("  %llu: player_id=%u, mother_base_id=%u, name=%s (E%d/%d/%d)",
						info.steam_id, info.player_id, info.mother_base_id, info.name.c_str(),
						info.espionage_score, info.espionage_win, info.espionage_total);
				}
			});

			command::add("fob_cache_clear", []()
			{
				g_data.access([&](target_data_t& data)
				{
					data.cached_players.clear();
				});
				console::info("Cached player info cleared.");
			});
		}
	};
}

REGISTER_COMPONENT(fob_target::component)