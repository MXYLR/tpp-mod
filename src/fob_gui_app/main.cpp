#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <set>
#include <unordered_map>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fob_gui
{
	constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\TPPMod_FOBControl";
	constexpr DWORD PIPE_BUFFER_SIZE = 65536;

	HWND g_hwnd = nullptr;
	ID3D11Device* g_pd3dDevice = nullptr;
	ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
	IDXGISwapChain* g_pSwapChain = nullptr;
	ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

	std::atomic<bool> connected{ false };
	std::atomic<bool> should_exit{ false };
	std::atomic<bool> resize_pending{ false };

	float scale_factor = 1.8f;
	char scale_input[8] = "1.8";

	struct WindowConfig
	{
		int x = 100;
		int y = 100;
		int w = 990;
		int h = 765;
		float font_scale = 1.8f;
	};

	WindowConfig g_window_config;
	std::mutex config_mutex;

	std::filesystem::path get_config_path()
	{
		wchar_t exe_path[MAX_PATH];
		GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
		auto p = std::filesystem::path(exe_path);
		return p.replace_extension("cfg");
	}

	void save_config()
	{
		std::lock_guard<std::mutex> lock(config_mutex);
		auto cfg_path = get_config_path();
		std::ofstream ofs(cfg_path);
		if (!ofs) return;
		ofs << g_window_config.x << "\n"
			<< g_window_config.y << "\n"
			<< g_window_config.w << "\n"
			<< g_window_config.h << "\n"
			<< g_window_config.font_scale << "\n";
	}

	void load_config()
	{
		std::lock_guard<std::mutex> lock(config_mutex);
		auto cfg_path = get_config_path();
		std::ifstream ifs(cfg_path);
		if (!ifs) return;
		ifs >> g_window_config.x
			>> g_window_config.y
			>> g_window_config.w
			>> g_window_config.h
			>> g_window_config.font_scale;
		if (g_window_config.font_scale < 0.5f) g_window_config.font_scale = 0.5f;
		if (g_window_config.font_scale > 5.0f) g_window_config.font_scale = 5.0f;
		if (g_window_config.w < 200) g_window_config.w = 200;
		if (g_window_config.h < 200) g_window_config.h = 200;
	}

	float btn_w() { return 120.0f * scale_factor; }
	float btn_h() { return 28.0f * scale_factor; }
	float topbar_h() { return 35.0f * scale_factor; }
	float popup_w() { return 500.0f * scale_factor; }
	float popup_h() { return 390.0f * scale_factor; }
	float list_child_h() { return 320.0f * scale_factor; }

	struct CommandInfo
	{
		std::string name;
		std::string description;
		std::string usage;
	};

	struct LogEntry
	{
		uint64_t id;
		int type;
		std::string message;
	};

	struct CachedPlayerInfo
	{
		std::string name;
		std::string steam_name;
		uint64_t steam_id;
		uint32_t player_id;
		uint32_t mother_base_id;
		int espionage_score;
		int espionage_win;
		int espionage_total;
	};

	struct FobTargetInfo
	{
		std::string name;
		std::string steam_name;
		uint64_t steam_id;
		uint32_t player_id;
		uint32_t mother_base_id;
	};

	std::mutex data_mutex;
	uint64_t my_player_id = 0;
	uint64_t target_player_id = 0;
	uint64_t target_steam_id = 0;
	bool has_session = false;
	char status_text[256] = "Ready";
	char input_text[512] = "";
	std::vector<CommandInfo> commands_list;
	std::set<std::string> filtered_cmds;
	char filter_buf[64] = "";
	const std::set<std::string> redundant_cmds = {
		"cheat_spp_staff",
		"cheat_add_gmp",
		"cheat_add_heroic_point",
		"cheat_set_ogre_point",
		"fob_add_support",
		"fob_open_wormhole",
		"fob_get_target_detail",
		"fob_status",
		"fob_add_target",
		"fob_remove_target",
		"fob_clear_targets",
		"fob_cache_clear",
		"fob_query",
	};

	std::mutex log_mutex;
	std::deque<LogEntry> log_entries;
	uint64_t last_log_id = 0;
	bool auto_scroll_logs = true;

	std::atomic<bool> show_convert_dialog{ false };
	std::vector<std::string> fob_list_types;
	int convert_from_idx = 0;
	int convert_to_idx = 1;

	const std::unordered_map<std::string, std::string> fob_type_descriptions = {
		{"TRIAL", "Training / Visit Destination"},
		{"PICKUP", "Infiltration Targets (PFs of equal grade)"},
		{"PICKUP_HIGH", "Infiltration Targets (High-Ranking PFs)"},
		{"ENEMY", "Retaliation Targets"},
		{"EVENT", "Events"},
		{"NUCLEAR", "Nuclear-equipped Targets"},
		{"FOLLOW", "Supporting"},
		{"FOLLOWER", "Supporters"},
		{"DEPLOYED", "FOB Unit Deployed List"},
		{"INJURY", "Intruder"},
		{"EMERGENCY", "Emergency"},
		{"FR_ENEMY", "Indirect Retaliation Targets"},
	};

	std::atomic<bool> show_cache_dialog{ false };
	std::vector<CachedPlayerInfo> cached_players;

	std::atomic<bool> show_targets_dialog{ false };
	std::vector<FobTargetInfo> fob_targets;

	std::mutex pipe_mutex;
	char scale_factor_buf[16] = "1.8";
	std::atomic<float> font_scale{ 1.8f };

	struct VarEntry
	{
		std::string name;
		int type;
		std::string current_value;
		std::string description;
	};

	std::atomic<bool> show_vars_dialog{ false };
	std::vector<VarEntry> vars_list;
	char var_set_name[128] = "";
	char var_set_value[128] = "";
	std::string vars_status_msg;

	struct SettingOption
	{
		std::string var;
		std::string value;
		bool is_command = false;
	};

	struct SettingsGroup
	{
		std::string name;
		std::vector<SettingOption> options;
	};

	std::vector<SettingsGroup> settings_groups = {
		{"Console", {
			{"console_log", "1"},
			{"con_input_box_color", "0.2 0.2 0.2 0.9"},
			{"con_input_hint_box_color", "0.3 0.3 0.3 1"},
			{"con_output_bar_color", "0.5 0.5 0.5 0.6"},
			{"con_output_slider_color", "0.85 0 0 1"},
			{"con_output_window_color", "0.25 0.25 0.25 0.85"},
			{"con_input_dvar_match_color", "1 1 0.8 1"},
			{"con_input_dvar_value_color", "1 1 0.8 1"},
			{"con_input_dvar_inactive_value_color", "0.8 0.8 0.8 1"},
			{"con_input_cmd_match_color", "0.8 0.8 1 1"},
		}},
		{"Network", {
			{"net_custom_server", ""},
			{"net_udp", "0"},
			{"net_port", "5377"},
		}},
		{"UI", {
			{"ui_draw_fps", "1"},
			{"ui_draw_ping", "1"},
			{"ui_skip_intro", "1"},
		}},
		{"Performance", {
			{"com_worker_count", "4"},
			{"com_unlock_fps", "0"},
			{"com_max_fps", "0"},
		}},
		{"Controls", {
			{"sensitivity", "1"},
			{"camera_fov_scale", "1"},
			{"camera_first_person_fov_scale", "1"},
			{"player_ramble_speed_scale", "1.51"},
			{"player_ramble_speed_patch", "0"},
		}},
		{"Other", {
			{"discord_enable", "1"},
			{"dsx_enable", "1"},
			{"lua_logging", "1"},
			{"lua_dump", "0"},
			{"net_server_logging", "1"},
		}},
	};

	std::atomic<bool> show_settings_dialog{ false };
	std::atomic<bool> show_cheats_dialog{ false };
	std::atomic<bool> show_fob_dialog{ false };
	std::string settings_status_msg;

	HANDLE g_pipe_handle = INVALID_HANDLE_VALUE;

	bool send_command(const std::string& cmd, std::string& response)
	{
		if (g_pipe_handle == INVALID_HANDLE_VALUE)
			return false;

		DWORD bytesWritten, bytesRead;
		BOOL success = WriteFile(g_pipe_handle, cmd.c_str(), (DWORD)cmd.size() + 1, &bytesWritten, nullptr);
		if (!success)
			return false;

		response.clear();
		constexpr DWORD chunk_size = 4096;
		std::vector<char> buffer(chunk_size);
		bool more_data = true;
		while (more_data)
		{
			success = ReadFile(g_pipe_handle, buffer.data(), (DWORD)buffer.size() - 1, &bytesRead, nullptr);
			if (!success)
				return false;
			if (bytesRead == 0)
				break;

			buffer[bytesRead] = '\0';
			response += buffer.data();

			if (bytesRead == buffer.size() - 1)
			{
				more_data = true;
			}
			else
			{
				more_data = false;
			}
		}

		return !response.empty();
	}

	bool connect_to_pipe()
	{
		if (g_pipe_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(g_pipe_handle);
			g_pipe_handle = INVALID_HANDLE_VALUE;
		}

		g_pipe_handle = CreateFileW(
			PIPE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr
		);

		if (g_pipe_handle == INVALID_HANDLE_VALUE)
			return false;

		DWORD mode = PIPE_READMODE_BYTE;
		if (!SetNamedPipeHandleState(g_pipe_handle, &mode, nullptr, nullptr))
		{
			CloseHandle(g_pipe_handle);
			g_pipe_handle = INVALID_HANDLE_VALUE;
			return false;
		}

		connected.store(true);
		return true;
	}

	void disconnect_pipe()
	{
		if (g_pipe_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(g_pipe_handle);
			g_pipe_handle = INVALID_HANDLE_VALUE;
		}
		connected.store(false);
	}

	void parse_info(const std::string& info)
	{
		std::lock_guard<std::mutex> lock(data_mutex);

		size_t pos1 = info.find("my_player_id:");
		size_t pos2 = info.find(";target_player_id:");
		size_t pos3 = info.find(";target_steam_id:");
		size_t pos4 = info.find(";has_session:");

		if (pos1 != std::string::npos && pos2 != std::string::npos)
		{
			std::string my_id_str = info.substr(pos1 + 13, pos2 - pos1 - 13);
			try { my_player_id = std::stoull(my_id_str); } catch (...) {}
		}

		if (pos2 != std::string::npos && pos3 != std::string::npos)
		{
			std::string target_id_str = info.substr(pos2 + 18, pos3 - pos2 - 18);
			try { target_player_id = std::stoull(target_id_str); } catch (...) {}
		}

		if (pos3 != std::string::npos && pos4 != std::string::npos)
		{
			std::string steam_id_str = info.substr(pos3 + 18, pos4 - pos3 - 18);
			try { target_steam_id = std::stoull(steam_id_str); } catch (...) {}
		}

		if (pos4 != std::string::npos)
		{
			std::string session_str = info.substr(pos4 + 13);
			has_session = (session_str == "1");
		}
	}

	void parse_commands(const std::string& data)
	{
		std::vector<CommandInfo> new_list;

		size_t pos = 0;
		while (pos < data.size())
		{
			size_t end_pos = data.find("||", pos);
			std::string cmd_entry;
			if (end_pos == std::string::npos)
			{
				cmd_entry = data.substr(pos);
				pos = data.size();
			}
			else
			{
				cmd_entry = data.substr(pos, end_pos - pos);
				pos = end_pos + 2;
			}

			size_t p1 = cmd_entry.find("|");
			size_t p2 = cmd_entry.find("|", p1 + 1);
			if (p1 != std::string::npos)
			{
				CommandInfo info;
				info.name = cmd_entry.substr(0, p1);
				if (p2 != std::string::npos)
				{
					info.description = cmd_entry.substr(p1 + 1, p2 - p1 - 1);
					info.usage = cmd_entry.substr(p2 + 1);
				}
				else
				{
					info.description = cmd_entry.substr(p1 + 1);
				}
				new_list.push_back(info);
			}
		}

		std::lock_guard<std::mutex> lock(data_mutex);
		commands_list = new_list;
		// Add virtual commands (GUI-only features)
		{
			CommandInfo info;
			info.name = "refresh_info";
			info.description = "Reload player ID and session status from the DLL";
			info.usage = "refresh_info";
			commands_list.push_back(info);
		}
		filtered_cmds.clear();
		for (const auto& c : commands_list)
		{
			if (redundant_cmds.find(c.name) == redundant_cmds.end())
				filtered_cmds.insert(c.name);
		}
	}

	void parse_logs(const std::string& data)
	{
		if (data.empty()) return;

		std::lock_guard<std::mutex> lock(log_mutex);
		size_t pos = 0;
		while (pos < data.size())
		{
			size_t end_pos = data.find("||", pos);
			std::string log_entry_str;
			if (end_pos == std::string::npos)
			{
				log_entry_str = data.substr(pos);
				pos = data.size();
			}
			else
			{
				log_entry_str = data.substr(pos, end_pos - pos);
				pos = end_pos + 2;
			}

			size_t p1 = log_entry_str.find("|");
			size_t p2 = log_entry_str.find("|", p1 + 1);
			if (p1 != std::string::npos && p2 != std::string::npos)
			{
				try
				{
					LogEntry entry;
					entry.id = std::stoull(log_entry_str.substr(0, p1));
					entry.type = std::stoi(log_entry_str.substr(p1 + 1, p2 - p1 - 1));
					entry.message = log_entry_str.substr(p2 + 1);
					log_entries.push_back(entry);
					if (entry.id > last_log_id)
					{
						last_log_id = entry.id;
					}
					while (log_entries.size() > 1000)
					{
						log_entries.pop_front();
					}
				}
				catch (...) {}
			}
		}
	}

	void update_info()
	{
		if (!connected.load())
		{
			if (!connect_to_pipe())
				return;
		}

		std::string response;
		if (!send_command("GET_INFO", response))
		{
			disconnect_pipe();
			return;
		}

		parse_info(response);
	}

	void fetch_commands()
	{
		if (!connected.load())
			return;

		std::string response;
		if (send_command("GET_COMMANDS", response))
		{
			parse_commands(response);
		}
	}

	void fetch_logs()
	{
		if (!connected.load())
			return;

		std::string cmd = "GET_LOGS:" + std::to_string(last_log_id);
		std::string response;
		if (send_command(cmd, response))
		{
			parse_logs(response);
		}
	}

	void fetch_fob_list_types()
	{
		if (!connected.load())
			return;

		std::string response;
		if (send_command("GET_FOB_LIST_TYPES", response))
		{
			std::vector<std::string> new_list;
			size_t pos = 0;
			while (pos < response.size())
			{
				size_t end_pos = response.find("|", pos);
				if (end_pos == std::string::npos)
				{
					new_list.push_back(response.substr(pos));
					break;
				}
				else
				{
					new_list.push_back(response.substr(pos, end_pos - pos));
					pos = end_pos + 1;
				}
			}
			fob_list_types = new_list;
		}
	}

	void fetch_cached_players()
	{
		if (!connected.load())
			return;

		std::string response;
		if (!send_command("GET_CACHED_PLAYERS", response))
		{
			return;
		}

		std::vector<CachedPlayerInfo> new_list;
		size_t pos = 0;
		while (pos < response.size())
		{
			size_t end_pos = response.find("||", pos);
			std::string player_entry;
			if (end_pos == std::string::npos)
			{
				player_entry = response.substr(pos);
				pos = response.size();
			}
			else
			{
				player_entry = response.substr(pos, end_pos - pos);
				pos = end_pos + 2;
			}

			std::vector<std::string> parts;
			size_t p = 0;
			for (int i = 0; i < 8; i++)
			{
				size_t ep = player_entry.find("|", p);
				if (ep == std::string::npos)
				{
					parts.push_back(player_entry.substr(p));
					break;
				}
				else
				{
					parts.push_back(player_entry.substr(p, ep - p));
					p = ep + 1;
				}
			}

			if (parts.size() >= 4)
			{
				try
				{
					CachedPlayerInfo info;
					info.name = parts[0];
					info.steam_name = parts[1];
					info.steam_id = std::stoull(parts[2]);
					info.player_id = (uint32_t)std::stoul(parts[3]);
					if (parts.size() > 4) info.mother_base_id = (uint32_t)std::stoul(parts[4]);
					if (parts.size() > 5) info.espionage_score = std::stoi(parts[5]);
					if (parts.size() > 6) info.espionage_win = std::stoi(parts[6]);
					if (parts.size() > 7) info.espionage_total = std::stoi(parts[7]);
					new_list.push_back(info);
				}
				catch (...) {}
			}
		}
		cached_players = new_list;
	}

	void fetch_fob_targets()
	{
		if (!connected.load())
			return;

		std::string response;
		if (!send_command("GET_FOB_TARGETS", response))
			return;

		std::vector<FobTargetInfo> new_list;

		size_t pos = 0;
		while (pos < response.size())
		{
			size_t end_pos = response.find("||", pos);
			std::string target_entry;
			if (end_pos == std::string::npos)
			{
				target_entry = response.substr(pos);
				pos = response.size();
			}
			else
			{
				target_entry = response.substr(pos, end_pos - pos);
				pos = end_pos + 2;
			}

			if (target_entry.empty())
				continue;

			std::vector<std::string> parts;
			size_t p = 0;
			for (int i = 0; i < 5; i++)
			{
				size_t ep = target_entry.find("|", p);
				if (ep == std::string::npos)
				{
					parts.push_back(target_entry.substr(p));
					break;
				}
				else
				{
					parts.push_back(target_entry.substr(p, ep - p));
					p = ep + 1;
				}
			}

			if (parts.size() >= 3)
			{
				try
				{
					FobTargetInfo info;
					info.name = parts[0];
					info.steam_name = parts[1];
					info.steam_id = std::stoull(parts[2]);
					info.player_id = (uint32_t)std::stoul(parts[3]);
					if (parts.size() > 4) info.mother_base_id = (uint32_t)std::stoul(parts[4]);
					new_list.push_back(info);
				}
				catch (...) {}
			}
		}
		fob_targets = new_list;
	}

	const std::set<std::string> redundant_vars = {
		"cheat_enabled",
		"cheat_unlockall_server_items",
		"cheat_disable_reporting",
		"cheat_unlockall_gear",
		"fob_target_list_num",
	};

	void fetch_vars()
	{
		if (!connected.load())
			return;

		std::string response;
		if (!send_command("GET_VARS", response))
			return;

		std::vector<VarEntry> new_list;

		size_t pos = 0;
		while (pos < response.size())
		{
			size_t end_pos = response.find("||", pos);
			std::string var_entry;
			if (end_pos == std::string::npos)
			{
				var_entry = response.substr(pos);
				pos = response.size();
			}
			else
			{
				var_entry = response.substr(pos, end_pos - pos);
				pos = end_pos + 2;
			}

			if (var_entry.empty())
				continue;

			size_t p1 = var_entry.find("|");
			size_t p2 = var_entry.find("|", p1 + 1);
			size_t p3 = var_entry.find("|", p2 + 1);
			if (p1 != std::string::npos && p2 != std::string::npos)
			{
				try
				{
					VarEntry info;
					info.name = var_entry.substr(0, p1);

					if (redundant_vars.find(info.name) != redundant_vars.end())
						continue;

					info.type = std::stoi(var_entry.substr(p1 + 1, p2 - p1 - 1));
					if (p3 != std::string::npos)
					{
						info.current_value = var_entry.substr(p2 + 1, p3 - p2 - 1);
						info.description = var_entry.substr(p3 + 1);
					}
					else
					{
						info.current_value = var_entry.substr(p2 + 1);
					}
					new_list.push_back(info);
				}
				catch (...) {}
			}
		}
		vars_list = new_list;
	}

	void heartbeat_thread_main()
	{
		while (!should_exit.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2000));

			if (connected.load())
			{
				std::string response;
				bool ok = send_command("PING", response);
				if (!ok || response != "PONG")
				{
					disconnect_pipe();
					PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
					return;
				}
				fetch_logs();
			}
		}
	}

	LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
			return true;

		switch (msg)
		{
		case WM_SIZE:
			if (g_pSwapChain && wParam != SIZE_MINIMIZED)
			{
				resize_pending.store(true);
				RECT rc;
				GetClientRect(hWnd, &rc);
				g_window_config.w = rc.right - rc.left;
				g_window_config.h = rc.bottom - rc.top;
			}
			return 0;
		case WM_MOVE:
		{
			RECT rc;
			GetWindowRect(hWnd, &rc);
			g_window_config.x = rc.left;
			g_window_config.y = rc.top;
		}
		return 0;
		case WM_DESTROY:
			save_config();
			should_exit.store(true);
			PostQuitMessage(0);
			return 0;
		case WM_CLOSE:
			save_config();
			DestroyWindow(hWnd);
			return 0;
		default:
			return DefWindowProc(hWnd, msg, wParam, lParam);
		}
	}

	void cleanup_d3d();

	bool create_render_target()
	{
		ID3D11Texture2D* pBackBuffer = nullptr;
		if (FAILED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))))
			return false;
		if (FAILED(g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView)))
		{
			pBackBuffer->Release();
			return false;
		}
		pBackBuffer->Release();
		return true;
	}

	void cleanup_render_target()
	{
		if (g_mainRenderTargetView)
		{
			g_mainRenderTargetView->Release();
			g_mainRenderTargetView = nullptr;
		}
	}

	void create_d3d_device()
	{
		DXGI_SWAP_CHAIN_DESC sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 2;
		sd.BufferDesc.Width = 0;
		sd.BufferDesc.Height = 0;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = g_hwnd;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		UINT createDeviceFlags = 0;
		D3D_FEATURE_LEVEL featureLevel;
		const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };

		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
			featureLevelArray, _countof(featureLevelArray), D3D11_SDK_VERSION, &sd, &g_pSwapChain,
			&g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

		if (FAILED(hr))
		{
			MessageBoxA(nullptr, "Failed to create D3D11 device! Your GPU may not support DirectX 11.", "Error", MB_OK | MB_ICONERROR);
			PostQuitMessage(1);
			return;
		}

		if (!create_render_target())
		{
			MessageBoxA(nullptr, "Failed to create render target!", "Error", MB_OK | MB_ICONERROR);
			cleanup_d3d();
			PostQuitMessage(1);
			return;
		}
	}

	void cleanup_d3d()
	{
		cleanup_render_target();
		if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
		if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
		if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
	}

	void set_status(const char* text)
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		strncpy_s(status_text, sizeof(status_text), text, _TRUNCATE);
	}

	void update_filter()
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		filtered_cmds.clear();
		std::string filter = filter_buf;
		for (char& c : filter) c = (char)std::tolower((unsigned char)c);

		for (const auto& c : commands_list)
		{
			if (redundant_cmds.find(c.name) != redundant_cmds.end())
				continue;
			std::string name = c.name;
			for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
			if (filter.empty() || name.find(filter) != std::string::npos)
			{
				filtered_cmds.insert(c.name);
			}
		}
	}

	void execute_tpp_command(const std::string& cmd)
	{
		if (!connected.load())
		{
			set_status("Not connected");
			return;
		}

		if (cmd == "fob_convert_list_type")
		{
			fetch_fob_list_types();
			if (fob_list_types.size() >= 2)
			{
				convert_from_idx = 0;
				convert_to_idx = 1;
				show_convert_dialog.store(true);
			}
			else
			{
				set_status("Failed to get list types");
			}
			return;
		}

		if (cmd == "fob_cache_list")
		{
			fetch_cached_players();
			show_cache_dialog.store(true);
			return;
		}

		if (cmd == "fob_target_list")
		{
			fetch_fob_targets();
			show_targets_dialog.store(true);
			return;
		}

		if (cmd == "refresh_info")
		{
			update_info();
			set_status("Info refreshed");
			return;
		}

		if (cmd == "var_list")
		{
			fetch_vars();
			show_vars_dialog.store(true);
			return;
		}

		std::string full_cmd = "EXEC:" + cmd;
		std::string response;
		if (send_command(full_cmd, response))
		{
			if (response == "EXECUTED")
			{
				char buf[256];
				snprintf(buf, sizeof(buf), "Executed: %s", cmd.c_str());
				set_status(buf);
			}
			else
			{
				set_status("Execution failed");
			}
		}
		else
		{
			set_status("Send failed");
		}
	}

	void render_convert_dialog()
	{
		if (!show_convert_dialog.load())
			return;

		ImGui::OpenPopup("Convert List Type");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h() * 0.45f), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("Convert List Type", nullptr, ImGuiWindowFlags_None))
		{
			ImGui::Text("Select source and target list types:");
			ImGui::Separator();

			ImGui::Text("From:");
			ImGui::SameLine();
			if (ImGui::BeginCombo("##from_combo", fob_list_types[convert_from_idx].c_str()))
			{
				for (size_t i = 0; i < fob_list_types.size(); i++)
				{
					const auto& name = fob_list_types[i];
					auto it = fob_type_descriptions.find(name);
					if (ImGui::Selectable(name.c_str(), convert_from_idx == (int)i))
					{
						convert_from_idx = (int)i;
					}
					if (it != fob_type_descriptions.end())
					{
						ImGui::SameLine();
						ImGui::TextDisabled(" - %s", it->second.c_str());
					}
				}
				ImGui::EndCombo();
			}

			ImGui::Text("To:  ");
			ImGui::SameLine();
			if (ImGui::BeginCombo("##to_combo", fob_list_types[convert_to_idx].c_str()))
			{
				for (size_t i = 0; i < fob_list_types.size(); i++)
				{
					const auto& name = fob_list_types[i];
					auto it = fob_type_descriptions.find(name);
					if (ImGui::Selectable(name.c_str(), convert_to_idx == (int)i))
					{
						convert_to_idx = (int)i;
					}
					if (it != fob_type_descriptions.end())
					{
						ImGui::SameLine();
						ImGui::TextDisabled(" - %s", it->second.c_str());
					}
				}
				ImGui::EndCombo();
			}

			ImGui::Separator();

			if (ImGui::Button("Convert", ImVec2(btn_w(), btn_h())))
			{
				if (convert_from_idx != convert_to_idx)
				{
					std::string tpp_cmd = "fob_convert_list_type " + fob_list_types[convert_to_idx] + " " + fob_list_types[convert_from_idx];
					execute_tpp_command(tpp_cmd);
					show_convert_dialog.store(false);
				}
				else
				{
					set_status("Source and target must be different");
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Execute the list type conversion with the selected source and target");
			ImGui::SameLine();
			if (ImGui::Button("Disable", ImVec2(btn_w(), btn_h())))
			{
				execute_tpp_command("fob_convert_list_type 0");
				show_convert_dialog.store(false);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Disable list type conversion");
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(btn_w(), btn_h())))
			{
				show_convert_dialog.store(false);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Close this dialog without making changes");

			ImGui::EndPopup();
		}
	}

	void render_cache_dialog()
	{
		if (!show_cache_dialog.load())
			return;

		ImGui::OpenPopup("Cached Players");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h()), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("Cached Players", nullptr, ImGuiWindowFlags_None))
		{
			if (cached_players.empty())
			{
				ImGui::Text("No cached players. Browse FOB lists to auto-cache players.");
			}
			else
			{
				ImGui::Text("Cached players (%zu):", cached_players.size());
				ImGui::Separator();

				ImGui::BeginChild("##cache_list", ImVec2(0, list_child_h()), true);
				for (size_t i = 0; i < cached_players.size(); i++)
				{
					const auto& player = cached_players[i];

					ImGui::PushID((int)i);

					ImGui::Text("[%zu] %s", i, player.name.c_str());
					ImGui::SameLine();
					if (ImGui::Button("Copy name"))
					{
						ImGui::SetClipboardText(player.name.c_str());
					}

					ImGui::Text("Steam Name: %s", player.steam_name.empty() ? "(unknown)" : player.steam_name.c_str());
					ImGui::SameLine();
					if (!player.steam_name.empty())
					{
						if (ImGui::Button("Copy steam name"))
						{
							ImGui::SetClipboardText(player.steam_name.c_str());
						}
					}

					ImGui::Indent();

					char steam_buf[32];
					snprintf(steam_buf, sizeof(steam_buf), "%llu", (unsigned long long)player.steam_id);
					ImGui::Text("Steam ID: %s", steam_buf);
					ImGui::SameLine();
					if (ImGui::Button("Copy steamid"))
					{
						ImGui::SetClipboardText(steam_buf);
					}

					char player_buf[16];
					snprintf(player_buf, sizeof(player_buf), "%u", player.player_id);
					ImGui::Text("Player ID: %s", player_buf);
					ImGui::SameLine();
					if (ImGui::Button("Copy playerid"))
					{
						ImGui::SetClipboardText(player_buf);
					}

					ImGui::Text("Mother Base ID: %u", player.mother_base_id);
					ImGui::Text("Espionage: %d points, %d wins, %d total",
						player.espionage_score, player.espionage_win, player.espionage_total);

					ImGui::Unindent();
					ImGui::Separator();

					ImGui::PopID();
				}
				ImGui::EndChild();
			}

			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(btn_w(), btn_h())))
			{
				show_cache_dialog.store(false);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Close the cached players list");

			ImGui::EndPopup();
		}
	}

	void render_targets_dialog()
	{
		if (!show_targets_dialog.load())
			return;

		ImGui::OpenPopup("FOB Target List");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h()), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("FOB Target List", nullptr, ImGuiWindowFlags_None))
		{
			if (fob_targets.empty())
			{
				ImGui::Text("No custom FOB targets. Use fob_add_target to add targets.");
			}
			else
			{
				ImGui::Text("Custom FOB targets (%zu):", fob_targets.size());
				ImGui::Separator();

				ImGui::BeginChild("##target_list", ImVec2(0, list_child_h()), true);
				for (size_t i = 0; i < fob_targets.size(); i++)
				{
					const auto& target = fob_targets[i];

					ImGui::PushID((int)i);

					ImGui::Text("[%zu] %s", i, target.name.c_str());
					ImGui::SameLine();
					if (ImGui::Button("Copy name"))
					{
						ImGui::SetClipboardText(target.name.c_str());
					}

					ImGui::Text("Steam Name: %s", target.steam_name.empty() ? "(unknown)" : target.steam_name.c_str());
					ImGui::SameLine();
					if (!target.steam_name.empty())
					{
						if (ImGui::Button("Copy steam name"))
						{
							ImGui::SetClipboardText(target.steam_name.c_str());
						}
					}

					ImGui::Indent();

					char steam_buf[32];
					snprintf(steam_buf, sizeof(steam_buf), "%llu", (unsigned long long)target.steam_id);
					ImGui::Text("Steam ID: %s", steam_buf);
					ImGui::SameLine();
					if (ImGui::Button("Copy steamid"))
					{
						ImGui::SetClipboardText(steam_buf);
					}

					char player_buf[16];
					snprintf(player_buf, sizeof(player_buf), "%u", target.player_id);
					ImGui::Text("Player ID: %s", player_buf);
					ImGui::SameLine();
					if (ImGui::Button("Copy playerid"))
					{
						ImGui::SetClipboardText(player_buf);
					}

					ImGui::Text("Mother Base ID: %u", target.mother_base_id);

					ImGui::Unindent();
					ImGui::Separator();

					ImGui::PopID();
				}
				ImGui::EndChild();
			}

			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(215, 50)))
			{
				show_targets_dialog.store(false);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Close the FOB target list");

			ImGui::EndPopup();
		}
	}

	void render_vars_dialog()
	{
		if (!show_vars_dialog.load())
			return;

		ImGui::OpenPopup("Variables");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w() + 100, popup_h() + 60), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("Variables", nullptr, ImGuiWindowFlags_None))
		{
			if (ImGui::Button("Refresh", ImVec2(btn_w(), btn_h())))
			{
				fetch_vars();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Reload all variable values from the DLL");

			ImGui::SameLine();
			ImGui::Text("(%zu vars)", vars_list.size());

			ImGui::Separator();

			float child_h = ImGui::GetContentRegionAvail().y - 90 * scale_factor;
			if (ImGui::BeginChild("##vars_list", ImVec2(0, child_h), true))
			{
				// Categorize by name prefix
				struct VarCategory
				{
					std::string name;
					std::string display;
					std::vector<size_t> indices;
				};

				VarCategory categories[] = {
					{"net_", "Network"},
					{"match_", "Match"},
					{"com_", "Performance"},
					{"sensitivity", "Performance"},
					{"camera_", "Performance"},
					{"player_", "Performance"},
					{"ui_skip", "Performance"},
					{"ui_draw_", "UI"},
					{"con_", "Console"},
					{"console_log", "Console"},
					{"chat_", "Chat"},
					{"discord_", "Other"},
					{"dsx_", "Other"},
					{"name", "Other"},
					{"staff_", "Other"},
					{"lua_", "Other"},
				};

				// Group indices by category display name
				std::unordered_map<std::string, std::vector<size_t>> cat_map;
				std::vector<std::string> cat_order;

				for (size_t i = 0; i < vars_list.size(); i++)
				{
					std::string cat = "Other";
					for (const auto& c : categories)
					{
						if (vars_list[i].name.rfind(c.name, 0) == 0)
						{
							cat = c.display;
							break;
						}
					}
					if (cat_map.find(cat) == cat_map.end())
					{
						cat_order.push_back(cat);
					}
					cat_map[cat].push_back(i);
				}

				for (const auto& cat_name : cat_order)
				{
					if (ImGui::CollapsingHeader(cat_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
					{
						for (auto idx : cat_map[cat_name])
						{
							const auto& var = vars_list[idx];
							ImGui::PushID((int)idx);

							ImGui::Text("%s", var.name.c_str());
							ImGui::SameLine();
							ImGui::TextDisabled("= %s", var.current_value.c_str());

							if (!var.description.empty())
							{
								ImGui::TextDisabled("  %s", var.description.c_str());
							}

							ImGui::SameLine();
							char set_btn[64];
							snprintf(set_btn, sizeof(set_btn), "Set##v%zu", idx);
							if (ImGui::SmallButton(set_btn))
							{
								strncpy_s(var_set_name, sizeof(var_set_name), var.name.c_str(), _TRUNCATE);
							}

							ImGui::PopID();
						}
					}
				}
			}
			ImGui::EndChild();

			ImGui::Separator();
			ImGui::Text("Set variable:");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(150 * scale_factor);
			ImGui::InputText("##var_name", var_set_name, sizeof(var_set_name));
			ImGui::SameLine();
			ImGui::Text("=");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(150 * scale_factor);
			ImGui::InputText("##var_value", var_set_value, sizeof(var_set_value));
			ImGui::SameLine();
			if (ImGui::Button("Apply", ImVec2(btn_w(), btn_h())))
			{
				if (var_set_name[0] != '\0' && var_set_value[0] != '\0')
				{
					std::string cmd = "set " + std::string(var_set_name) + " " + std::string(var_set_value);
					execute_tpp_command(cmd);
					vars_status_msg = "Sent: " + cmd;
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Execute 'set <name> <value>' on the tpp-mod DLL");

			if (!vars_status_msg.empty())
			{
				ImGui::TextDisabled("%s", vars_status_msg.c_str());
			}

			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(btn_w(), btn_h())))
			{
				show_vars_dialog.store(false);
				ImGui::CloseCurrentPopup();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Close the variables dialog");

			ImGui::EndPopup();
		}
	}

	void render_settings_dialog()
	{
		if (!show_settings_dialog.load())
			return;

		ImGui::OpenPopup("Quick Settings");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h() + 60), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("Quick Settings", nullptr, ImGuiWindowFlags_None))
		{
			if (ImGui::Button("Apply ALL settings", ImVec2(btn_w() * 1.5f, btn_h())))
			{
				int count = 0;
				for (const auto& group : settings_groups)
				{
					for (const auto& opt : group.options)
					{
						std::string cmd = "set " + opt.var + " \"" + opt.value + "\"";
						execute_tpp_command(cmd);
						count++;
					}
				}
				char buf[64];
				snprintf(buf, sizeof(buf), "Applied all %d settings", count);
				settings_status_msg = buf;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Apply every setting from all groups at once");

			ImGui::Separator();

			float child_h = ImGui::GetContentRegionAvail().y - 60 * scale_factor;
			if (ImGui::BeginChild("##settings_list", ImVec2(0, child_h), true))
			{
				for (const auto& group : settings_groups)
				{
					if (ImGui::CollapsingHeader(group.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
					{
						ImGui::SameLine();
						char apply_group_btn[64];
						snprintf(apply_group_btn, sizeof(apply_group_btn), "Apply %s##g", group.name.c_str());
						if (ImGui::SmallButton(apply_group_btn))
						{
							int count = 0;
							for (const auto& opt : group.options)
							{
								std::string cmd = "set " + opt.var + " \"" + opt.value + "\"";
								execute_tpp_command(cmd);
								count++;
							}
							char buf[64];
							snprintf(buf, sizeof(buf), "Applied %s (%d settings)", group.name.c_str(), count);
							settings_status_msg = buf;
						}

						for (const auto& opt : group.options)
						{
							ImGui::PushID((opt.var + opt.value).c_str());
							ImGui::Text("set %s", opt.var.c_str());
							ImGui::SameLine();
							ImGui::TextDisabled(" \"%s\"", opt.value.c_str());
							ImGui::SameLine();
							char apply_btn[64];
							snprintf(apply_btn, sizeof(apply_btn), "Apply##s");
							if (ImGui::SmallButton(apply_btn))
							{
								std::string cmd = "set " + opt.var + " \"" + opt.value + "\"";
								execute_tpp_command(cmd);
								settings_status_msg = "Sent: " + cmd;
							}
							ImGui::PopID();
						}
					}
				}
			}
			ImGui::EndChild();

			if (!settings_status_msg.empty())
			{
				ImGui::TextDisabled("%s", settings_status_msg.c_str());
			}

			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(btn_w(), btn_h())))
			{
				show_settings_dialog.store(false);
				settings_status_msg.clear();
				ImGui::CloseCurrentPopup();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Close the quick settings dialog");

			ImGui::EndPopup();
		}
	}

	void render_cheats_dialog()
	{
		if (!show_cheats_dialog.load())
			return;

		static const SettingOption cheat_options[] = {
			{"cheat_unlockall_server_items", "1"},
			{"cheat_disable_reporting", "1"},
			{"cheat_spp_staff", "", true},
			{"cheat_add_gmp", "", true},
			{"cheat_add_heroic_point", "", true},
			{"cheat_set_ogre_point", "", true},
		};

		static const bool cheat_needs_param[] = {
			true, true,
			false,
			true, true, true,
		};

		static const char* cheat_param_hints[] = {
			"0 or 1",
			"0 or 1",
			"",
			"amount (500000)",
			"amount (50000)",
			"amount (50000)",
		};

		static char cheat_param_buf[6][128] = {};

		ImGui::OpenPopup("Cheats");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h() + 60), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("Cheats", nullptr, ImGuiWindowFlags_None))
		{
			if (ImGui::Button("Apply ALL##cheats", ImVec2(btn_w() * 1.5f, btn_h())))
			{
				int count = 0;
				for (int i = 0; i < 6; i++)
				{
					const auto& opt = cheat_options[i];
					std::string cmd;
					if (opt.is_command)
					{
						cmd = opt.var;
						std::string param = cheat_param_buf[i];
						if (!param.empty())
							cmd += " " + param;
					}
					else
					{
						std::string param = cheat_param_buf[i];
						std::string value = param.empty() ? opt.value : param;
						cmd = "set " + opt.var + " \"" + value + "\"";
					}
					execute_tpp_command(cmd);
					count++;
				}
				char buf[64];
				snprintf(buf, sizeof(buf), "Applied all %d cheats", count);
				settings_status_msg = buf;
			}

			ImGui::Separator();

			float child_h = ImGui::GetContentRegionAvail().y - 60 * scale_factor;
			if (ImGui::BeginChild("##cheats_list", ImVec2(0, child_h), true))
			{
				for (int i = 0; i < 6; i++)
				{
					const auto& opt = cheat_options[i];
					ImGui::PushID(i);

					if (opt.is_command)
					{
						ImGui::Text("%s", opt.var.c_str());
						if (cheat_needs_param[i])
						{
							ImGui::SameLine();
							ImGui::SetNextItemWidth(160 * scale_factor);
							ImGui::InputTextWithHint("##param", cheat_param_hints[i],
								cheat_param_buf[i], sizeof(cheat_param_buf[i]));
						}
					}
					else
					{
						ImGui::Text("set %s", opt.var.c_str());
						if (cheat_needs_param[i])
						{
							ImGui::SameLine();
							ImGui::SetNextItemWidth(80 * scale_factor);
							ImGui::InputTextWithHint("##param", cheat_param_hints[i],
								cheat_param_buf[i], sizeof(cheat_param_buf[i]));
						}
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("Apply"))
					{
						std::string cmd;
						if (opt.is_command)
						{
							cmd = opt.var;
							std::string param = cheat_param_buf[i];
							if (!param.empty())
								cmd += " " + param;
						}
						else
						{
							std::string param = cheat_param_buf[i];
							std::string value = param.empty() ? opt.value : param;
							cmd = "set " + opt.var + " \"" + value + "\"";
						}
						execute_tpp_command(cmd);
						settings_status_msg = "Sent: " + cmd;
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();

			if (!settings_status_msg.empty())
				ImGui::TextDisabled("%s", settings_status_msg.c_str());

			ImGui::Separator();
			if (ImGui::Button("Close##cheats", ImVec2(btn_w(), btn_h())))
			{
				show_cheats_dialog.store(false);
				for (int i = 0; i < 6; i++) cheat_param_buf[i][0] = '\0';
				settings_status_msg.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	void execute_fob_command_internal(const std::string& cmd)
	{
		if (!connected.load())
		{
			set_status("Not connected");
			return;
		}

		std::string full_cmd = "EXEC:" + cmd;
		std::string response;
		if (send_command(full_cmd, response))
		{
			if (response == "EXECUTED")
			{
				char buf[256];
				snprintf(buf, sizeof(buf), "Executed: %s", cmd.c_str());
				set_status(buf);
			}
			else
			{
				set_status("Execution failed");
			}
		}
		else
		{
			set_status("Send failed");
		}
	}

	void render_fob_dialog()
	{
		if (!show_fob_dialog.load())
			return;

		static const SettingOption fob_options[] = {
			{"fob_target_list_num", "0"},
			{"fob_add_support", "", true},
			{"fob_open_wormhole", "", true},
			{"fob_get_target_detail", "", true},
			{"fob_status", "", true},
			{"fob_add_target", "", true},
			{"fob_remove_target", "", true},
			{"fob_clear_targets", "", true},
			{"fob_cache_clear", "", true},
			{"fob_query", "", true},
		};

		static const bool fob_needs_param[] = {
			true,
			true,
			true,
			true,
			false,
			true,
			true,
			false,
			false,
			true,
		};

		static const char* fob_param_hints[] = {
			"value (0-1000)",
			"steam_id (e.g. 76561198000000000)",
			"target_steam_id",
			"target_steam_id",
			"",
			"steam_id [player_id] [mother_base_id] [name]",
			"steam_id",
			"",
			"",
			"steam_id",
		};

		static char fob_param_buf[10][128] = {};

		ImGui::OpenPopup("FOB Commands");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h() + 60), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("FOB Commands", nullptr, ImGuiWindowFlags_None))
		{
			if (ImGui::Button("Run ALL##fob", ImVec2(btn_w() * 1.5f, btn_h())))
			{
				int count = 0;
				for (int i = 0; i < 10; i++)
				{
					const auto& opt = fob_options[i];
					std::string cmd;
					if (opt.is_command)
					{
						cmd = opt.var;
						std::string param = fob_param_buf[i];
						if (!param.empty())
							cmd += " " + param;
					}
					else
					{
						std::string param = fob_param_buf[i];
						std::string value = param.empty() ? opt.value : param;
						cmd = "set " + opt.var + " \"" + value + "\"";
					}
					execute_fob_command_internal(cmd);
					count++;
				}
				char buf[64];
				snprintf(buf, sizeof(buf), "Applied all %d fob settings", count);
				settings_status_msg = buf;
			}

			ImGui::Separator();

			float child_h = ImGui::GetContentRegionAvail().y - 60 * scale_factor;
			if (ImGui::BeginChild("##fob_list", ImVec2(0, child_h), true))
			{
				for (int i = 0; i < 10; i++)
				{
					const auto& opt = fob_options[i];
					ImGui::PushID(i);

					if (opt.is_command)
					{
						ImGui::Text("%s", opt.var.c_str());
						if (fob_needs_param[i])
						{
							ImGui::SameLine();
							ImGui::SetNextItemWidth(180 * scale_factor);
							ImGui::InputTextWithHint("##param", fob_param_hints[i],
								fob_param_buf[i], sizeof(fob_param_buf[i]));
						}
					}
					else
					{
						ImGui::Text("set %s", opt.var.c_str());
						if (fob_needs_param[i])
						{
							ImGui::SameLine();
							ImGui::SetNextItemWidth(120 * scale_factor);
							ImGui::InputTextWithHint("##param", fob_param_hints[i],
								fob_param_buf[i], sizeof(fob_param_buf[i]));
						}
					}

					ImGui::SameLine();
					if (ImGui::SmallButton("Apply"))
					{
						std::string cmd;
						if (opt.is_command)
						{
							cmd = opt.var;
							std::string param = fob_param_buf[i];
							if (!param.empty())
								cmd += " " + param;
						}
						else
						{
							std::string param = fob_param_buf[i];
							std::string value = param.empty() ? opt.value : param;
							cmd = "set " + opt.var + " \"" + value + "\"";
						}
						execute_fob_command_internal(cmd);
						settings_status_msg = "Sent: " + cmd;
					}

					ImGui::PopID();
				}
			}
			ImGui::EndChild();

			if (!settings_status_msg.empty())
				ImGui::TextDisabled("%s", settings_status_msg.c_str());

			ImGui::Separator();
			if (ImGui::Button("Close##fob", ImVec2(btn_w(), btn_h())))
			{
				show_fob_dialog.store(false);
				for (int i = 0; i < 10; i++) fob_param_buf[i][0] = '\0';
				settings_status_msg.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	void render_gui()
	{
		static bool show_test_window = false;
		if (ImGui::IsKeyPressed(ImGuiKey_F10))
			show_test_window = !show_test_window;

		if (show_test_window)
		{
			ImGui::Begin("Debug Info", &show_test_window);
			ImGui::Text("DisplaySize: %.0f x %.0f", ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
			ImGui::Text("Framerate: %.1f FPS", ImGui::GetIO().Framerate);
			ImGui::Text("FontGlobalScale: %.2f", ImGui::GetIO().FontGlobalScale);
			ImGui::Text("Connected: %s", connected.load() ? "Yes" : "No");
			ImGui::Text("ForgroundDrawList: %s", ImGui::GetForegroundDrawList() ? "OK" : "NULL");
			ImGui::End();
		}

		render_convert_dialog();
		render_cache_dialog();
		render_targets_dialog();
		render_vars_dialog();
		render_settings_dialog();
		render_cheats_dialog();
		render_fob_dialog();

		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
		ImGui::Begin("TPP-Mod Control", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

		ImGui::BeginChild("##TopBar", ImVec2(0, topbar_h()), false);
		{
			ImGui::Text("TPP-Mod Control");
			ImGui::SameLine();
			if (ImGui::Button(connected.load() ? "Reconnect" : "Connect"))
			{
				if (connected.load())
					disconnect_pipe();
				if (connect_to_pipe())
				{
					update_info();
					fetch_commands();
					fetch_logs();
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Connect to / reconnect the named pipe to the tpp-mod DLL");

			ImGui::SameLine();
			ImGui::Text(connected.load() ? "Connected" : "Not connected");

			ImGui::SameLine();
			ImGui::SetNextItemWidth(60.0f);
			ImGui::InputText("##scale", scale_input, sizeof(scale_input));
			ImGui::SameLine();
			if (ImGui::Button("Set"))
			{
				float new_scale = (float)atof(scale_input);
				if (new_scale < 0.5f) new_scale = 0.5f;
				if (new_scale > 5.0f) new_scale = 5.0f;
				scale_factor = new_scale;
				g_window_config.font_scale = new_scale;
				snprintf(scale_input, sizeof(scale_input), "%.1f", scale_factor);
				ImGui::GetIO().FontGlobalScale = scale_factor;
				ImGui::GetIO().Fonts->Build();
				ImGui_ImplDX11_InvalidateDeviceObjects();
				ImGui_ImplDX11_CreateDeviceObjects();
				save_config();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Apply UI scale factor (0.5 - 5.0)");
		}
		ImGui::EndChild();

		ImGui::Separator();

		float left_width = ImGui::GetContentRegionAvail().x * 0.35f;
		float right_width = ImGui::GetContentRegionAvail().x * 0.62f;
		ImGui::BeginChild("##LeftPanel", ImVec2(left_width, 0), true);
		{
			ImGui::Text("=== Status ===");

			char my_buf[32] = "";
			char target_steam_buf[32] = "";
			{
				std::lock_guard<std::mutex> lock(data_mutex);
				snprintf(my_buf, sizeof(my_buf), "%llu", (unsigned long long)my_player_id);
				snprintf(target_steam_buf, sizeof(target_steam_buf), "%llu", (unsigned long long)target_steam_id);
			}

			ImGui::Text("My player_id:");
			ImGui::SameLine();
			ImGui::InputText("##my", my_buf, sizeof(my_buf), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_ReadOnly);

			ImGui::Text("Session:");
			ImGui::SameLine();
			{
				std::lock_guard<std::mutex> lock(data_mutex);
				ImGui::Text(has_session ? "Active" : "Inactive");
			}

			ImGui::Separator();
			ImGui::Text("=== FOB Wormhole ===");

			ImGui::Text("Target SteamID:");
			if (ImGui::InputText("##target_steam", target_steam_buf, sizeof(target_steam_buf), ImGuiInputTextFlags_CharsDecimal))
			{
				if (connected.load())
				{
					std::string cmd = "SET_TARGET_STEAMID:" + std::string(target_steam_buf);
					std::string response;
					if (send_command(cmd, response))
					{
						if (response == "OK")
						{
							set_status("SteamID resolved");
						}
						else if (response == "NOT_CACHED")
						{
							set_status("SteamID not cached");
						}
						else
						{
							set_status("Invalid SteamID");
						}
					}
				}
			}

			if (ImGui::Button("Open wormhole"))
			{
				if (connected.load())
				{
					std::string response;
					if (send_command("OPEN_WORMHOLE", response))
					{
						set_status(response == "OK" ? "Wormhole opened!" : "Failed");
					}
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Send CMD_OPEN_WORMHOLE to the server for the target SteamID");

			ImGui::SameLine();
			if (ImGui::Button("Refresh info"))
			{
				update_info();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Reload player_id and session status from the DLL");

			ImGui::SameLine();
			if (ImGui::Button("Quick Settings"))
			{
				show_settings_dialog.store(true);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Open the quick settings dialog to apply preset parameters");

			ImGui::Separator();
			ImGui::Text("=== Tools ===");

			if (ImGui::Button("Cheats##btn"))
			{
				show_cheats_dialog.store(true);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Open cheats panel: unlockall, add gmp/heroic, set ogre, spp staff");

			ImGui::SameLine();
			if (ImGui::Button("FOB##btn"))
			{
				show_fob_dialog.store(true);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Open FOB commands panel: add support, open wormhole, convert list");

			ImGui::Separator();
			ImGui::Text("=== Commands ===");

			if (ImGui::Button("Refresh commands"))
			{
				fetch_commands();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Reload the available tpp-mod command list from the DLL");

			ImGui::SameLine();
			if (ImGui::InputText("Filter", filter_buf, sizeof(filter_buf)))
			{
				update_filter();
			}

			{
				std::lock_guard<std::mutex> lock(data_mutex);
				ImGui::BeginChild("##commands", ImVec2(0, -80), true);
				for (const auto& name : filtered_cmds)
				{
					if (ImGui::Button(name.c_str()))
					{
						for (const auto& c : commands_list)
						{
							if (c.name == name)
							{
								strncpy_s(input_text, sizeof(input_text), c.name.c_str(), _TRUNCATE);
								break;
							}
						}
					}
					if (ImGui::IsItemHovered())
					{
						for (const auto& c : commands_list)
						{
							if (c.name == name)
							{
								std::string tip;
								if (!c.description.empty())
									tip += c.description;
								if (!c.usage.empty())
								{
									if (!tip.empty()) tip += "\n";
									tip += "Usage: " + c.usage;
								}
								if (!tip.empty())
									ImGui::SetTooltip("%s", tip.c_str());
								break;
							}
						}
					}
					ImGui::SameLine();
					ImGui::TextDisabled("|");
				}
				ImGui::EndChild();
			}

			ImGui::Text("Execute:");
			if (ImGui::InputText("##exec_input", input_text, sizeof(input_text)))
			{
			}

			ImGui::SameLine();
			if (ImGui::Button("Execute"))
			{
				if (input_text[0] != '\0')
				{
					execute_tpp_command(input_text);
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Send the typed command to the tpp-mod DLL for execution");

			ImGui::Separator();
			{
				std::lock_guard<std::mutex> lock(data_mutex);
				ImGui::Text("Status: %s", status_text);
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##RightPanel", ImVec2(right_width, 0), true);
		{
			ImGui::Text("=== Console Log ===");
			ImGui::SameLine();
			ImGui::Checkbox("Auto-scroll", &auto_scroll_logs);
			ImGui::SameLine();
			if (ImGui::Button("Clear"))
			{
				std::lock_guard<std::mutex> lock(log_mutex);
				log_entries.clear();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Clear all console log entries");

			ImGui::Separator();

			ImGui::BeginChild("##log_content", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
			{
				std::lock_guard<std::mutex> lock(log_mutex);
				for (const auto& entry : log_entries)
				{
					ImVec4 color;
					switch (entry.type)
					{
					case 1: // error
						color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
						break;
					case 2: // debug
						color = ImVec4(0.3f, 0.8f, 1.0f, 1.0f);
						break;
					case 3: // warning
						color = ImVec4(1.0f, 0.9f, 0.3f, 1.0f);
						break;
					default: // info
						color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
						break;
					}
					ImGui::PushStyleColor(ImGuiCol_Text, color);
					ImGui::TextUnformatted(entry.message.c_str());
					ImGui::PopStyleColor();
				}
			}
			if (auto_scroll_logs && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
				ImGui::SetScrollHereY(1.0f);
			ImGui::EndChild();
		}
		ImGui::EndChild();

		ImGui::End();
	}

	int run()
	{
		load_config();
		scale_factor = g_window_config.font_scale;
		snprintf(scale_input, sizeof(scale_input), "%.1f", scale_factor);

		WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L,
			GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
			"FOBGuiWndClass", nullptr };
		RegisterClassExA(&wc);
		g_hwnd = CreateWindowExA(0, wc.lpszClassName, "TPP-Mod Control",
			WS_OVERLAPPEDWINDOW,
			g_window_config.x, g_window_config.y,
			g_window_config.w, g_window_config.h, nullptr, nullptr, wc.hInstance, nullptr);

		create_d3d_device();

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.FontGlobalScale = scale_factor;

		const char* font_paths[] = {
			"C:\\Windows\\Fonts\\msyh.ttc",
			"C:\\Windows\\Fonts\\msyhbd.ttc",
			"C:\\Windows\\Fonts\\simsun.ttc",
			"C:\\Windows\\Fonts\\simhei.ttf",
			"C:\\Windows\\Fonts\\meiryob.ttc",
			"C:\\Windows\\Fonts\\msgothic.ttc",
		};
		ImFont* loaded_font = nullptr;
		for (const auto& path : font_paths)
		{
			DWORD attr = GetFileAttributesA(path);
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
			{
				loaded_font = io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr,
					io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
				if (loaded_font) break;
			}
		}
		if (!loaded_font)
		{
			ImFontConfig font_cfg;
			font_cfg.SizePixels = 18.0f;
			io.Fonts->AddFontDefault(&font_cfg);
		}

		io.Fonts->Build();

		ImGui::StyleColorsDark();
		if (!ImGui_ImplWin32_Init(g_hwnd))
		{
			MessageBoxA(nullptr, "ImGui_ImplWin32_Init failed!", "Error", MB_OK | MB_ICONERROR);
			PostQuitMessage(1);
			return 1;
		}
		if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext))
		{
			MessageBoxA(nullptr, "ImGui_ImplDX11_Init failed! Your GPU may not support the required DirectX 11 features.", "Error", MB_OK | MB_ICONERROR);
			PostQuitMessage(1);
			return 1;
		}

		ShowWindow(g_hwnd, SW_SHOWDEFAULT);
		UpdateWindow(g_hwnd);

		for (int i = 0; i < 5; i++)
		{
			if (connect_to_pipe())
			{
				update_info();
				fetch_commands();
				fetch_logs();
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		std::thread heartbeat_thread(heartbeat_thread_main);
		heartbeat_thread.detach();

		MSG msg;
		ZeroMemory(&msg, sizeof(msg));
		while (msg.message != WM_QUIT)
		{
			if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
				continue;
			}

			if (resize_pending.exchange(false))
			{
				cleanup_render_target();
				RECT rc;
				GetClientRect(g_hwnd, &rc);
				g_pSwapChain->ResizeBuffers(0, rc.right - rc.left, rc.bottom - rc.top,
					DXGI_FORMAT_UNKNOWN, 0);
				create_render_target();
				ImGui_ImplDX11_InvalidateDeviceObjects();
				ImGui_ImplDX11_CreateDeviceObjects();
			}

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			render_gui();

			ImGui::Render();
			g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
			const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
			g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			g_pSwapChain->Present(1, 0);
		}

		should_exit.store(true);
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		cleanup_d3d();
		disconnect_pipe();
		DestroyWindow(g_hwnd);
		UnregisterClassA(wc.lpszClassName, wc.hInstance);

		return 0;
	}
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	return fob_gui::run();
}
