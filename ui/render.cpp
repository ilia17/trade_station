#include "render.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_sdl2.h>
#include <imgui/backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
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
// When enabled, limit price must stay within ±5% of best bid/ask mid on that panel
static bool g_fatfinger_protect = false;
static constexpr double k_fatfinger_frac = 0.05;

// Keep History tab manageable: remove oldest done entries once over the cap
static void trim_history(std::vector<PlacedOrder>& orders, int max_done = 20) {
    int n = 0;
    for (auto& o : orders) if (o.cancelled) n++;
    auto it = orders.begin();
    while (n > max_done && it != orders.end()) {
        if (it->cancelled) { it = orders.erase(it); --n; }
        else               { ++it; }
    }
}

// ── Formatting helpers ────────────────────────────────────────────────────────

// Format with comma thousands-separators and a fixed decimal count.
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

// Format with commas + automatically strip trailing zeros after the decimal.
static std::string fmt_smart(double v, int max_dec) {
    if (v <= 0.0) return "0";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", max_dec, v);
    std::string s(buf);
    // Strip trailing zeros
    if (s.find('.') != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last != std::string::npos && s[last] == '.') --last;
        s = s.substr(0, last + 1);
    }
    // Add comma separators to the integer part
    size_t dot = s.find('.');
    std::string ipart = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string dpart = (dot == std::string::npos) ? "" : s.substr(dot);
    int i = (int)ipart.size() - 3;
    while (i > 0) { ipart.insert(i, ","); i -= 3; }
    return ipart + dpart;
}

static std::string fmt_price(double p) {
    if (p <= 0.0) return "0";
    if (p >= 1000.0) return fmt_smart(p, 2);   // 97,500.34
    if (p >= 1.0)    return fmt_smart(p, 6);   // 1.234500 → 1.2345
    if (p >= 0.01)   return fmt_smart(p, 6);   // 0.123456
    return             fmt_smart(p, 8);        // 0.00001234
}

static std::string fmt_qty(double q) {
    if (q <= 0.0) return "0";
    if (q >= 1000.0) return fmt_smart(q, 2);
    if (q >= 1.0)    return fmt_smart(q, 4);
    return             fmt_smart(q, 6);
}

static std::string fmt_total(double t) { return fmt_smart(t, 2); }

// Book level vs resting order price (float-safe)
static bool prices_near(double a, double b) {
    double d = std::abs(a - b);
    if (d <= 1e-12) return true;
    double s = std::max(std::abs(a), std::abs(b));
    if (s <= 0.0) return d <= 1e-12;
    return d / s <= 1e-7;
}

static bool symbol_matches_book(const PlacedOrder& o, const OrderBookUpdate& ex) {
    if (o.symbol.empty() || ex.symbol[0] == '\0') return true;
    return o.symbol == ex.symbol;
}

// level_is_asks: true = ask side of book (reds) → only SELL orders; bids → BUY
static bool resting_order_at_level(const PlacedOrder& o, double level_px, bool level_is_asks,
                                   const OrderBookUpdate& ex) {
    if (o.cancelled) return false;
    if (!symbol_matches_book(o, ex)) return false;
    if (level_is_asks) {
        if (o.side != Side::SELL) return false;
    } else {
        if (o.side != Side::BUY) return false;
    }
    return prices_near(o.price, level_px);
}

static bool any_my_order_at_level(const OrderFormState& f, double level_px, bool level_is_asks,
                                  const OrderBookUpdate& ex) {
    for (const auto& o : f.orders)
        if (resting_order_at_level(o, level_px, level_is_asks, ex)) return true;
    return false;
}

static void cap_placed_orders(std::vector<PlacedOrder>& v, size_t max_n) {
    while (v.size() >= max_n) {
        auto it = std::min_element(v.begin(), v.end(),
            [](const PlacedOrder& a, const PlacedOrder& b) {
                long long sa = a.ui_seq ? a.ui_seq : 0;
                long long sb = b.ui_seq ? b.ui_seq : 0;
                return sa < sb;
            });
        v.erase(it);
    }
}

