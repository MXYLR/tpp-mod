#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "game/structs.hpp"

namespace fob_target
{
	struct custom_target
	{
		std::uint64_t steam_id;
		std::uint32_t player_id;
		std::uint32_t mother_base_id;
		std::string name;
		bool has_cached_info;
		game::tpp::mbm::PlayerBasicInfo cached_info;
	};

	struct cached_player_info
	{
		std::uint64_t steam_id;
		std::uint32_t player_id;
		std::uint32_t mother_base_id;
		std::string name;
		int espionage_score;
		int espionage_win;
		int espionage_total;
		game::tpp::mbm::PlayerBasicInfo full_info;
	};

	void add_target(std::uint64_t steam_id, std::uint32_t player_id = 0,
		std::uint32_t mother_base_id = 0, const std::string& name = {});

	void remove_target(std::uint64_t steam_id);

	std::vector<custom_target> get_targets();

	bool has_custom_targets();

	void cache_player_info(const game::tpp::mbm::PlayerBasicInfo& player_info);

	std::vector<cached_player_info> get_cached_players();

	bool has_cached_player(std::uint64_t steam_id);

	std::optional<cached_player_info> get_cached_player(std::uint64_t steam_id);
}
