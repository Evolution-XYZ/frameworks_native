/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "FpsReporter"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <algorithm>

#include "FpsReporter.h"
#include "Layer.h"
#include "SurfaceFlinger.h"

namespace android {

FpsReporter::FpsReporter(frametimeline::FrameTimeline& frameTimeline, std::unique_ptr<Clock> clock)
      : mFrameTimeline(frameTimeline), mClock(std::move(clock)) {
    LOG_ALWAYS_FATAL_IF(mClock == nullptr, "Passed in null clock when constructing FpsReporter!");
}

void FpsReporter::dispatchLayerFps(const frontend::LayerHierarchy& layerHierarchy) {
    const auto now = mClock->now();
    if (now - mLastDispatch < kMinDispatchDuration) {
        return;
    }

    std::vector<TrackedListener> localListeners;
    {
        std::scoped_lock lock(mMutex);
        if (mListeners.empty()) {
            return;
        }

        std::transform(mListeners.begin(), mListeners.end(), std::back_inserter(localListeners),
                       [](const std::pair<wp<IBinder>, TrackedListener>& entry) {
                           return entry.second;
                       });
    }

    std::unordered_set<int32_t> seenTasks;
    std::vector<std::pair<TrackedListener, const frontend::LayerHierarchy*>>
            listenersAndLayersToReport;

    layerHierarchy.traverse([&](const frontend::LayerHierarchy& hierarchy,
                                const frontend::LayerHierarchy::TraversalPath& traversalPath) {
        if (traversalPath.variant == frontend::LayerHierarchy::Variant::Detached) {
            return false;
        }
        const auto& metadata = hierarchy.getLayer()->metadata;
        if (metadata.has(gui::METADATA_TASK_ID)) {
            int32_t taskId = metadata.getInt32(gui::METADATA_TASK_ID, 0);
            if (seenTasks.count(taskId) == 0) {
                // localListeners is expected to be tiny
                for (TrackedListener& listener : localListeners) {
                    if (listener.taskId == taskId) {
                        seenTasks.insert(taskId);
                        listenersAndLayersToReport.push_back({listener, &hierarchy});
                        break;
                    }
                }
            }
        }
        return true;
    });

    for (const auto& [listener, hierarchy] : listenersAndLayersToReport) {
        std::unordered_set<int32_t> layerIds;

        hierarchy->traverse([&](const frontend::LayerHierarchy& hierarchy,
                                const frontend::LayerHierarchy::TraversalPath& traversalPath) {
            if (traversalPath.variant == frontend::LayerHierarchy::Variant::Detached) {
                return false;
            }
            layerIds.insert(static_cast<int32_t>(hierarchy.getLayer()->id));
            return true;
        });

        listener.listener->onFpsReported(mFrameTimeline.computeFps(layerIds));
    }

    mLastDispatch = now;
}

void FpsReporter::binderDied(const wp<IBinder>& who) {
    std::scoped_lock lock(mMutex);
    mListeners.erase(who);
}

void FpsReporter::addListener(const sp<gui::IFpsListener>& listener, int32_t taskId) {
    sp<IBinder> asBinder = IInterface::asBinder(listener);
    asBinder->linkToDeath(sp<DeathRecipient>::fromExisting(this));
    std::lock_guard lock(mMutex);
    mListeners.emplace(wp<IBinder>(asBinder), TrackedListener{listener, taskId});
}

void FpsReporter::removeListener(const sp<gui::IFpsListener>& listener) {
    std::lock_guard lock(mMutex);
    mListeners.erase(wp<IBinder>(IInterface::asBinder(listener)));
}

} // namespace android
