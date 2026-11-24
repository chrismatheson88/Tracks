#include "CalendarPopupComponent.h"
#include "../../Common/AppTheme.h"
#include <iomanip>
#include <sstream>

namespace tracks {

CalendarPopupComponent::CalendarPopupComponent(const juce::String& initialDateStr) {
    auto now = juce::Time::getCurrentTime();
    todayYear_ = now.getYear();
    todayMonth_ = now.getMonth();
    todayDay_ = now.getDayOfMonth();

    parseInitialDate(initialDateStr);

    // Prev / Next buttons
    prevMonthButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    prevMonthButton_.onClick = [this]() {
        viewMonth_--;
        if (viewMonth_ < 0) {
            viewMonth_ = 11;
            viewYear_--;
        }
        updateMonthView();
    };
    addAndMakeVisible(prevMonthButton_);

    nextMonthButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    nextMonthButton_.onClick = [this]() {
        viewMonth_++;
        if (viewMonth_ > 11) {
            viewMonth_ = 0;
            viewYear_++;
        }
        updateMonthView();
    };
    addAndMakeVisible(nextMonthButton_);

    // Month & Year Label
    monthYearLabel_.setFont(juce::Font(13.0f, juce::Font::bold));
    monthYearLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    monthYearLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(monthYearLabel_);

    // Today Button
    todayButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    todayButton_.onClick = [this]() {
        viewYear_ = todayYear_;
        viewMonth_ = todayMonth_;
        selectDay(todayDay_);
    };
    addAndMakeVisible(todayButton_);

    // Day buttons
    for (int i = 0; i < totalCells; ++i) {
        dayButtons_[i].setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
        addAndMakeVisible(dayButtons_[i]);
    }

    updateMonthView();
    setSize(260, 240);
}

void CalendarPopupComponent::parseInitialDate(const juce::String& str) {
    auto parts = juce::StringArray::fromTokens(str.trim(), "-", "");
    if (parts.size() == 3) {
        int y = parts[0].getIntValue();
        int m = parts[1].getIntValue() - 1; // 1-based to 0-based
        int d = parts[2].getIntValue();
        if (y >= 1970 && m >= 0 && m <= 11 && d >= 1 && d <= 31) {
            selectedYear_ = y;
            selectedMonth_ = m;
            selectedDay_ = d;
            viewYear_ = y;
            viewMonth_ = m;
            return;
        }
    }
    // Default to today
    selectedYear_ = todayYear_;
    selectedMonth_ = todayMonth_;
    selectedDay_ = todayDay_;
    viewYear_ = todayYear_;
    viewMonth_ = todayMonth_;
}

int CalendarPopupComponent::getDaysInMonth(int year, int month) {
    if (month == 1) { // February
        bool isLeap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return isLeap ? 29 : 28;
    }
    if (month == 3 || month == 5 || month == 8 || month == 10) // April, June, Sept, Nov
        return 30;
    return 31;
}

void CalendarPopupComponent::updateMonthView() {
    juce::Time firstDay(viewYear_, viewMonth_, 1, 12, 0);
    monthYearLabel_.setText(firstDay.getMonthName(false) + " " + juce::String(viewYear_), juce::dontSendNotification);

    int startDayOfWeek = firstDay.getDayOfWeek(); // 0 = Sun, 6 = Sat
    int daysInCurrentMonth = getDaysInMonth(viewYear_, viewMonth_);

    for (int i = 0; i < totalCells; ++i) {
        int dayNum = i - startDayOfWeek + 1;
        if (dayNum >= 1 && dayNum <= daysInCurrentMonth) {
            dayButtons_[i].setVisible(true);
            dayButtons_[i].setButtonText(juce::String(dayNum));

            bool isSelected = (viewYear_ == selectedYear_ && viewMonth_ == selectedMonth_ && dayNum == selectedDay_);
            bool isToday = (viewYear_ == todayYear_ && viewMonth_ == todayMonth_ && dayNum == todayDay_);

            if (isSelected) {
                dayButtons_[i].setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
            } else if (isToday) {
                dayButtons_[i].setColour(juce::TextButton::buttonColourId, AppTheme::accentGreen.withAlpha(0.4f));
            } else {
                dayButtons_[i].setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
            }

            dayButtons_[i].onClick = [this, dayNum]() {
                selectDay(dayNum);
            };
        } else {
            dayButtons_[i].setVisible(false);
        }
    }
}

void CalendarPopupComponent::selectDay(int day) {
    selectedYear_ = viewYear_;
    selectedMonth_ = viewMonth_;
    selectedDay_ = day;

    std::ostringstream oss;
    oss << selectedYear_ << "-"
        << std::setw(2) << std::setfill('0') << (selectedMonth_ + 1) << "-"
        << std::setw(2) << std::setfill('0') << selectedDay_;

    juce::String formattedDate = oss.str();
    if (onDateSelected) {
        onDateSelected(formattedDate);
    }

    // Dismiss callout box if inside one
    if (auto* callout = findParentComponentOfClass<juce::CallOutBox>()) {
        callout->dismiss();
    }
}

void CalendarPopupComponent::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgPanel);

    // Days of week header
    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(AppTheme::textMuted);

    const char* days[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    float colW = static_cast<float>(getWidth() - 16) / 7.0f;
    for (int i = 0; i < 7; ++i) {
        g.drawText(days[i], 8.0f + static_cast<float>(i) * colW, 36.0f, colW, 16.0f, juce::Justification::centred);
    }

    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(54, 8.0f, static_cast<float>(getWidth() - 8));
}

void CalendarPopupComponent::resized() {
    auto area = getLocalBounds().reduced(8);

    // Header: Prev, Month Year, Next, Today
    auto header = area.removeFromTop(26);
    prevMonthButton_.setBounds(header.removeFromLeft(24));
    header.removeFromLeft(4);
    nextMonthButton_.setBounds(header.removeFromLeft(24));
    header.removeFromLeft(4);
    todayButton_.setBounds(header.removeFromRight(46));
    monthYearLabel_.setBounds(header);

    area.removeFromTop(22); // space for days of week header painted in paint()

    // 7x6 Grid of day buttons
    float colW = static_cast<float>(area.getWidth()) / 7.0f;
    float rowH = static_cast<float>(area.getHeight()) / 6.0f;

    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 7; ++c) {
            int idx = r * 7 + c;
            int x = area.getX() + static_cast<int>(static_cast<float>(c) * colW + 1.0f);
            int y = area.getY() + static_cast<int>(static_cast<float>(r) * rowH + 1.0f);
            int w = static_cast<int>(colW - 2.0f);
            int h = static_cast<int>(rowH - 2.0f);
            dayButtons_[idx].setBounds(x, y, w, h);
        }
    }
}

} // namespace tracks
