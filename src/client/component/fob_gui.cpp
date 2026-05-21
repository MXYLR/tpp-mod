#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "renderer.hpp"
#include "scheduler.hpp"
#include "vars.hpp"
#include "server_logging.hpp"

#include <utils/hook.hpp>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

namespace fob_gui
{
	namespace
	{
		bool imgui_initialized = false;
		WNDPROC original_wndproc = nullptr;
		std::uint64_t target_player_id = 3777848;
		std::uint64_t my_player_id = 3924452;

		std::atomic<bool> auto_send_active{false};
		std::thread auto_send_thread;
		std::atomic<std::uint64_t> auto_send_count{0};

		std::string status_text;
		std::chrono::steady_clock::time_point status_time;

		bool show_gui = true;

		void set_status(const std::string& text)
		{
			status_text = text;
			status_time = std::chrono::steady_clock::now();
		}

		LRESULT CALLBACK wnd_proc_hook(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
		{
			if (imgui_initialized && show_gui)
			{
				ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param);
			}
			return CallWindowProc(original_wndproc, hwnd, msg, w_param, l_param);
		}

		void draw_fob_panel()
		{
			if (!show_gui)
			{
				return;
			}

			ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);

			ImGui::Begin("FOB Wormhole Control", &show_gui, ImGuiWindowFlags_NoSavedSettings);

			ImGui::Text("Player IDs");
			ImGui::Separator();

			ImGui::InputScalar("Target player_id", ImGuiDataType_U64, &target_player_id);
			ImGui::InputScalar("My player_id", ImGuiDataType_U64, &my_player_id);

			if (!server_logging::has_session_key())
			{
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "No session key! Login and visit FOB menu first.");
			}
			else
			{
				ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "Session key ready");
			}

			ImGui::Separator();
			ImGui::Text("Wormhole");
			ImGui::Separator();

			if (ImGui::Button("Open wormhole", ImVec2(-1, 30)))
			{
				if (server_logging::open_wormhole(target_player_id, my_player_id))
				{
					set_status("Wormhole opened successfully! Re-select target in FOB menu.");
				}
				else
				{
					set_status("Wormhole request failed. Check console for details.");
				}
			}

			ImGui::Separator();
			ImGui::Text("Auto wormhole");
			ImGui::Separator();

			if (!auto_send_active.load())
			{
				if (ImGui::Button("Start auto wormhole", ImVec2(-1, 30)))
				{
					auto_send_count.store(0);
					auto_send_active.store(true);
					auto_send_thread = std::thread([&]()
					{
						while (auto_send_active.load())
						{
							server_logging::open_wormhole(target_player_id, my_player_id);
							auto_send_count.fetch_add(1);
							std::this_thread::sleep_for(std::chrono::milliseconds(100));
						}
					});
					set_status("Auto wormhole started");
				}
			}
			else
			{
				ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "Auto wormhole is running...");
				if (ImGui::Button("Stop auto wormhole", ImVec2(-1, 30)))
				{
					auto_send_active.store(false);
					if (auto_send_thread.joinable())
					{
						auto_send_thread.join();
					}
					set_status("Auto wormhole stopped");
				}
			}

			ImGui::Text("Requests sent: %llu", (unsigned long long)auto_send_count.load());

			if (!status_text.empty())
			{
				auto elapsed = std::chrono::steady_clock::now() - status_time;
				if (elapsed < std::chrono::seconds(10))
				{
					ImGui::Separator();
					ImGui::TextWrapped("%s", status_text.c_str());
				}
				else
				{
					status_text.clear();
				}
			}

			ImGui::End();
		}

		void on_frame(game::fox::gr::dg::plugins::Draw2DRenderer*)
		{
			if (!imgui_initialized)
			{
				return;
			}

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			draw_fob_panel();

			ImGui::Render();
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		}

		std::once_flag init_flag;

		void initialize_imgui()
		{
			std::call_once(init_flag, []()
			{
				const auto device = *game::s_deviceD3D;
				const auto context = *game::s_immediateContextD3D;

				if (!device || !context)
				{
					return;
				}

				IMGUI_CHECKVERSION();
				ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO();
				io.FontGlobalScale = 1.5f;
				io.IniFilename = nullptr;

				ImGui::StyleColorsDark();

				const auto hwnd = FindWindowA(NULL, "METAL GEAR SOLID V: THE PHANTOM PAIN");
				if (!hwnd)
				{
					return;
				}

				if (!ImGui_ImplWin32_Init(hwnd))
				{
					return;
				}

				if (!ImGui_ImplDX11_Init(device, context))
				{
					ImGui_ImplWin32_Shutdown();
					return;
				}

				original_wndproc = reinterpret_cast<WNDPROC>(
					SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook))
				);

				imgui_initialized = true;
			});
		}

		void shutdown_imgui()
		{
			if (auto_send_active.load())
			{
				auto_send_active.store(false);
				if (auto_send_thread.joinable())
				{
					auto_send_thread.join();
				}
			}

			if (imgui_initialized)
			{
				imgui_initialized = false;

				ImGui_ImplDX11_Shutdown();
				ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext();

				const auto hwnd = FindWindowA(NULL, "METAL GEAR SOLID V: THE PHANTOM PAIN");
				if (hwnd && original_wndproc)
				{
					SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wndproc));
					original_wndproc = nullptr;
				}
			}
		}

		vars::var_ptr var_fob_gui_enabled;

		void update_gui_visibility()
		{
			show_gui = var_fob_gui_enabled->current.enabled();
		}
	}

	struct component : component_interface
	{
		void post_load() override
		{
			initialize_imgui();
		}

		void post_start() override
		{
			renderer::on_frame(on_frame);
		}

		void pre_load() override
		{
			var_fob_gui_enabled = vars::register_bool("fob_gui_enabled", true, vars::var_flag_saved, "enable FOB wormhole GUI panel");
		}

		void start() override
		{
			scheduler::loop(update_gui_visibility, scheduler::pipeline::main, 1s);
		}

		void end() override
		{
			shutdown_imgui();
		}
	};
}

REGISTER_COMPONENT(fob_gui::component)
