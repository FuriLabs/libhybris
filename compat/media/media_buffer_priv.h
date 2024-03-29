/*
 * Copyright (C) 2016 Canonical Ltd
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
 *
 * Authored by: Simon Fels <simon.fels@canonical.com>
 */

#ifndef MEDIA_BUFFER_PRIV_H_
#define MEDIA_BUFFER_PRIV_H_

#include <hybris/media/media_buffer_layer.h>

#if ANDROID_VERSION_MAJOR>=8
#include <media/MediaCodecBuffer.h>
#endif
#include <media/stagefright/foundation/ABuffer.h>

#include <media/stagefright/MediaBuffer.h>

struct MediaBufferPrivate : public android::MediaBufferObserver
{
public:
    static MediaBufferPrivate* toPrivate(MediaBufferWrapper *source);

#if ANDROID_VERSION_MAJOR>=8
    MediaBufferPrivate(android::MediaBufferBase *data, bool managedByWrapper = true);
#else
    MediaBufferPrivate(android::MediaBuffer *data, bool managedByWrapper = true);
#endif
    MediaBufferPrivate();
    ~MediaBufferPrivate();

#if ANDROID_VERSION_MAJOR>=8
    void signalBufferReturned(android::MediaBufferBase *buffer);
    
    android::MediaBufferBase *buffer;
#else
    void signalBufferReturned(android::MediaBuffer *buffer);

    android::MediaBuffer *buffer;
#endif
    MediaBufferReturnCallback return_callback;
    void *return_callback_data;
    bool isBufferManagedByWrapper;
};

struct MediaABufferPrivate
{
public:
    static MediaABufferPrivate* toPrivate(MediaABufferWrapper *source);

    MediaABufferPrivate();
#if ANDROID_VERSION_MAJOR>=8
    MediaABufferPrivate(android::sp<android::MediaCodecBuffer> buffer);
#else
    MediaABufferPrivate(android::sp<android::ABuffer> buffer);
#endif

public:
#if ANDROID_VERSION_MAJOR>=8
    android::sp<android::MediaCodecBuffer> buffer;
#else
    android::sp<android::ABuffer> buffer;
#endif
};

#endif
