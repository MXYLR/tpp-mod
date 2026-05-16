#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

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
		vars::var_ptr var_fob_security_challenge_mode;
		vars::var_ptr var_fob_override_list_type;
		vars::var_ptr var_fob_override_list_mode;
		vars::var_ptr var_dispatch_intercept_mode;
		vars::var_ptr var_dispatch_success_rate;

		std::string pending_list_type;

		bool dispatch_intercept_enabled()
		{
			return var_dispatch_intercept_mode->current.get_int() == 1;
		}

		enum class dispatch_command_type
		{
			unknown,
			sneak_mother_base,
			check_defence_motherbase,
			abort_mother_base,
			deploy_mission,
			send_troops,
			cancel_combat_deploy,
			cancel_combat_deploy_single,
			elapse_combat_deploy,
			get_combat_deploy_list,
			get_combat_deploy_result,
		};

		struct pending_dispatch_t
		{
			bool active = false;
			std::string command_name;
			dispatch_command_type type = dispatch_command_type::unknown;
			nlohmann::json fake_response;
		};

		std::unordered_map<std::string, dispatch_command_type> dispatch_command_map =
		{
			{"CMD_SNEAK_MOTHER_BASE", dispatch_command_type::sneak_mother_base},
			{"CMD_CHECK_DEFENCE_MOTHERBASE", dispatch_command_type::check_defence_motherbase},
			{"CMD_ABORT_MOTHER_BASE", dispatch_command_type::abort_mother_base},
			{"CMD_DEPLOY_MISSION", dispatch_command_type::deploy_mission},
			{"CMD_SEND_TROOPS", dispatch_command_type::send_troops},
			{"CMD_CANCEL_COMBAT_DEPLOY", dispatch_command_type::cancel_combat_deploy},
			{"CMD_CANCEL_COMBAT_DEPLOY_SINGLE", dispatch_command_type::cancel_combat_deploy_single},
			{"CMD_ELAPSE_COMBAT_DEPLOY", dispatch_command_type::elapse_combat_deploy},
			{"CMD_GET_COMBAT_DEPLOY_LIST", dispatch_command_type::get_combat_deploy_list},
			{"CMD_GET_COMBAT_DEPLOY_RESULT", dispatch_command_type::get_combat_deploy_result},
		};

		pending_dispatch_t pending_dispatch;

		bool custom_lobbies_enabled()
		{
			return var_fob_security_challenge_mode->current.get_int() == 1;
		}

		bool should_override_list(const std::string& list_type)
		{
			const auto override_type = var_fob_override_list_type->current.get_int();
			if (override_type == 0)
			{
				return false;
			}

			static const std::unordered_map<int, std::string> type_map =
			{
				{1, "TRIAL"},
				{2, "PICKUP"},
				{3, "PICKUP_HIGH"},
				{4, "ENEMY"},
				{5, "EVENT"},
				{6, "NUCLEAR"},
				{7, "FOLLOW"},
				{8, "FOLLOWER"},
				{9, "DEPLOYED"},
				{10, "INJURY"},
				{11, "EMERGENCY"},
				{12, "FR_ENEMY"},
			};

			const auto iter = type_map.find(override_type);
			if (iter == type_map.end())
			{
				return false;
			}

			return list_type == iter->second;
		}

		bool should_replace_list()
		{
			return var_fob_override_list_mode->current.get_int() == 0;
		}

		std::string get_fox_buffer(game::fox::Buffer* buffer)
		{
			const auto buf = game::fox::Buffer_::GetBuffer(buffer);
			const auto buf_size = game::fox::Buffer_::GetSize(buffer);
			const auto data = std::string{buf, buf + buf_size};
			return data;
		}

		nlohmann::json create_fake_sneak_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";

			result["damage_param"] = nlohmann::json::array();
			result["event_fob_params"] = {0, 0, 0, 0, 0};

			result["fob_deploy_damage_param"]["cluster_index"] = 0;
			result["fob_deploy_damage_param"]["expiration_date"] = 0;
			result["fob_deploy_damage_param"]["motherbase_id"] = 0;
			for (auto i = 0; i < 16; i++)
			{
				result["fob_deploy_damage_param"]["damage_values"][i] = 0;
			}

			result["is_event"] = request.contains("is_event") ? request["is_event"].get<int>() : 0;
			result["is_security_contract"] = 0;
			result["owner_gmp"] = 0;

			result["recover_resource"]["biotic_resource"] = 0;
			result["recover_resource"]["common_metal"] = 0;
			result["recover_resource"]["fuel_resource"] = 0;
			result["recover_resource"]["minor_metal"] = 0;
			result["recover_resource"]["precious_metal"] = 0;

			result["recover_soldier"] = nlohmann::json::array();
			result["recover_soldier_count"] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
			result["recover_soldier_num"] = 0;
			result["reward_id"] = 0;
			result["reward_soldier"] = nlohmann::json::array();
			result["reward_soldier_num"] = 0;
			result["reward_soldier_rank"] = 0;
			result["reward_soldier_type"] = 0;

			result["security_soldier"] = nlohmann::json::array();
			result["security_soldier_num"] = 0;
			result["security_soldier_rank"] = 0;

			auto& stage_param = result["stage_param"];

			stage_param["cluster_param"] = nlohmann::json::object();
			stage_param["build"] = {0, 0, 0, 0, 0, 0, 0};
			stage_param["construct_param"] = 0;
			stage_param["fob_index"] = 0;
			stage_param["mother_base_id"] = request.contains("mother_base_id") ? request["mother_base_id"].get<std::uint64_t>() : 0;
			stage_param["nuclear"] = 0;
			stage_param["owner_player_id"] = request.contains("player_id") ? request["player_id"].get<std::uint64_t>() : 0;

			stage_param["placement"]["emplacement_gun_east"] = 0;
			stage_param["placement"]["emplacement_gun_west"] = 0;
			stage_param["placement"]["gatling_gun"] = 0;
			stage_param["placement"]["gatling_gun_east"] = 0;
			stage_param["placement"]["gatling_gun_west"] = 0;
			stage_param["placement"]["mortar_normal"] = 0;

			stage_param["platform"] = request.contains("platform") ? request["platform"].get<int>() : 0;
			stage_param["equip_grade"] = 50;
			stage_param["security_level"] = 50;

			stage_param["processing_resource"]["fuel_resource"] = 0;
			stage_param["processing_resource"]["biotic_resource"] = 0;
			stage_param["processing_resource"]["common_metal"] = 0;
			stage_param["processing_resource"]["minor_metal"] = 0;
			stage_param["processing_resource"]["precious_metal"] = 0;

			stage_param["section_level"]["base_development"] = 50;
			stage_param["section_level"]["command"] = 50;
			stage_param["section_level"]["combat"] = 50;
			stage_param["section_level"]["intelligence"] = 50;
			stage_param["section_level"]["medical"] = 50;
			stage_param["section_level"]["rd"] = 50;
			stage_param["section_level"]["support"] = 50;

			stage_param["usable_resource"]["fuel_resource"] = 0;
			stage_param["usable_resource"]["biotic_resource"] = 0;
			stage_param["usable_resource"]["common_metal"] = 0;
			stage_param["usable_resource"]["minor_metal"] = 0;
			stage_param["usable_resource"]["precious_metal"] = 0;

			result["wormhole_player_id"] = request.contains("wormhole_player_id") ? request["wormhole_player_id"].get<std::uint64_t>() : 0;

			return result;
		}

		nlohmann::json create_fake_check_defence_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			result["check_result"] = 0;
			return result;
		}

		nlohmann::json create_fake_abort_mother_base_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			return result;
		}

		nlohmann::json create_fake_deploy_mission_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			result["deploy_id"] = 1;
			result["arrival_date"] = static_cast<std::int64_t>(std::time(nullptr)) + 60;
			return result;
		}

		nlohmann::json create_fake_send_troops_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			result["deploy_id"] = 1;
			return result;
		}

		nlohmann::json create_fake_cancel_combat_deploy_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			return result;
		}

		nlohmann::json create_fake_elapse_combat_deploy_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			result["result_list"] = nlohmann::json::array();
			result["result_num"] = 0;
			return result;
		}

		nlohmann::json create_fake_get_combat_deploy_list_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			result["deploy_list"] = nlohmann::json::array();
			result["deploy_num"] = 0;
			return result;
		}

		nlohmann::json create_fake_get_combat_deploy_result_response(const nlohmann::json& request)
		{
			nlohmann::json result;
			result["result"] = "NOERR";
			result["xuid"] = nlohmann::json::array();
			result["result_list"] = nlohmann::json::array();
			result["result_num"] = 0;
			return result;
		}

		nlohmann::json create_fake_response(dispatch_command_type type, const nlohmann::json& request)
		{
			switch (type)
			{
			case dispatch_command_type::sneak_mother_base:
				return create_fake_sneak_response(request);
			case dispatch_command_type::check_defence_motherbase:
				return create_fake_check_defence_response(request);
			case dispatch_command_type::abort_mother_base:
				return create_fake_abort_mother_base_response(request);
			case dispatch_command_type::deploy_mission:
				return create_fake_deploy_mission_response(request);
			case dispatch_command_type::send_troops:
				return create_fake_send_troops_response(request);
			case dispatch_command_type::cancel_combat_deploy:
			case dispatch_command_type::cancel_combat_deploy_single:
				return create_fake_cancel_combat_deploy_response(request);
			case dispatch_command_type::elapse_combat_deploy:
				return create_fake_elapse_combat_deploy_response(request);
			case dispatch_command_type::get_combat_deploy_list:
				return create_fake_get_combat_deploy_list_response(request);
			case dispatch_command_type::get_combat_deploy_result:
				return create_fake_get_combat_deploy_result_response(request);
			default:
				{
					nlohmann::json result;
					result["result"] = "NOERR";
					return result;
				}
			}
		}

		bool should_fake_success()
		{
			const auto success_rate = var_dispatch_success_rate->current.get_int();
			if (success_rate <= 0)
			{
				return false;
			}
			if (success_rate >= 100)
			{
				return true;
			}

			std::random_device rd;
			std::mt19937 gen(rd());
			std::uniform_int_distribution<> dist(1, 100);
			return dist(gen) <= success_rate;
		}

		void on_lobby_match_list(game::LobbyMatchList_t* match_list);
		void on_lobby_created(game::LobbyCreated_t* lobby_enter);

		class lobby_list_handler : public game::CCallbackBase
		{
		public:
			lobby_list_handler()
			{
				this->m_iCallback = game::LobbyMatchList_t::k_iCallback;
			}

			void Run(void* pvParam) override
			{
				on_lobby_match_list(reinterpret_cast<game::LobbyMatchList_t*>(pvParam));
			}

			void Run(void* pvParam, bool bIOFailure, game::SteamAPICall_t hSteamAPICall) override
			{
				on_lobby_match_list(reinterpret_cast<game::LobbyMatchList_t*>(pvParam));
			}

			int GetCallbackSizeBytes() override
			{
				return 8;
			}
		};

		class lobby_create_handler : public game::CCallbackBase
		{
		public:
			lobby_create_handler()
			{
				this->m_iCallback = game::LobbyCreated_t::k_iCallback;
			}

			void Run(void* pvParam) override
			{
				on_lobby_created(reinterpret_cast<game::LobbyCreated_t*>(pvParam));
			}

			void Run(void* pvParam, bool bIOFailure, game::SteamAPICall_t hSteamAPICall) override
			{
				on_lobby_created(reinterpret_cast<game::LobbyCreated_t*>(pvParam));
			}

			int GetCallbackSizeBytes() override
			{
				return 8;
			}
		};

		struct
		{
			bool initialized;
			game::ISteamMatchmaking* (*steam_matchmaking)();
			void (*register_callback)(game::CCallbackBase*, unsigned __int64);
			void (*register_call_result)(game::CCallbackBase*, unsigned __int64);
			void (*run_calbacks)();
			lobby_list_handler lobby_handler{};
			lobby_create_handler lobby_create_handler{};
		} steam_api{};

		struct lobby_t
		{
			struct mother_base_t
			{
				std::uint32_t construct_param;
				std::uint32_t platform_count;
				std::uint32_t mother_base_id;
				std::uint32_t security_rank;
			};

			game::steam_id lobby_id;
			game::steam_id owner_id;
			std::uint32_t player_id;
			std::uint32_t mother_base_num;
			mother_base_t mother_base_param[4];
			std::int32_t name_plate_id;
			std::int32_t league_rank;
			std::int32_t league_grade;
			std::int32_t espionage_rank;
			std::int32_t espionage_grade;
			std::int32_t espionage_point;
			std::int32_t espionage_win;
			std::int32_t espionage_lose;
			std::int32_t staff_count[10];
			game::tpp::mbm::PlayerBasicInfo::Resource total_resource;
			game::tpp::mbm::PlayerBasicInfo::Emblem emblem;
		};

		struct state_t
		{
			bool got_lobby_list;
			std::int32_t lobby_count;
			std::vector<lobby_t> lobby_list;
			std::uint64_t lobby_list_request;

			bool requested_lobby_create;
			bool is_lobby_created;
			std::int32_t lobby_create_result;
			game::steam_id own_lobby_id;

			lobby_t own_lobby_info{};
		};

		utils::hook::detour cmd_get_fob_target_list_option_pack_hook;
		utils::hook::detour cmd_get_fob_target_list_result_unpack_hook;

		utils::hook::detour cmd_set_security_challenge_option_pack_hook;
		utils::hook::detour cmd_set_security_challenge_result_unpack_hook;

		utils::hook::detour cmd_get_playerlist_result_unpack_hook;
		utils::hook::detour cmd_set_currentplayer_result_unpack_hook;
		utils::hook::detour cmd_sync_mother_base_option_pack_hook;
		utils::hook::detour cmd_sync_soldier_bin_pack_hook;
		utils::hook::detour cmd_sync_resource_result_unpack_hook;
		utils::hook::detour cmd_get_own_fob_list_result_unpack_hook;

		utils::hook::detour fob_target_receive_enemy_basic_info_hook;

		utils::concurrency::container<state_t> state;
		
		void update_lobby(state_t& s)
		{
			const auto steam_matchmaking = steam_api.steam_matchmaking();
			if (s.own_lobby_id.bits == 0)
			{
				return;
			}

			const auto set_lobby_data = [&](const char* key, const int index, const int value)
			{
				const auto value_str = utils::string::va("%i", value);

				if (index != -1)
				{
					key = utils::string::va("%s_%i", key, index);
				}

				steam_matchmaking->__vftable->SetLobbyData(steam_matchmaking, s.own_lobby_id, key, value_str);
			};

			const auto owner_id_str = utils::string::va("%llu", s.own_lobby_info.owner_id.bits);
			steam_matchmaking->__vftable->SetLobbyData(steam_matchmaking, s.own_lobby_id, "owner_id", owner_id_str);

			set_lobby_data("is_security_challenge", -1, 1);
			set_lobby_data("player_id", -1, s.own_lobby_info.player_id);
			set_lobby_data("league_rank", -1, s.own_lobby_info.league_rank);
			set_lobby_data("league_grade", -1, s.own_lobby_info.league_grade);
			set_lobby_data("espionage_rank", -1, s.own_lobby_info.espionage_rank);
			set_lobby_data("espionage_grade", -1, s.own_lobby_info.espionage_grade);
			set_lobby_data("espionage_point", -1, s.own_lobby_info.espionage_point);
			set_lobby_data("espionage_win", -1, s.own_lobby_info.espionage_win);
			set_lobby_data("espionage_lose", -1, s.own_lobby_info.espionage_lose);
			set_lobby_data("mother_base_num", -1, s.own_lobby_info.mother_base_num);

			for (auto i = 0; i < 10; i++)
			{
				set_lobby_data("staff_count", i, s.own_lobby_info.staff_count[i]);
			}

			set_lobby_data("fuel_resource", -1, s.own_lobby_info.total_resource.fuel_resource);
			set_lobby_data("biotic_resource", -1, s.own_lobby_info.total_resource.biotic_resource);
			set_lobby_data("common_metal", -1, s.own_lobby_info.total_resource.common_metal);
			set_lobby_data("minor_metal", -1, s.own_lobby_info.total_resource.minor_metal);
			set_lobby_data("precious_metal", -1, s.own_lobby_info.total_resource.precious_metal);

			set_lobby_data("name_plate_id", -1, s.own_lobby_info.name_plate_id);

			for (auto i = 0u; i < 4; i++)
			{
				set_lobby_data("emblem_texture_tag", i, s.own_lobby_info.emblem.texture_tag[i]);
				set_lobby_data("emblem_base_color", i, s.own_lobby_info.emblem.base_color[i]);
				set_lobby_data("emblem_frame_color", i, s.own_lobby_info.emblem.frame_color[i]);
				set_lobby_data("emblem_position_x", i, s.own_lobby_info.emblem.position_x[i]);
				set_lobby_data("emblem_position_y", i, s.own_lobby_info.emblem.position_y[i]);
				set_lobby_data("emblem_scale", i, s.own_lobby_info.emblem.scale[i]);
				set_lobby_data("emblem_rotate", i, s.own_lobby_info.emblem.rotate[i]);
			}

			for (auto i = 0u; i < s.own_lobby_info.mother_base_num; i++)
			{
				set_lobby_data("construct_param", i, s.own_lobby_info.mother_base_param[i].construct_param);
				set_lobby_data("security_rank", i, s.own_lobby_info.mother_base_param[i].security_rank);
				set_lobby_data("mother_base_id", i, s.own_lobby_info.mother_base_param[i].mother_base_id);
				set_lobby_data("platform_count", i, s.own_lobby_info.mother_base_param[i].platform_count);
			}
		}

		void load_lobbies(state_t& s, int num_lobbies)
		{
			s.lobby_list.clear();

			const auto steam_matchmaking = steam_api.steam_matchmaking();
			const auto add_lobby = [&](game::steam_id lobby_id)
			{
				lobby_t lobby{};

				const auto get_lobby_data = [&](const char* key, const int index = -1)
				{
					if (index != -1)
					{
						key = utils::string::va("%s_%i", key, index);
					}

					return std::atoi(steam_matchmaking->__vftable->GetLobbyData(steam_matchmaking,
						lobby_id, key));
				};

				const auto owner_id = steam_matchmaking->__vftable->GetLobbyData(steam_matchmaking, lobby_id, "owner_id");

				lobby.owner_id.bits = std::strtoull(owner_id, nullptr, 0);
				lobby.player_id = get_lobby_data("player_id");
				lobby.league_rank = get_lobby_data("league_rank");
				lobby.league_grade = get_lobby_data("league_grade");
				lobby.espionage_rank = get_lobby_data("espionage_rank");
				lobby.espionage_grade = get_lobby_data("espionage_grade");
				lobby.espionage_point = get_lobby_data("espionage_point");
				lobby.espionage_win = get_lobby_data("espionage_win");
				lobby.espionage_lose = get_lobby_data("espionage_lose");

				for (auto i = 0; i < 10; i++)
				{
					lobby.staff_count[i] = get_lobby_data("staff_count", i);
				}

				lobby.total_resource.fuel_resource = get_lobby_data("fuel_resource");
				lobby.total_resource.biotic_resource = get_lobby_data("biotic_resource");
				lobby.total_resource.common_metal = get_lobby_data("common_metal");
				lobby.total_resource.minor_metal = get_lobby_data("minor_metal");
				lobby.total_resource.precious_metal = get_lobby_data("precious_metal");

				lobby.name_plate_id = get_lobby_data("name_plate_id");

				for (auto i = 0; i < 4; i++)
				{
					lobby.emblem.texture_tag[i] = get_lobby_data("emblem_texture_tag", i);
					lobby.emblem.base_color[i] = get_lobby_data("emblem_base_color", i);
					lobby.emblem.frame_color[i] = get_lobby_data("emblem_frame_color", i);
					lobby.emblem.position_x[i] = static_cast<char>(get_lobby_data("emblem_position_x", i));
					lobby.emblem.position_y[i] = static_cast<char>(get_lobby_data("emblem_position_y", i));
					lobby.emblem.rotate[i] = static_cast<char>(get_lobby_data("emblem_rotate", i));
					lobby.emblem.scale[i] = static_cast<char>(get_lobby_data("emblem_scale", i));
				}

				lobby.mother_base_num = get_lobby_data("mother_base_num");

				for (auto i = 0u; i < lobby.mother_base_num; i++)
				{
					lobby.mother_base_param[i].construct_param = get_lobby_data("construct_param", i);
					lobby.mother_base_param[i].security_rank = get_lobby_data("security_rank", i);
					lobby.mother_base_param[i].mother_base_id = get_lobby_data("mother_base_id", i);
					lobby.mother_base_param[i].platform_count = get_lobby_data("platform_count", i);
				}

				s.lobby_list.emplace_back(lobby);
			};

			s.lobby_list.emplace_back(s.own_lobby_info);

			for (auto i = 0; i < num_lobbies; i++)
			{
				game::steam_id lobby_id{};
				steam_matchmaking->__vftable->GetLobbyByIndex(steam_matchmaking, &lobby_id, i);

				if (lobby_id.bits == s.own_lobby_id.bits)
				{
					continue;
				}

				add_lobby(lobby_id);
			}
		}

		void on_lobby_match_list(game::LobbyMatchList_t* match_list)
		{
			state.access([&](state_t& s)
			{
				load_lobbies(s, match_list->num_lobbies);
				s.got_lobby_list = true;
			});
		}

		void on_lobby_created(game::LobbyCreated_t* lobby_enter)
		{
			state.access([&](state_t& s)
			{
				s.lobby_create_result = lobby_enter->result == 1;
				s.own_lobby_id = lobby_enter->lobby_id;
				s.requested_lobby_create = false;
				update_lobby(s);
			});
		}

		void request_create_lobby(state_t& s)
		{
			if (!steam_api.initialized)
			{
				return;
			}

			const auto matchmaking = (*steam_api.steam_matchmaking)();
			matchmaking->__vftable->CreateLobby(matchmaking, 2, 2);

			s.own_lobby_id.bits = 0;
			s.requested_lobby_create = true;
			s.lobby_create_result = -1;
		}

		void request_close_lobby(state_t& s)
		{
			if (!steam_api.initialized)
			{
				return;
			}

			const auto matchmaking = (*steam_api.steam_matchmaking)();
			game::steam_id lobby_id{};
			matchmaking->__vftable->LeaveLobby(matchmaking, s.own_lobby_id);
			s.own_lobby_id.bits = 0;
		}

		void request_lobby_list(state_t& s)
		{
			if (!steam_api.initialized)
			{
				return;
			}

			const auto matchmaking = (*steam_api.steam_matchmaking)();
			matchmaking->__vftable->AddRequestLobbyListStringFilter(matchmaking, "is_security_challenge", "1", 0);
			const auto handle = matchmaking->__vftable->RequestLobbyList(matchmaking);

			s.got_lobby_list = false;
			s.lobby_list.clear();

			steam_api.register_call_result(&steam_api.lobby_handler, handle);
		}

		void wait_for_lobby_list()
		{
			const auto should_wait = [&]()
			{
				return state.access<bool>([&](state_t& s)
				{
					return !s.got_lobby_list;
				});
			};

			const auto start = std::chrono::steady_clock::now();
			while (should_wait() && std::chrono::steady_clock::now() - start < 5s)
			{
				std::this_thread::sleep_for(10ms);
			}
		}

		char cmd_get_fob_target_list_option_pack_stub(game::tpp::net::CmdGetFobTargetListOption* option)
		{
			pending_list_type = option->type.data->buffer;

			if (custom_lobbies_enabled() && option->type.data->buffer == "CHALLENGE"s)
			{
				state.access([&](state_t& s)
				{
					request_lobby_list(s);
				});
			}

			return cmd_get_fob_target_list_option_pack_hook.invoke<char>(option);
		}

		char cmd_get_fob_target_list_result_unpack_stub(game::tpp::net::CmdGetFobTargetListResult<16> *list)
		{
			const auto res = cmd_get_fob_target_list_result_unpack_hook.invoke<char>(list);

			if (custom_lobbies_enabled())
			{
				state.access([&](state_t& s)
				{
					list->enable_security_challenge = s.own_lobby_id.bits != 0;
				});

				if (list->type.data->buffer == "CHALLENGE"s)
				{
					wait_for_lobby_list();
				}
			}

			return res;
		}

		char cmd_set_security_challenge_option_pack_stub(game::tpp::net::CmdSetSecurityChallengeOption* option)
		{
			const auto prev_buffer = option->status.data->buffer;

			if (custom_lobbies_enabled())
			{
				option->status.data->buffer = "DISABLE";

				state.access([&](state_t& s)
				{
					if (s.own_lobby_id.bits != 0)
					{
						request_close_lobby(s);
					}
					else
					{
						request_create_lobby(s);
					}
				});
			}

			const auto res = cmd_set_security_challenge_option_pack_hook.invoke<char>(option);
			option->status.data->buffer = prev_buffer;
			return res;
		}

		char cmd_set_security_challenge_result_unpack_stub(game::tpp::net::CmdSetSecurityChallengeResult<16>* result)
		{
			if (custom_lobbies_enabled())
			{
				const auto start = std::chrono::steady_clock::now();
				const auto should_wait = [&]()
				{
					return state.access<bool>([&](state_t& s)
					{
						return s.requested_lobby_create && s.own_lobby_id.bits == 0;
					});
				};

				while (should_wait() && (std::chrono::steady_clock::now() - start) < 5s)
				{
					std::this_thread::sleep_for(10ms);
				}
			}

			return cmd_set_security_challenge_result_unpack_hook.invoke<char>(result);
		}

		char cmd_get_playerlist_result_unpack_stub(game::tpp::net::CmdGetPlayerlistResult<16>* result)
		{
			const auto res = cmd_get_playerlist_result_unpack_hook.invoke<char>(result);
			const auto steam_user = (*game::SteamUser)();

			state.access([&](state_t& s)
			{
				steam_user->__vftable->GetSteamID(steam_user, &s.own_lobby_info.owner_id);
				s.own_lobby_info.league_rank = result->player_data[0].league_rank;
				s.own_lobby_info.league_grade = result->player_data[0].league_grade;
				s.own_lobby_info.espionage_rank = result->player_data[0].fob_rank;
				s.own_lobby_info.espionage_grade = result->player_data[0].fob_grade;
				s.own_lobby_info.espionage_point = result->player_data[0].fob_point;
				s.own_lobby_info.espionage_win = result->player_data[0].espionage_win;
				s.own_lobby_info.espionage_lose = result->player_data[0].espionage_lose;
			});

			return res;
		}

		char cmd_set_currentplayer_result_unpack_stub(game::tpp::net::CmdSetCurrentplayerResult<16> *result)
		{
			const auto res = cmd_set_currentplayer_result_unpack_hook.invoke<char>(result);
			
			state.access([&](state_t& s)
			{
				s.own_lobby_info.player_id = result->player_id;
			});

			return res;
		}

		char cmd_sync_mother_base_option_pack_stub(game::tpp::net::CmdSyncMotherBaseOption* option)
		{
			state.access([&](state_t& s)
			{
				s.own_lobby_info.mother_base_num = option->mother_base_num;
				for (auto i = 0; i < option->mother_base_num; i++)
				{
					s.own_lobby_info.mother_base_param[i].construct_param = option->mother_base_param[i].construct_param;
					s.own_lobby_info.mother_base_param[i].security_rank = option->mother_base_param[i].security_rank;
					s.own_lobby_info.mother_base_param[i].platform_count = option->mother_base_param[i].platform_count;
				}

				s.own_lobby_info.name_plate_id = option->name_plate_id;

				const auto emblem = game::fox::GetQuarkSystemTable()->applicationSystem->scriptVars->emblem;
				std::memcpy(&s.own_lobby_info.emblem, &emblem, sizeof(game::tpp::mbm::PlayerBasicInfo::Emblem));

				update_lobby(s);
			});

			return cmd_sync_mother_base_option_pack_hook.invoke<char>(option);
		}

		char cmd_sync_soldier_bin_pack_stub(game::tpp::net::CmdSyncSoldierBinOption* option)
		{
			state.access([&](state_t& s)
			{
				std::memset(s.own_lobby_info.staff_count, 0, sizeof(s.own_lobby_info.staff_count));

				for (auto i = 0; i < option->soldier_num; i++)
				{
					auto staff = option->soldier_param[i];
					staff.fields.header.data = _byteswap_ulong(staff.fields.header.data);
					staff.fields.status_sync.data = _byteswap_ulong(staff.fields.status_sync.data);
					if (staff.fields.status_sync.fields.designation >= 1 && staff.fields.status_sync.fields.designation <= 7)
					{
						s.own_lobby_info.staff_count[staff.fields.header.fields.peak_rank]++;
					}
				}

				update_lobby(s);
			});

			return cmd_sync_soldier_bin_pack_hook.invoke<char>(option);
		}

		char cmd_get_own_fob_list_result_unpack_stub(game::tpp::net::CmdGetOwnFobListResult<16>* result)
		{
			const auto res = cmd_get_own_fob_list_result_unpack_hook.invoke<char>(result);

			state.access([&](state_t& s)
			{
				for (auto i = 0; i < result->fob_count; i++)
				{
					s.own_lobby_info.mother_base_param[i].mother_base_id = result->fob[i].mother_base_id;
				}
			});

			return res;
		}

		char cmd_sync_resource_result_unpack_stub(game::tpp::net::CmdSyncResourceResult<16>* result)
		{
			const auto res = cmd_sync_resource_result_unpack_hook.invoke<char>(result);

			state.access([&](state_t& s)
			{
				std::memset(&s.own_lobby_info.total_resource, 0, sizeof(s.own_lobby_info.total_resource));

				s.own_lobby_info.total_resource.fuel_resource += result->diff_resource1[0];
				s.own_lobby_info.total_resource.biotic_resource += result->diff_resource1[1];
				s.own_lobby_info.total_resource.common_metal += result->diff_resource1[2];
				s.own_lobby_info.total_resource.minor_metal += result->diff_resource1[3];
				s.own_lobby_info.total_resource.precious_metal += result->diff_resource1[4];

				s.own_lobby_info.total_resource.fuel_resource += result->diff_resource2[0];
				s.own_lobby_info.total_resource.biotic_resource += result->diff_resource2[1];
				s.own_lobby_info.total_resource.common_metal += result->diff_resource2[2];
				s.own_lobby_info.total_resource.minor_metal += result->diff_resource2[3];
				s.own_lobby_info.total_resource.precious_metal += result->diff_resource2[4];

				s.own_lobby_info.total_resource.fuel_resource += result->fix_resource1[0];
				s.own_lobby_info.total_resource.biotic_resource += result->fix_resource1[1];
				s.own_lobby_info.total_resource.common_metal += result->fix_resource1[2];
				s.own_lobby_info.total_resource.minor_metal += result->fix_resource1[3];
				s.own_lobby_info.total_resource.precious_metal += result->fix_resource1[4];

				s.own_lobby_info.total_resource.fuel_resource += result->fix_resource2[0];
				s.own_lobby_info.total_resource.biotic_resource += result->fix_resource2[1];
				s.own_lobby_info.total_resource.common_metal += result->fix_resource2[2];
				s.own_lobby_info.total_resource.minor_metal += result->fix_resource2[3];
				s.own_lobby_info.total_resource.precious_metal += result->fix_resource2[4];

				update_lobby(s);
			});

			return res;
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

		void fob_target_receive_enemy_basic_info_stub(game::tpp::net::FobTarget* fob_target, game::tpp::net::CmdGetFobTargetListResult<0>* list)
		{
			const auto current_list_type = pending_list_type;
			pending_list_type.clear();

			const bool is_override_list = should_override_list(current_list_type);
			const bool is_replace_mode = should_replace_list();

			console::info("[FOB] Requested list type: %s, override: %s, mode: %s",
				current_list_type.c_str(),
				is_override_list ? "YES" : "NO",
				is_replace_mode ? "REPLACE" : "APPEND");

			if (!custom_lobbies_enabled())
			{
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

				if (is_override_list && fob_target::has_custom_targets())
				{
					if (is_replace_mode)
					{
						console::info("[FOB] Replacing list with custom targets (%d)", fob_target::get_targets().size());

						game::tpp::net::DisplayName_::ClearList(fob_target->displayName1);
						game::tpp::net::DisplayName_::ClearList(fob_target->displayName2);

						std::memset(fob_target->playerInfos, 0, sizeof(fob_target->playerInfos));

						short current_idx = 0;
						inject_custom_targets_to_fob_target(fob_target, current_idx);
					}
					else
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
					}

					game::tpp::net::DisplayName_::GetDisplayName(fob_target->displayName1);
					game::tpp::net::DisplayName_::GetDisplayName(fob_target->displayName2);
				}
				else if (fob_target::has_custom_targets())
				{
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

				return;
			}

			game::tpp::net::DisplayName_::ClearList(fob_target->displayName1);
			game::tpp::net::DisplayName_::ClearList(fob_target->displayName2);

			state.access([&](state_t& s)
			{
				const auto count = std::min(fob_target->maxPlayers, static_cast<short>(s.lobby_list.size()));

				console::info("[FOB] Caching players from Steam lobby list...");
				for (auto i = 0; i < count; i++)
				{
					fob_target->playerInfos[i].owner_account.id = s.lobby_list[i].owner_id.bits;
					fob_target->playerInfos[i].mother_base_num = static_cast<char>(s.lobby_list[i].mother_base_num) + 1;
					fob_target->playerInfos[i].owner_player_id = s.lobby_list[i].player_id;

					for (auto o = 0u; o < s.lobby_list[i].mother_base_num; o++)
					{
						fob_target->playerInfos[i].mother_base_id[o] = s.lobby_list[i].mother_base_param[o].mother_base_id;
						fob_target->playerInfos[i].area_id[o] = static_cast<char>((s.lobby_list[i].mother_base_param[o].construct_param >> 1) & 0x7F);
						fob_target->playerInfos[i].construct_param2[o] = static_cast<char>((s.lobby_list[i].mother_base_param[o].construct_param >> 22) & 0x3F);
						fob_target->playerInfos[i].security_rank[o] = static_cast<char>(s.lobby_list[i].mother_base_param[o].security_rank);
						fob_target->playerInfos[i].platform_count[o] = static_cast<char>(s.lobby_list[i].mother_base_param[o].platform_count);
					}

					fob_target->playerInfos[i].owner_ugc = 1;
					fob_target->playerInfos[i].league_rank_grade = static_cast<char>(s.lobby_list[i].league_grade);
					fob_target->playerInfos[i].league_rank_rank = s.lobby_list[i].league_rank;
					fob_target->playerInfos[i].sneak_rank_grade = static_cast<char>(s.lobby_list[i].espionage_grade);
					fob_target->playerInfos[i].sneak_rank_rank = s.lobby_list[i].espionage_rank;
					fob_target->playerInfos[i].espionage_score = s.lobby_list[i].espionage_point;
					fob_target->playerInfos[i].espionage_win = static_cast<short>(s.lobby_list[i].espionage_win);
					fob_target->playerInfos[i].espionage_total = static_cast<short>(s.lobby_list[i].espionage_win + s.lobby_list[i].espionage_lose);

					for (auto o = 0; o < 10; o++)
					{
						fob_target->playerInfos[i].staff_count[o] = static_cast<short>(s.lobby_list[i].staff_count[o]);
						fob_target->playerInfos[i].staff_num += fob_target->playerInfos[i].staff_count[o];
					}

					fob_target->playerInfos[i].usable_resource.fuel_resource = s.lobby_list[i].total_resource.fuel_resource;
					fob_target->playerInfos[i].usable_resource.biotic_resource = s.lobby_list[i].total_resource.biotic_resource;
					fob_target->playerInfos[i].usable_resource.common_metal = s.lobby_list[i].total_resource.common_metal;
					fob_target->playerInfos[i].usable_resource.minor_metal = s.lobby_list[i].total_resource.minor_metal;
					fob_target->playerInfos[i].usable_resource.precious_metal = s.lobby_list[i].total_resource.precious_metal;

					fob_target->playerInfos[i].nameplate_id = static_cast<char>(s.lobby_list[i].name_plate_id);

					std::memcpy(&fob_target->playerInfos[i].owner_emblem, &s.lobby_list[i].emblem, sizeof(game::tpp::mbm::PlayerBasicInfo::Emblem));

					fob_target::cache_player_info(fob_target->playerInfos[i]);

					game::tpp::net::DisplayName_::AddList(fob_target->displayName1, &fob_target->playerInfos[i].owner_account);
				}

				console::info("[FOB] Cached %d players from Steam lobby list", count);

				if (fob_target::has_custom_targets())
				{
					short current_idx = static_cast<short>(s.lobby_list.size());
					inject_custom_targets_to_fob_target(fob_target, current_idx);
				}
			});

			game::tpp::net::DisplayName_::GetDisplayName(fob_target->displayName1);
			game::tpp::net::DisplayName_::GetDisplayName(fob_target->displayName2);
		}

		void initialize_steam()
		{
			if (steam_api.initialized)
			{
				return;
			}

			const auto steam = utils::nt::library("steam_api64.dll");

			steam_api.steam_matchmaking = steam.get_proc<decltype(steam_api.steam_matchmaking)>("SteamMatchmaking");
			steam_api.register_callback = steam.get_proc<decltype(steam_api.register_callback)>("SteamAPI_RegisterCallback");
			steam_api.register_call_result = steam.get_proc<decltype(steam_api.register_call_result)>("SteamAPI_RegisterCallResult");
			steam_api.run_calbacks = steam.get_proc<decltype(steam_api.run_calbacks)>("SteamAPI_RunCallbacks");
			steam_api.register_callback(&steam_api.lobby_create_handler, game::LobbyCreated_t::k_iCallback);

			steam_api.initialized = true;
		}

		void run_frame()
		{
			initialize_steam();
			if (var_fob_security_challenge_mode->changed)
			{
				state.access([&](state_t& s)
				{
					if (s.own_lobby_id.bits != 0)
					{
						request_close_lobby(s);
					}
				});

				var_fob_security_challenge_mode->changed = false;
			}
		}
	}

	// Public interface for server_logging.cpp to call
	bool process_dispatch_request(const std::string& cmd, const nlohmann::json& request)
	{
		if (!dispatch_intercept_enabled() || !should_fake_success())
		{
			return false;
		}

		const auto iter = dispatch_command_map.find(cmd);
		if (iter == dispatch_command_map.end())
		{
			return false;
		}

		const auto type = iter->second;
		console::info("[Dispatch] Intercepting %s request", cmd.c_str());

		nlohmann::json request_data = request.contains("data") ? request["data"] : nlohmann::json::object();
		pending_dispatch.fake_response = create_fake_response(type, request_data);
		pending_dispatch.fake_response["msgid"] = cmd;
		pending_dispatch.command_name = cmd;
		pending_dispatch.type = type;
		pending_dispatch.active = true;

		console::info("[Dispatch] Created fake response for %s", cmd.c_str());
		return true;
	}

	bool should_process_dispatch_response()
	{
		return pending_dispatch.active;
	}

	nlohmann::json get_fake_dispatch_response()
	{
		return pending_dispatch.fake_response;
	}

	void clear_pending_dispatch()
	{
		pending_dispatch.active = false;
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

			var_fob_security_challenge_mode = vars::register_int("fob_security_challenge_mode", 0, 0, 1,
				vars::var_flag_saved, "security challenge mode (0 = konami, 1 = steam lobbies)");

			var_fob_override_list_type = vars::register_int("fob_override_list_type", 0, 0, 12,
				vars::var_flag_saved, "FOB list type to override (0=disabled, 1=TRIAL, 2=PICKUP, 3=PICKUP_HIGH, 4=ENEMY, 5=EVENT, 6=NUCLEAR, 7=FOLLOW, 8=FOLLOWER, 9=DEPLOYED, 10=INJURY, 11=EMERGENCY, 12=FR_ENEMY)");

			var_fob_override_list_mode = vars::register_int("fob_override_list_mode", 0, 0, 1,
				vars::var_flag_saved, "FOB list override mode (0=replace, 1=append)");

			var_dispatch_intercept_mode = vars::register_int("dispatch_intercept_mode", 1, 0, 1,
				vars::var_flag_saved, "Dispatch intercept mode (0 = disabled, 1 = enabled, 100% success rate)");

			var_dispatch_success_rate = vars::register_int("dispatch_success_rate", 100, 0, 100,
				vars::var_flag_saved, "Dispatch fake success rate (0-100, default 100)");
		}

		void start() override
		{
			if (!game::environment::is_tpp())
			{
				return;
			}

			cmd_get_fob_target_list_result_unpack_hook.create(SELECT_VALUE_LANG(0x140816F40, 0x140816B90), cmd_get_fob_target_list_result_unpack_stub);
			cmd_get_fob_target_list_option_pack_hook.create(SELECT_VALUE_LANG(0x145B1AE00, 0x1474F31B0), cmd_get_fob_target_list_option_pack_stub);

			cmd_set_security_challenge_option_pack_hook.create(SELECT_VALUE_LANG(0x145BAE010, 0x14758B7D0), cmd_set_security_challenge_option_pack_stub);
			cmd_set_security_challenge_result_unpack_hook.create(SELECT_VALUE_LANG(0x145B22420, 0x1474FB2E0), cmd_set_security_challenge_result_unpack_stub);

			cmd_get_playerlist_result_unpack_hook.create(SELECT_VALUE_LANG(0x1407E2820, 0x1407E2450), cmd_get_playerlist_result_unpack_stub);
			cmd_set_currentplayer_result_unpack_hook.create(SELECT_VALUE_LANG(0x1459CDBC0, 0x147414460), cmd_set_currentplayer_result_unpack_stub);
			cmd_sync_mother_base_option_pack_hook.create(SELECT_VALUE_LANG(0x145B05460, 0x1474DA240), cmd_sync_mother_base_option_pack_stub);
			cmd_get_own_fob_list_result_unpack_hook.create(SELECT_VALUE_LANG(0x140845C80, 0x1408458B0), cmd_get_own_fob_list_result_unpack_stub);
			cmd_sync_soldier_bin_pack_hook.create(SELECT_VALUE_LANG(0x145B0B710, 0x1474E11F0), cmd_sync_soldier_bin_pack_stub);
			cmd_sync_resource_result_unpack_hook.create(SELECT_VALUE_LANG(0x145B09E50, 0x1474DEB10), cmd_sync_resource_result_unpack_stub);

			fob_target_receive_enemy_basic_info_hook.create(SELECT_VALUE_LANG(0x1459F5940, 0x147443580), fob_target_receive_enemy_basic_info_stub);

			scheduler::loop(run_frame, scheduler::net);
		}
	};
}

REGISTER_COMPONENT(fobs::component)
