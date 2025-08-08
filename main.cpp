#include <windows.h>
#include <iostream>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include "tsvdata.hpp"
#include "config.hpp"

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif


#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

namespace
{
	// Config for example app
	static const int APP_NUM_FRAMES_IN_FLIGHT = 2;
	static const int APP_NUM_BACK_BUFFERS = 2;
	static const int APP_SRV_HEAP_SIZE = 64;

	struct FrameContext
	{
		ID3D12CommandAllocator* CommandAllocator;
		UINT64                      FenceValue;
	};

	// Simple free list based allocator
	struct ExampleDescriptorHeapAllocator
	{
		ID3D12DescriptorHeap* Heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
		D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
		D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
		UINT                        HeapHandleIncrement;
		ImVector<int>               FreeIndices;

		void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap)
		{
			IM_ASSERT(Heap == nullptr && FreeIndices.empty());
			Heap = heap;
			D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
			HeapType = desc.Type;
			HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
			HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
			HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
			FreeIndices.reserve((int)desc.NumDescriptors);
			for (int n = desc.NumDescriptors; n > 0; n--)
				FreeIndices.push_back(n - 1);
		}
		void Destroy()
		{
			Heap = nullptr;
			FreeIndices.clear();
		}
		void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
		{
			IM_ASSERT(FreeIndices.Size > 0);
			int idx = FreeIndices.back();
			FreeIndices.pop_back();
			out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
			out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
		}
		void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
		{
			int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
			int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
			IM_ASSERT(cpu_idx == gpu_idx);
			FreeIndices.push_back(cpu_idx);
		}
	};

	// Data
	FrameContext                 g_frameContext[APP_NUM_FRAMES_IN_FLIGHT] = {};
	UINT                         g_frameIndex = 0;

	ID3D12Device*					g_pd3dDevice = nullptr;
	ID3D12DescriptorHeap*			g_pd3dRtvDescHeap = nullptr;
	ID3D12DescriptorHeap*			g_pd3dSrvDescHeap = nullptr;
	ExampleDescriptorHeapAllocator	g_pd3dSrvDescHeapAlloc;
	ID3D12CommandQueue*				g_pd3dCommandQueue = nullptr;
	ID3D12GraphicsCommandList*		g_pd3dCommandList = nullptr;
	ID3D12Fence*					g_fence = nullptr;
	HANDLE							g_fenceEvent = nullptr;
	UINT64							g_fenceLastSignaledValue = 0;
	IDXGISwapChain3*				g_pSwapChain = nullptr;
	bool							g_SwapChainOccluded = false;
	HANDLE							g_hSwapChainWaitableObject = nullptr;
	ID3D12Resource*					g_mainRenderTargetResource[APP_NUM_BACK_BUFFERS] = {};
	D3D12_CPU_DESCRIPTOR_HANDLE		g_mainRenderTargetDescriptor[APP_NUM_BACK_BUFFERS] = {};

	// Forward declarations of helper functions
	bool CreateDeviceD3D(HWND hWnd);
	void CleanupDeviceD3D();
	void CreateRenderTarget();
	void CleanupRenderTarget();
	void WaitForLastSubmittedFrame();
	FrameContext* WaitForNextFrameResources();
	LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
			return true;

		switch (msg)
		{
		case WM_SIZE:
			if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
			{
				WaitForLastSubmittedFrame();
				CleanupRenderTarget();
				HRESULT result = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
				assert(SUCCEEDED(result) && "Failed to resize swapchain.");
				CreateRenderTarget();
			}
			return 0;
		case WM_SYSCOMMAND:
			if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
				return 0;
			break;
		case WM_DESTROY:
			::PostQuitMessage(0);
			return 0;
		}

		return ::DefWindowProcW(hWnd, msg, wParam, lParam);
	}


	std::atomic_bool s_exiting(false);

	struct View
	{
		bool visible = false;
		std::string sorts;

		/*
						ImGuiSelectableFlags selectable_flags = (contents_type == CT_SelectableSpanRow) ? ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap : ImGuiSelectableFlags_None;
						if (ImGui::Selectable(label, item_is_selected, selectable_flags, ImVec2(0, row_min_height)))
		*/
		ImVector<int> selection;

		int select_from = -1;
		int select_to = -1;
	};

	struct ViewState
	{
		std::unordered_map<std::string, View> views;
	};

	std::unordered_map<std::string, ViewState> m_state;

	ViewState& getViewState(data::DbDataSet& db)
	{
		auto [path, pattern] = db.GetPath();
		static std::string key;
		key.clear();
		key.reserve(path.size() + pattern.size());
		key.append(path).append(pattern);
		return m_state[key];
	}

	constexpr int DefaultWidth = 1280;
	constexpr int DefaultHeight = 800;

	std::unordered_map<std::string, std::unique_ptr<data::DbDataSet>> s_data;

	std::unordered_map<std::string, bool> s_opened;

	config::Config s_config;

	void logMsg(const std::string& msg)
	{
		std::cout << msg;
	}

	void DrawTableView(data::DbDataSet& db, const data::DbTableMetaData& table, bool* opened)
	{
		ImGui::SetNextWindowSize(ImVec2(1024, 768), ImGuiCond_Once);

		if (!ImGui::Begin(table.table_name.c_str(), opened, 0))
		{
			ImGui::End();
			return;
		}

		ImGuiTableFlags flags =
			ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable
			| ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_NoBordersInBody
			| ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;

		std::string table_view_name = table.table_name;
		table_view_name.append("_data");

		static const float TEXT_BASE_WIDTH = ImGui::CalcTextSize("A").x;
		static const float TEXT_BASE_HEIGHT = ImGui::GetTextLineHeightWithSpacing();

		int initHeight = table.count > 30 ? 30 : int(table.count);
		int initWidth = table.columns.size() > 50 ? 50 : int(table.columns.size());

		ViewState& viewState = getViewState(db);

		if (ImGui::BeginTable(table_view_name.c_str(), int(table.columns.size()), flags, ImVec2(0, 0/*initHeight * (TEXT_BASE_HEIGHT + 5)*/), 0/*initWidth * TEXT_BASE_WIDTH * 10*/))
		{
			View& view = viewState.views[table_view_name];

			int i = 0;
			for (const auto& col : table.columns)
			{
				ImGui::TableSetupColumn(col.c_str(), (i++ == 0 ? ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide : ImGuiTableColumnFlags_None));
			}

			ImGui::TableSetupScrollFreeze(1, 1); // Make row always visible
			ImGui::TableHeadersRow();

			ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();

			if (sort_specs && sort_specs->SpecsDirty)
			{
				std::string& sort = view.sorts;

				sort.clear();

				for (int i = 0; i < sort_specs->SpecsCount; ++i)
				{
					const auto spec = sort_specs->Specs[i];
					assert(spec.ColumnIndex < table.columns.size());

					sort.append(table.columns[spec.ColumnIndex]);
					if (spec.SortDirection == ImGuiSortDirection_Ascending)
						sort.append(" ASC, ");
					else
						sort.append(" DESC, ");
				}

				if (sort_specs->SpecsCount)
					sort.resize(sort.size() - 2);

				view.selection.resize(0);
				view.select_to = -1;
				view.select_from = -1;
				sort_specs->SpecsDirty = false;
			}

			ImGuiListClipper clipper;
			clipper.Begin(int(table.count));
			while (clipper.Step())
			{
				auto start = clipper.DisplayStart;
				auto end = clipper.DisplayEnd;

				bool will_range_select = false;
				bool range_selecting = false;

				db.GetRows(table, viewState.views[table_view_name].sorts, [&](const std::vector<data::DbDataSet::ValType>& data)
					{
						int id = std::get<int>(data[0]);

						auto& selection = view.selection;

						if (!will_range_select)
						{
							if (!range_selecting && (id == view.select_from || id == view.select_to))
								range_selecting = true;
							else if (range_selecting && (id == view.select_to || id == view.select_from))
							{
								range_selecting = false;
								selection.push_back(id);
								view.select_from = -1;
								view.select_to = -1;
							}
						}

						if (range_selecting)
						{
							if (!selection.contains(id))
							{
								selection.push_back(id);
							}
						}

						const bool item_is_selected = selection.contains(id);

						ImGui::PushID(id);
						ImGui::TableNextRow();

						ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;


						ImGui::TableNextColumn();
						char label[32];
						sprintf_s(label, std::extent<decltype(label)>(), "%d", id);
						if (ImGui::Selectable(label, item_is_selected, selectable_flags))
						{
							if (ImGui::GetIO().KeyCtrl)
							{
								if (item_is_selected)
									selection.find_erase_unsorted(id);
								else
									selection.push_back(id);
							}
							else if (ImGui::GetIO().KeyShift)
							{
								if (view.select_to == -1)
								{
									view.select_from = selection.empty() ? 0 : selection.back();
									view.select_to = id;
									will_range_select = true;
								}
							}
							else
							{
								selection.clear();
								selection.push_back(id);
							}
						}

						//ImGui::Text("%d", std::get<int>(data[0]));
						for (auto it = data.begin() + 1; it != data.end(); ++it)
						{
							ImGui::TableNextColumn();

							const auto& str = std::get<std::string>(*it);

							ImGui::TextUnformatted(str.c_str());
						}
						ImGui::PopID();
					}, logMsg, end, start);
			}

			ImGui::EndTable();
		}


		ImGui::End();
	}

	void DrawHistoryWindow()
	{
		if (!ImGui::Begin("History", 0, ImGuiWindowFlags_NoDocking))
		{
			ImGui::End();
			return;
		}

		static std::string add;
		if (add.size() < 255)
			add.resize(255);

		if (ImGui::InputText("path", add.data(), int(add.size()), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			std::string add_config(add.begin(), add.begin() + strlen(add.c_str()));

			s_config.HistoryAdd(add_config);
			add[0] = '\0';
			add.clear();
		}

		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(0xFF0000FF));

		std::vector<int> erase;

		s_config.HistoryGet([&](int id, const char* hData)
			{
				std::string history(hData);
				ImGui::PushID(id);
				if (ImGui::SmallButton("X"))
				{
					erase.push_back(id);
					ImGui::PopID();
					return;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton(hData))
				{
					if (s_data.count(history))
					{
						if (s_opened.count(history) && !s_opened[history])
						{
							s_opened[history] = true;
						}
						else
						{
							s_opened[history] = false;
						}
					}
					else
					{
						try
						{
							s_data[history] = std::make_unique<data::DbDataSet>();
							s_data[history]->LoadFromPath(hData, ".txt", logMsg);
							s_opened[history] = true;
						}
						catch (...)
						{
							erase.push_back(id);
						}
					}
				}
				ImGui::PopID();
			});

		for (const auto& id : erase)
		{
			s_config.HistoryRem(id);
		}

		ImGui::PopStyleColor();
		ImGui::End();
	}

	void DrawMetaWindow(data::DbDataSet& db, bool* opened)
	{
		if (opened && !*opened)
			return;

		static std::string name;

		name.clear();
		name.append("DB - ");
		name.append(std::get<0>(db.GetPath()));

		if (!ImGui::Begin(name.c_str(), opened, ImGuiWindowFlags_NoDocking))
		{
			ImGui::End();
			return;
		}


		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(0x000000FF));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(0xFF0000FF));
		ImGui::TextUnformatted("Path:");
		ImGui::SameLine();
		ImGui::SmallButton(std::get<0>(db.GetPath()).c_str());
		ImGui::PopStyleColor();
		ImGui::PopStyleColor();
		ImGui::Spacing();
		ImGui::Spacing();

		ImGuiTreeNodeFlags base_flags = ImGuiTreeNodeFlags_DrawLinesFull | ImGuiTreeNodeFlags_DefaultOpen;
		if (ImGui::TreeNodeEx("Tables", base_flags))
		{
			ImGuiTreeNodeFlags child_flags = ImGuiTreeNodeFlags_DrawLinesFull;

			const auto& meta = db.GetTableMetaData();

			ViewState& viewState = getViewState(db);

			for (const auto& tab : meta.tables)
			{
				static std::stringstream name;

				name.str("");
				name.clear();
				name << tab.table_name << " (" << tab.count << ")";

				if (ImGui::TreeNodeEx(name.str().c_str(), child_flags))
				{
					if (ImGui::Button("table"))
					{
						viewState.views[tab.table_name].visible = (!viewState.views.count(tab.table_name) || !viewState.views[tab.table_name].visible) ? true : false;
					}

					if (viewState.views[tab.table_name].visible)
					{
						DrawTableView(db, tab, &viewState.views[tab.table_name].visible);
					}


					ImGui::Text(tab.file_name.c_str());

					name.str("");
					name.clear();
					name << "columns (" << tab.columns.size() << ")";
					if (ImGui::TreeNodeEx(name.str().c_str(), child_flags))
					{
						for (const auto& col : tab.columns)
						{
							ImGui::Text(col.c_str());
						}
						ImGui::TreePop();
					}
					ImGui::TreePop();
				}

			}

			ImGui::TreePop();
		}

		ImGui::End();
	}
}

