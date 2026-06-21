#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "fobs.hpp"
#include "fob_target.hpp"
#include "vars.hpp"
#include "console.hpp"
#include "scheduler.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/concurrency.hpp>

#include <random>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <string>

namespace fobs
{
	namespace
	{
		vars::var_ptr var_fob_target_list_num;

		std::string pending_list_type;

		std::string get_fox_buffer(game::fox::Buffer* buffer)
		{
			const auto buf = game::fox::Buffer_::GetBuffer(buffer);
			const auto buf_size = game::fox::Buffer_::GetSize(buffer);
			const auto data = std::string{buf, buf + buf_size};
			return data;
		}

		utils::hook::detour cmd_get_fob_target_list_option_pack_hook;
		utils::hook::detour cmd_get_fob_target_list_result_unpack_hook;
		utils::hook::detour fob_target_receive_enemy_basic_info_hook;

		char cmd_get_fob_target_list_option_pack_stub(game::tpp::net::CmdGetFobTargetListOption* option)
		{
			pending_list_type = option->type.data->buffer;

			const auto custom_num = var_fob_target_list_num->current.get_int();
			if (custom_num > 0)
			{
				console::info("[FOB] Overriding num parameter: %d -> %d (type=%s)",
					option->num, custom_num, option->type.data->buffer);
				option->num = custom_num;
			}

			return cmd_get_fob_target_list_option_pack_hook.invoke<char>(option);
		}

		char cmd_get_fob_target_list_result_unpack_stub(game::tpp::net::CmdGetFobTargetListResult<16> *list)
		{
			return cmd_get_fob_target_list_result_unpack_hook.invoke<char>(list);
		}

		void inject_custom_targets_to_fob_target(game::tpp::net::FobTarget* fob_target, short& current_idx)
		{
			const auto& custom_targets = fob_target::get_targets();

			for (const auto& custom_target : custom_targets)
			{
				if (current_idx >= fob_target->maxPlayers)
				{
					break;
				}

				auto& player_info = fob_target->playerInfos[current_idx];

				if (custom_target.has_cached_info)
				{
					std::memcpy(&player_info, &custom_target.cached_info, sizeof(player_info));
					console::info("Injected custom target (from cache): %llu - player_id: %u, mother_base_num: %d, mother_base_id[0]: %u",
						custom_target.steam_id, player_info.owner_player_id,
						player_info.mother_base_num, player_info.mother_base_num > 0 ? player_info.mother_base_id[0] : 0);
				}
				else
				{
					std::memset(&player_info, 0, sizeof(player_info));

					player_info.owner_account.id = custom_target.steam_id;
					player_info.mother_base_num = 1;
					player_info.owner_player_id = custom_target.player_id;

					player_info.mother_base_id[0] = custom_target.mother_base_id;
					player_info.area_id[0] = 0;
					player_info.security_rank[0] = 50;
					player_info.platform_count[0] = 32;
					player_info.construct_param2[0] = 0;

					player_info.owner_ugc = 1;
					player_info.league_rank_grade = 4;
					player_info.league_rank_rank = 1000;
					player_info.sneak_rank_grade = 4;
					player_info.sneak_rank_rank = 1000;
					player_info.espionage_score = 0;
					player_info.espionage_win = 0;
					player_info.espionage_total = 0;

					player_info.staff_num = 0;
					for (auto o = 0; o < 10; o++)
					{
						player_info.staff_count[o] = 0;
					}

					player_info.usable_resource.fuel_resource = 100000;
					player_info.usable_resource.biotic_resource = 100000;
					player_info.usable_resource.common_metal = 100000;
					player_info.usable_resource.minor_metal = 100000;
					player_info.usable_resource.precious_metal = 100000;

					player_info.nameplate_id = 0;

					std::memset(&player_info.owner_emblem, 0, sizeof(game::tpp::mbm::PlayerBasicInfo::Emblem));

					console::info("Injected custom target (default): %llu - player_id: %u, mother_base_id: %u",
						custom_target.steam_id, custom_target.player_id, custom_target.mother_base_id);
				}

				game::tpp::net::DisplayName_::AddList(fob_target->displayName1, &player_info.owner_account);
				current_idx++;
			}
		}

