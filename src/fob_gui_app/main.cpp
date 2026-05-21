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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fob_gui
{
	constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\TPPMod_FOBControl";
	constexpr DWORD PIPE_BUFFER_SIZE = 8192;

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

	struct FriendInfo
	{
		std::string name;
		uint64_t steam_id;
	};

	std::atomic<bool> show_all_friends_dialog{ false };
	std::vector<FriendInfo> all_friends;

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

	HANDLE g_pipe_handle = INVALID_HANDLE_VALUE;

	bool send_command(const std::string& cmd, std::string& response)
	{
		if (g_pipe_handle == INVALID_HANDLE_VALUE)
			return false;

		DWORD bytesWritten, bytesRead;
		BOOL success = WriteFile(g_pipe_handle, cmd.c_str(), (DWORD)cmd.size() + 1, &bytesWritten, nullptr);
		if (!success)
			return false;

		char buffer[PIPE_BUFFER_SIZE];
		success = ReadFile(g_pipe_handle, buffer, PIPE_BUFFER_SIZE - 1, &bytesRead, nullptr);
		if (!success || bytesRead == 0)
			return false;

		buffer[bytesRead] = '\0';
		response = buffer;
		return true;
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
		{
			CommandInfo info;
			info.name = "all_friends";
			info.description = "Fetch all Steam friends (bypasses game 50 limit)";
			info.usage = "all_friends";
			commands_list.push_back(info);
		}
		filtered_cmds.clear();
		for (const auto& c : commands_list)
		{
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

	void fetch_all_friends()
	{
		if (!connected.load())
			return;

		// Trigger a cache refresh on the game's main thread
		std::string dummy;
		send_command("REFRESH_FRIENDS", dummy);

		// Wait for the game thread to process and cache the friend list
		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		std::string response;
		if (!send_command("GET_ALL_FRIENDS", response))
			return;

		std::vector<FriendInfo> new_list;

		size_t pos = 0;
		while (pos < response.size())
		{
			size_t end_pos = response.find("||", pos);
			std::string friend_entry;
			if (end_pos == std::string::npos)
			{
				friend_entry = response.substr(pos);
				pos = response.size();
			}
			else
			{
				friend_entry = response.substr(pos, end_pos - pos);
				pos = end_pos + 2;
			}

			if (friend_entry.empty())
				continue;

			size_t sep = friend_entry.find("|");
			if (sep != std::string::npos)
			{
				try
				{
					FriendInfo info;
					info.name = friend_entry.substr(0, sep);
					info.steam_id = std::stoull(friend_entry.substr(sep + 1));
					new_list.push_back(info);
				}
				catch (...) {}
			}
		}
		all_friends = new_list;
	}

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
			}
			return 0;
		case WM_DESTROY:
			should_exit.store(true);
			PostQuitMessage(0);
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

		D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
			featureLevelArray, _countof(featureLevelArray), D3D11_SDK_VERSION, &sd, &g_pSwapChain,
			&g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

		if (!create_render_target())
		{
			cleanup_d3d();
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

		if (cmd == "all_friends")
		{
			fetch_all_friends();
			show_all_friends_dialog.store(true);
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

		if (ImGui::BeginPopupModal("Convert List Type", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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

		if (ImGui::BeginPopupModal("Cached Players", nullptr, ImGuiWindowFlags_NoResize))
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

		if (ImGui::BeginPopupModal("FOB Target List", nullptr, ImGuiWindowFlags_NoResize))
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

	void render_all_friends_dialog()
	{
		if (!show_all_friends_dialog.load())
			return;

		ImGui::OpenPopup("All Friends");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w(), popup_h()), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("All Friends", nullptr, ImGuiWindowFlags_NoResize))
		{
			if (ImGui::Button("Refresh", ImVec2(btn_w(), btn_h())))
			{
				fetch_all_friends();
			}
			ImGui::SameLine();
			ImGui::Text("(%zu)", all_friends.size());

			ImGui::Separator();

			float child_h = ImGui::GetContentRegionAvail().y - btn_h() - ImGui::GetStyle().ItemSpacing.y;
			if (ImGui::BeginChild("##all_friends_list", ImVec2(0, child_h), true))
			{
				for (size_t i = 0; i < all_friends.size(); ++i)
				{
					auto& friend_info = all_friends[i];
					char label[64];
					sprintf_s(label, "%s", friend_info.name.c_str());

					ImVec2 item_size = ImVec2(ImGui::GetContentRegionAvail().x - btn_w() - ImGui::GetStyle().ItemSpacing.x, btn_h());
					ImGui::Selectable(label, false, 0, item_size);

					if (ImGui::IsItemHovered())
					{
						char sid_str[64];
						sprintf_s(sid_str, "SteamID: %llu", friend_info.steam_id);
						ImGui::SetTooltip("%s", sid_str);
					}

					ImGui::SameLine();
					char copy_btn_label[64];
					sprintf_s(copy_btn_label, "Copy##af%zu", i);
					if (ImGui::Button(copy_btn_label, ImVec2(btn_w(), btn_h())))
					{
						ImGui::SetClipboardText(friend_info.name.c_str());
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Copy name to clipboard");
					}
				}
			}
			ImGui::EndChild();

			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(btn_w(), btn_h())))
			{
				show_all_friends_dialog.store(false);
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	void render_vars_dialog()
	{
		if (!show_vars_dialog.load())
			return;

		ImGui::OpenPopup("Variables");
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(popup_w() + 100, popup_h() + 60), ImGuiCond_Appearing);

		if (ImGui::BeginPopupModal("Variables", nullptr, ImGuiWindowFlags_NoResize))
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

	void render_gui()
	{
		render_convert_dialog();
		render_cache_dialog();
		render_targets_dialog();
		render_all_friends_dialog();
		render_vars_dialog();

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
				snprintf(scale_input, sizeof(scale_input), "%.1f", scale_factor);
				ImGui::GetIO().FontGlobalScale = scale_factor;
				ImGui::GetIO().Fonts->Build();
				ImGui_ImplDX11_InvalidateDeviceObjects();
				ImGui_ImplDX11_CreateDeviceObjects();
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
		WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L,
			GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
			"FOBGuiWndClass", nullptr };
		RegisterClassExA(&wc);
		g_hwnd = CreateWindowExA(0, wc.lpszClassName, "TPP-Mod Control",
			WS_OVERLAPPEDWINDOW,
			100, 100, (int)(550 * scale_factor), (int)(425 * scale_factor), nullptr, nullptr, wc.hInstance, nullptr);

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

		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(g_hwnd);
		ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

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