void DrawDataWindows()
{
	for (const auto& data : s_data)
	{
		auto& [key, pData] = data;
		if (s_opened.count(key) && s_opened[key])
		{
			auto& flag = s_opened[key];
			DrawMetaWindow(*std::get<1>(data), &flag);
		}
	}
}

int main(int argc, char* argv[])
{
	std::string configP("config.db");

	for (int i = 1; i < argc; i++)
	{
		if (_stricmp("-config",argv[i]) == 0)
		{
			if (i + 1 < argc)
			{
				configP = argv[i + 1];
				i++;
			}
		}
	}

	std::filesystem::path cfg_path(configP);

	if (!s_config.CreatePaths(cfg_path))
	{
		std::cout << "Failed to create config database path: " << cfg_path.parent_path() << "\n";
		return 1;
	}

	s_config.Load(configP);

	//s_data = std::make_unique<data::DbDataSet>();
	//s_data->LoadFromPath(datapath.c_str(), ".txt", logMsg);

	ImGui_ImplWin32_EnableDpiAwareness();
	float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

	// Create application window
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"Gui4Life", nullptr };
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Gui4Life", WS_OVERLAPPEDWINDOW, 100, 100, (int)(DefaultWidth * main_scale), (int)(DefaultHeight * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

	// Initialize Direct3D
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);


	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows

	ImGui::StyleColorsDark();

	// Setup scaling
	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
	style.FontScaleDpi = main_scale;        // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)
	io.ConfigDpiScaleFonts = true;          // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
	io.ConfigDpiScaleViewports = true;      // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(hwnd);

	ImGui_ImplDX12_InitInfo init_info = {};
	init_info.Device = g_pd3dDevice;
	init_info.CommandQueue = g_pd3dCommandQueue;
	init_info.NumFramesInFlight = APP_NUM_FRAMES_IN_FLIGHT;
	init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
	// Allocating SRV descriptors (for textures) is up to the application, so we provide callbacks.
	// (current version of the backend will only allocate one descriptor, future versions will need to allocate more)
	init_info.SrvDescriptorHeap = g_pd3dSrvDescHeap;
	init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
	init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };
	ImGui_ImplDX12_Init(&init_info);

	bool show_demo_window = false;
	bool show_another_window = false;
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	while (!s_exiting)
	{
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				s_exiting = true;
		}
		if (s_exiting)
			break;

		// Handle window screen locked
		if ((g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd))
		{
			::Sleep(10);
			continue;
		}
		g_SwapChainOccluded = false;

		// Start the Dear ImGui frame
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (show_demo_window)
			ImGui::ShowDemoWindow(&show_demo_window);

		DrawHistoryWindow();
		DrawDataWindows();

		ImGui::Render();

		FrameContext* frameCtx = WaitForNextFrameResources();
		UINT backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();
		frameCtx->CommandAllocator->Reset();

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
		g_pd3dCommandList->ResourceBarrier(1, &barrier);

		// Render Dear ImGui graphics
		const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
		g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
		g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
		g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		g_pd3dCommandList->ResourceBarrier(1, &barrier);
		g_pd3dCommandList->Close();

		g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pd3dCommandList);

		// Update and Render additional Platform Windows
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}

		// Present
		HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
		//HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
		g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

		UINT64 fenceValue = g_fenceLastSignaledValue + 1;
		g_pd3dCommandQueue->Signal(g_fence, fenceValue);
		g_fenceLastSignaledValue = fenceValue;
		frameCtx->FenceValue = fenceValue;

	}

	WaitForLastSubmittedFrame();

	// Cleanup
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);
	return 0;
}

