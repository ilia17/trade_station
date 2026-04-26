#include "render.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_sdl2.h>
#include <imgui/backends/imgui_impl_opengl3.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <string>
#include <sstream>
#include <iomanip>

// ── Global symbol state (base/quote strings) ──────────────────────────────────
static char g_base[16]  = "BTC";
static char g_quote[16] = "USDT";

// ── Formatting helpers ────────────────────────────────────────────────────────

static std::string fmt_commas(double v, int decimals) {
    bool neg = v < 0; if (neg) v = -v;
    long long int_part = (long long)v;
    double frac = v - (double)int_part;
    std::string s = std::to_string(int_part);
    int ins = (int)s.size() - 3;
    while (ins > 0) { s.insert(ins, ","); ins -= 3; }
    if (decimals > 0) {
        char dec[32];
        snprintf(dec, sizeof(dec), "%.*f", decimals, frac);
        s += (dec + 1);
    }
    return neg ? "-" + s : s;
}

static std::string fmt_price(double p) {
    if (p >= 1000.0) return fmt_commas(p, 2);
    if (p >= 1.0)    return fmt_commas(p, 4);
    return fmt_commas(p, 4);
}
static std::string fmt_qty(double q) {
    if (q >= 1000.0) return fmt_commas(q, 2);
    if (q >= 1.0)    return fmt_commas(q, 4);
    return fmt_commas(q, 4);
}
static std::string fmt_total(double t) { return fmt_commas(t, 2); }

// ── Per-panel order form state ────────────────────────────────────────────────

static OrderFormState g_forms[4];

// ── Order book table ──────────────────────────────────────────────────────────

static void draw_book_table(int idx, const OrderBookUpdate& ex,
                            OrderFormState& f, bool is_asks) {
    ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                          ImGuiTableFlags_SizingFixedFit;
    char tid[32]; snprintf(tid, sizeof(tid), "##tbl%s%d", is_asks ? "a" : "b", idx);
    if (!ImGui::BeginTable(tid, 3, tfl, ImVec2(-1, 0))) return;

    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);

    int count = is_asks ? ex.ask_count : ex.bid_count;
    for (int r = 0; r < count; r++) {
        int    i = is_asks ? (ex.ask_count - 1 - r) : r;
        double p = is_asks ? ex.asks[i].price    : ex.bids[i].price;
        double q = is_asks ? ex.asks[i].quantity : ex.bids[i].quantity;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        char sel_id[32];
        snprintf(sel_id, sizeof(sel_id), "##sel%s%d_%d",
                 is_asks ? "a" : "b", idx, i);
        if (ImGui::Selectable(sel_id, false,
                ImGuiSelectableFlags_SpanAllColumns,
                ImVec2(0, ImGui::GetTextLineHeightWithSpacing()))) {
            snprintf(f.price_buf, sizeof(f.price_buf), "%.8g", p);
            snprintf(f.qty_buf,   sizeof(f.qty_buf),   "%.8g", q);  // auto-fill qty
            f.side = is_asks ? 1 : 0;
        }
        ImGui::SameLine(0, 0);
        ImGui::Text("%s", fmt_price(p).c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s", fmt_qty(q).c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", fmt_total(p * q).c_str());
    }
    ImGui::EndTable();
}

// ── Recent trades table ───────────────────────────────────────────────────────