static void collect_indices_open_desc(const std::vector<PlacedOrder>& orders,
                                      std::vector<size_t>& ix_out) {
    ix_out.clear();
    for (size_t i = 0; i < orders.size(); ++i)
        if (!orders[i].cancelled) ix_out.push_back(i);
    std::sort(ix_out.begin(), ix_out.end(), [&](size_t a, size_t b) {
        long long sa = orders[a].ui_seq ? orders[a].ui_seq : 0;
        long long sb = orders[b].ui_seq ? orders[b].ui_seq : 0;
        return sa > sb;
    });
}

static void collect_indices_hist_desc(const std::vector<PlacedOrder>& orders,
                                      std::vector<size_t>& ix_out) {
    ix_out.clear();
    for (size_t i = 0; i < orders.size(); ++i)
        if (orders[i].cancelled) ix_out.push_back(i);
    std::sort(ix_out.begin(), ix_out.end(), [&](size_t a, size_t b) {
        long long sa = orders[a].ui_seq ? orders[a].ui_seq : 0;
        long long sb = orders[b].ui_seq ? orders[b].ui_seq : 0;
        return sa > sb;
    });
}

// ── Per-panel order form state ────────────────────────────────────────────────

static OrderFormState g_forms[4];

// ── Order book table ──────────────────────────────────────────────────────────

static void draw_book_table(int idx, const OrderBookUpdate& ex,
                            OrderFormState& f, bool is_asks, bool arb_highlight = false) {
    ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                          ImGuiTableFlags_SizingFixedFit;
    char tid[32]; snprintf(tid, sizeof(tid), "##tbl%s%d", is_asks ? "a" : "b", idx);
    if (!ImGui::BeginTable(tid, 4, tfl, ImVec2(-1, 0))) return;

    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,   14.f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);

    int count = is_asks ? ex.ask_count : ex.bid_count;
    float lh = ImGui::GetTextLineHeightWithSpacing();
    for (int r = 0; r < count; r++) {
        int    i = is_asks ? (ex.ask_count - 1 - r) : r;
        double p = is_asks ? ex.asks[i].price    : ex.bids[i].price;
        double q = is_asks ? ex.asks[i].quantity : ex.bids[i].quantity;

        bool mine = any_my_order_at_level(f, p, is_asks, ex);
        bool dot_green = mine && !is_asks;  // bid side → BUY (green)

        bool is_arb_row = arb_highlight && (is_asks ? r == count - 1 : r == 0);

        ImGui::TableNextRow();
        if (is_arb_row)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(230, 210, 0, 220));

        ImGui::TableSetColumnIndex(0);
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(12, lh));
        if (mine) {
            ImU32 col = dot_green ? IM_COL32(70, 200, 95, 255) : IM_COL32(230, 75, 75, 255);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(c0.x + 6.f, c0.y + lh * 0.5f), 4.f, col);
        }

        ImGui::TableSetColumnIndex(1);
        ImVec2 price_pos = ImGui::GetCursorScreenPos();
        char sel_id[32];
        snprintf(sel_id, sizeof(sel_id), "##sel%s%d_%d",
                 is_asks ? "a" : "b", idx, i);
        if (ImGui::Selectable(sel_id, false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, lh))) {
            snprintf(f.price_buf, sizeof(f.price_buf), "%.8g", p);
            snprintf(f.qty_buf,   sizeof(f.qty_buf),   "%.8g", q);
        }
        {
            ImU32 px_col = is_arb_row ? IM_COL32(20,  15,  0,   255)
                         : is_asks    ? IM_COL32(255, 150, 150, 255)
                                      : IM_COL32(150, 235, 170, 255);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(price_pos.x + 4.f, price_pos.y + 1.f),
                px_col,
                fmt_price(p).c_str());
        }

        ImGui::TableSetColumnIndex(2);
        if (is_arb_row) ImGui::TextColored(ImVec4(0.08f,0.06f,0.0f,1.f), "%s", fmt_qty(q).c_str());
        else            ImGui::Text("%s", fmt_qty(q).c_str());

        ImGui::TableSetColumnIndex(3);
        if (is_arb_row) ImGui::TextColored(ImVec4(0.08f,0.06f,0.0f,1.f), "%s", fmt_total(p*q).c_str());
        else            ImGui::Text("%s", fmt_total(p * q).c_str());
    }
    ImGui::EndTable();
}

