#include "gui.h"
#include "nfa_client.h"

#include <imgui_internal.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <shellapi.h>

using Microsoft::WRL::ComPtr;

namespace
{
    namespace col
    {
        constexpr ImU32 black   = IM_COL32(0, 0, 0, 255);
        constexpr ImU32 edge    = IM_COL32(60, 60, 60, 255);
        constexpr ImU32 border  = IM_COL32(40, 40, 40, 255);
        constexpr ImU32 outline = IM_COL32(48, 48, 48, 255);
        constexpr ImU32 bg      = IM_COL32(20, 20, 20, 255);
        constexpr ImU32 dots    = IM_COL32(15, 15, 15, 255);
        constexpr ImU32 group   = IM_COL32(23, 23, 23, 255);
        constexpr ImU32 header  = IM_COL32(26, 26, 26, 255);
        constexpr ImU32 panel   = IM_COL32(35, 35, 35, 255);
        constexpr ImU32 text    = IM_COL32(193, 193, 193, 255);
        constexpr ImU32 muted   = IM_COL32(173, 173, 173, 255);
        constexpr ImU32 accent  = IM_COL32(123, 194, 21, 255);
    }

    enum class page { signin, accounts, store, tools, settings };

    struct entry
    {
        std::string text;
        bool highlight = false;
        bool error = false;
    };

    ID3D11Device* device;
    ComPtr<ID3D11ShaderResourceView> pattern, icon;
    ImFont* regular;
    ImFont* bold;

    page current = page::signin;
    nfa_client client;
    char token[65536]{};
    char api_key[1024]{}, steam_path[2048]{}, nickname[256]{};
    char search[128]{}, import_text[262144]{}, replace_order[256]{}, replace_reason[512]{};
    std::vector<entry> status;
    struct account { std::string id, name, persona, token, expires, rating, order, warranty, state; };
    struct product { std::string id, type, name, price; int count; };
    struct cached_account { std::string id, name, persona; };
    struct weapon { unsigned def; std::string name, category; bool t, ct, starter; };
    std::vector<account> accounts;
    std::vector<product> products;
    std::vector<cached_account> cached_accounts;
    std::vector<weapon> weapons;
    std::map<unsigned, unsigned> t_loadout, ct_loadout;
    std::string selected_id, selected_product, selected_cached;
    std::string balance, data_folder, pending_purchase, pending_time;
    std::string inspect_id, inspect_expires, inspect_status;
    bool has_key = false, show_token = false, log_changed = false;
    bool loadout_dirty = false;
    int account_mode = 0, tool_mode = 0;
    std::string modal_action, modal_title, modal_detail;
    std::vector<std::string> modal_args;
    bool open_modal = false;

    template<size_t N> void assign(char (&out)[N], const std::string& value)
    {
        memcpy(out, value.data(), (std::min)(N - 1, value.size()));
        out[(std::min)(N - 1, value.size())] = 0;
    }

    void log(const std::string& message, bool error = false)
    {
        SYSTEMTIME now; GetLocalTime(&now);
        char time[16]; snprintf(time, sizeof(time), "[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
        status.push_back({ time + message, !error, error });
        if (status.size() > 250) status.erase(status.begin());
        log_changed = true;
    }

    void consume_events()
    {
        for (const auto& e : client.poll())
        {
            const auto& f = e.fields;
            if (e.type == "ready" || e.type == "status" || e.type == "error")
            {
                if (!f.empty()) log(f[0], e.type == "error");
            }
            else if (e.type == "history-clear") accounts.clear();
            else if (e.type == "account" && f.size() == 9) accounts.push_back({ f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8] });
            else if (e.type == "stock-clear") products.clear();
            else if (e.type == "stock" && f.size() == 5) products.push_back({ f[0], f[1], f[2], f[3], atoi(f[4].c_str()) });
            else if (e.type == "balance" && !f.empty()) balance = f[0];
            else if (e.type == "settings" && f.size() == 4)
            {
                has_key = f[0] == "1"; assign(steam_path, f[1]); data_folder = f[2]; assign(nickname, f[3]);
                SecureZeroMemory(api_key, sizeof(api_key));
            }
            else if (e.type == "pending" && f.size() == 2) { pending_purchase = f[0]; pending_time = f[1]; }
            else if (e.type == "selected" && !f.empty())
            {
                selected_id = f[0]; current = page::accounts;
                for (const auto& a : accounts) if (a.id == selected_id) assign(token, a.token);
            }
            else if (e.type == "inspection" && f.size() == 4) { inspect_id = f[0]; inspect_expires = f[1]; inspect_status = f[2]; }
            else if (e.type == "cached-clear") cached_accounts.clear();
            else if (e.type == "cached" && f.size() == 3) cached_accounts.push_back({ f[0], f[1], f[2] });
            else if (e.type == "loadout-clear") { weapons.clear(); t_loadout.clear(); ct_loadout.clear(); loadout_dirty = false; }
            else if (e.type == "weapon" && f.size() == 6) weapons.push_back({ static_cast<unsigned>(atoi(f[0].c_str())), f[1], f[2], f[3] == "1", f[4] == "1", f[5] == "1" });
            else if (e.type == "slot" && f.size() == 3) (f[0] == "CT" ? ct_loadout : t_loadout)[atoi(f[1].c_str())] = atoi(f[2].c_str());
        }
    }

