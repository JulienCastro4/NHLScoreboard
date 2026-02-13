#pragma once

#include "display/scene.h"

class RecapScene : public Scene {
public:
    void render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t nowMs) override;
    void start(uint32_t nowMs, const GameSnapshot& data);
    bool isComplete(uint32_t nowMs) const;
    bool hasPages() const { return pageCount > 0; }

    static constexpr int kMaxPages = 40;

    enum class PageType : uint8_t {
        TitleIntro,
        TitleFinal,
        Score,
        TitleSog,
        Sog,
        TitleGoals,
        TeamGoalsTitle,
        GoalDetail
    };

    struct Page {
        PageType type;
        uint8_t teamIndex;
        uint8_t goalIndex;
    };

    static uint32_t totalContentDurationMs(const Page* pageList, int pageCount);

private:
    void rebuildPages(const GameSnapshot& data, uint32_t nowMs);
    void clearPages();

    uint32_t lastTextHash = 0;
    uint32_t startMs = 0;
    uint32_t transitionStartMs = 0;
    int pageCount = 0;
    int cachedPageCount = 0;
    int lastPageIndex = -1;
    int previousPageIndex = -1;
    Page pages[kMaxPages] = {};
};