// ── Recent trades table ───────────────────────────────────────────────────────

static void draw_trades_table(int idx, const OrderBookUpdate& ex,
                              SharedTrades& shared_trades, float force_h = 0.f) {
    Trade  t_snap[SharedTrades::MAX]{};
    int    t_count = 0;
    shared_trades.snapshot(ex.exchange, t_snap, t_count);

    ImGui::SeparatorText("Trades");

    ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_ScrollY;
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float tbl_h = (force_h > row_h * 4.f)
                  ? force_h
                  : row_h * 10.f + ImGui::GetStyle().ScrollbarSize;

    char tid[32]; snprintf(tid, sizeof(tid), "##tr%d", idx);
    if (!ImGui::BeginTable(tid, 4, tfl, ImVec2(-1, tbl_h))) return;

    ImGui::TableSetupColumn("Price",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Side",   ImGuiTableColumnFlags_WidthFixed,   36.f);
    ImGui::TableSetupColumn("Time",   ImGuiTableColumnFlags_WidthFixed,   58.f);
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

        ImGui::TableSetColumnIndex(3);
        if (t.time_ms > 0) {
            std::time_t sec = (std::time_t)(t.time_ms / 1000);
            std::tm* tm_info = std::localtime(&sec);
            char tbuf[16];
            std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_info);
            ImGui::TextDisabled("%s", tbuf);
        } else {
            ImGui::TextDisabled("--");
        }
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
            po.exchange = ex.exchange;
            po.symbol   = ex.symbol;
            po.side     = f.side == 0 ? Side::BUY : Side::SELL;
            try { po.price = std::stod(f.price_buf); } catch (...) {}
            try { po.qty   = std::stod(f.qty_buf);   } catch (...) {}
            po.ui_seq = next_placed_order_seq();
            cap_placed_orders(f.orders, 10);
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

    double mid = 0.0;
    bool have_mid = (ex.bid_count > 0 && ex.ask_count > 0);
    if (have_mid)
        mid = 0.5 * (ex.bids[0].price + ex.asks[0].price);

    bool fatfinger_ok = true;
    if (g_fatfinger_protect) {
        if (!have_mid || mid <= 0.0)
            fatfinger_ok = false;
        else {
            double lo = mid * (1.0 - k_fatfinger_frac);
            double hi = mid * (1.0 + k_fatfinger_frac);
            fatfinger_ok = (p >= lo && p <= hi);
        }
    }
    if (g_fatfinger_protect && have_mid && mid > 0.0) {
        double lo = mid * (1.0 - k_fatfinger_frac);
        double hi = mid * (1.0 + k_fatfinger_frac);
        ImGui::TextDisabled("Mid %s  allowed %s – %s",
            fmt_price(mid).c_str(), fmt_price(lo).c_str(), fmt_price(hi).c_str());
    } else if (g_fatfinger_protect && !have_mid) {
        ImGui::TextColored(ImVec4(1.f,0.65f,0.2f,1.f),
            "Fat-finger: need bid/ask to compute mid");
    }

    // Place button
    bool is_pending = f.pending.valid() &&
        f.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    bool can_place  = has_keys && !is_pending && p > 0 && q > 0 && fatfinger_ok;

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

// Per-panel exchange metadata used for fetch
struct PanelMeta { const char* name; };
static const PanelMeta k_panels[4] = {{"MEXC"}, {"Gate"}, {"BingX"}, {"LBank"}};

static ImVec4 exchange_accent_color(const char* ex) {
    if (!ex || !ex[0])          return ImVec4(0.5f, 0.5f, 0.5f, 1.f);
    if (strcmp(ex, "MEXC")  == 0) return ImVec4(0.30f, 0.55f, 0.95f, 1.f);
    if (strcmp(ex, "Gate")  == 0) return ImVec4(0.28f, 0.82f, 0.44f, 1.f);
    if (strcmp(ex, "BingX") == 0) return ImVec4(0.65f, 0.40f, 0.92f, 1.f);
    if (strcmp(ex, "LBank") == 0) return ImVec4(0.96f, 0.58f, 0.20f, 1.f);
    return ImVec4(0.6f, 0.6f, 0.6f, 1.f);
}

static ImVec4 exchange_bg_color(const char* ex) {
    if (!ex || !ex[0])          return ImVec4(0.11f, 0.11f, 0.14f, 1.f);
    if (strcmp(ex, "MEXC")  == 0) return ImVec4(0.10f, 0.11f, 0.17f, 1.f);
    if (strcmp(ex, "Gate")  == 0) return ImVec4(0.09f, 0.14f, 0.10f, 1.f);
    if (strcmp(ex, "BingX") == 0) return ImVec4(0.12f, 0.10f, 0.16f, 1.f);
    if (strcmp(ex, "LBank") == 0) return ImVec4(0.15f, 0.12f, 0.09f, 1.f);
    return ImVec4(0.11f, 0.11f, 0.14f, 1.f);
}

// Build exchange-specific symbol string from base/quote for a given exchange name
static std::string exchange_symbol(const std::string& exchange) {
    std::string b(g_base), q(g_quote);
    if (exchange == "MEXC")  return b + q;                    // BTCUSDT
    if (exchange == "Gate")  return b + "_" + q;              // BTC_USDT
    if (exchange == "BingX") return b + "-" + q;              // BTC-USDT
    // LBank: btc_usdt (lowercase)
    std::string s = b + "_" + q;
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static void draw_open_orders(int idx, const OrderBookUpdate& ex,
                             TradeManager& trader) {
    OrderFormState& f = g_forms[idx];
    static std::vector<size_t> s_ord_ix;

    // Poll fetch future — merge results (avoid duplicate order_ids)
    if (f.fetch_fut.valid() &&
        f.fetch_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto fetched = f.fetch_fut.get();
        f.fetching = false;
        for (auto& fo : fetched) {
            bool dup = false;
            for (auto& existing : f.orders)
                if (existing.order_id == fo.order_id) { dup = true; break; }
            if (!dup) {
                if (fo.ui_seq == 0) fo.ui_seq = next_placed_order_seq();
                f.orders.push_back(std::move(fo));
            }
        }
    }

    // Poll cancel-all future — on success move all open to cancelled
    if (f.cancel_all_fut.valid() &&
        f.cancel_all_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        OrderResult r = f.cancel_all_fut.get();
        f.cancelling_all = false;
        if (r.success) {
            for (auto& o : f.orders)
                if (!o.cancelled) { o.cancelled = true; o.cancel_msg = "Cancelled"; }
            trim_history(f.orders);
        }
    }

    // Poll per-order cancel futures
    for (auto& o : f.orders) {
        if (o.cancelling && o.cancel_fut && o.cancel_fut->valid() &&
            o.cancel_fut->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            OrderResult r = o.cancel_fut->get();
            o.cancelling = false;
            if (r.success) { o.cancelled   = true; o.cancel_msg = "Cancelled"; trim_history(f.orders); }
            else            { o.cancel_error = true; o.cancel_msg = r.message;  }
        }
    }

    // Count open vs done
    int n_open = 0, n_done = 0;
    for (auto& o : f.orders)
        (o.cancelled ? n_done : n_open)++;

    ImGui::Spacing();

    // ── Tab bar ──────────────────────────────────────────────────────────────
    char tb_id[32]; snprintf(tb_id, sizeof(tb_id), "##tabs%d", idx);
    if (!ImGui::BeginTabBar(tb_id)) return;

    // ── Open tab ─────────────────────────────────────────────────────────────
    char open_lbl[32]; snprintf(open_lbl, sizeof(open_lbl), "Open (%d)###open%d", n_open, idx);
    if (ImGui::BeginTabItem(open_lbl)) {
        if (f.fetching)       ImGui::TextDisabled("  Fetching open orders...");
        if (f.cancelling_all) ImGui::TextDisabled("  Cancelling all...");

        // Cancel All button — only meaningful for the open tab
        bool has_k      = trader.has_keys(ex.exchange);
        bool can_cancel = has_k && !f.cancelling_all && n_open > 0;
        char ca_id[32]; snprintf(ca_id, sizeof(ca_id), "Cancel All##ca%d", idx);
        if (!can_cancel) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.08f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.12f, 0.12f, 1.f));
        if (ImGui::Button(ca_id, ImVec2(-1, 0)) && can_cancel) {
            f.cancelling_all = true;
            f.cancel_all_fut = trader.cancel_all(ex.exchange, exchange_symbol(ex.exchange));
        }
        ImGui::PopStyleColor(2);
        if (!can_cancel) {
            ImGui::EndDisabled();
            if (!has_k && n_open > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("No API key for %s", ex.exchange);
        }

        // Render active orders — but use g_forms directly for mutable Cancel button
        float row_h = ImGui::GetTextLineHeightWithSpacing();
        float tbl_h = std::min((float)(n_open + 1) * row_h + ImGui::GetStyle().ScrollbarSize,
                               22.f * row_h + ImGui::GetStyle().ScrollbarSize);
        char tid[32]; snprintf(tid, sizeof(tid), "##oo%d", idx);
        ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollY        |
                              ImGuiTableFlags_BordersInnerH;
        if (n_open == 0) {
            ImGui::TextDisabled("  No open orders.");
        } else if (ImGui::BeginTable(tid, 5, tfl, ImVec2(-1, tbl_h))) {
            ImGui::TableSetupColumn("Side",   ImGuiTableColumnFlags_WidthFixed,   40.f);
            ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed,   72.f);
            ImGui::TableSetupColumn("Price",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed,   84.f);
            ImGui::TableHeadersRow();

            collect_indices_open_desc(f.orders, s_ord_ix);
            for (size_t k = 0; k < s_ord_ix.size(); ++k) {
                auto& o = f.orders[s_ord_ix[k]];
                ImGui::TableNextRow();
                bool is_buy = (o.side == Side::BUY);
                ImVec4 sc = is_buy ? ImVec4(0.35f,0.85f,0.45f,1.f) : ImVec4(0.9f,0.35f,0.35f,1.f);

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(sc, "%s", is_buy ? "BUY" : "SELL");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", o.symbol.empty() ? "-" : o.symbol.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", fmt_price(o.price).c_str());
                ImGui::TableSetColumnIndex(3);
                if (o.cum_qty > 0)
                    ImGui::Text("%s/%s", fmt_qty(o.cum_qty).c_str(), fmt_qty(o.qty).c_str());
                else
                    ImGui::Text("%s", fmt_qty(o.qty).c_str());

                ImGui::TableSetColumnIndex(4);
                if (o.cancel_error) {
                    ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "Error");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", o.cancel_msg.c_str());
                } else if (o.cancelling) {
                    ImGui::TextDisabled("Cancelling..");
                } else {
                    bool hk = trader.has_keys(o.exchange);
                    char btn_id[48];
                    snprintf(btn_id, sizeof(btn_id), "Cancel##%s", o.order_id.c_str());
                    if (!hk) ImGui::BeginDisabled();
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f,0.08f,0.08f,1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f,0.12f,0.12f,1.f));
                    if (ImGui::Button(btn_id, ImVec2(-1,0)) && hk) {
                        o.cancelling = true;
                        o.cancel_fut = std::make_shared<std::future<OrderResult>>(
                            trader.cancel(o.exchange, o.symbol, o.order_id));
                    }
                    ImGui::PopStyleColor(2);
                    if (!hk) {
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("No API key for %s", o.exchange.c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ── History tab ───────────────────────────────────────────────────────────
    char hist_lbl[32]; snprintf(hist_lbl, sizeof(hist_lbl), "History (%d)###hist%d", n_done, idx);
    if (ImGui::BeginTabItem(hist_lbl)) {
        // Clear history button
        if (n_done > 0) {
            char clr_id[32]; snprintf(clr_id, sizeof(clr_id), "Clear History##clr%d", idx);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.2f, 0.25f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.38f, 1.f));
            if (ImGui::Button(clr_id, ImVec2(-1, 0))) {
                f.orders.erase(
                    std::remove_if(f.orders.begin(), f.orders.end(),
                                   [](const PlacedOrder& o){ return o.cancelled; }),
                    f.orders.end());
            }
            ImGui::PopStyleColor(2);
        }

        float row_h = ImGui::GetTextLineHeightWithSpacing();
        float tbl_h = std::min((float)(n_done + 1) * row_h + ImGui::GetStyle().ScrollbarSize,
                               22.f * row_h + ImGui::GetStyle().ScrollbarSize);
        char tid[32]; snprintf(tid, sizeof(tid), "##oh%d", idx);
        ImGuiTableFlags tfl = ImGuiTableFlags_NoHostExtendX |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollY        |
                              ImGuiTableFlags_BordersInnerH;
        if (n_done == 0) {
            ImGui::TextDisabled("  No history yet.");
        } else if (ImGui::BeginTable(tid, 5, tfl, ImVec2(-1, tbl_h))) {
            ImGui::TableSetupColumn("Side",   ImGuiTableColumnFlags_WidthFixed,   40.f);
            ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed,   72.f);
            ImGui::TableSetupColumn("Price",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,   84.f);
            ImGui::TableHeadersRow();

            collect_indices_hist_desc(f.orders, s_ord_ix);
            for (size_t k = 0; k < s_ord_ix.size(); ++k) {
                auto& o = f.orders[s_ord_ix[k]];
                ImGui::TableNextRow();
                bool is_buy = (o.side == Side::BUY);
                ImVec4 sc = is_buy ? ImVec4(0.35f,0.85f,0.45f,1.f) : ImVec4(0.9f,0.35f,0.35f,1.f);

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(sc, "%s", is_buy ? "BUY" : "SELL");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", o.symbol.empty() ? "-" : o.symbol.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", fmt_price(o.price).c_str());
                ImGui::TableSetColumnIndex(3);
                if (o.cum_qty > 0)
                    ImGui::Text("%s/%s", fmt_qty(o.cum_qty).c_str(), fmt_qty(o.qty).c_str());
                else
                    ImGui::Text("%s", fmt_qty(o.qty).c_str());
                ImGui::TableSetColumnIndex(4);
                ImVec4 col;
                if      (o.cancel_msg == "Filled")       col = ImVec4(0.3f,  0.85f, 0.4f,  1.f);
                else if (o.cancel_msg == "Part.Cancel")  col = ImVec4(1.f,   0.8f,  0.2f,  1.f);
                else                                     col = ImVec4(0.45f, 0.45f, 0.45f, 1.f);
                ImGui::TextColored(col, "%s", o.cancel_msg.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

// ── Full panel ────────────────────────────────────────────────────────────────

static void draw_panel(int idx, const OrderBookUpdate& ex,
                       TradeManager& trader, SharedTrades& shared_trades,
                       bool arb_ask = false, bool arb_bid = false) {
    // Header
    char header[64];
    snprintf(header, sizeof(header), "%.15s  %.15s", ex.exchange, ex.symbol);
    ImGui::PushStyleColor(ImGuiCol_Text, exchange_accent_color(ex.exchange));
    ImGui::SeparatorText(header);
    ImGui::PopStyleColor();

    // Column headers — match table stretch ratios
    if (ImGui::BeginTable("##hdr", 4,
            ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_SizingFixedFit,
            ImVec2(-1, 0))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,   14.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled(" ");
        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("Price (%s)", g_quote);
        ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Qty (%s)",   g_base);
        ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("Total (%s)", g_quote);
        ImGui::EndTable();
    }
    ImGui::Separator();

    OrderFormState& f = g_forms[idx];

    // Asks (worst → best)
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.9f,0.35f,0.35f,1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.55f,0.15f,0.15f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.7f,0.2f,0.2f,0.8f));
    draw_book_table(idx, ex, f, true, arb_ask);
    ImGui::PopStyleColor(3);

    // Spread
    if (ex.bid_count > 0 && ex.ask_count > 0)
        ImGui::TextDisabled("  spread  %s", fmt_price(ex.asks[0].price - ex.bids[0].price).c_str());
    else
        ImGui::TextDisabled("  spread  --");
    ImGui::TextDisabled("  Book: filled circle = your order at that price");

    // Bids (best → worst)
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.35f,0.85f,0.45f,1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.1f,0.45f,0.15f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.15f,0.6f,0.2f,0.8f));
    draw_book_table(idx, ex, f, false, arb_bid);
    ImGui::PopStyleColor(3);

    // Recent trades — fill remaining space minus reservation for order form + open orders
    {
        float rh       = ImGui::GetTextLineHeightWithSpacing();
        float avail    = ImGui::GetContentRegionAvail().y;
        float reserved = rh * 14.f;
        float trades_h = (avail > reserved + rh * 5.f) ? (avail - reserved) : 0.f;
        draw_trades_table(idx, ex, shared_trades, trades_h);
    }

    // Order form
    draw_order_form(idx, ex, trader);

    // Open orders for this panel
    draw_open_orders(idx, ex, trader);
}