    ImTextureID id(const ComPtr<ID3D11ShaderResourceView>& srv)
    {
        return reinterpret_cast<ImTextureID>(srv.Get());
    }

    ComPtr<ID3D11ShaderResourceView> make_texture(const void* pixels, UINT w, UINT h)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{ pixels, w * 4 };
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
            device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
        return srv;
    }

    ComPtr<ID3D11ShaderResourceView> load_texture(const std::filesystem::path& file)
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;

        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))
            || FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
            || FAILED(decoder->GetFrame(0, &frame))
            || FAILED(wic->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return nullptr;

        UINT w, h;
        converter->GetSize(&w, &h);

        std::vector<uint32_t> pixels(w * h);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(pixels.data()))))
            return nullptr;

        return make_texture(pixels.data(), w, h);
    }

    std::filesystem::path asset(const wchar_t* name)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return std::filesystem::path(exe).parent_path() / L"assets" / name;
    }

    void text(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* str, float size = 11.0f)
    {
        dl->AddText(bold, size, pos, color, str);
    }

    void window(ImDrawList* dl, ImVec2 size)
    {
        const struct { float inset; ImU32 color; } layers[] = {
            { 0, col::black }, { 1, col::edge }, { 2, col::border }, { 5, col::edge },
        };

        for (auto [inset, color] : layers)
            dl->AddRectFilled({ inset, inset }, { size.x - inset, size.y - inset }, color);

        ImVec2 min{ 6, 6 }, max = size - min;
        dl->AddImage(id(pattern), min, max, { 0, 0 }, (max - min) / 512.0f);

        constexpr ImU32 left = IM_COL32(59, 175, 222, 255);
        constexpr ImU32 mid = IM_COL32(202, 70, 205, 255);
        constexpr ImU32 right = IM_COL32(201, 227, 58, 255);

        float x = min.x + 1, y = min.y + 1, half = (max.x - min.x - 2) * 0.5f;
        dl->AddRectFilledMultiColor({ x, y }, { x + half, y + 2 }, left, mid, mid, left);
        dl->AddRectFilledMultiColor({ x + half, y }, { max.x - 1, y + 2 }, mid, right, right, mid);
        dl->AddRectFilled({ x, y + 1 }, { max.x - 1, y + 2 }, IM_COL32(0, 0, 0, 120));
        dl->AddRectFilled({ x, y + 2 }, { max.x - 1, y + 3 }, IM_COL32(8, 8, 8, 255));
    }

    void box(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 fill, const char* label = nullptr)
    {
        ImVec2 end = pos + size;
        dl->AddRectFilled(pos, end, fill);
        dl->AddRect(pos, end, col::black);
        dl->AddRect(pos + ImVec2(1, 1), end - ImVec2(1, 1), col::outline);

        if (!label)
            return;

        float w = bold->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, label).x;
        dl->AddRectFilled({ pos.x + 10, pos.y }, { pos.x + 17 + w, pos.y + 2 }, col::dots);
        text(dl, { pos.x + 14, pos.y - 4 }, col::text, label);
    }

    bool button(const char* label, ImVec2 pos, ImVec2 size, float font_size = 11.0f)
    {
        ImGui::SetCursorPos(pos);
        bool pressed = ImGui::InvisibleButton(label, size);
        bool held = ImGui::IsItemActive();
        bool hovered = ImGui::IsItemHovered();

        ImU32 top = held ? IM_COL32(25, 25, 25, 255) : hovered ? IM_COL32(43, 43, 43, 255) : IM_COL32(35, 35, 35, 255);
        ImU32 bottom = held ? IM_COL32(35, 35, 35, 255) : hovered ? IM_COL32(31, 31, 31, 255) : IM_COL32(25, 25, 25, 255);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        dl->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
        dl->AddRect(min, max, col::black);
        dl->AddRect(min + ImVec2(1, 1), max - ImVec2(1, 1), col::outline);

        ImVec2 ts = bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
        text(dl, ImTrunc(min + (size - ts) * 0.5f + ImVec2(0.5f, 0.5f)), ImGui::GetStyle().Alpha < 1.0f ? IM_COL32(90,90,90,255) : col::text, label, font_size);
        return pressed;
    }

    void caret(ImVec2 frame)
    {
        ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID());
        if (!state || !ImGui::IsItemActive())
            return;

        if (state->CursorAnim > 0.0f && std::fmod(state->CursorAnim, 1.2f) > 0.8f)
            return;

        const char* str = state->TextA.Data;
        int cursor = state->GetCursorPos();
        float offset = state->Flags & ImGuiInputTextFlags_Password
            ? ImGui::CalcTextSize("*").x * ImTextCountCharsFromUtf8(str, str + cursor)
            : ImGui::CalcTextSize(str, str + cursor).x;

        float x = ImTrunc(frame.x + ImGui::GetStyle().FramePadding.x + offset - state->Scroll.x) + (offset > 0 ? 1 : -3);
        float y = frame.y + 4;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({ x - 1, y + 1 }, { x, y + 11 }, IM_COL32(17, 17, 17, 255));
        dl->AddRectFilled({ x, y }, { x + 1, y + 12 }, IM_COL32(17, 17, 17, 255));
        dl->AddRectFilled({ x, y + 1 }, { x + 1, y + 11 }, IM_COL32(87, 137, 17, 255));
        dl->AddRectFilled({ x + 1, y + 1 }, { x + 2, y + 11 }, IM_COL32(61, 89, 22, 255));
    }

    bool field(const char* id, const char* hint, char* buf, size_t size, ImVec2 pos, ImGuiInputTextFlags flags = 0)
    {
        ImGui::PushFont(regular, 10.0f);
        ImGui::SetCursorPos(pos);
        ImGui::SetNextItemWidth(123);
        bool entered = ImGui::InputTextWithHint(id, hint, buf, size, flags | ImGuiInputTextFlags_EnterReturnsTrue);
        caret(ImGui::GetItemRectMin());
        ImGui::PopFont();
        return entered;
    }

    bool action(const char* label, float width = 120)
    {
        return button(label, ImGui::GetCursorPos(), { width, 25 });
    }

    void note(const std::string& value, bool accent = false)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(accent ? col::accent : col::muted).Value);
        ImGui::TextWrapped("%s", value.c_str());
        ImGui::PopStyleColor();
    }

    void confirm(std::string command, std::string title, std::string detail, std::vector<std::string> args)
    {
        modal_action = std::move(command); modal_title = std::move(title); modal_detail = std::move(detail);
        modal_args = std::move(args); open_modal = true;
    }

    void send(const std::string& command, const std::vector<std::string>& args = {})
    {
        if (client.send(command, args)) log("Working: " + command + "...");
        else log(client.busy() ? "Another operation is still running." : "NFA services unavailable. Restart the tool.", true);
    }

    void token_changed()
    {
        inspect_id.clear(); inspect_expires.clear(); inspect_status.clear();
    }

    void sign_in_page()
    {
        note("Steam sign-in", true);
        note("Paste a login token or a complete steamid----token line.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##token", "Login token", token, sizeof(token), show_token ? 0 : ImGuiInputTextFlags_Password)) token_changed();
        ImGui::Checkbox("Show token", &show_token);
        if (action("Inspect locally", 116)) send("inspect", { token });
        ImGui::SameLine();
        if (action("Verify with Steam", 136)) send("verify", { token });
        ImGui::SameLine();
        if (action("Sign in to Steam", 140)) confirm("login", "Sign in to Steam", "Steam will close and restart using this account. The account will be saved to History.", { token });
        ImGui::SameLine();
        if (action("Clear", 76)) { SecureZeroMemory(token, sizeof(token)); token_changed(); }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        note("Steam ID: " + (inspect_id.empty() ? std::string("--") : inspect_id));
        note("Expires: " + (inspect_expires.empty() ? std::string("--") : inspect_expires));
        note("Status: " + (inspect_status.empty() ? std::string("Not checked") : inspect_status));
        ImGui::Spacing();
        note("Accounts contains saved tokens and remembered Steam logins. Tools uses the token in this field.");
    }

    const account* selected_account()
    {
        for (const auto& a : accounts) if (a.id == selected_id) return &a;
        return nullptr;
    }

    void accounts_page()
    {
        if (ImGui::RadioButton("History", account_mode == 0)) account_mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Remembered Steam logins", account_mode == 1)) { account_mode = 1; send("cached"); }
        ImGui::SameLine();
        if (action("Refresh", 80)) send(account_mode ? "cached" : "history");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search account, nickname or Steam ID", search, sizeof(search));
        ImGui::BeginChild("account-list", { 342, 176 }, ImGuiChildFlags_Borders);
        auto matches = [](const std::string& value)
        {
            std::string hay = value, needle = search;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
            return hay.find(needle) != std::string::npos;
        };
        if (!account_mode)
        {
            if (accounts.empty()) note("No saved accounts. Import tokens below.");
            for (const auto& a : accounts)
            {
                if (!matches(a.name + " " + a.persona + " " + a.id)) continue;
                ImGui::PushID(a.id.c_str());
                std::string label = (a.persona.empty() ? a.name : a.persona) + " / " + a.id;
                if (ImGui::Selectable(label.c_str(), selected_id == a.id)) selected_id = a.id;
                ImGui::PopID();
            }
        }
        else
        {
            if (cached_accounts.empty()) note("No remembered logins. Sign in once using Steam.");
            for (const auto& a : cached_accounts)
            {
                if (!matches(a.name + " " + a.persona + " " + a.id)) continue;
                if (ImGui::Selectable((a.name + "##" + a.id).c_str(), selected_cached == a.id)) selected_cached = a.id;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (!account_mode)
        {
            if (auto a = selected_account())
            {
                note(a->name, true); note(a->id);
                note("Expires: " + (a->expires.empty() ? "Unknown" : a->expires));
                note("Premier: " + (a->rating.empty() ? "Not queried" : a->rating));
                if (!a->warranty.empty()) note(a->warranty);
                if (action("Use token", 106)) { assign(token, a->token); token_changed(); current = page::signin; }
                ImGui::SameLine();
                if (action("Query CS2", 106)) send("query", { a->token });
                if (action("Replace", 106))
                {
                    assign(replace_order, a->order); replace_reason[0] = 0;
                    confirm("replace", "Replace account", "Request a replacement from nfa.pub for " + a->id + ". A working account uses no replacement.", { a->id });
                }
                ImGui::SameLine();
                if (action("Remove", 106)) confirm("delete", "Remove saved account", "Remove " + a->id + " from local History?", { a->id });
            }
            else note("Select an account to use its token, query CS2 or request a replacement.");
        }
        else
        {
            note("Restore a remembered Steam login.");
            note("Steam will close and restart. Steam may ask for authentication if its saved login has expired.");
            ImGui::BeginDisabled(selected_cached.empty());
            if (action("Restore login", 160)) confirm("restore", "Restore Steam login", "Restart Steam with the selected remembered account?", { selected_cached });
            ImGui::EndDisabled();
        }
        ImGui::EndGroup();
        if (!account_mode && action("Import tokens", 126))
        {
            import_text[0] = 0;
            confirm("import", "Import accounts", "One token or steamid----token line per account. All lines are checked before saving.", {});
        }
    }

    void store_page()
    {
        note("nfa.pub / CS2 accounts", true);
        ImGui::SameLine(300); note("Balance: " + (balance.empty() ? std::string("--") : "EUR " + balance));
        if (action("Refresh stock", 124)) send("stock");
        ImGui::SameLine(); note(has_key ? "API key saved" : "Save your API key in Settings to buy.");
        ImGui::BeginChild("stock-list", { 0, 139 }, ImGuiChildFlags_Borders);
        if (products.empty()) note("Click Refresh stock to fetch products, prices and availability.");
        for (const auto& p : products)
        {
            std::string label = p.name + " / EUR " + p.price + " / " + std::to_string(p.count) + " available";
            if (ImGui::Selectable((label + "##" + p.id).c_str(), selected_product == p.id)) selected_product = p.id;
        }
        ImGui::EndChild();
        auto item = std::find_if(products.begin(), products.end(), [](const product& p) { return p.id == selected_product; });
        ImGui::BeginDisabled(!has_key || item == products.end() || (item != products.end() && item->count < 1) || !pending_purchase.empty());
        if (action("Buy one account", 146) && item != products.end())
            confirm("buy", "Confirm purchase", "Buy 1 x " + item->name + " for EUR " + item->price + " from your nfa.pub balance?", { item->id, item->price });
        ImGui::EndDisabled();
        if (!pending_purchase.empty())
        {
            note("Pending: " + pending_purchase + " / " + pending_time, true);
            if (action("Finish pending purchase", 190)) confirm("finish-purchase", "Recover pending purchase", "Retry the original request with its saved idempotency key. This recovers the original order.", {});
            ImGui::SameLine();
            if (action("Clear resolved record", 178)) confirm("clear-pending", "Clear pending purchase", "Only clear this record AFTER checking the order at nfa.pub and recovering any delivered account. Clearing it removes duplicate-charge protection for that request.", {});
        }
    }

    bool weapon_fits(const weapon& w, unsigned slot, bool ct)
    {
        if (!(ct ? w.ct : w.t)) return false;
        if (slot == 2) return w.starter;
        if (slot >= 3 && slot <= 6) return w.category == "Pistol" && !w.starter;
        if (slot >= 8 && slot <= 12) return w.category == "Mid";
        return w.category == "Rifle";
    }

    std::string loadout_value()
    {
        std::string value;
        for (bool ct : { false, true })
            for (auto [slot, def] : ct ? ct_loadout : t_loadout)
                value += std::string(ct ? "CT," : "T,") + std::to_string(slot) + "," + std::to_string(def) + ";";
        return value;
    }

    void loadout_editor()
    {
        if (action("Save preset", 110)) send("save-loadout", { loadout_value() });
        ImGui::SameLine();
        ImGui::BeginDisabled(loadout_dirty);
        if (action("Apply to token", 130)) confirm("loadout", "Apply loadout", "Apply the saved T and CT loadouts to the token on the Sign in tab? Close CS2 first.", { token });
        ImGui::EndDisabled();
        ImGui::SameLine(); note(loadout_dirty ? "Unsaved changes" : "Saved preset");
        if (weapons.empty()) { note("Waiting for the weapon catalog..."); return; }
        if (ImGui::BeginTable("teams", 2, ImGuiTableFlags_SizingStretchSame))
        {
            for (bool ct : { false, true })
            {
                ImGui::TableNextColumn(); ImGui::PushID(ct ? 1 : 0);
                note(ct ? "Counter-Terrorists" : "Terrorists", true);
                auto& slots = ct ? ct_loadout : t_loadout;
                for (unsigned slot : { 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 11u, 12u, 14u, 15u, 16u, 17u, 18u })
                {
                    ImGui::PushID(static_cast<int>(slot));
                    const weapon* selected = nullptr;
                    for (const auto& w : weapons) if (w.def == slots[slot]) selected = &w;
                    ImGui::SetNextItemWidth(190);
                    if (ImGui::BeginCombo("##weapon", selected ? selected->name.c_str() : "Choose weapon"))
                    {
                        for (const auto& w : weapons) if (weapon_fits(w, slot, ct))
                        {
                            bool duplicate = false;
                            for (auto [other_slot, def] : slots) if (other_slot != slot && def == w.def) duplicate = true;
                            ImGui::BeginDisabled(duplicate);
                            if (ImGui::Selectable(w.name.c_str(), w.def == slots[slot])) { slots[slot] = w.def; loadout_dirty = true; }
                            ImGui::EndDisabled();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s %u", slot < 8 ? "Pistol" : slot < 14 ? "Mid" : "Rifle", slot < 8 ? slot - 1 : slot < 14 ? slot - 7 : slot - 13);
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void tools_page()
    {
        const char* tabs[] = { "Loadout", "Profile", "Workshop" };
        for (int i = 0; i < 3; ++i)
        {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(tabs[i], tool_mode == i)) tool_mode = i;
        }
        note("Target: token currently entered on the Sign in tab."); ImGui::Separator(); ImGui::Spacing();
        if (tool_mode == 0) loadout_editor();
        else if (tool_mode == 1)
        {
            note("Steam nickname", true);
            ImGui::SetNextItemWidth(300);
            ImGui::InputTextWithHint("##nickname", "Nickname", nickname, sizeof(nickname));
            if (action("Apply nickname", 140)) confirm("profile", "Update Steam profile", "Set the current account's Steam nickname to " + std::string(nickname) + "?", { token, nickname });
        }
        else
        {
            note("Clear Workshop subscriptions", true);
            note("Unsubscribe the current account from all CS2 and Wallpaper Engine Workshop items.");
            ImGui::Spacing();
            if (action("Clear Workshop", 160)) confirm("workshop", "Clear Workshop subscriptions", "Unsubscribe ALL CS2 and Wallpaper Engine Workshop items for the account on the Sign in tab?", { token });
        }
    }

    void settings_page()
    {
        note("nfa.pub API key", true);
        note(has_key ? "A key is saved. Leave this field blank to keep it." : "No API key saved.");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##api-key", "API key", api_key, sizeof(api_key), ImGuiInputTextFlags_Password);
        note("Steam folder (leave blank for automatic detection)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##steam-path", "Folder containing steam.exe", steam_path, sizeof(steam_path));
        if (action("Save settings", 124)) send("save-settings", { api_key, steam_path });
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_key);
        if (action("Remove API key", 132)) confirm("remove-key", "Remove API key", "Remove the saved nfa.pub API key from this PC?", {});
        ImGui::EndDisabled();
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        note("Data: " + data_folder);
        note("Uses the original NFA history and settings. The API key stays encrypted with Windows DPAPI.");
        if (action("Open data folder", 144) && !data_folder.empty())
        {
            int length = MultiByteToWideChar(CP_UTF8, 0, data_folder.c_str(), -1, nullptr, 0);
            std::wstring path(length, 0);
            MultiByteToWideChar(CP_UTF8, 0, data_folder.c_str(), -1, path.data(), length);
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void confirmation_modal()
    {
        if (open_modal) { ImGui::OpenPopup("Confirm action"); open_modal = false; }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, { .5f, .5f });
        ImGui::SetNextWindowSize({ 490, 0 }, ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 16, 16 });
        if (ImGui::BeginPopupModal("Confirm action", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            note(modal_title, true); ImGui::Spacing(); note(modal_detail); ImGui::Spacing();
            if (modal_action == "import")
            {
                ImGui::InputTextMultiline("##import", import_text, sizeof(import_text), { 452, 130 });
            }
            if (modal_action == "replace")
            {
                ImGui::SetNextItemWidth(452); ImGui::InputTextWithHint("##order", "Order ID", replace_order, sizeof(replace_order));
                ImGui::SetNextItemWidth(452); ImGui::InputTextWithHint("##reason", "Reason (optional)", replace_reason, sizeof(replace_reason));
            }
            ImGui::BeginDisabled(client.busy());
            if (action("Confirm", 116))
            {
                if (modal_action == "import") modal_args = { import_text };
                if (modal_action == "replace") { modal_args.resize(1); modal_args.push_back(replace_order); modal_args.push_back(replace_reason); }
                send(modal_action, modal_args);
                modal_args.clear(); SecureZeroMemory(import_text, sizeof(import_text));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (action("Cancel", 116)) { modal_args.clear(); SecureZeroMemory(import_text, sizeof(import_text)); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
}

void gui::init(ID3D11Device* dev)
{
    device = dev;

    std::vector<uint32_t> pixels(512 * 512);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x)
            pixels[y * 512 + x] = x % 2 == 0 && y % 4 != x % 4 ? col::dots : col::bg;

    pattern = make_texture(pixels.data(), 512, 512);
    // The NFA interface draws its branding; it needs no adjacent asset files.

    ImFontConfig cfg;
    cfg.GlyphOffset.y = -1;

    ImGuiIO& io = ImGui::GetIO();
    regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 10.0f, &cfg);
    bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdanab.ttf", 11.0f, &cfg);
    if (!regular) regular = io.Fonts->AddFontDefault();
    if (!bold) bold = regular;
    io.FontDefault = bold;

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = { 0, 0 };
    style.WindowBorderSize = 0;
    style.FrameBorderSize = 1;
    style.FramePadding = { 6, 5 };
    style.AntiAliasedLines = false;
    style.AntiAliasedFill = false;
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = style.ScrollbarRounding = 0;
    style.ItemSpacing = { 8, 8 };
    style.ScrollbarSize = 10;
    style.ChildBorderSize = 1;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImColor(col::text);
    c[ImGuiCol_TextDisabled] = ImColor(85, 85, 85);
    c[ImGuiCol_FrameBg] = c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_FrameBgActive] = ImColor(34, 34, 34);
    c[ImGuiCol_Border] = ImColor(col::black);
    c[ImGuiCol_TextSelectedBg] = ImColor(87, 137, 17, 110);
    c[ImGuiCol_InputTextCursor] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_NavCursor] = ImColor(col::accent);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = ImColor(col::bg);
    c[ImGuiCol_ChildBg] = ImColor(23, 23, 23);
    c[ImGuiCol_Header] = ImColor(52, 65, 34);
    c[ImGuiCol_HeaderHovered] = ImColor(48, 48, 48);
    c[ImGuiCol_HeaderActive] = ImColor(58, 70, 38);
    c[ImGuiCol_CheckMark] = ImColor(col::accent);
    c[ImGuiCol_Button] = ImColor(col::panel);
    c[ImGuiCol_ButtonHovered] = ImColor(48, 48, 48);
    c[ImGuiCol_ScrollbarBg] = ImColor(20, 20, 20);
    c[ImGuiCol_ScrollbarGrab] = ImColor(60, 60, 60);
    log("Starting NFA services...");
    if (!client.start()) log("Backend startup failed. Build with Build.ps1 and run dist/JazzNfa.exe.", true);
}

ImVec2 gui::size()
{
    return { 760, 560 };
}

bool gui::busy() { return client.busy(); }

bool gui::render()
{
    consume_events();
    ImVec2 size = gui::size();
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##loader", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
                                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    window(dl, size);
    text(dl, { 23, 23 }, col::text, "jazz", 14);
    text(dl, { 53, 23 }, col::accent, "nfa", 14);
    text(dl, { 92, 25 }, col::muted, "Steam account tool / 0.5.5", 10);
    if (button("_", { 670, 20 }, { 26, 23 })) ShowWindow(static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw), SW_MINIMIZE);
    ImGui::BeginDisabled(client.busy());
    bool open = !button("X", { 704, 20 }, { 32, 23 });
    ImGui::EndDisabled();
    const char* tabs[] = { "Sign in", "Accounts", "Store", "Tools", "Settings" };
    for (int i = 0; i < 5; ++i)
    {
        float x = 21.0f + i * 145.0f;
        if (button(tabs[i], { x, 51 }, { 137, 24 })) current = static_cast<page>(i);
        if (static_cast<int>(current) == i) dl->AddRectFilled({ x + 1, 74 }, { x + 136, 76 }, col::accent);
    }
    box(dl, { 21, 87 }, { 715, 294 }, col::group);
    ImGui::SetCursorPos({ 33, 98 });
    ImGui::BeginChild("content", { 692, 272 });
    ImGui::BeginDisabled(client.busy() || !client.connected());
    switch (current)
    {
    case page::signin: sign_in_page(); break;
    case page::accounts: accounts_page(); break;
    case page::store: store_page(); break;
    case page::tools: tools_page(); break;
    case page::settings: settings_page(); break;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    box(dl, { 21, 395 }, { 715, 136 }, col::group, "Status");
    box(dl, { 31, 408 }, { 695, 112 }, col::panel);
    ImGui::SetCursorPos({ 39, 415 });
    ImGui::BeginChild("status-log", { 678, 98 });
    for (const auto& item : status)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(item.error ? IM_COL32(225, 125, 115, 255) : item.highlight ? col::accent : col::muted).Value);
        ImGui::TextWrapped("%s", item.text.c_str());
        ImGui::PopStyleColor();
    }
    if (log_changed) { ImGui::SetScrollHereY(1.0f); log_changed = false; }
    ImGui::EndChild();
    text(dl, { 23, 540 }, col::muted, client.busy() ? "Working... keep the tool open" : client.connected() ? "Ready" : "Services disconnected", 10);
    text(dl, { 515, 540 }, col::muted, "ImGui / DirectX 11 / source build", 10);
    confirmation_modal();

    ImGui::End();
    return open;
}