namespace
{

	bool CreateDeviceD3D(HWND hWnd)
	{
		// Setup swap chain
		DXGI_SWAP_CHAIN_DESC1 sd;
		{
			ZeroMemory(&sd, sizeof(sd));
			sd.BufferCount = APP_NUM_BACK_BUFFERS;
			sd.Width = 0;
			sd.Height = 0;
			sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.SampleDesc.Count = 1;
			sd.SampleDesc.Quality = 0;
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
			sd.Scaling = DXGI_SCALING_STRETCH;
			sd.Stereo = FALSE;
		}

		// [DEBUG] Enable debug interface
#ifdef DX12_ENABLE_DEBUG_LAYER
		ID3D12Debug* pdx12Debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
			pdx12Debug->EnableDebugLayer();
#endif

		// Create device
		D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
		if (D3D12CreateDevice(nullptr, featureLevel, IID_PPV_ARGS(&g_pd3dDevice)) != S_OK)
			return false;

		// [DEBUG] Setup debug interface to break on any warnings/errors
#ifdef DX12_ENABLE_DEBUG_LAYER
		if (pdx12Debug != nullptr)
		{
			ID3D12InfoQueue* pInfoQueue = nullptr;
			g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
			pInfoQueue->Release();
			pdx12Debug->Release();
		}
#endif

		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			desc.NumDescriptors = APP_NUM_BACK_BUFFERS;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			desc.NodeMask = 1;
			if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK)
				return false;