// ── Main render loop ──────────────────────────────────────────────────────────

void render_run(SharedDisplay& display, SharedTrades& shared_trades,
                TradeManager& trader, Exchange* handlers[4],
                std::atomic<bool>& running,
                SharedOrderEvents* order_events) {

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
    bool initial_fetch_done = false;
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

        // Drain real-time order events (MEXC protobuf stream + Gate spot.orders WS)
        if (order_events) {
            for (auto& ev : order_events->drain()) {
                for (int i = 0; i < count; i++) {
                    if (std::string(books[i].exchange) != ev.exchange) continue;

                    // ── "new" event: order created externally or by our UI ─────────────
                    if (ev.event_type == "new") {
                        bool dup = false;
                        for (auto& o : g_forms[i].orders)
                            if (o.order_id == ev.order_id) { dup = true; break; }
                        if (!dup && ev.price > 0 && ev.qty > 0) {
                            PlacedOrder po;
                            po.exchange = ev.exchange;
                            po.order_id = ev.order_id;
                            po.symbol   = ev.symbol;
                            po.side     = (ev.side == 0) ? Side::BUY : Side::SELL;
                            po.price    = ev.price;
                            po.qty      = ev.qty;
                            po.ui_seq   = next_placed_order_seq();
                            cap_placed_orders(g_forms[i].orders, 50);
                            g_forms[i].orders.push_back(std::move(po));
                        }
                        continue;
                    }

                    // ── "update" / "finish" events: find or auto-insert the order ────
                    // BingX (and occasionally other exchanges) skips the NEW event for
                    // fast fills, so the first event we see may be PARTIALLY_FILLED or
                    // FILLED. Auto-insert the row so it always appears in the UI.
                    PlacedOrder* matched = nullptr;
                    for (auto& o : g_forms[i].orders) {
                        if (o.cancelled) continue;
                        if (o.order_id == ev.order_id ||
                            (!ev.client_id.empty() && o.order_id == ev.client_id)) {
                            matched = &o;
                            break;
                        }
                    }
                    if (!matched && !ev.order_id.empty()) {
                        // First time seeing this order — create a placeholder row
                        PlacedOrder po;
                        po.exchange = ev.exchange;
                        po.order_id = ev.order_id;
                        po.symbol   = ev.symbol;
                        po.side     = (ev.side == 0) ? Side::BUY : Side::SELL;
                        po.price    = ev.price;
                        po.qty      = ev.qty;  // stream guarantees fallback to Q/cum_qty
                        po.ui_seq   = next_placed_order_seq();
                        cap_placed_orders(g_forms[i].orders, 50);
                        g_forms[i].orders.push_back(std::move(po));
                        matched = &g_forms[i].orders.back();
                    }
                    if (!matched) continue;

                    auto& o = *matched;
                    if (ev.event_type == "update") {
                        if (ev.cum_qty > 0) o.cum_qty = ev.cum_qty;

                    } else if (ev.event_type == "finish") {
                        if (ev.cum_qty > 0) o.cum_qty = ev.cum_qty;

                        std::string msg;
                        if (ev.exchange == "MEXC") {
                            if      (ev.status == 2) msg = "Filled";
                            else if (ev.status == 5) msg = "Part.Cancel";
                            else                     msg = "Cancelled";
                        } else {
                            if      (ev.finish_as == "filled") msg = "Filled";
                            else if (!ev.finish_as.empty())    msg = ev.finish_as;
                            else                               msg = "Cancelled";
                        }
                        o.cancelled  = true;
                        o.cancel_msg = msg;
                        trim_history(g_forms[i].orders);
                    }
                }
            }
        }

        // One-shot: fetch open orders for the default symbol on first frame
        if (!initial_fetch_done) {
            initial_fetch_done = true;
            for (int i = 0; i < count; i++) {
                if (trader.has_keys(books[i].exchange)) {
                    g_forms[i].fetch_fut = trader.fetch_orders(books[i].exchange,
                                              exchange_symbol(books[i].exchange));
                    g_forms[i].fetching = true;
                }
            }
        }

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
            // Fetch open orders for the new symbol from each connected exchange
            for (int i = 0; i < count; i++) {
                if (trader.has_keys(books[i].exchange)) {
                    g_forms[i].fetch_fut = trader.fetch_orders(books[i].exchange,
                                              exchange_symbol(books[i].exchange));
                    g_forms[i].fetching = true;
                }
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Fat-finger protect (±5% from mid)", &g_fatfinger_protect);
        ImGui::SameLine();
        ImGui::TextDisabled("(click book to fill price + qty)");

        ImGui::Separator();

        // ── Arbitrage detection — all exchange pairs ──────────────────────────
        bool arb_ask_flag[4]{};
        bool arb_bid_flag[4]{};
        for (int i = 0; i < count; i++) {
            if (books[i].ask_count == 0 || books[i].asks[0].price <= 0) continue;
            for (int j = 0; j < count; j++) {
                if (i == j || books[j].bid_count == 0 || books[j].bids[0].price <= 0) continue;
                if (books[j].bids[0].price > books[i].asks[0].price) {
                    arb_ask_flag[i] = true;
                    arb_bid_flag[j] = true;
                }
            }
        }

        // ── Exchange panels ───────────────────────────────────────────────────
        float avail_w = ImGui::GetContentRegionAvail().x;
        float avail_h = ImGui::GetContentRegionAvail().y;
        float gap     = 8.f;
        float panel_w = count > 0
            ? (avail_w - gap * (count - 1)) / count : avail_w;

        for (int i = 0; i < count; i++) {
            char pid[16]; snprintf(pid, sizeof(pid), "##ex%d", i);
            bool arb_ask_panel = arb_ask_flag[i];
            bool arb_bid_panel = arb_bid_flag[i];
            ImGui::PushStyleColor(ImGuiCol_ChildBg, exchange_bg_color(books[i].exchange));
            ImGui::PushStyleColor(ImGuiCol_Border,  exchange_accent_color(books[i].exchange));
            if (ImGui::BeginChild(pid, ImVec2(panel_w, avail_h),
                                  ImGuiChildFlags_Borders)) {
                draw_panel(i, books[i], trader, shared_trades, arb_ask_panel, arb_bid_panel);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
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

    // Signal threads to stop BEFORE SDL teardown so they have time to react
    running.store(false, std::memory_order_release);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
