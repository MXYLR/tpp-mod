#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "server_logging.hpp"
#include "fob_target.hpp"
#include "command.hpp"
#include "console.hpp"
#include "vars.hpp"

#include <utils/hook.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <sstream>
#include <map>
#include <deque>
#include <vector>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace fob_control
{
	namespace
	{
		std::atomic<bool> pipe_server_running{ false };
		std::thread pipe_server_thread;

		std::atomic<bool> auto_send_active{ false };
		std::thread auto_send_thread;
		std::atomic<uint64_t> auto_send_count{ 0 };

		std::atomic<bool> stop_music_active{ false };
		std::thread stop_music_thread;

		std::atomic<bool> occupy_active{ false };
		std::atomic<bool> occupy_established{ false };
		std::atomic<uint64_t> occupy_count{ 0 };
		std::thread occupy_thread;

		std::mutex data_mutex;
		uint64_t target_player_id = 0;
		uint64_t target_steam_id = 0;
		uint64_t my_player_id = 0;

		// FOB occupy target (guarded by data_mutex)
		uint64_t occupy_steam_id = 0;
		uint32_t occupy_player_id = 0;
		uint32_t occupy_mother_base_id = 0;
		std::string occupy_status_msg = "Idle";

		constexpr size_t MAX_LOG_ENTRIES = 1000;
		struct log_entry
		{
			int type;
			std::string message;
			uint64_t id;
		};

		std::mutex log_mutex;
		std::deque<log_entry> log_queue;
		uint64_t next_log_id = 0;

		void add_log_entry(int type, const std::string& message)
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			log_entry entry;
			entry.type = type;
			entry.message = message;
			entry.id = next_log_id++;
			log_queue.push_back(entry);
			if (log_queue.size() > MAX_LOG_ENTRIES)
			{
				log_queue.pop_front();
			}
		}

		std::string get_logs_since(uint64_t since_id)
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			std::ostringstream oss;
			bool first = true;
			for (const auto& entry : log_queue)
			{
				if (entry.id > since_id)
				{
					if (!first) oss << "||";
					first = false;
					oss << entry.id << "|" << entry.type << "|" << entry.message;
				}
			}
			return oss.str();
		}

		constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\TPPMod_FOBControl";
		constexpr DWORD PIPE_BUFFER_SIZE = 65536;

		void launch_gui_exe()
		{
			wchar_t dll_path[MAX_PATH] = {};
			GetModuleFileNameW(utils::nt::library::get_current_handle(), dll_path, MAX_PATH);

			std::wstring gui_path = dll_path;
			size_t pos = gui_path.find_last_of(L"\\/");
			if (pos != std::wstring::npos)
			{
				gui_path = gui_path.substr(0, pos + 1) + L"fob-gui.exe";
			}

			if (GetFileAttributesW(gui_path.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				return;
			}

			ShellExecuteW(nullptr, L"open", gui_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		std::string get_status()
		{
			std::lock_guard<std::mutex> lock(data_mutex);
			std::string status = "Ready";
			if (!server_logging::has_session_key())
			{
				status = "No session key";
			}
			else if (auto_send_active.load())
			{
				char buf[128];
				snprintf(buf, sizeof(buf), "Auto running... Count: %llu", (unsigned long long)auto_send_count.load());
				status = buf;
			}
			else if (stop_music_active.load())
			{
				status = "Stop Music active";
			}
			return status;
		}

		std::string get_info()
		{
			std::lock_guard<std::mutex> lock(data_mutex);
			char buf[1024];
			snprintf(buf, sizeof(buf),
				"my_player_id:%llu;target_player_id:%llu;target_steam_id:%llu;has_session:%d",
				(unsigned long long)my_player_id,
				(unsigned long long)target_player_id,
				(unsigned long long)target_steam_id,
				server_logging::has_session_key() ? 1 : 0);
			return std::string(buf);
		}

		std::string get_commands_list()
		{
			auto& cmds = command::get_commands();
			std::ostringstream oss;
			bool first = true;
			for (const auto& entry : cmds)
			{
				if (!first) oss << "||";
				first = false;
				oss << entry.first;
				if (!entry.second.description.empty() || !entry.second.usage.empty())
				{
					oss << "|" << entry.second.description << "|" << entry.second.usage;
				}
			}
			return oss.str();
		}

		bool execute_open_wormhole()
		{
			if (!server_logging::has_session_key())
			{
				return false;
			}

			std::lock_guard<std::mutex> lock(data_mutex);
			if (target_player_id == 0)
			{
				return false;
			}
			return server_logging::open_wormhole(target_player_id, my_player_id);
		}

		void toggle_auto_wormhole()
		{
			if (!server_logging::has_session_key())
			{
				return;
			}

			if (auto_send_active.load())
			{
				auto_send_active.store(false);
				if (auto_send_thread.joinable())
					auto_send_thread.join();
			}
			else
			{
				std::lock_guard<std::mutex> lock(data_mutex);
				if (target_player_id == 0)
				{
					return;
				}

				auto_send_count.store(0);
				auto_send_active.store(true);

				auto_send_thread = std::thread([]()
				{
					while (auto_send_active.load())
					{
						{
							std::lock_guard<std::mutex> lock(data_mutex);
							server_logging::open_wormhole(target_player_id, my_player_id);
							auto_send_count.fetch_add(1);
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
					}
				});
			}
		}

		void stop_auto_wormhole()
		{
			if (auto_send_active.load())
			{
				auto_send_active.store(false);
				if (auto_send_thread.joinable())
					auto_send_thread.join();
			}
		}

		void toggle_stop_music()
		{
			if (stop_music_active.load())
			{
				stop_music_active.store(false);
				if (stop_music_thread.joinable())
					stop_music_thread.join();
			}
			else
			{
				stop_music_active.store(true);

				stop_music_thread = std::thread([]()
				{
					while (stop_music_active.load())
					{
						try
						{
							command::execute("script_exec_n TppMusicManager.StopMusic()", true);
						}
						catch (...) {}
						std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					}
				});
			}
		}

		void stop_stop_music()
		{
			if (stop_music_active.load())
			{
				stop_music_active.store(false);
				if (stop_music_thread.joinable())
					stop_music_thread.join();
			}
		}

		// FOB occupy (defense against infiltrators):
		// CMD_GET_FOB_TARGET_DETAIL (mode=ACTUAL) on the target base marks it as
		// being infiltrated; as long as the player session is kept alive with
		// CMD_UPDATE_SESSION, no one else can infiltrate it.
		// CMD_ABORT_MOTHER_BASE releases the occupation.
		// Runs on its own thread (1 MB stack) using the standalone TPP client,
		// which never touches the game's NclDaemon structures directly.
		bool occupy_send_session_command(const nlohmann::json& data, nlohmann::json& response)
		{
			try
			{
				auto resp = server_logging::send_session_command(data);
				if (!resp.has_value())
				{
					return false;
				}
				response = *resp;
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		void set_occupy_status(const std::string& msg)
		{
			std::lock_guard<std::mutex> lock(data_mutex);
			occupy_status_msg = msg;
		}

		void stop_occupy();

		std::string occupy_resolve_and_store(uint64_t steam_id)
		{
			if (!server_logging::has_session_key())
			{
				return "NO_SESSION";
			}

			auto cached = fob_target::get_cached_player(steam_id);
			if (!cached.has_value())
			{
				return "NOT_CACHED";
			}

			{
				std::lock_guard<std::mutex> lock(data_mutex);
				occupy_steam_id = steam_id;
				occupy_player_id = cached->player_id;
				occupy_mother_base_id = cached->mother_base_id;
				target_steam_id = steam_id;
				target_player_id = cached->player_id;
				occupy_status_msg = "Target resolved";
			}

			std::ostringstream oss;
			oss << "OK|" << cached->name << "|" << cached->player_id << "|" << cached->mother_base_id;
			return oss.str();
		}

		std::string get_occupy_status_str()
		{
			std::lock_guard<std::mutex> lock(data_mutex);
			char buf[512];
			snprintf(buf, sizeof(buf),
				"active:%d;established:%d;steam_id:%llu;player_id:%llu;mother_base_id:%llu;count:%llu;msg:%s",
				occupy_active.load() ? 1 : 0,
				occupy_established.load() ? 1 : 0,
				(unsigned long long)occupy_steam_id,
				(unsigned long long)occupy_player_id,
				(unsigned long long)occupy_mother_base_id,
				(unsigned long long)occupy_count.load(),
				occupy_status_msg.c_str());
			return std::string(buf);
		}

		// Worker running on its own thread (default 1 MB stack). Sends the
		// occupation heartbeat every 45 s and sends CMD_ABORT_MOTHER_BASE when
		// the occupation is stopped.
		void occupy_worker()
		{
			auto last_cycle = std::chrono::steady_clock::now() - std::chrono::seconds(50);

			while (occupy_active.load())
			{
				const auto now = std::chrono::steady_clock::now();
				if (now - last_cycle >= std::chrono::seconds(45))
				{
					last_cycle = now;

					try
					{
						uint32_t mother_base_id = 0;
						{
							std::lock_guard<std::mutex> lock(data_mutex);
							mother_base_id = occupy_mother_base_id;
						}

						if (mother_base_id == 0)
						{
							set_occupy_status("No target mother_base_id");
							continue;
						}

						// Establish / refresh the occupation mark
						nlohmann::json detail;
						detail["high_rank"] = 1134;
						detail["is_event"] = 0;
						detail["is_plus"] = 1;
						detail["is_sneak"] = 1;
						detail["mode"] = "ACTUAL";
						detail["mother_base_id"] = mother_base_id;
						detail["msgid"] = "CMD_GET_FOB_TARGET_DETAIL";
						detail["rqid"] = 0;

						nlohmann::json detail_resp;
						if (occupy_send_session_command(detail, detail_resp))
						{
							const auto result = detail_resp.value("result", "");
							int is_invalid = 1;
							if (detail_resp.contains("session") && detail_resp["session"].is_object())
							{
								is_invalid = detail_resp["session"].value("is_invalid", 1);
							}

							if (result == "NOERR")
							{
								occupy_established.store(true);
								occupy_count.fetch_add(1);
								if (is_invalid == 0)
								{
									set_occupy_status("Occupying (NOERR, session valid)");
								}
								else
								{
									set_occupy_status("DETAIL NOERR but session invalid");
								}
							}
							else
							{
								occupy_established.store(false);
								set_occupy_status("DETAIL failed: " + result);
							}
						}
						else
						{
							occupy_established.store(false);
							set_occupy_status("DETAIL request failed (no response)");
						}

						// Keep the player session alive
						nlohmann::json session;
						session["msgid"] = "CMD_UPDATE_SESSION";
						session["rqid"] = 0;
						nlohmann::json session_resp;
						if (occupy_send_session_command(session, session_resp))
						{
							const auto result = session_resp.value("result", "");
							if (result != "NOERR")
							{
								set_occupy_status("Heartbeat failed: " + result);
							}
						}
						else
						{
							set_occupy_status("Heartbeat request failed");
						}
					}
					catch (...)
					{
						set_occupy_status("Occupy exception");
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}

			// Occupation stopped: release the base
			if (occupy_established.load())
			{
				nlohmann::json abort;
				abort["msgid"] = "CMD_ABORT_MOTHER_BASE";
				abort["rqid"] = 0;
				nlohmann::json abort_resp;
				if (occupy_send_session_command(abort, abort_resp))
				{
					const auto result = abort_resp.value("result", "");
					if (result == "NOERR")
					{
						set_occupy_status("Released");
					}
					else
					{
						set_occupy_status("Release failed: " + result);
					}
				}
				else
				{
					set_occupy_status("Release request failed");
				}
				occupy_established.store(false);
			}
			else
			{
				set_occupy_status("Stopped");
			}
		}

		std::string toggle_occupy()
		{
			if (!server_logging::has_session_key())
			{
				return "NO_SESSION";
			}

			if (occupy_active.load())
			{
				stop_occupy();
				return "STOPPED";
			}

			{
				std::lock_guard<std::mutex> lock(data_mutex);
				if (occupy_mother_base_id == 0)
				{
					return "NO_TARGET";
				}
			}

			occupy_established.store(false);
			occupy_count.store(0);
			occupy_active.store(true);
			if (occupy_thread.joinable())
			{
				occupy_thread.join();
			}
			occupy_thread = std::thread(occupy_worker);
			return "STARTED";
		}

		void stop_occupy()
		{
			if (!occupy_active.load())
			{
				return;
			}

			occupy_active.store(false);
			if (occupy_thread.joinable())
			{
				occupy_thread.join();
			}
		}

		void update_my_player_id()
		{
			if (server_logging::has_session_key())
			{
				auto cached = server_logging::get_my_player_id_from_cache();
				if (cached != 0)
				{
					std::lock_guard<std::mutex> lock(data_mutex);
					if (my_player_id == 0)
					{
						my_player_id = cached;
					}
				}
			}
		}

		bool resolve_steam_id_to_player_id(uint64_t steam_id)
		{
			auto cached = fob_target::get_cached_player(steam_id);
			if (cached.has_value())
			{
				std::lock_guard<std::mutex> lock(data_mutex);
				target_steam_id = steam_id;
				target_player_id = cached->player_id;
				return true;
			}
			return false;
		}

		std::string execute_tpp_command(const std::string& cmd)
		{
			try
			{
				command::execute(cmd, true);
				return "EXECUTED";
			}
			catch (...)
			{
				return "FAILED";
			}
		}

		std::string get_vars_list()
		{
			auto& var_list = vars::get_var_list();
			std::ostringstream oss;
			bool first = true;
			for (const auto& var : var_list)
			{
				if (!first) oss << "||";
				first = false;
				oss << var->name << "|"
					<< (int)var->type << "|"
					<< var->current.to_string() << "|"
					<< var->description;
			}
			return oss.str();
		}

		std::string get_steam_name(uint64_t steam_id)
		{
			try
			{
				const auto steam_friends = (*game::SteamFriends)();
				if (steam_friends != nullptr)
				{
					game::steam_id sid{};
					sid.bits = steam_id;
					const char* name = steam_friends->__vftable->GetFriendPersonaName(steam_friends, sid);
					if (name && name[0])
					{
						return std::string(name);
					}
				}
			}
			catch (...) {}
			return "";
		}

		std::string process_command(const std::string& cmd)
		{
			if (cmd == "GET_STATUS")
			{
				return get_status();
			}
			if (cmd == "GET_INFO")
			{
				update_my_player_id();
				return get_info();
			}
			if (cmd == "GET_COMMANDS")
			{
				return get_commands_list();
			}
			if (cmd == "GET_VARS")
			{
				return get_vars_list();
			}
			if (cmd == "OPEN_WORMHOLE")
			{
				bool ok = execute_open_wormhole();
				return ok ? "OK" : "FAILED";
			}
			if (cmd == "TOGGLE_AUTO")
			{
				toggle_auto_wormhole();
				return auto_send_active.load() ? "STARTED" : "STOPPED";
			}
			if (cmd == "STOP_AUTO")
			{
				stop_auto_wormhole();
				return "STOPPED";
			}
			if (cmd == "STOP_MUSIC")
			{
				toggle_stop_music();
				return stop_music_active.load() ? "STARTED" : "STOPPED";
			}
			if (cmd == "GET_STOP_MUSIC_STATUS")
			{
				return stop_music_active.load() ? "1" : "0";
			}
			if (cmd == "PING")
			{
				return "PONG";
			}
			if (cmd == "GET_OCCUPY_STATUS")
			{
				return get_occupy_status_str();
			}
			if (cmd.rfind("OCCUPY_SET_STEAMID:", 0) == 0)
			{
				std::string value = cmd.substr(19);
				try
				{
					uint64_t steam_id = std::stoull(value);
					return occupy_resolve_and_store(steam_id);
				}
				catch (...)
				{
					return "INVALID";
				}
			}
			if (cmd == "OCCUPY_TOGGLE")
			{
				return toggle_occupy();
			}
			if (cmd == "OCCUPY_STOP")
			{
				stop_occupy();
				return "STOPPED";
			}
			if (cmd.rfind("SET_TARGET:", 0) == 0)
			{
				std::string value = cmd.substr(11);
				try
				{
					uint64_t id = std::stoull(value);
					std::lock_guard<std::mutex> lock(data_mutex);
					target_player_id = id;
					return "OK";
				}
				catch (...)
				{
					return "INVALID";
				}
			}
			if (cmd.rfind("SET_TARGET_STEAMID:", 0) == 0)
			{
				std::string value = cmd.substr(19);
				try
				{
					uint64_t steam_id = std::stoull(value);
					bool ok = resolve_steam_id_to_player_id(steam_id);
					return ok ? "OK" : "NOT_CACHED";
				}
				catch (...)
				{
					return "INVALID";
				}
			}
			if (cmd.rfind("EXEC:", 0) == 0)
			{
				std::string tpp_cmd = cmd.substr(5);
				return execute_tpp_command(tpp_cmd);
			}
			if (cmd.rfind("GET_LOGS:", 0) == 0)
			{
				std::string value = cmd.substr(9);
				try
				{
					uint64_t since_id = std::stoull(value);
					return get_logs_since(since_id);
				}
				catch (...)
				{
					return "";
				}
			}
			if (cmd == "GET_FOB_LIST_TYPES")
			{
				return "TRIAL|PICKUP|PICKUP_HIGH|ENEMY|EVENT|NUCLEAR|FOLLOW|FOLLOWER|DEPLOYED|INJURY|EMERGENCY|FR_ENEMY";
			}
			if (cmd == "GET_CACHED_PLAYERS")
			{
				const auto players = fob_target::get_cached_players();
				std::ostringstream oss;
				bool first = true;
				for (const auto& player : players)
				{
					if (!first) oss << "||";
					first = false;

					std::string steam_name = get_steam_name(player.steam_id);

					oss << player.name << "|"
						<< steam_name << "|"
						<< player.steam_id << "|"
						<< player.player_id << "|"
						<< player.mother_base_id << "|"
						<< player.espionage_score << "|"
						<< player.espionage_win << "|"
						<< player.espionage_total;
				}
				return oss.str();
			}
			if (cmd == "GET_FOB_TARGETS")
			{
				const auto targets = fob_target::get_targets();
				std::ostringstream oss;
				bool first = true;
				for (const auto& target : targets)
				{
					if (!first) oss << "||";
					first = false;

					std::string steam_name = get_steam_name(target.steam_id);

					oss << target.name << "|"
						<< steam_name << "|"
						<< target.steam_id << "|"
						<< target.player_id << "|"
						<< target.mother_base_id;
				}
				return oss.str();
			}
			// Exchange probe: send a custom CMD_EXCHANGE_LEAGUE_POINT2 request to
			// the official server. Format:
			//   EXCHANGE_PROBE:type|unique_id|num_to_exchange|common_value|is_event
			// Returns the raw response JSON (or FAILED/NO_SESSION).
			if (cmd.rfind("EXCHANGE_PROBE:", 0) == 0)
			{
				if (!server_logging::has_session_key())
				{
					return "NO_SESSION";
				}

				std::istringstream iss(cmd.substr(15));
				std::string part;
				std::vector<std::string> parts;
				while (std::getline(iss, part, '|'))
				{
					parts.push_back(part);
				}

				try
				{
					const auto parse_u32 = [](const std::string& s)
					{
						unsigned long long v = std::stoull(s);
						if (v > 0xFFFFFFFFull)
						{
							throw std::out_of_range("value too large");
						}
						return static_cast<std::uint32_t>(v);
					};

					std::uint32_t type = parts.size() > 0 ? parse_u32(parts[0]) : 0;
					std::uint32_t unique_id = parts.size() > 1 ? parse_u32(parts[1]) : 0;
					std::uint32_t num_to_exchange = parts.size() > 2 ? parse_u32(parts[2]) : 1;
					std::uint32_t common_value = parts.size() > 3 ? parse_u32(parts[3]) : 0;
					std::uint32_t is_event = parts.size() > 4 ? parse_u32(parts[4]) : 1;

					nlohmann::json req;
					req["msgid"] = "CMD_EXCHANGE_LEAGUE_POINT2";
					req["rqid"] = 0;
					req["type"] = type;
					req["unique_id"] = unique_id;
					req["num_to_exchange"] = num_to_exchange;
					req["common_value"] = common_value;
					req["is_event"] = is_event;

					char log_buf[256];
					snprintf(log_buf, sizeof(log_buf),
						"EXCHANGE_PROBE sending: type=%u unique_id=%u num=%u common=%u is_event=%u",
						type, unique_id, num_to_exchange, common_value, is_event);
					add_log_entry(1, log_buf);

					auto resp = server_logging::send_session_command(req);
					if (!resp.has_value())
					{
						add_log_entry(3, "EXCHANGE_PROBE failed (no response)");
						return "FAILED";
					}

					add_log_entry(1, "EXCHANGE_PROBE response: " + resp->dump());
					return resp->dump();
				}
				catch (...)
				{
					return "INVALID";
				}
			}
			return "UNKNOWN";
		}

		void pipe_server_main()
		{
			while (pipe_server_running.load())
			{
				HANDLE hPipe = CreateNamedPipeW(
					PIPE_NAME,
					PIPE_ACCESS_DUPLEX,
					PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
					PIPE_UNLIMITED_INSTANCES,
					PIPE_BUFFER_SIZE,
					PIPE_BUFFER_SIZE,
					0,
					nullptr
				);

				if (hPipe == INVALID_HANDLE_VALUE)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					continue;
				}

				BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

				if (connected && pipe_server_running.load())
				{
					char buffer[PIPE_BUFFER_SIZE];
					DWORD bytesRead, bytesWritten;

					while (pipe_server_running.load())
					{
						BOOL success = ReadFile(hPipe, buffer, PIPE_BUFFER_SIZE - 1, &bytesRead, nullptr);
						if (!success || bytesRead == 0)
						{
							break;
						}

						buffer[bytesRead] = '\0';
						std::string response = process_command(std::string(buffer));

						WriteFile(hPipe, response.c_str(), (DWORD)response.size() + 1, &bytesWritten, nullptr);
					}

					FlushFileBuffers(hPipe);
					DisconnectNamedPipe(hPipe);
				}

				CloseHandle(hPipe);
			}
		}
	}

	void add_log(int type, const std::string& message)
	{
		add_log_entry(type, message);
	}

	class component final : public component_interface
	{
	public:
		void pre_load() override
		{
			add_log_entry(7, "FOB Control initialized");
		}

		void start() override
		{
			pipe_server_running.store(true);
			pipe_server_thread = std::thread(pipe_server_main);
			pipe_server_thread.detach();

			std::thread([]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				launch_gui_exe();
			}).detach();
		}

		void end() override
		{
			pipe_server_running.store(false);

			stop_auto_wormhole();
			stop_stop_music();
			stop_occupy();
		}
	};
}

REGISTER_COMPONENT(fob_control::component)