		void receive_custom_challenge_list(game::tpp::net::FobTarget* fob_target, game::tpp::net::CmdGetFobTargetListResult<0>* list)
		{
			const auto current_list_type = pending_list_type;
			pending_list_type.clear();

			console::info("[FOB] Requested list type: %s, has_custom_targets: %s",
				current_list_type.c_str(),
				fob_target::has_custom_targets() ? "YES" : "NO");

			fob_target_receive_enemy_basic_info_hook.invoke<void>(fob_target, list);

			console::info("[FOB] Caching players from official list...");

			int cached_count = 0;
			for (auto i = 0; i < fob_target->maxPlayers; i++)
			{
				const auto& player_info = fob_target->playerInfos[i];
				if (player_info.owner_account.id == 0)
				{
					break;
				}

				fob_target::cache_player_info(player_info);
				cached_count++;
			}

			console::info("[FOB] Cached %d players from official list", cached_count);

			if (fob_target::has_custom_targets())
			{
				console::info("[FOB] Appending custom targets to list");

				short current_idx = 0;
				for (auto i = 0; i < fob_target->maxPlayers; i++)
				{
					if (fob_target->playerInfos[i].owner_account.id == 0)
					{
						current_idx = static_cast<short>(i);
						break;
					}
					current_idx = static_cast<short>(i + 1);
				}

				inject_custom_targets_to_fob_target(fob_target, current_idx);
				game::tpp::net::DisplayName_::GetDisplayName(fob_target->displayName1);
				game::tpp::net::DisplayName_::GetDisplayName(fob_target->displayName2);
			}
		}

		bool request_refresh_current_tab{};

		void request_refresh_tab()
		{
			request_refresh_current_tab = true;
		}

		void refresh_current_tab(game::tpp::ui::menu::mbm::impl::FobMission2CallbackImpl* fob_mission)
		{
			if (fob_mission->state != 52)
			{
				return;
			}

			fob_mission->hasLoadedTab[fob_mission->currentTab] = 0;
			switch (fob_mission->currentTab)
			{
			case 0:
				fob_mission->state = 20;
				break;
			case 1:
				fob_mission->state = 22;
				break;
			case 2:
				fob_mission->state = 24;
				break;
			case 3:
				fob_mission->state = 28;
				break;
			case 4:
				fob_mission->state = 30;
				break;
			case 5:
				fob_mission->state = 26;
				break;
			case 6:
				fob_mission->state = 32;
				break;
			case 7:
				fob_mission->state = 35;
				break;
			case 8:
				fob_mission->state = 39;
				break;
			case 9:
				fob_mission->state = 41;
				break;
			default:
				fob_mission->state = 45;
				break;
			}
		}

		void fob_mission2_callback_update_stub(game::tpp::ui::menu::mbm::impl::FobMission2CallbackImpl* fob_mission, void* a2, void* a3)
		{
			if (request_refresh_current_tab)
			{
				refresh_current_tab(fob_mission);
				request_refresh_current_tab = false;
			}

			fob_mission2_callback_update_hook.invoke<void>(fob_mission, a2, a3);
		}
	}

	void add_custom_fob_target(const std::string& type, const game::tpp::mbm::PlayerBasicInfo& info)
	{
		custom_fob_targets.access([&](custom_fob_targets_t& types)
		{
			auto& targets = types[type];
			for (auto& target : targets)
			{
				if (target.owner_account.id == info.owner_account.id)
				{
					return;
				}
			}

			targets.emplace_back(info);
		});
	}

	void remove_custom_fob_target(const std::string& type, const std::uint64_t steam_id)
	{
		custom_fob_targets.access([&](custom_fob_targets_t& types)
		{
			auto& targets = types[type];
			for (auto i = targets.begin(); i != targets.end(); ++i)
			{
				if (i->owner_account.id == steam_id)
				{
					i = targets.erase(i);
					return;
				}
			}
		});
	}

	void clear_custom_fob_targets()
	{
		custom_fob_targets.access([&](custom_fob_targets_t& types)
		{
			types.clear();
		});
	}

	void access_custom_fob_targets(const std::function<void(custom_fob_targets_t&)> callback)
	{
		custom_fob_targets.access(callback);
	}

	std::uint32_t get_own_player_id()
	{
		return state.access<std::uint32_t>([&](state_t& s)
		{
			return s.own_lobby_info.player_id;
		});
	}

	class component final : public component_interface
	{
	public:
		void pre_load() override
		{
			if (!game::environment::is_tpp())
			{
				return;
			}

			var_fob_target_list_num = vars::register_int("fob_target_list_num", 0, 0, 1000,
				vars::var_flag_saved, "Override the num parameter sent in CMD_GET_FOB_TARGET_LIST (0 = disabled, use original value)");
		}

		void start() override
		{
			if (!game::environment::is_tpp())
			{
				return;
			}

			cmd_get_fob_target_list_result_unpack_hook.create(SELECT_VALUE_LANG(0x140817CB0, 0x140816B90), cmd_get_fob_target_list_result_unpack_stub);
			cmd_get_fob_target_list_option_pack_hook.create(SELECT_VALUE_LANG(0x140817B60, 0x1474F31B0), cmd_get_fob_target_list_option_pack_stub);

			fob_target_receive_enemy_basic_info_hook.create(SELECT_VALUE_LANG(0x1459F5940, 0x147443580), fob_target_receive_enemy_basic_info_stub);
		}
	};
}

REGISTER_COMPONENT(fobs::component)