static void draw_trades_table(int idx, const OrderBookUpdate& ex,
                              SharedTrades& shared_trades) {
    Trade  t_snap[SharedTrades::MAX]{};
    int    t_count = 0;
    shared_trades.snapshot(ex.exchange, t_snap, t_count);

    ImGui::SeparatorText("Trades");

    ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_ScrollY;
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float tbl_h = row_h * 10.f + ImGui::GetStyle().ScrollbarSize;

    char tid[32]; snprintf(tid, sizeof(tid), "##tr%d", idx);
    if (!ImGui::BeginTable(tid, 3, tfl, ImVec2(-1, tbl_h))) return;

    ImGui::TableSetupColumn("Price",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Side",   ImGuiTableColumnFlags_WidthFixed,   36.f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < t_count; i++) {
        const Trade& t = t_snap[i];
        ImGui::TableNextRow();

        ImVec4 col = t.is_buy
            ? ImVec4(0.35f, 0.85f, 0.45f, 1.f)   // green = buy
            : ImVec4(0.9f,  0.35f, 0.35f, 1.f);   // red   = sell

        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(col, "%s", fmt_price(t.price).c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(col, "%s", fmt_qty(t.qty).c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(col, "%s", t.is_buy ? "BUY" : "SELL");
    }
    if (t_count == 0)
        ImGui::TextDisabled("  waiting...");

    ImGui::EndTable();
}

// ── Order form ────────────────────────────────────────────────────────────────

static void draw_order_form(int idx, const OrderBookUpdate& ex,
                            TradeManager& trader) {
    OrderFormState& f = g_forms[idx];

    // Poll pending future (non-blocking)
    if (f.pending.valid() &&
        f.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        OrderResult r = f.pending.get();
        f.status     = r.success ? OrderStatus::OK : OrderStatus::ERROR;
        f.status_msg = r.success ? ("ID: " + r.order_id) : r.message;
        if (r.success && !r.order_id.empty()) {
            PlacedOrder po;
            po.order_id = r.order_id;
            po.symbol   = ex.symbol;
            po.side     = f.side == 0 ? Side::BUY : Side::SELL;
            try { po.price = std::stod(f.price_buf); } catch (...) {}
            try { po.qty   = std::stod(f.qty_buf);   } catch (...) {}
            if (f.orders.size() >= 10)
                f.orders.erase(f.orders.begin());
            f.orders.push_back(std::move(po));
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Order");

    bool has_keys = trader.has_keys(ex.exchange);

    // BUY / SELL toggle
    ImGui::PushStyleColor(ImGuiCol_Button, f.side == 0
        ? ImVec4(0.15f,0.55f,0.2f,1.f) : ImVec4(0.22f,0.22f,0.25f,1.f));
    if (ImGui::Button("BUY##side"))  f.side = 0;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, f.side == 1
        ? ImVec4(0.65f,0.15f,0.15f,1.f) : ImVec4(0.22f,0.22f,0.25f,1.f));
    if (ImGui::Button("SELL##side")) f.side = 1;
    ImGui::PopStyleColor();

    float lbl_w = ImGui::CalcTextSize("Price (XXXX)  ").x;

    // Price label with quote currency
    char price_lbl[32]; snprintf(price_lbl, sizeof(price_lbl), "Price (%s)", g_quote);
    ImGui::Text("%s", price_lbl); ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##price", f.price_buf, sizeof(f.price_buf));

    // Qty label with base currency
    char qty_lbl[32]; snprintf(qty_lbl, sizeof(qty_lbl), "Qty   (%s)", g_base);
    ImGui::Text("%s", qty_lbl); ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##qty", f.qty_buf, sizeof(f.qty_buf));

    // Total
    double p = 0, q = 0;
    try { p = std::stod(f.price_buf); } catch (...) {}
    try { q = std::stod(f.qty_buf);   } catch (...) {}
    ImGui::TextDisabled("Total (%s)  %s", g_quote, fmt_total(p * q).c_str());

    // Place button
    bool is_pending = f.pending.valid() &&
        f.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    bool can_place  = has_keys && !is_pending && p > 0 && q > 0;

    if (!can_place) ImGui::BeginDisabled();
    if (ImGui::Button("Place Limit Order", ImVec2(-1, 0)) && can_place) {
        f.status     = OrderStatus::PENDING;
        f.status_msg = "Sending...";
        f.pending    = trader.submit(
            ex.exchange, f.side == 0 ? Side::BUY : Side::SELL,
            ex.symbol, p, q);
    }
    if (!can_place) ImGui::EndDisabled();

    if (!has_keys) ImGui::TextDisabled("No API key set");

    if      (f.status == OrderStatus::PENDING)
        ImGui::TextColored(ImVec4(1,1,0,1),        "Sending...");
    else if (f.status == OrderStatus::OK)
        ImGui::TextColored(ImVec4(0.3f,1,0.3f,1),  "Placed");
    else if (f.status == OrderStatus::ERROR)
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),  "ERR %s", f.status_msg.c_str());
}

// ── Open orders table (placed this session, with Cancel button) ───────────────

static void draw_open_orders(int idx, const OrderBookUpdate& ex,
                             TradeManager& trader) {
    OrderFormState& f = g_forms[idx];
    if (f.orders.empty()) return;

    ImGui::Spacing();
    ImGui::SeparatorText("Open Orders");

    // Poll cancel futures
    for (auto& o : f.orders) {
        if (o.cancelling && o.cancel_fut && o.cancel_fut->valid() &&
            o.cancel_fut->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            OrderResult r = o.cancel_fut->get();
            o.cancelling = false;
            if (r.success) { o.cancelled   = true; o.cancel_msg = "Cancelled"; }
            else            { o.cancel_error = true; o.cancel_msg = r.message; }
        }
    }

    float row_h = ImGui::GetTextLineHeightWithSpacing();
    int   rows  = (int)f.orders.size() + 1;           // +1 for header
    float tbl_h = std::min((float)rows * row_h + ImGui::GetStyle().ScrollbarSize,
                           6.f * row_h + ImGui::GetStyle().ScrollbarSize);

    char tid[32]; snprintf(tid, sizeof(tid), "##oo%d", idx);
    ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_ScrollY       |
                          ImGuiTableFlags_BordersInnerH;

    if (!ImGui::BeginTable(tid, 4, tfl, ImVec2(-1, tbl_h))) return;

    ImGui::TableSetupColumn("Side",  ImGuiTableColumnFlags_WidthFixed,   40.f);
    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Qty",   ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed,   76.f);
    ImGui::TableHeadersRow();

    for (auto& o : f.orders) {
        ImGui::TableNextRow();
        bool is_buy = (o.side == Side::BUY);
        ImVec4 side_col = is_buy
            ? ImVec4(0.35f, 0.85f, 0.45f, 1.f)
            : ImVec4(0.9f,  0.35f, 0.35f, 1.f);

        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(side_col, "%s", is_buy ? "BUY" : "SELL");

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s", fmt_price(o.price).c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", fmt_qty(o.qty).c_str());

        ImGui::TableSetColumnIndex(3);
        if (o.cancelled) {
            ImGui::TextColored(ImVec4(0.45f,0.45f,0.45f,1.f), "Cancelled");
        } else if (o.cancel_error) {
            ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "Error");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", o.cancel_msg.c_str());
        } else if (o.cancelling) {
            ImGui::TextDisabled("Cancelling..");
        } else {
            char btn_id[40];
            snprintf(btn_id, sizeof(btn_id), "Cancel##%s", o.order_id.c_str());
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.45f, 0.08f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.65f, 0.12f, 0.12f, 1.f));
            if (ImGui::Button(btn_id, ImVec2(-1, 0))) {
                o.cancelling = true;
                o.cancel_fut = std::make_shared<std::future<OrderResult>>(
                    trader.cancel(ex.exchange, o.symbol, o.order_id));
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::EndTable();
}