			SIZE_T rtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
			for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
			{
				g_mainRenderTargetDescriptor[i] = rtvHandle;
				rtvHandle.ptr += rtvDescriptorSize;
			}
		}

		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = APP_SRV_HEAP_SIZE;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK)
				return false;
			g_pd3dSrvDescHeapAlloc.Create(g_pd3dDevice, g_pd3dSrvDescHeap);
		}

		{
			D3D12_COMMAND_QUEUE_DESC desc = {};
			desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			desc.NodeMask = 1;
			if (g_pd3dDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_pd3dCommandQueue)) != S_OK)
				return false;
		}

		for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
			if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK)
				return false;

		if (g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
			g_pd3dCommandList->Close() != S_OK)
			return false;

		if (g_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
			return false;

		g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (g_fenceEvent == nullptr)
			return false;

		{
			IDXGIFactory4* dxgiFactory = nullptr;
			IDXGISwapChain1* swapChain1 = nullptr;
			if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK)
				return false;
			if (dxgiFactory->CreateSwapChainForHwnd(g_pd3dCommandQueue, hWnd, &sd, nullptr, nullptr, &swapChain1) != S_OK)
				return false;
			if (swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain)) != S_OK)
				return false;
			swapChain1->Release();
			dxgiFactory->Release();
			g_pSwapChain->SetMaximumFrameLatency(APP_NUM_BACK_BUFFERS);
			g_hSwapChainWaitableObject = g_pSwapChain->GetFrameLatencyWaitableObject();
		}

		CreateRenderTarget();
		return true;
	}

	void CleanupDeviceD3D()
	{
		CleanupRenderTarget();
		if (g_pSwapChain) { g_pSwapChain->SetFullscreenState(false, nullptr); g_pSwapChain->Release(); g_pSwapChain = nullptr; }
		if (g_hSwapChainWaitableObject != nullptr) { CloseHandle(g_hSwapChainWaitableObject); }
		for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
			if (g_frameContext[i].CommandAllocator) { g_frameContext[i].CommandAllocator->Release(); g_frameContext[i].CommandAllocator = nullptr; }
		if (g_pd3dCommandQueue) { g_pd3dCommandQueue->Release(); g_pd3dCommandQueue = nullptr; }
		if (g_pd3dCommandList) { g_pd3dCommandList->Release(); g_pd3dCommandList = nullptr; }
		if (g_pd3dRtvDescHeap) { g_pd3dRtvDescHeap->Release(); g_pd3dRtvDescHeap = nullptr; }
		if (g_pd3dSrvDescHeap) { g_pd3dSrvDescHeap->Release(); g_pd3dSrvDescHeap = nullptr; }
		if (g_fence) { g_fence->Release(); g_fence = nullptr; }
		if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
		if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }

