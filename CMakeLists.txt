cmake_minimum_required(VERSION 3.22)
project(Tracks VERSION 1.0.0 LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE/CMakeLists.txt")
    add_subdirectory(ThirdParty/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

find_package(SQLite3 REQUIRED)

# Embedded audio binary data (clicks and female guide voice)
juce_add_binary_data(AudioBinaryData
    SOURCES
        assets/clicks/tone/quarter.wav
        assets/clicks/tone/accent.wav
        assets/clicks/tone/eighth.wav
        assets/clicks/tone/sixteenth.wav
        assets/guide/female/count-1.wav
        assets/guide/female/count-2.wav
        assets/guide/female/count-3.wav
        assets/guide/female/count-4.wav
        assets/guide/female/count-5.wav
        assets/guide/female/count-6.wav
        assets/guide/female/count-7.wav
        assets/guide/female/count-8.wav
        assets/icon/TracksIcon.png
        assets/icon/TracksLogo.png
)

juce_add_gui_app(Tracks
    PRODUCT_NAME "Tracks"
    COMPANY_NAME "Tracks"
    BUNDLE_ID "com.tracks.desktop"
    ICON_BIG assets/icon/TracksIcon.png
)

target_sources(Tracks PRIVATE
    Source/Main.cpp
    Source/MainComponent.h
    Source/MainComponent.cpp
    Source/Common/AppTheme.h
    Source/Common/AppTheme.cpp
    Source/Common/MacDockIcon.h
    Source/Common/MacDockIcon.mm
    Source/Models/AvailableSections.h
    Source/Models/Song.h
    Source/Models/SongTrack.h
    Source/Models/Setlist.h
    Source/Models/SetlistEntry.h
    Source/Database/DatabaseService.h
    Source/Database/DatabaseService.cpp
    Source/Audio/AudioEngine.h
    Source/Audio/AudioEngine.cpp
    Source/Audio/Metronome.h
    Source/Audio/Metronome.cpp
    Source/Audio/PitchShiftUtils.h
    Source/Audio/SectionJumpUtils.h
    Source/Services/AudioAttachmentService.h
    Source/Services/AudioAttachmentService.cpp
    Source/UI/Widgets/BeatPlayheadComponent.h
    Source/UI/Widgets/BeatPlayheadComponent.cpp
    Source/UI/Widgets/CalendarPopupComponent.h
    Source/UI/Widgets/CalendarPopupComponent.cpp
    Source/UI/Widgets/TimePopupComponent.h
    Source/UI/Widgets/TimePopupComponent.cpp
    Source/UI/Widgets/OutputMeterComponent.h
    Source/UI/Widgets/OutputMeterComponent.cpp
    Source/UI/Widgets/TransportComponent.h
    Source/UI/Widgets/TransportComponent.cpp
    Source/UI/Views/LibraryView.h
    Source/UI/Views/LibraryView.cpp
    Source/UI/Views/OpenSetView.h
    Source/UI/Views/OpenSetView.cpp
    Source/UI/Views/SettingsView.h
    Source/UI/Views/SettingsView.cpp
)

target_include_directories(Tracks PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/signalsmith-stretch"
)

target_link_libraries(Tracks
    PRIVATE
        AudioBinaryData
        SQLite3::SQLite3
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
)

juce_generate_juce_header(Tracks)

# Unit / Integration Tests
add_executable(DatabaseTests
    Source/Tests/DatabaseTests.cpp
    Source/Database/DatabaseService.cpp
)
target_link_libraries(DatabaseTests PRIVATE SQLite3::SQLite3)

add_executable(AudioEngineTests
    Source/Tests/AudioEngineTests.cpp
    Source/Audio/Metronome.cpp
    Source/Services/AudioAttachmentService.cpp
)
target_include_directories(AudioEngineTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/signalsmith-stretch"
)

target_link_libraries(AudioEngineTests
    PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_audio_basics
        juce::juce_audio_formats
)

