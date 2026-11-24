#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace tracks {

class CalendarPopupComponent : public juce::Component {
public:
    explicit CalendarPopupComponent(const juce::String& initialDateStr = {});
    ~CalendarPopupComponent() override = default;

    std::function<void(const juce::String&)> onDateSelected;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int viewYear_ = 2026;
    int viewMonth_ = 10; // 0-based: 0 = Jan, 11 = Dec

    int selectedYear_ = 2026;
    int selectedMonth_ = 10;
    int selectedDay_ = 1;

    int todayYear_ = 2026;
    int todayMonth_ = 10;
    int todayDay_ = 1;

    juce::TextButton prevMonthButton_{"<"};
    juce::TextButton nextMonthButton_{">"};
    juce::Label monthYearLabel_;
    juce::TextButton todayButton_{"Today"};

    static constexpr int totalCells = 42; // 6 weeks * 7 days
    juce::TextButton dayButtons_[totalCells];

    void parseInitialDate(const juce::String& str);
    void updateMonthView();
    void selectDay(int day);
    static int getDaysInMonth(int year, int month);
};

} // namespace tracks
