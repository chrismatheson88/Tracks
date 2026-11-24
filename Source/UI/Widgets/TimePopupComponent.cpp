#include "TimePopupComponent.h"
#include "../../Common/AppTheme.h"
#include <iomanip>
#include <sstream>

namespace tracks {

TimePopupComponent::TimePopupComponent(const juce::String& initialTimeStr) {
    parseInitialTime(initialTimeStr);

    // AM / PM buttons
    amButton_.onClick = [this]() {
        isPm_ = false;
        updateButtonHighlights();
    };
    addAndMakeVisible(amButton_);

    pmButton_.onClick = [this]() {
        isPm_ = true;
        updateButtonHighlights();
    };
    addAndMakeVisible(pmButton_);

    // Now button
    nowButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    nowButton_.onClick = [this]() {
        auto now = juce::Time::getCurrentTime();
        int h = now.getHours();
        isPm_ = (h >= 12);
        selectedHour_ = h % 12;
        if (selectedHour_ == 0) selectedHour_ = 12;

        int m = now.getMinutes();
        if (m < 8) selectedMinute_ = 0;
        else if (m < 23) selectedMinute_ = 15;
        else if (m < 38) selectedMinute_ = 30;
        else if (m < 53) selectedMinute_ = 45;
        else {
            selectedMinute_ = 0;
            selectedHour_ = (selectedHour_ % 12) + 1;
        }

        updateButtonHighlights();
        finishSelection();
    };
    addAndMakeVisible(nowButton_);

    // 12 Hours: 1 through 12
    for (int i = 0; i < 12; ++i) {
        int h = i + 1;
        hourButtons_[i].setButtonText(juce::String(h));
        hourButtons_[i].onClick = [this, h]() {
            selectedHour_ = h;
            updateButtonHighlights();
        };
        addAndMakeVisible(hourButtons_[i]);
    }

    // 4 Minutes: :00, :15, :30, :45
    const char* minLabels[4] = { ":00", ":15", ":30", ":45" };
    const int minVals[4] = { 0, 15, 30, 45 };
    for (int i = 0; i < 4; ++i) {
        int m = minVals[i];
        minuteButtons_[i].setButtonText(minLabels[i]);
        minuteButtons_[i].onClick = [this, m]() {
            selectedMinute_ = m;
            updateButtonHighlights();
            finishSelection(); // Clicking minute auto-completes selection
        };
        addAndMakeVisible(minuteButtons_[i]);
    }

    updateButtonHighlights();
    setSize(250, 200);
}

void TimePopupComponent::parseInitialTime(const juce::String& str) {
    juce::String s = str.trim().toLowerCase();
    if (s.isEmpty()) {
        selectedHour_ = 10;
        selectedMinute_ = 0;
        isPm_ = false;
        return;
    }

    isPm_ = s.contains("pm");

    // Remove AM/PM text
    s = s.replace("am", "").replace("pm", "").trim();

    auto parts = juce::StringArray::fromTokens(s, ":", "");
    if (!parts.isEmpty()) {
        int h = parts[0].getIntValue();
        if (h >= 13) {
            isPm_ = true;
            h -= 12;
        }
        if (h >= 1 && h <= 12) {
            selectedHour_ = h;
        }

        if (parts.size() >= 2) {
            int m = parts[1].getIntValue();
            if (m < 8) selectedMinute_ = 0;
            else if (m < 23) selectedMinute_ = 15;
            else if (m < 38) selectedMinute_ = 30;
            else selectedMinute_ = 45;
        }
    }
}

void TimePopupComponent::updateButtonHighlights() {
    amButton_.setColour(juce::TextButton::buttonColourId,
                        !isPm_ ? AppTheme::primaryBlue : AppTheme::bgInput);
    pmButton_.setColour(juce::TextButton::buttonColourId,
                        isPm_ ? AppTheme::primaryBlue : AppTheme::bgInput);

    for (int i = 0; i < 12; ++i) {
        int h = i + 1;
        bool isSel = (h == selectedHour_);
        hourButtons_[i].setColour(juce::TextButton::buttonColourId,
                                  isSel ? AppTheme::primaryBlue : AppTheme::bgInput);
    }

    const int minVals[4] = { 0, 15, 30, 45 };
    for (int i = 0; i < 4; ++i) {
        bool isSel = (minVals[i] == selectedMinute_);
        minuteButtons_[i].setColour(juce::TextButton::buttonColourId,
                                    isSel ? AppTheme::accentGreen : AppTheme::bgInput);
    }
}

void TimePopupComponent::finishSelection() {
    std::ostringstream oss;
    oss << selectedHour_ << ":"
        << std::setw(2) << std::setfill('0') << selectedMinute_
        << (isPm_ ? " PM" : " AM");

    juce::String formattedTime = oss.str();
    if (onTimeSelected) {
        onTimeSelected(formattedTime);
    }

    if (auto* callout = findParentComponentOfClass<juce::CallOutBox>()) {
        callout->dismiss();
    }
}

void TimePopupComponent::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgPanel);

    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(AppTheme::textMuted);
    g.drawText("HOUR", 10, 40, 140, 14, juce::Justification::centredLeft);
    g.drawText("MINUTES", 160, 40, 80, 14, juce::Justification::centredLeft);

    g.setColour(AppTheme::borderCol);
    g.drawVerticalLine(152, 40.0f, static_cast<float>(getHeight() - 8));
}

void TimePopupComponent::resized() {
    auto area = getLocalBounds().reduced(8);

    // Top Row: AM, PM, Now
    auto topRow = area.removeFromTop(26);
    amButton_.setBounds(topRow.removeFromLeft(44));
    topRow.removeFromLeft(4);
    pmButton_.setBounds(topRow.removeFromLeft(44));
    topRow.removeFromLeft(8);
    nowButton_.setBounds(topRow.removeFromRight(46));

    area.removeFromTop(22); // space for section headers in paint()

    // Left 140px: 4 rows x 3 columns for hours 1..12
    auto hourArea = area.removeFromLeft(138);
    float colW = static_cast<float>(hourArea.getWidth()) / 3.0f;
    float rowH = static_cast<float>(hourArea.getHeight()) / 4.0f;

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            int idx = r * 3 + c;
            int x = hourArea.getX() + static_cast<int>(static_cast<float>(c) * colW + 1.0f);
            int y = hourArea.getY() + static_cast<int>(static_cast<float>(r) * rowH + 1.0f);
            int w = static_cast<int>(colW - 2.0f);
            int h = static_cast<int>(rowH - 2.0f);
            hourButtons_[idx].setBounds(x, y, w, h);
        }
    }

    area.removeFromLeft(14); // divider space

    // Right ~80px: 4 rows for minutes :00, :15, :30, :45
    auto minArea = area;
    float minRowH = static_cast<float>(minArea.getHeight()) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        int y = minArea.getY() + static_cast<int>(static_cast<float>(i) * minRowH + 1.0f);
        int h = static_cast<int>(minRowH - 2.0f);
        minuteButtons_[i].setBounds(minArea.getX(), y, minArea.getWidth(), h);
    }
}

} // namespace tracks
