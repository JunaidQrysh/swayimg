// SPDX-License-Identifier: MIT
// Touchpad fling (kinetic scrolling) velocity estimation.

#pragma once

#include <cstdint>
#include <deque>
#include <utility>

class FlingTracker {
public:
    /**
     * Add scroll sample.
     * @param time event timestamp in milliseconds (compositor clock)
     * @param dx,dy scroll distance in pixels
     */
    void add(const uint32_t time, const double dx, const double dy)
    {
        samples.push_back({ time, dx, dy });
        while (samples.size() > 1 && time - samples.front().time > WINDOW) {
            samples.pop_front();
        }
    }

    /**
     * Finger lifted: get release velocity and reset tracker.
     * @param time timestamp of the release event in milliseconds
     * @return velocity in pixels per second (x, y)
     */
    std::pair<double, double> release(const uint32_t time)
    {
        std::pair<double, double> velocity { 0.0, 0.0 };
        if (samples.size() >= 2) {
            const uint32_t last = samples.back().time;
            const uint32_t span = last - samples.front().time;
            if (time - last <= MAX_REST && span >= MIN_SPAN) {
                // the oldest sample only marks the start of the interval
                double dx = 0;
                double dy = 0;
                for (size_t i = 1; i < samples.size(); ++i) {
                    dx += samples[i].dx;
                    dy += samples[i].dy;
                }
                velocity.first = dx * 1000.0 / span;
                velocity.second = dy * 1000.0 / span;
            }
        }
        samples.clear();
        return velocity;
    }

    /**
     * Check if there are no samples (no gesture in progress).
     * @return true if tracker is empty
     */
    [[nodiscard]] bool empty() const { return samples.empty(); }

private:
    // Time window used to estimate finger velocity (ms)
    static constexpr uint32_t WINDOW = 100;
    // No fling if the finger rested before lifting longer than this (ms)
    static constexpr uint32_t MAX_REST = 40;
    // Min time span of samples to estimate velocity (ms)
    static constexpr uint32_t MIN_SPAN = 10;

    struct Sample {
        uint32_t time;
        double dx, dy;
    };
    std::deque<Sample> samples;
};
