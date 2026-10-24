// Calendar.xapp — Calendar app for FreeBSD-X
//
// Features:
//   • Month view: 7-column grid with event dots per day; click day to switch to day view
//   • Week view: 7-column × 24-row time grid with event blocks
//   • Day view: hourly slot list with event detail rows
//   • Add Event dialog: title, date, start/end time, description, location, reminder
//   • Delete events from day view
//   • iCalendar (.ics) import via file path
//   • EventReminder XBus broadcast → system notification via com.freebsdx.Notification
//   • com.freebsdx.Calendar XBus service (AddEvent/UpdateEvent/DeleteEvent/ListEvents)

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <functional>
#include <sstream>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Calendar helpers ──────────────────────────────────────────────────────────
// Date math (daysInMonth, firstWeekday — both 0=Sun) comes from xkit/calendar.h;
// this app used to carry its own copies (leap math + Zeller's congruence).

static const char* kMonthNames[] = {
    nullptr, "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};
static const char* kDayAbbr[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

// ── Event local model ─────────────────────────────────────────────────────────

struct CalEvent {
    uint32_t    id{};
    uint16_t    year{};
    uint8_t     month{};
    uint8_t     day{};
    uint8_t     hour{};
    uint8_t     minute{};
    uint8_t     end_hour{};
    uint8_t     end_minute{};
    uint16_t    reminder_min{};
    uint8_t     all_day{};
    std::string title;
    std::string description;
    std::string location;
};

// ── View enum ─────────────────────────────────────────────────────────────────

enum class View { Month, Week, Day };

// ── App ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // ── State ─────────────────────────────────────────────────────────────────
    static std::vector<CalEvent> events;
    static View   cur_view    = View::Month;
    static int    cur_year    = 2026;
    static int    cur_month   = 7;   // 1-based
    static int    cur_day     = 1;
    static XBusClient* g_xbus = nullptr;

    // Get today
    {
        std::time_t t = std::time(nullptr);
        std::tm* lt = std::localtime(&t);
        if (lt) {
            cur_year  = lt->tm_year + 1900;
            cur_month = lt->tm_mon  + 1;
            cur_day   = lt->tm_mday;
        }
    }

    // Main content area (rebuilt on every navigation/view change)
    static std::shared_ptr<FlexColumn> content_area;
    static std::shared_ptr<TextWidget> header_title;

    // Forward declarations
    static std::function<void()>              rebuild_view;
    static std::function<void()>              load_events;
    static std::function<void(int,int,int)>   open_day;
    static std::function<void(const CalEvent&)> show_add_edit;

    // ── Load events for current month/year from XBus ─────────────────────────
    load_events = []() {
        if (!g_xbus) return;
        events.clear();

        [[maybe_unused]] static uint32_t total_pages = 1;
        total_pages = 1;

        std::function<void(uint32_t)> fetch = [&](uint32_t page) {
            CalendarListRequest req{};
            req.year_  = (uint16_t)cur_year;
            req.month_ = (uint8_t)cur_month;
            req.page_  = page;
            g_xbus->Call("com.freebsdx.Calendar",
                static_cast<uint32_t>(CalendarOpcode::ListEvents),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [page](const XBusMessage& resp) {
                    if (resp.opcode_ !=
                        static_cast<uint32_t>(CalendarOpcode::EventPage)) return;
                    CalendarEventPage ep{};
                    std::memcpy(&ep, resp.payload_, sizeof(ep));
                    total_pages = ep.total_ > 0 ? ep.total_ : 1;
                    if (ep.valid_) {
                        CalEvent e;
                        e.id          = ep.event_id_;
                        e.year        = ep.year_;
                        e.month       = ep.month_;
                        e.day         = ep.day_;
                        e.hour        = ep.hour_;
                        e.minute      = ep.minute_;
                        e.end_hour    = ep.end_hour_;
                        e.end_minute  = ep.end_minute_;
                        e.reminder_min = ep.reminder_min_;
                        e.all_day     = ep.all_day_;
                        e.title       = std::string(ep.title_,
                            strnlen(ep.title_, sizeof(ep.title_)));
                        e.description = std::string(ep.description_,
                            strnlen(ep.description_, sizeof(ep.description_)));
                        e.location    = std::string(ep.location_,
                            strnlen(ep.location_, sizeof(ep.location_)));
                        events.push_back(std::move(e));
                    }
                    // Compositor sends one EventPage per Call (for the given page).
                    // Subsequent pages are fetched with new Calls.
                    if (page + 1 < ep.total_) {
                        // fetch next page
                    }
                    rebuild_view();
                });
        };
        // Fetch all pages sequentially via repeated Calls
        // (simplified: fetch up to 64 events via individual page requests)
        for (uint32_t p = 0; p < 64; ++p) {
            CalendarListRequest req{};
            req.year_  = (uint16_t)cur_year;
            req.month_ = (uint8_t)cur_month;
            req.page_  = p;
            g_xbus->Call("com.freebsdx.Calendar",
                static_cast<uint32_t>(CalendarOpcode::ListEvents),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [p](const XBusMessage& resp) {
                    if (resp.opcode_ !=
                        static_cast<uint32_t>(CalendarOpcode::EventPage)) return;
                    CalendarEventPage ep{};
                    std::memcpy(&ep, resp.payload_, sizeof(ep));
                    if (!ep.valid_ || p >= ep.total_) return;
                    CalEvent e;
                    e.id          = ep.event_id_;
                    e.year        = ep.year_;
                    e.month       = ep.month_;
                    e.day         = ep.day_;
                    e.hour        = ep.hour_;
                    e.minute      = ep.minute_;
                    e.end_hour    = ep.end_hour_;
                    e.end_minute  = ep.end_minute_;
                    e.reminder_min = ep.reminder_min_;
                    e.all_day     = ep.all_day_;
                    e.title       = std::string(ep.title_,
                        strnlen(ep.title_, sizeof(ep.title_)));
                    e.description = std::string(ep.description_,
                        strnlen(ep.description_, sizeof(ep.description_)));
                    e.location    = std::string(ep.location_,
                        strnlen(ep.location_, sizeof(ep.location_)));
                    // Deduplicate
                    for (auto& ex : events) if (ex.id == e.id) return;
                    events.push_back(std::move(e));
                    rebuild_view();
                });
        }
    };

    // ── Add/Edit dialog ───────────────────────────────────────────────────────
    show_add_edit = [](const CalEvent& tmpl) {
        if (!content_area) return;
        content_area->clearChildren();

        auto title_hdr = std::make_shared<TextWidget>(
            tmpl.id ? "Edit Event" : "New Event", 16.0f);
        title_hdr->setColor(Color{0.95f, 0.95f, 0.95f, 1.0f});

        // Title field
        auto title_lbl = std::make_shared<TextWidget>("Title", 12.0f);
        title_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        auto title_in  = std::make_shared<TextInputWidget>(tmpl.title);

        // Date — a calendar date picker (dogfoods xkit DatePickerWidget), seeded
        // to the event's date or the day the calendar is showing.
        auto date_lbl  = std::make_shared<TextWidget>("Date", 12.0f);
        date_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        auto date_picker = std::make_shared<DatePickerWidget>(Date{
            tmpl.year  ? (int)tmpl.year  : cur_year,
            tmpl.month ? (int)tmpl.month : cur_month,
            tmpl.day   ? (int)tmpl.day   : cur_day});

        // Time fields
        auto time_lbl  = std::make_shared<TextWidget>("Start time (HH MM)", 12.0f);
        time_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        char time_str[12]{};
        std::snprintf(time_str, sizeof(time_str), "%02d %02d",
                      tmpl.hour, tmpl.minute);
        auto time_in   = std::make_shared<TextInputWidget>(time_str);

        auto etime_lbl = std::make_shared<TextWidget>("End time  (HH MM)", 12.0f);
        etime_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        char etime_str[12]{};
        std::snprintf(etime_str, sizeof(etime_str), "%02d %02d",
                      tmpl.end_hour, tmpl.end_minute);
        auto etime_in  = std::make_shared<TextInputWidget>(etime_str);

        // Location
        auto loc_lbl   = std::make_shared<TextWidget>("Location", 12.0f);
        loc_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        auto loc_in    = std::make_shared<TextInputWidget>(tmpl.location);

        // Description
        auto desc_lbl  = std::make_shared<TextWidget>("Notes", 12.0f);
        desc_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        auto desc_in   = std::make_shared<TextInputWidget>(tmpl.description);

        // Reminder
        auto rem_lbl   = std::make_shared<TextWidget>("Reminder (minutes, 0=none)", 12.0f);
        rem_lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
        char rem_str[8]{};
        std::snprintf(rem_str, sizeof(rem_str), "%d", (int)tmpl.reminder_min);
        auto rem_in    = std::make_shared<TextInputWidget>(rem_str);

        // Status label
        auto status_lbl = std::make_shared<TextWidget>("", 12.0f);
        status_lbl->setColor(Color{0.55f, 0.8f, 0.55f, 1.0f});

        // Save button
        uint32_t existing_id = tmpl.id;
        auto save_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>(tmpl.id ? "Update" : "Save", 13.0f),
            [=]() mutable {
                if (!g_xbus) return;
                // Parse date
                int y=0,mo=0,d=0,h=0,mi=0,eh=0,em=0;
                Date dv = date_picker->value();
                y = dv.year; mo = dv.month; d = dv.day;
                std::istringstream ts(time_in->text());
                ts >> h >> mi;
                std::istringstream es(etime_in->text());
                es >> eh >> em;
                int rem = 0;
                try { rem = std::stoi(rem_in->text()); } catch(...) {}

                CalendarEvent req{};
                req.id_         = existing_id;
                req.year_       = (uint16_t)y;
                req.month_      = (uint8_t)mo;
                req.day_        = (uint8_t)d;
                req.hour_       = (uint8_t)h;
                req.minute_     = (uint8_t)mi;
                req.end_hour_   = (uint8_t)eh;
                req.end_minute_ = (uint8_t)em;
                req.reminder_min_ = (uint16_t)rem;
                std::strncpy(req.title_,  title_in->text().c_str(), 79);
                std::strncpy(req.description_, desc_in->text().c_str(), 79);
                std::strncpy(req.location_, loc_in->text().c_str(), 31);

                auto opcode = existing_id
                    ? CalendarOpcode::UpdateEvent
                    : CalendarOpcode::AddEvent;
                g_xbus->Call("com.freebsdx.Calendar",
                    static_cast<uint32_t>(opcode),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                    [status_lbl](const XBusMessage&) {
                        status_lbl->setText("Saved.");
                        load_events();
                        rebuild_view();
                    });
            });
        save_btn->setBackgroundColor(Color{0.22f, 0.50f, 0.85f, 1.0f});
        save_btn->setPadding(8.0f);

        // Cancel button
        auto cancel_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("Cancel", 13.0f),
            []() { rebuild_view(); });
        cancel_btn->setBackgroundColor(Color{0.25f, 0.25f, 0.28f, 1.0f});
        cancel_btn->setPadding(8.0f);

        auto btn_row = std::make_shared<FlexRow>(
            std::vector<std::shared_ptr<Widget>>{save_btn,
                std::make_shared<Spacer>(12,0), cancel_btn});
        btn_row->setCrossAxisAlignment(CrossAxisAlignment::Center);

        auto form = std::make_shared<FlexColumn>(
            std::vector<std::shared_ptr<Widget>>{
                title_hdr,
                std::make_shared<Spacer>(0,12),
                title_lbl, title_in,
                std::make_shared<Spacer>(0,6),
                date_lbl, date_picker,
                std::make_shared<Spacer>(0,6),
                time_lbl, time_in,
                std::make_shared<Spacer>(0,6),
                etime_lbl, etime_in,
                std::make_shared<Spacer>(0,6),
                loc_lbl, loc_in,
                std::make_shared<Spacer>(0,6),
                desc_lbl, desc_in,
                std::make_shared<Spacer>(0,6),
                rem_lbl, rem_in,
                std::make_shared<Spacer>(0,16),
                btn_row,
                std::make_shared<Spacer>(0,8),
                status_lbl,
            });
        form->setGap(2.0f);
        form->setPadding(20.0f);
        form->setBackgroundColor(Color{0.13f, 0.13f, 0.16f, 1.0f});
        form->setFixedSize(480.0f, 0.0f);

        auto center = std::make_shared<FlexColumn>(
            std::vector<std::shared_ptr<Widget>>{form});
        center->setMainAxisAlignment(MainAxisAlignment::Center);
        center->setCrossAxisAlignment(CrossAxisAlignment::Center);
        center->setFlexGrow(1.0f);
        content_area->addChild(center);
    };

    // ── Day view ──────────────────────────────────────────────────────────────
    open_day = [](int y, int m, int d) {
        cur_view = View::Day;
        cur_year = y; cur_month = m; cur_day = d;
        if (header_title)
            header_title->setText(std::string(kMonthNames[m]) + " " +
                std::to_string(d) + ", " + std::to_string(y));
        rebuild_view();
    };

    // ── Rebuild main content ──────────────────────────────────────────────────
    rebuild_view = []() {
        if (!content_area) return;
        content_area->clearChildren();

        if (cur_view == View::Month) {
            // ── Month view ────────────────────────────────────────────────────
            // Day-of-week header
            auto dow_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{});
            for (int i = 0; i < 7; ++i) {
                auto lbl = std::make_shared<TextWidget>(kDayAbbr[i], 11.0f);
                lbl->setColor(Color{0.6f, 0.6f, 0.6f, 1.0f});
                lbl->setFlexGrow(1.0f);
                dow_row->addChild(lbl);
            }
            dow_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            dow_row->setPadding(4.0f);
            dow_row->setBackgroundColor(Color{0.13f, 0.13f, 0.15f, 1.0f});
            content_area->addChild(dow_row);
            content_area->addChild(std::make_shared<DividerWidget>());

            int first_dow = calendar::firstWeekday(cur_year, cur_month);
            int dim = calendar::daysInMonth(cur_year, cur_month);

            // Get today
            std::time_t now = std::time(nullptr);
            std::tm* lt = std::localtime(&now);
            int today_y = lt ? lt->tm_year + 1900 : 0;
            int today_m = lt ? lt->tm_mon  + 1    : 0;
            int today_d = lt ? lt->tm_mday         : 0;

            int cell = 0;
            auto week_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{});
            week_row->setFlexGrow(1.0f);

            auto flush_row = [&]() {
                // Pad remaining cells in row with empty spacers
                while (cell % 7 != 0) {
                    auto pad = std::make_shared<Spacer>(0, 0);
                    pad->setFlexGrow(1.0f);
                    week_row->addChild(pad);
                    ++cell;
                }
                week_row->setGap(1.0f);
                content_area->addChild(week_row);
                content_area->addChild(std::make_shared<DividerWidget>(0.5f));
                week_row = std::make_shared<FlexRow>(
                    std::vector<std::shared_ptr<Widget>>{});
                week_row->setFlexGrow(1.0f);
            };

            // Leading empty cells
            for (int i = 0; i < first_dow; ++i) {
                auto pad = std::make_shared<Spacer>(0, 0);
                pad->setFlexGrow(1.0f);
                week_row->addChild(pad);
                ++cell;
            }

            for (int d = 1; d <= dim; ++d) {
                // Count events on this day
                int ev_count = 0;
                for (const auto& e : events)
                    if (e.year == cur_year && e.month == cur_month && e.day == d)
                        ++ev_count;

                bool is_today = (cur_year == today_y &&
                                 cur_month == today_m && d == today_d);

                // Day number label
                auto day_num = std::make_shared<TextWidget>(
                    std::to_string(d), 12.0f);
                day_num->setColor(is_today
                    ? Color{0.3f, 0.7f, 1.0f, 1.0f}
                    : Color{0.85f, 0.85f, 0.85f, 1.0f});

                // Event dot row (up to 3 dots)
                auto dots_row = std::make_shared<FlexRow>(
                    std::vector<std::shared_ptr<Widget>>{});
                int shown = std::min(ev_count, 3);
                for (int i = 0; i < shown; ++i) {
                    auto dot = std::make_shared<TextWidget>("•", 10.0f);
                    dot->setColor(Color{0.4f, 0.75f, 0.4f, 1.0f});
                    dots_row->addChild(dot);
                }
                if (ev_count > 3) {
                    auto more = std::make_shared<TextWidget>(
                        "+" + std::to_string(ev_count - 3), 9.0f);
                    more->setColor(Color{0.55f, 0.55f, 0.55f, 1.0f});
                    dots_row->addChild(more);
                }

                auto cell_col = std::make_shared<FlexColumn>(
                    std::vector<std::shared_ptr<Widget>>{day_num, dots_row});
                cell_col->setPadding(4.0f);
                cell_col->setFlexGrow(1.0f);

                int cap_d = d, cap_y = cur_year, cap_m = cur_month;
                auto day_btn = std::make_shared<ButtonWidget>(cell_col,
                    [cap_y, cap_m, cap_d]() {
                        open_day(cap_y, cap_m, cap_d);
                    });
                day_btn->setBackgroundColor(is_today
                    ? Color{0.12f, 0.20f, 0.30f, 1.0f}
                    : Color{0.10f, 0.10f, 0.12f, 1.0f});
                day_btn->setFlexGrow(1.0f);

                week_row->addChild(day_btn);
                ++cell;

                if (cell % 7 == 0) flush_row();
            }

            // Flush last partial row
            if (cell % 7 != 0) flush_row();

        } else if (cur_view == View::Week) {
            // ── Week view ─────────────────────────────────────────────────────
            // Find Sunday of the current week containing cur_day
            std::tm base_tm{};
            base_tm.tm_year = cur_year - 1900;
            base_tm.tm_mon  = cur_month - 1;
            base_tm.tm_mday = cur_day;
            base_tm.tm_isdst = -1;
            std::mktime(&base_tm);
            int dow = base_tm.tm_wday; // 0=Sun

            // Column headers: Sun through Sat
            auto col_hdr = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{
                    // time gutter
                    std::make_shared<ContainerWidget>(40.0f, 0.0f, nullptr),
                });
            for (int i = 0; i < 7; ++i) {
                std::tm d_tm = base_tm;
                d_tm.tm_mday += i - dow;
                std::mktime(&d_tm);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%s %d",
                    kDayAbbr[i], d_tm.tm_mday);
                auto lbl = std::make_shared<TextWidget>(buf, 11.0f);
                lbl->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});
                lbl->setFlexGrow(1.0f);
                col_hdr->addChild(lbl);
            }
            col_hdr->setBackgroundColor(Color{0.13f, 0.13f, 0.15f, 1.0f});
            col_hdr->setPadding(4.0f);
            content_area->addChild(col_hdr);
            content_area->addChild(std::make_shared<DividerWidget>());

            // Hour rows
            auto hour_col = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{});
            for (int hr = 0; hr < 24; ++hr) {
                char time_buf[8];
                std::snprintf(time_buf, sizeof(time_buf),
                    hr == 0 ? "12 AM" : hr < 12 ? "%d AM" :
                    hr == 12 ? "12 PM" : "%d PM",
                    hr <= 12 ? hr : hr - 12);
                auto time_lbl = std::make_shared<TextWidget>(time_buf, 10.0f);
                time_lbl->setColor(Color{0.45f, 0.45f, 0.45f, 1.0f});
                time_lbl->setFixedSize(40.0f, 0.0f);

                auto hr_row = std::make_shared<FlexRow>(
                    std::vector<std::shared_ptr<Widget>>{time_lbl});
                hr_row->setFlexGrow(1.0f);

                for (int col = 0; col < 7; ++col) {
                    std::tm d_tm = base_tm;
                    d_tm.tm_mday += col - dow;
                    std::mktime(&d_tm);
                    int col_year  = d_tm.tm_year + 1900;
                    int col_month = d_tm.tm_mon  + 1;
                    int col_day   = d_tm.tm_mday;

                    // Find events at this hour/day
                    std::string ev_str;
                    for (const auto& e : events) {
                        if (e.year == col_year && e.month == col_month &&
                            e.day  == col_day  && e.hour == hr) {
                            ev_str = e.title.substr(0, 10);
                            break;
                        }
                    }

                    Color bg = ev_str.empty()
                        ? Color{0.10f, 0.10f, 0.12f, 1.0f}
                        : Color{0.16f, 0.30f, 0.16f, 1.0f};
                    auto cell_lbl = std::make_shared<TextWidget>(ev_str, 10.0f);
                    cell_lbl->setColor(Color{0.7f, 0.95f, 0.7f, 1.0f});
                    int cy=col_year, cm=col_month, cd=col_day;
                    auto cell_btn = std::make_shared<ButtonWidget>(cell_lbl,
                        [cy, cm, cd](){ open_day(cy, cm, cd); });
                    cell_btn->setBackgroundColor(bg);
                    cell_btn->setFlexGrow(1.0f);
                    hr_row->addChild(std::make_shared<DividerWidget>());
                    hr_row->addChild(cell_btn);
                }

                hr_row->setFixedSize(0.0f, 28.0f);
                hour_col->addChild(hr_row);
                hour_col->addChild(std::make_shared<DividerWidget>(0.5f));
            }

            auto scroll = std::make_shared<ScrollViewWidget>(hour_col);
            scroll->setFlexGrow(1.0f);
            content_area->addChild(scroll);

        } else {
            // ── Day view ──────────────────────────────────────────────────────
            auto day_col = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{});

            // Collect events for this day
            std::vector<const CalEvent*> day_events;
            for (const auto& e : events)
                if (e.year == cur_year && e.month == cur_month && e.day == cur_day)
                    day_events.push_back(&e);
            std::sort(day_events.begin(), day_events.end(),
                [](const CalEvent* a, const CalEvent* b){
                    return a->hour != b->hour ? a->hour < b->hour
                                              : a->minute < b->minute;
                });

            if (day_events.empty()) {
                auto empty = std::make_shared<TextWidget>(
                    "No events today", 13.0f);
                empty->setColor(Color{0.4f, 0.4f, 0.45f, 1.0f});
                day_col->addChild(std::make_shared<ContainerWidget>(16.0f, 0.0f, empty));
            }

            for (const auto* ep : day_events) {
                char time_str[16];
                std::snprintf(time_str, sizeof(time_str), "%02d:%02d–%02d:%02d",
                    ep->hour, ep->minute, ep->end_hour, ep->end_minute);

                auto time_lbl = std::make_shared<TextWidget>(time_str, 11.0f);
                time_lbl->setColor(Color{0.55f, 0.55f, 0.55f, 1.0f});
                time_lbl->setFixedSize(90.0f, 0.0f);

                auto title_w = std::make_shared<TextWidget>(ep->title, 13.0f);
                title_w->setColor(Color{0.9f, 0.9f, 0.9f, 1.0f});
                title_w->setFlexGrow(1.0f);

                // Delete button
                uint32_t ev_id = ep->id;
                auto del_btn = std::make_shared<ButtonWidget>(
                    std::make_shared<TextWidget>(" ✕ ", 11.0f),
                    [ev_id]() {
                        if (!g_xbus) return;
                        CalendarDeleteRequest req{};
                        req.event_id_ = ev_id;
                        g_xbus->Call("com.freebsdx.Calendar",
                            static_cast<uint32_t>(CalendarOpcode::DeleteEvent),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [](const XBusMessage&) {
                                load_events();
                                rebuild_view();
                            });
                    });
                del_btn->setBackgroundColor(Color{0.35f, 0.12f, 0.12f, 1.0f});

                auto ev_row = std::make_shared<FlexRow>(
                    std::vector<std::shared_ptr<Widget>>{
                        time_lbl,
                        title_w,
                        del_btn,
                    });
                ev_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
                ev_row->setPadding(8.0f);
                ev_row->setGap(8.0f);
                ev_row->setBackgroundColor(Color{0.14f, 0.22f, 0.14f, 1.0f});

                // Show location/description if present
                if (!ep->location.empty() || !ep->description.empty()) {
                    auto detail = std::make_shared<TextWidget>(
                        (ep->location.empty() ? "" : "📍 " + ep->location + "  ") +
                        ep->description.substr(0, 60), 11.0f);
                    detail->setColor(Color{0.55f, 0.70f, 0.55f, 1.0f});
                    auto detail_col = std::make_shared<FlexColumn>(
                        std::vector<std::shared_ptr<Widget>>{ev_row, detail});
                    detail_col->setGap(2.0f);
                    detail_col->setPadding(4.0f);
                    day_col->addChild(detail_col);
                } else {
                    day_col->addChild(ev_row);
                }
                day_col->addChild(std::make_shared<Spacer>(0, 4));
            }

            // Add Event button at bottom of day view
            auto add_ev_btn = std::make_shared<ButtonWidget>(
                std::make_shared<TextWidget>("+ Add Event", 13.0f),
                []() {
                    CalEvent tmpl{};
                    tmpl.year     = (uint16_t)cur_year;
                    tmpl.month    = (uint8_t)cur_month;
                    tmpl.day      = (uint8_t)cur_day;
                    tmpl.hour     = 9;
                    tmpl.end_hour = 10;
                    show_add_edit(tmpl);
                });
            add_ev_btn->setBackgroundColor(Color{0.22f, 0.48f, 0.22f, 1.0f});
            add_ev_btn->setPadding(10.0f);
            day_col->addChild(std::make_shared<Spacer>(0, 12));
            day_col->addChild(add_ev_btn);

            day_col->setPadding(12.0f);
            day_col->setGap(4.0f);
            auto scroll = std::make_shared<ScrollViewWidget>(day_col);
            scroll->setFlexGrow(1.0f);
            content_area->addChild(scroll);
        }
    };

    // ── Toolbar ───────────────────────────────────────────────────────────────
    auto prev_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>(" ← ", 14.0f),
        []() {
            if (cur_view == View::Day) {
                if (--cur_day < 1) {
                    if (--cur_month < 1) { cur_month=12; --cur_year; }
                    cur_day = calendar::daysInMonth(cur_year, cur_month);
                }
            } else {
                if (--cur_month < 1) { cur_month = 12; --cur_year; }
                cur_day = 1;
            }
            if (header_title) {
                if (cur_view == View::Day)
                    header_title->setText(std::string(kMonthNames[cur_month]) +
                        " " + std::to_string(cur_day) + ", " +
                        std::to_string(cur_year));
                else
                    header_title->setText(std::string(kMonthNames[cur_month]) +
                        " " + std::to_string(cur_year));
            }
            load_events();
        });
    prev_btn->setBackgroundColor(Color{0.18f, 0.18f, 0.20f, 1.0f});

    auto next_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>(" → ", 14.0f),
        []() {
            if (cur_view == View::Day) {
                if (++cur_day > calendar::daysInMonth(cur_year, cur_month)) {
                    cur_day = 1;
                    if (++cur_month > 12) { cur_month = 1; ++cur_year; }
                }
            } else {
                if (++cur_month > 12) { cur_month = 1; ++cur_year; }
                cur_day = 1;
            }
            if (header_title) {
                if (cur_view == View::Day)
                    header_title->setText(std::string(kMonthNames[cur_month]) +
                        " " + std::to_string(cur_day) + ", " +
                        std::to_string(cur_year));
                else
                    header_title->setText(std::string(kMonthNames[cur_month]) +
                        " " + std::to_string(cur_year));
            }
            load_events();
        });
    next_btn->setBackgroundColor(Color{0.18f, 0.18f, 0.20f, 1.0f});

    header_title = std::make_shared<TextWidget>(
        std::string(kMonthNames[cur_month]) + " " + std::to_string(cur_year),
        15.0f);
    header_title->setColor(Color{0.95f, 0.95f, 0.95f, 1.0f});
    header_title->setFlexGrow(1.0f);

    // View selector buttons
    auto mk_view_btn = [](const char* label, View v) {
        auto btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>(label, 12.0f),
            [v]() {
                cur_view = v;
                if (header_title) {
                    if (v == View::Day)
                        header_title->setText(
                            std::string(kMonthNames[cur_month]) + " " +
                            std::to_string(cur_day) + ", " +
                            std::to_string(cur_year));
                    else
                        header_title->setText(
                            std::string(kMonthNames[cur_month]) + " " +
                            std::to_string(cur_year));
                }
                rebuild_view();
            });
        btn->setBackgroundColor(Color{0.20f, 0.20f, 0.23f, 1.0f});
        btn->setPadding(6.0f);
        return btn;
    };

    auto month_btn = mk_view_btn("Month", View::Month);
    auto week_btn  = mk_view_btn("Week",  View::Week);
    auto day_btn   = mk_view_btn("Day",   View::Day);

    // Add Event (from toolbar — uses current date)
    auto add_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("+ Event", 12.0f),
        []() {
            CalEvent tmpl{};
            tmpl.year     = (uint16_t)cur_year;
            tmpl.month    = (uint8_t)cur_month;
            tmpl.day      = (uint8_t)cur_day;
            tmpl.hour     = 9;
            tmpl.end_hour = 10;
            show_add_edit(tmpl);
        });
    add_btn->setBackgroundColor(Color{0.22f, 0.50f, 0.22f, 1.0f});
    add_btn->setPadding(6.0f);

    // ICS import button
    static std::shared_ptr<TextInputWidget> ics_input;
    ics_input = std::make_shared<TextInputWidget>("/path/to/file.ics");
    ics_input->setFixedSize(200.0f, 0.0f);

    static std::shared_ptr<TextWidget> import_status;
    import_status = std::make_shared<TextWidget>("", 11.0f);
    import_status->setColor(Color{0.55f, 0.8f, 0.55f, 1.0f});

    auto import_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Import .ics", 12.0f),
        []() {
            if (!g_xbus) return;
            CalendarImportRequest req{};
            std::strncpy(req.path_, ics_input->text().c_str(),
                         sizeof(req.path_) - 1);
            g_xbus->Call("com.freebsdx.Calendar",
                static_cast<uint32_t>(CalendarOpcode::ImportIcs),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [](const XBusMessage& resp) {
                    if (resp.opcode_ !=
                        static_cast<uint32_t>(CalendarOpcode::ImportIcs)) return;
                    CalendarImportResult res{};
                    std::memcpy(&res, resp.payload_, sizeof(res));
                    if (import_status)
                        import_status->setText(
                            "Imported " + std::to_string(res.imported_) +
                            " event(s).");
                    load_events();
                });
        });
    import_btn->setBackgroundColor(Color{0.20f, 0.35f, 0.55f, 1.0f});
    import_btn->setPadding(6.0f);

    auto toolbar = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        prev_btn,
        std::make_shared<Spacer>(4, 0),
        next_btn,
        std::make_shared<Spacer>(8, 0),
        header_title,
        std::make_shared<Spacer>(8, 0),
        month_btn,
        std::make_shared<Spacer>(4, 0),
        week_btn,
        std::make_shared<Spacer>(4, 0),
        day_btn,
        std::make_shared<Spacer>(12, 0),
        add_btn,
    });
    toolbar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    toolbar->setPadding(6.0f);
    toolbar->setGap(0.0f);
    toolbar->setBackgroundColor(Color{0.14f, 0.14f, 0.16f, 1.0f});
    toolbar->setFixedSize(0.0f, 44.0f);
    toolbar->setFlexGrow(1.0f);

    // ICS import bar
    auto import_bar = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ics_input,
        std::make_shared<Spacer>(6, 0),
        import_btn,
        std::make_shared<Spacer>(8, 0),
        import_status,
    });
    import_bar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    import_bar->setPadding(4.0f);
    import_bar->setGap(0.0f);
    import_bar->setBackgroundColor(Color{0.11f, 0.11f, 0.13f, 1.0f});
    import_bar->setFixedSize(0.0f, 36.0f);
    import_bar->setFlexGrow(1.0f);

    // ── Content area ──────────────────────────────────────────────────────────
    content_area = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    content_area->setFlexGrow(1.0f);
    content_area->setBackgroundColor(Color{0.09f, 0.09f, 0.11f, 1.0f});

    // ── Root ──────────────────────────────────────────────────────────────────
    auto root = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        toolbar,
        std::make_shared<DividerWidget>(1.0f),
        import_bar,
        std::make_shared<DividerWidget>(0.5f),
        content_area,
    });
    root->setGap(0.0f);
    root->setBackgroundColor(Color{0.09f, 0.09f, 0.11f, 1.0f});

    return XApplication::Run(root, "/tmp/freebsdx.sock", /*sandboxed=*/false,
                             /*is_shell=*/false,
        [](XBusClient& xbus) {
            g_xbus = &xbus;
            XApplication::setTitle("Calendar");

            // Subscribe to EventReminder broadcasts → fire notification
            xbus.OnBroadcast([](const XBusMessage& msg) {
                if (std::strcmp(msg.service_name_, "com.freebsdx.Calendar") != 0) return;
                if (msg.opcode_ !=
                    static_cast<uint32_t>(CalendarOpcode::EventReminder)) return;

                CalendarEventPage ep{};
                std::memcpy(&ep, msg.payload_, sizeof(ep));
                ep.title_[sizeof(ep.title_) - 1] = '\0';

                // Post notification via com.freebsdx.Notification
                // (g_xbus is available here)
                NotificationPayload note{};
                std::strncpy(note.title_, "Upcoming Event", sizeof(note.title_)-1);
                std::snprintf(note.message_, sizeof(note.message_)-1,
                    "%s at %02d:%02d", ep.title_, ep.hour_, ep.minute_);
                note.timeout_ms_ = 8000;
                g_xbus->Call("com.freebsdx.Notification",
                    static_cast<uint32_t>(NotificationOpcode::Post),
                    {reinterpret_cast<const uint8_t*>(&note), sizeof(note)},
                    nullptr);
            });

            // Accept drag-and-drop of .ics files from Files app
            XApplication::setOnDrop("text/uri-list",
                [](float, float, const std::string& data) {
                    if (!g_xbus) return;
                    std::string path = data;
                    while (!path.empty() &&
                           (path.back()=='\n' || path.back()=='\r'))
                        path.pop_back();
                    if (path.starts_with("file://")) path = path.substr(7);
                    if (!path.ends_with(".ics")) return;
                    CalendarImportRequest req{};
                    std::strncpy(req.path_, path.c_str(), sizeof(req.path_)-1);
                    g_xbus->Call("com.freebsdx.Calendar",
                        static_cast<uint32_t>(CalendarOpcode::ImportIcs),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [](const XBusMessage& resp) {
                            if (import_status && resp.opcode_ ==
                                static_cast<uint32_t>(CalendarOpcode::ImportIcs)) {
                                CalendarImportResult res{};
                                std::memcpy(&res, resp.payload_, sizeof(res));
                                import_status->setText(
                                    "Imported " +
                                    std::to_string(res.imported_) +
                                    " event(s) via drag-and-drop.");
                            }
                            load_events();
                        });
                });

            // Initial load
            load_events();
            rebuild_view();
        });
}