#ifdef DX12_ENABLE_DEBUG_LAYER
		IDXGIDebug1* pDebug = nullptr;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
		{
			pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
			pDebug->Release();
		}
#endif
	}

	void CreateRenderTarget()
	{
		for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
		{
			ID3D12Resource* pBackBuffer = nullptr;
			g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
			g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_mainRenderTargetDescriptor[i]);
			g_mainRenderTargetResource[i] = pBackBuffer;
		}
	}

	void CleanupRenderTarget()
	{
		WaitForLastSubmittedFrame();

		for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
			if (g_mainRenderTargetResource[i]) { g_mainRenderTargetResource[i]->Release(); g_mainRenderTargetResource[i] = nullptr; }
	}

	void WaitForLastSubmittedFrame()
	{
		FrameContext* frameCtx = &g_frameContext[g_frameIndex % APP_NUM_FRAMES_IN_FLIGHT];

		UINT64 fenceValue = frameCtx->FenceValue;
		if (fenceValue == 0)
			return; // No fence was signaled

		frameCtx->FenceValue = 0;
		if (g_fence->GetCompletedValue() >= fenceValue)
			return;

		g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
		WaitForSingleObject(g_fenceEvent, INFINITE);
	}

	FrameContext* WaitForNextFrameResources()
	{
		UINT nextFrameIndex = g_frameIndex + 1;
		g_frameIndex = nextFrameIndex;

		HANDLE waitableObjects[] = { g_hSwapChainWaitableObject, nullptr };
		DWORD numWaitableObjects = 1;

		FrameContext* frameCtx = &g_frameContext[nextFrameIndex % APP_NUM_FRAMES_IN_FLIGHT];
		UINT64 fenceValue = frameCtx->FenceValue;
		if (fenceValue != 0) // means no fence was signaled
		{
			frameCtx->FenceValue = 0;
			g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
			waitableObjects[1] = g_fenceEvent;
			numWaitableObjects = 2;
		}

		WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);

		return frameCtx;
	}
}