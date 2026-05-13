#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include "UserGUI.h"
#include "../network/headers/Node.h"
#include <deque>
#include <unordered_set>
#include <ctime>

using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

struct UserGUI::Impl {
    std::shared_ptr<Node>          node;
    HWND                           hwnd = nullptr;
    WNDCLASSEXW                    wc{};
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain>         swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;

    NodeSnapshot            snap;
    std::deque<ChatMessage> chat_history;
    char                    input_buf[1024]{};
    int                     selected_dst  = 0;
    bool                    scroll_bottom = false;
    bool                    done          = false;

    explicit Impl(std::shared_ptr<Node> n) : node(std::move(n)) {}

    // DX11

    void create_device_and_swapchain() {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount       = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow      = hwnd;
        sd.SampleDesc.Count  = 1;
        sd.Windowed          = TRUE;
        sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
        sd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        D3D_FEATURE_LEVEL fl;
        D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            fls, 2, D3D11_SDK_VERSION, &sd, &swapchain, &device, &fl, &ctx);
    }

    void create_rtv() {
        ComPtr<ID3D11Texture2D> back;
        swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
        device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    }

    void cleanup_rtv() { rtv.Reset(); }

    void resize_swapchain(UINT w, UINT h) {
        cleanup_rtv();
        swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        create_rtv();
    }

    // panels

    void render_node_info_bar() {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({io.DisplaySize.x, 40});
        ImGui::Begin("##bar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::Text("ID: %.12s  |  Level: %d  |  %s  |  Parent: %.12s",
            snap.node_id.c_str(), snap.level,
            snap.is_root ? "ROOT" : "node",
            snap.daddy.empty() ? "none" : snap.daddy.c_str());
        ImGui::End();
    }

    void render_peer_list() {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 40});
        ImGui::SetNextWindowSize({300, io.DisplaySize.y - 40});
        ImGui::Begin("Peers", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Peers with an active or in-progress circuit are hidden
        std::unordered_set<std::string> busy;
        for (const auto& ci : snap.circuits)
            if (ci.state == CircuitState::READY || ci.state == CircuitState::INITIATING)
                busy.insert(ci.dst);

        size_t available = 0;
        for (const auto& pid : snap.known_peers)
            if (pid != snap.node_id && !busy.count(pid)) available++;

        if (snap.level >= 0) ImGui::BeginDisabled();
        if (ImGui::Button("Register"))
            asio::post(node->io(), [n = node]{ n->handle_command("register"); });
        ImGui::SameLine();
        if (ImGui::Button("Become Root"))
            asio::post(node->io(), [n = node]{ n->handle_command("root"); });
        if (snap.level >= 0) ImGui::EndDisabled();
        ImGui::Separator();

        ImGui::Text("Available (%zu):", available);
        ImGui::Separator();
        for (const auto& pid : snap.known_peers) {
            if (pid == snap.node_id) continue;
            if (busy.count(pid))    continue;
            ImGui::Text("%.12s", pid.c_str());
            ImGui::SameLine();
            std::string btn = "Connect##" + pid;
            if (ImGui::Button(btn.c_str())) {
                std::string cmd = "circuit " + pid;
                asio::post(node->io(), [n = node, cmd]{ n->handle_command(cmd); });
            }
        }
        ImGui::End();
    }

    void render_circuit_status() {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({io.DisplaySize.x - 300, io.DisplaySize.y - 200});
        ImGui::SetNextWindowSize({300, 200});
        ImGui::Begin("Circuits", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (snap.circuits.empty()) {
            ImGui::TextDisabled("No active circuits");
        } else {
            for (const auto& ci : snap.circuits) {
                const char* label =
                    ci.state == CircuitState::READY      ? "READY"    :
                    ci.state == CircuitState::INITIATING ? "BUILDING" : "FAILED";
                ImVec4 col =
                    ci.state == CircuitState::READY      ? ImVec4(0.2f,0.9f,0.2f,1) :
                    ci.state == CircuitState::INITIATING ? ImVec4(0.9f,0.9f,0.2f,1) :
                                                           ImVec4(0.9f,0.2f,0.2f,1);
                ImGui::TextColored(col, "[%s]", label);
                ImGui::SameLine();
                ImGui::Text("-> %.12s", ci.dst.c_str());
            }
        }
        ImGui::End();
    }

    void render_chat_window() {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({300, 40});
        ImGui::SetNextWindowSize({io.DisplaySize.x - 300, io.DisplaySize.y - 40});
        ImGui::Begin("Chat", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Collect READY destinations for recipient picker
        std::vector<std::string> ready_dsts;
        for (const auto& ci : snap.circuits)
            if (ci.state == CircuitState::READY)
                ready_dsts.push_back(ci.dst);
        if (selected_dst >= static_cast<int>(ready_dsts.size()))
            selected_dst = 0;

        // Scrollable message history
        float footer_h = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
        ImGui::BeginChild("##history", {0, -footer_h}, true);
        for (const auto& msg : chat_history) {
            if (ready_dsts.empty()) break;
            const std::string& peer = ready_dsts[selected_dst];
            bool relevant = (msg.from == snap.node_id && msg.to == peer)
                         || (msg.from == peer);
            if (!relevant || msg.conn_req) continue;
            auto tt = std::chrono::system_clock::to_time_t(msg.when);
            std::tm tm{};
            localtime_s(&tm, &tt);
            char ts[6];
            std::strftime(ts, sizeof(ts), "%H:%M", &tm);
            ImGui::TextColored({0.5f,0.8f,1.0f,1.0f}, "[%s] %.12s:", ts, msg.from.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", msg.text.c_str());
        }
        if (scroll_bottom) { ImGui::SetScrollHereY(1.0f); scroll_bottom = false; }
        ImGui::EndChild();

        // Recipient combo
        ImGui::Text("To:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        if (ready_dsts.empty()) {
            ImGui::TextDisabled("(no ready circuits)");
        } else {
            std::string preview = ready_dsts[selected_dst].substr(0, 12);
            if (ImGui::BeginCombo("##to", preview.c_str())) {
                for (int i = 0; i < static_cast<int>(ready_dsts.size()); i++) {
                    std::string label = ready_dsts[i].substr(0, 12);
                    bool sel = (i == selected_dst);
                    if (ImGui::Selectable(label.c_str(), sel)) selected_dst = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        // Text input + Send button
        ImGui::SetNextItemWidth(-80);
        bool send = ImGui::InputText("##msg", input_buf, sizeof(input_buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        send |= ImGui::Button("Send", {75, 0});
        if (send && input_buf[0] && !ready_dsts.empty()) {
            std::string cmd = "sanon " + ready_dsts[selected_dst] + " " + input_buf;
            asio::post(node->io(), [n = node, cmd]{ n->handle_command(cmd); });
            input_buf[0] = '\0';
        }
        ImGui::End();
    }

    void render_frame() {
        render_node_info_bar();
        render_peer_list();
        render_circuit_status();
        render_chat_window();
    }

    // WIN32 msg proc

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
        auto* im = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_SIZE:
                if (im && im->device && wp != SIZE_MINIMIZED)
                    im->resize_swapchain(LOWORD(lp), HIWORD(lp));
                return 0;
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_APP + 2:
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
};

// GUI

UserGUI::UserGUI(std::shared_ptr<Node> node)
    : impl_(std::make_unique<Impl>(std::move(node))) {}

UserGUI::~UserGUI() = default;

void UserGUI::run() {
    Impl& im = *impl_;

    im.wc               = {};
    im.wc.cbSize        = sizeof(WNDCLASSEXW);
    im.wc.style         = CS_CLASSDC;
    im.wc.lpfnWndProc   = Impl::WndProc;
    im.wc.hInstance     = GetModuleHandleW(nullptr);
    im.wc.lpszClassName = L"TrojanGUI";
    RegisterClassExW(&im.wc);

    im.hwnd = CreateWindowExW(0, L"TrojanGUI", L"Trojan Message",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        nullptr, nullptr, im.wc.hInstance, nullptr);
    SetWindowLongPtrW(im.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&im));

    im.create_device_and_swapchain();
    im.create_rtv();
    ShowWindow(im.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(im.hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(im.hwnd);
    ImGui_ImplDX11_Init(im.device.Get(), im.ctx.Get());

    while (!im.done) {
        // When hidden, block until a message arrives — near-zero CPU
        if (!IsWindowVisible(im.hwnd))
            WaitMessage();

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) im.done = true;
        }
        if (im.done || !IsWindowVisible(im.hwnd)) continue;

        im.snap = im.node->get_snapshot();
        for (auto& m : im.node->take_new_messages()) {
            im.chat_history.push_back(std::move(m));
            im.scroll_bottom = true;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        im.render_frame();
        ImGui::Render();

        constexpr float bg[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        im.ctx->OMSetRenderTargets(1, im.rtv.GetAddressOf(), nullptr);
        im.ctx->ClearRenderTargetView(im.rtv.Get(), bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        im.swapchain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    im.rtv.Reset();
    im.swapchain.Reset();
    im.ctx.Reset();
    im.device.Reset();
    DestroyWindow(im.hwnd);
    UnregisterClassW(im.wc.lpszClassName, im.wc.hInstance);
}
