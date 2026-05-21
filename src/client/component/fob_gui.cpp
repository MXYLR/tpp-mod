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

		std::mutex data_mutex;
		uint64_t target_player_id = 0;
		uint64_t target_steam_id = 0;
		uint64_t my_player_id = 0;

		constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\TPPMod_FOBControl";
		constexpr DWORD PIPE_BUFFER_SIZE = 8192;

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
			std::map<std::string, std::pair<std::string, std::string>> sorted_cmds;
			
			for (const auto& entry : cmds)
			{
				sorted_cmds[entry.first] = { entry.second.description, entry.second.usage };
			}

			std::ostringstream oss;
			bool first = true;
			for (const auto& entry : sorted_cmds)
			{
				if (!first) oss << "||";
				first = false;
				oss << entry.first << "|" << entry.second.first << "|" << entry.second.second;
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
						std::lock_guard<std::mutex> lock(data_mutex);
						server_logging::open_wormhole(target_player_id, my_player_id);
						auto_send_count.fetch_add(1);
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
			if (cmd == "PING")
			{
				return "PONG";
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

					std::string steam_name;
					try
					{
						const auto steam_friends = (*game::SteamFriends)();
						if (steam_friends != nullptr)
						{
							game::steam_id sid{};
							sid.bits = player.steam_id;
							const char* name = steam_friends->__vftable->GetFriendPersonaName(steam_friends, sid);
							if (name && name[0])
							{
								steam_name = name;
							}
						}
					}
					catch (...) {}

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

					std::string steam_name;
					try
					{
						const auto steam_friends = (*game::SteamFriends)();
						if (steam_friends != nullptr)
						{
							game::steam_id sid{};
							sid.bits = target.steam_id;
							const char* name = steam_friends->__vftable->GetFriendPersonaName(steam_friends, sid);
							if (name && name[0])
							{
								steam_name = name;
							}
						}
					}
					catch (...) {}

					oss << target.name << "|"
						<< steam_name << "|"
						<< target.steam_id << "|"
						<< target.player_id << "|"
						<< target.mother_base_id;
				}
				return oss.str();
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

	class component final : public component_interface
	{
	public:
		void pre_load() override
		{
			add_log_entry(7, "FOB Control initialized");
		}

		void start() override
		{
			console::add_print_callback([](int type, const std::string& message)
			{
				add_log_entry(type, message);
			});

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
		}
	};
}

REGISTER_COMPONENT(fob_control::component)