// ── Full panel ────────────────────────────────────────────────────────────────

static void draw_panel(int idx, const OrderBookUpdate& ex,
                       TradeManager& trader, SharedTrades& shared_trades) {
    // Header
    char header[64];
    snprintf(header, sizeof(header), "%.15s  %.15s", ex.exchange, ex.symbol);
    ImGui::SeparatorText(header);

    // Column headers — match table stretch ratios
    if (ImGui::BeginTable("##hdr", 3,
            ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_SizingFixedFit,
            ImVec2(-1, 0))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Price (%s)", g_quote);
        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("Qty (%s)",   g_base);
        ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Total (%s)", g_quote);
        ImGui::EndTable();
    }
    ImGui::Separator();

    OrderFormState& f = g_forms[idx];

    // Asks (worst → best)
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.9f,0.35f,0.35f,1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.55f,0.15f,0.15f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.7f,0.2f,0.2f,0.8f));
    draw_book_table(idx, ex, f, true);
    ImGui::PopStyleColor(3);

    // Spread
    if (ex.bid_count > 0 && ex.ask_count > 0)
        ImGui::TextDisabled("  spread  %s", fmt_price(ex.asks[0].price - ex.bids[0].price).c_str());
    else
        ImGui::TextDisabled("  spread  --");

    // Bids (best → worst)
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.35f,0.85f,0.45f,1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.1f,0.45f,0.15f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.15f,0.6f,0.2f,0.8f));
    draw_book_table(idx, ex, f, false);
    ImGui::PopStyleColor(3);

    // Recent trades
    draw_trades_table(idx, ex, shared_trades);

    // Order form
    draw_order_form(idx, ex, trader);

    // Open orders for this panel
    draw_open_orders(idx, ex, trader);
}

// ── Main render loop ──────────────────────────────────────────────────────────

void render_run(SharedDisplay& display, SharedTrades& shared_trades,
                TradeManager& trader, Exchange* handlers[4],
                std::atomic<bool>& running) {

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[UI] SDL_Init error: %s\n", SDL_GetError());
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,         0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,  8);

    SDL_Window* window = SDL_CreateWindow(
        "Trade Station",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1600, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "[UI] SDL_CreateWindow error: %s\n", SDL_GetError());
        SDL_Quit(); return;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_ENABLE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename     = nullptr;
    io.MouseDrawCursor = false;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool window_open = true;
    while (window_open) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) window_open = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                window_open = false;
        }

        OrderBookUpdate books[4]{};
        int count = 0;
        display.snapshot(books, count);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);

        // ── Symbol bar ────────────────────────────────────────────────────────
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Symbol");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("##base", g_base, sizeof(g_base));
        ImGui::SameLine();
        ImGui::Text("/");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("##quote", g_quote, sizeof(g_quote));
        ImGui::SameLine();
        if (ImGui::Button("Apply All")) {
            for (char* c = g_base;  *c; c++) *c = (char)toupper((unsigned char)*c);
            for (char* c = g_quote; *c; c++) *c = (char)toupper((unsigned char)*c);
            std::string base(g_base), quote(g_quote);
            display.clear_books();
            shared_trades.clear_all();
            for (int i = 0; i < 4; i++) g_forms[i] = OrderFormState{};
            for (int i = 0; i < 4; i++)
                if (handlers[i]) handlers[i]->change_symbol(base, quote);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(click a price level to fill price + qty)");

        ImGui::Separator();

        // ── Exchange panels ───────────────────────────────────────────────────
        float avail_w = ImGui::GetContentRegionAvail().x;
        float avail_h = ImGui::GetContentRegionAvail().y;
        float gap     = 8.f;
        float panel_w = count > 0
            ? (avail_w - gap * (count - 1)) / count : avail_w;

        for (int i = 0; i < count; i++) {
            char pid[16]; snprintf(pid, sizeof(pid), "##ex%d", i);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f,0.11f,0.14f,1.f));
            if (ImGui::BeginChild(pid, ImVec2(panel_w, avail_h),
                                  ImGuiChildFlags_Borders)) {
                draw_panel(i, books[i], trader, shared_trades);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            if (i < count - 1) ImGui::SameLine(0.f, gap);
        }

        if (count == 0)
            ImGui::TextDisabled("Waiting for exchange data...");

        ImGui::End();

        ImGui::Render();
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.07f, 0.07f, 0.09f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    running.store(false, std::memory_order_release);
}
