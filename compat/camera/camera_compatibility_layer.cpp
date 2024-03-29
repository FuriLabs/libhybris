/*
 * Copyright (C) 2013-2014 Canonical Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Authored by: Thomas Voß <thomas.voss@canonical.com>
 *				Ricardo Salveti de Araujo <ricardo.salveti@canonical.com>
 *				Jim Hodapp <jim.hodapp@canonical.com>
 */

//#define LOG_NDEBUG 0

#include <hybris/internal/camera_control.h>
#include <hybris/camera/camera_compatibility_layer.h>
#include <hybris/camera/camera_compatibility_layer_capabilities.h>
#include <hybris/camera/camera_compatibility_layer_configuration_translator.h>

#include <hybris/internal/surface_flinger_compatibility_layer_internal.h>

#include <binder/ProcessState.h>
#include <camera/Camera.h>
#include <camera/CameraParameters.h>
#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=2
#include <gui/SurfaceTexture.h>
#else
#include <gui/GLConsumer.h>
#endif
#if ANDROID_VERSION_MAJOR>=5
#include <gui/IGraphicBufferProducer.h>
#endif
#include <ui/GraphicBuffer.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#undef LOG_TAG
#define LOG_TAG "CameraCompatibilityLayer"
#include <utils/Debug.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/Log.h>
#include <utils/String16.h>
#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
#include <gui/NativeBufferAlloc.h>
#endif

#include <cstring>

#define REPORT_FUNCTION() ALOGV("%s \n", __PRETTY_FUNCTION__)

using android::CompileTimeAssert; // So COMPILE_TIME_ASSERT works

// From android::GLConsumer::FrameAvailableListener
#if ANDROID_VERSION_MAJOR==5 && ANDROID_VERSION_MINOR>=1 || ANDROID_VERSION_MAJOR>=6
  void CameraControl::onFrameAvailable(const android::BufferItem& item)
#else
  void CameraControl::onFrameAvailable()
#endif
{
	REPORT_FUNCTION();
	if (listener)
		listener->on_preview_texture_needs_update_cb(listener->context);
}

// From android::CameraListener
void CameraControl::notify(int32_t msg_type, int32_t ext1, int32_t ext2)
{
	REPORT_FUNCTION();
	printf("\text1: %d, ext2: %d \n", ext1, ext2);

	if (!listener)
		return;

	switch (msg_type) {
	case CAMERA_MSG_ERROR:
		if (listener->on_msg_error_cb)
			listener->on_msg_error_cb(listener->context);
		break;
	case CAMERA_MSG_SHUTTER:
		if (listener->on_msg_shutter_cb)
			listener->on_msg_shutter_cb(listener->context);
		break;
	case CAMERA_MSG_ZOOM:
		if (listener->on_msg_zoom_cb)
			listener->on_msg_zoom_cb(listener->context, ext1);
		break;
	case CAMERA_MSG_FOCUS:
		if (listener->on_msg_focus_cb)
			listener->on_msg_focus_cb(listener->context);
		break;
	default:
		break;
	}
}

void CameraControl::postData(
		int32_t msg_type,
		const android::sp<android::IMemory>& data,
		camera_frame_metadata_t* metadata)
{
	REPORT_FUNCTION();

	if (!listener)
		return;

	switch (msg_type) {
	case CAMERA_MSG_RAW_IMAGE:
		if (listener->on_data_raw_image_cb)
#if ANDROID_VERSION_MAJOR>=11
			listener->on_data_raw_image_cb(data->unsecurePointer(), data->size(), listener->context);
#else
			listener->on_data_raw_image_cb(data->pointer(), data->size(), listener->context);
#endif
		break;
	case CAMERA_MSG_COMPRESSED_IMAGE:
		if (listener->on_data_compressed_image_cb)
#if ANDROID_VERSION_MAJOR>=11
			listener->on_data_compressed_image_cb(data->unsecurePointer(), data->size(), listener->context);
#else
			listener->on_data_compressed_image_cb(data->pointer(), data->size(), listener->context);
#endif
		break;
	case CAMERA_MSG_PREVIEW_FRAME:
		if (listener->on_preview_frame_cb)
#if ANDROID_VERSION_MAJOR>=11
			listener->on_preview_frame_cb(data->unsecurePointer(), data->size(), listener->context);
#else
			listener->on_preview_frame_cb(data->pointer(), data->size(), listener->context);
#endif
		break;
	default:
		break;
	}
}

void CameraControl::postDataTimestamp(
		nsecs_t timestamp,
		int32_t msg_type,
		const android::sp<android::IMemory>& data)
{
	REPORT_FUNCTION();
	(void) timestamp;
	(void) msg_type;
	(void) data;
}

#if ANDROID_VERSION_MAJOR >= 7
void CameraControl::postRecordingFrameHandleTimestamp(nsecs_t /*timestamp*/, native_handle_t* /*handle*/)
{
	REPORT_FUNCTION();
}
#endif

void CameraControl::postRecordingFrameHandleTimestampBatch(const std::vector<nsecs_t>& /*timestamps*/,
                                                const std::vector<native_handle_t*>& /*handles*/)
{
	REPORT_FUNCTION();
}

#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
namespace android
{
NativeBufferAlloc::NativeBufferAlloc() {
}

NativeBufferAlloc::~NativeBufferAlloc() {
}

sp<GraphicBuffer> NativeBufferAlloc::createGraphicBuffer(uint32_t w, uint32_t h,
		PixelFormat format, uint32_t usage, status_t* error) {
	sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(w, h, format, usage));
	status_t err = graphicBuffer->initCheck();
	*error = err;
	if (err != 0 || graphicBuffer->handle == 0) {
		if (err == NO_MEMORY) {
			GraphicBuffer::dumpAllocationsToSystemLog();
		}
		ALOGI("GraphicBufferAlloc::createGraphicBuffer(w=%d, h=%d) "
				"failed (%s), handle=%p",
				w, h, strerror(-err), graphicBuffer->handle);
		return 0;
	}
	return graphicBuffer;
}
}
#endif

static void setParameters_resilient(CameraControl* control)
{
	android::status_t err = control->camera->setParameters(control->camera_parameters.flatten());

	if (err != android::NO_ERROR) {
		// The parameters that we have is now stale. Reset to the current parameter
		// to prevent the bad value from blocking other values.

		ALOGW("Fails to set camera parameters: %s", strerror(-err));
		control->camera_parameters = android::CameraParameters(control->camera->getParameters());
	}
}

int android_camera_get_number_of_devices()
{
	REPORT_FUNCTION();
	return android::Camera::getNumberOfCameras();
}

int android_camera_get_device_info(int32_t camera_id, int* facing, int* orientation)
{
	REPORT_FUNCTION();

	if (!facing || !orientation)
		return android::BAD_VALUE;

	COMPILE_TIME_ASSERT_FUNCTION_SCOPE(CAMERA_FACING_BACK == static_cast<int>(BACK_FACING_CAMERA_TYPE));
	COMPILE_TIME_ASSERT_FUNCTION_SCOPE(CAMERA_FACING_FRONT == static_cast<int>(FRONT_FACING_CAMERA_TYPE));

	android::CameraInfo ci;

	int rv = android::Camera::getCameraInfo(camera_id, &ci);
	if (rv != android::OK)
		return rv;

	*facing = ci.facing;
	*orientation = ci.orientation;

	return android::OK;
}

CameraControl* android_camera_connect_to(CameraType camera_type, CameraControlListener* listener)
{
	REPORT_FUNCTION();

	const int32_t camera_count = android::Camera::getNumberOfCameras();

	for (int32_t camera_id = 0; camera_id < camera_count; camera_id++) {
		android::CameraInfo ci;
		android::Camera::getCameraInfo(camera_id, &ci);

		if (ci.facing != camera_type)
			continue;

		return android_camera_connect_by_id(camera_id, listener);
	}

	return NULL;
}

CameraControl* android_camera_connect_by_id(int32_t camera_id, struct CameraControlListener* listener)
{
	if (camera_id < 0 || camera_id >= android::Camera::getNumberOfCameras())
		return NULL;

	android::sp<CameraControl> cc = new CameraControl();
	cc->listener = listener;
#if  ANDROID_VERSION_MAJOR>=7
	cc->camera = android::Camera::connect(camera_id, android::String16("hybris"), android::Camera::USE_CALLING_UID, android::Camera::USE_CALLING_PID);
#elif ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR>=3 || ANDROID_VERSION_MAJOR>=5
	cc->camera = android::Camera::connect(camera_id, android::String16("hybris"), android::Camera::USE_CALLING_UID);
#else
	cc->camera = android::Camera::connect(camera_id);
#endif

	if (cc->camera == NULL)
		return NULL;

	cc->camera_parameters = android::CameraParameters(cc->camera->getParameters());

	// android::Camera holds a strong reference to the listener, keeping
	// |cc| alive
	cc->camera->setListener(cc);
	cc->camera->lock();

	// TODO: Move this to a more generic component
	android::ProcessState::self()->startThreadPool();

	return cc.get();
}

void android_camera_disconnect(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	if (control->preview_texture != NULL)
		control->preview_texture->abandon();

	control->camera->disconnect();
	control->camera->unlock();
}

int android_camera_lock(CameraControl* control)
{
	android::Mutex::Autolock al(control->guard);
	return control->camera->lock();
}

int android_camera_unlock(CameraControl* control)
{
	android::Mutex::Autolock al(control->guard);
	return control->camera->unlock();
}

void android_camera_delete(CameraControl* control)
{
	android::sp<android::Camera> camera = control->camera;
	control->camera.clear();
	camera.clear();
}

void android_camera_dump_parameters(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	printf("%s \n", control->camera->getParameters().string());
}

void android_camera_set_flash_mode(CameraControl* control, FlashMode mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera_parameters.set(
			android::CameraParameters::KEY_FLASH_MODE,
			flash_modes[mode]);
	setParameters_resilient(control);
}

void android_camera_get_flash_mode(CameraControl* control, FlashMode* mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	static const char* flash_mode = control->camera_parameters.get(
			android::CameraParameters::KEY_FLASH_MODE);
	if (flash_mode)
		*mode = flash_modes_lut.valueFor(android::String8(flash_mode));
	else
		*mode = FLASH_MODE_OFF;
}

void android_camera_enumerate_supported_flash_modes(CameraControl* control, flash_mode_callback cb, void* ctx)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	const char* raw_modes_cstr =
			control->camera_parameters.get(
				android::CameraParameters::KEY_SUPPORTED_FLASH_MODES);
	if (!raw_modes_cstr)
		return;

	android::String8 raw_modes = android::String8(raw_modes_cstr);

	const char delimiter[2] = ",";
	char *token;
	android::String8 mode;
	char *raw_modes_mutable = strdup(raw_modes.string());

	token = strtok(raw_modes_mutable, delimiter);

	while (token != NULL) {
		uint32_t index = flash_modes_lut.indexOfKey(mode);

		mode = android::String8(token);
		if (flash_modes_lut.indexOfKey(mode) >= 0) {
			cb(ctx, flash_modes_lut.valueFor(mode));
		}
		token = strtok(NULL, delimiter);
	}
}

void android_camera_set_white_balance_mode(CameraControl* control, WhiteBalanceMode mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_WHITE_BALANCE,
			white_balance_modes[mode]);
	setParameters_resilient(control);
}

void android_camera_get_white_balance_mode(CameraControl* control, WhiteBalanceMode* mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*mode = white_balance_modes_lut.valueFor(
			android::String8(
				control->camera_parameters.get(
					android::CameraParameters::KEY_WHITE_BALANCE)));
}

void android_camera_set_scene_mode(CameraControl* control, SceneMode mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_SCENE_MODE,
			scene_modes[mode]);
	setParameters_resilient(control);
}

void android_camera_enumerate_supported_scene_modes(CameraControl* control, scene_mode_callback cb, void* ctx)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	android::String8 raw_modes;
	raw_modes = android::String8(
					control->camera_parameters.get(
						android::CameraParameters::KEY_SUPPORTED_SCENE_MODES));

	const char delimiter[2] = ",";
	char *token;
	android::String8 mode;
	char *raw_modes_mutable = strdup(raw_modes.string());

	token = strtok(raw_modes_mutable, delimiter);

	while (token != NULL) {
		mode = android::String8(token);
		cb(ctx, scene_modes_lut.valueFor(mode));
		token = strtok(NULL, delimiter);
	}
}

void android_camera_get_scene_mode(CameraControl* control, SceneMode* mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*mode = scene_modes_lut.valueFor(
			android::String8(
				control->camera_parameters.get(
					android::CameraParameters::KEY_SCENE_MODE)));
}

void android_camera_set_auto_focus_mode(CameraControl* control, AutoFocusMode mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_FOCUS_MODE,
			auto_focus_modes[mode]);
	setParameters_resilient(control);
}

void android_camera_get_auto_focus_mode(CameraControl* control, AutoFocusMode* mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*mode = auto_focus_modes_lut.valueFor(
			android::String8(
				control->camera_parameters.get(
					android::CameraParameters::KEY_FOCUS_MODE)));
}


void android_camera_set_effect_mode(CameraControl* control, EffectMode mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_EFFECT,
			effect_modes[mode]);
	setParameters_resilient(control);
}

void android_camera_get_effect_mode(CameraControl* control, EffectMode* mode)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*mode = effect_modes_lut.valueFor(
			android::String8(
				control->camera_parameters.get(
					android::CameraParameters::KEY_EFFECT)));
}

void android_camera_get_preview_fps_range(CameraControl* control, int* min, int* max)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.getPreviewFpsRange(min, max);
}

void android_camera_set_preview_fps(CameraControl* control, int fps)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera_parameters.setPreviewFrameRate(fps);
	setParameters_resilient(control);
}

void android_camera_get_preview_fps(CameraControl* control, int* fps)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	*fps = control->camera_parameters.getPreviewFrameRate();
}

void android_camera_enumerate_supported_preview_sizes(CameraControl* control, size_callback cb, void* ctx)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	android::Vector<android::Size> sizes;
	control->camera_parameters.getSupportedPreviewSizes(sizes);

	for (unsigned int i = 0; i < sizes.size(); i++) {
		cb(ctx, sizes[i].width, sizes[i].height);
	}
}

void android_camera_enumerate_supported_picture_sizes(CameraControl* control, size_callback cb, void* ctx)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	android::Vector<android::Size> sizes;
	control->camera_parameters.getSupportedPictureSizes(sizes);

	for (unsigned int i = 0; i < sizes.size(); i++) {
		cb(ctx, sizes[i].width, sizes[i].height);
	}
}

void android_camera_get_preview_size(CameraControl* control, int* width, int* height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.getPreviewSize(width, height);
}

void android_camera_set_preview_size(CameraControl* control, int width, int height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.setPreviewSize(width, height);
	setParameters_resilient(control);
}

void android_camera_get_picture_size(CameraControl* control, int* width, int* height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.getPictureSize(width, height);
}

void android_camera_set_thumbnail_size(struct CameraControl* control, int width, int height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,
			width);
	control->camera_parameters.set(
			android::CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,
			height);
	setParameters_resilient(control);
}

void android_camera_get_thumbnail_size(struct CameraControl* control, int* width, int* height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*width = atoi(control->camera_parameters.get(android::CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH));
	*height = atoi(control->camera_parameters.get(android::CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT));
}

void android_camera_enumerate_supported_thumbnail_sizes(struct CameraControl* control, size_callback cb, void* ctx)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	// e.g. 800x600,320x240
	android::String8 sizes = android::String8(
			control->camera_parameters.get(
				android::CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES));

	const char delimiter[2] = ",";
	const char size_delimiter[2] = "x";
	char *token, *save_ptr, *save_ptr1;
	int height = 0, width = 0;
	char *sizes_mutable = strdup(sizes.string());

	ALOGD("Supported thumbnail sizes: %s", sizes.string());
	// Get the first <width>x<height to the left of ','
	token = strtok_r(sizes_mutable, delimiter, &save_ptr);

	while (token != NULL) {
		// Parse <width>x<height> token
		char *w = strtok_r(token, size_delimiter, &save_ptr1);
		char *h = strtok_r(NULL, size_delimiter, &save_ptr1);
		width = atoi(w);
		height = atoi(h);
		if (width > 0 && height > 0)
			cb(ctx, width, height);
		// Get the next <width>x<height> pair
		token = strtok_r(NULL, delimiter, &save_ptr);
	}
}

void android_camera_set_picture_size(CameraControl* control, int width, int height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.setPictureSize(width, height);
	setParameters_resilient(control);
}

void android_camera_get_current_zoom(CameraControl* control, int* zoom)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*zoom = control->camera_parameters.getInt(android::CameraParameters::KEY_ZOOM);
}

void android_camera_get_max_zoom(CameraControl* control, int* zoom)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*zoom = control->camera_parameters.getInt(android::CameraParameters::KEY_MAX_ZOOM);
}

void android_camera_set_display_orientation(CameraControl* control, int32_t clockwise_rotation_degree)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	static const int32_t ignored_parameter = 0;
	control->camera->sendCommand(CAMERA_CMD_SET_DISPLAY_ORIENTATION, clockwise_rotation_degree, ignored_parameter);
}

void android_camera_get_preview_texture_transformation(CameraControl* control, float m[16])
{
	REPORT_FUNCTION();
	assert(control);

	if (control->preview_texture == NULL)
		return;

	control->preview_texture->getTransformMatrix(m);
}

void android_camera_update_preview_texture(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	control->preview_texture->updateTexImage();
}

void android_camera_set_preview_texture(CameraControl* control, int texture_id)
{
	REPORT_FUNCTION();
	assert(control);

	if (texture_id == 0) {
#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
		control->camera->setPreviewTexture(NULL);
#else
		control->camera->setPreviewTarget(NULL);
#endif
		return;
	}

	static const bool allow_synchronous_mode = false;
	static const bool is_controlled_by_app = true;

	if (control->preview_texture == NULL || control->preview_texture_id != texture_id) {
#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
		android::sp<android::NativeBufferAlloc> native_alloc(
			new android::NativeBufferAlloc()
			);
#endif

#if ANDROID_VERSION_MAJOR>=5
		android::sp<android::IGraphicBufferProducer> producer;
		android::sp<android::IGraphicBufferConsumer> consumer;
		android::BufferQueue::createBufferQueue(&producer, &consumer);
		control->preview_bq = producer;
#else
		android::sp<android::BufferQueue> buffer_queue(
#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
			new android::BufferQueue(false, NULL, native_alloc)
#else
			new android::BufferQueue(NULL)
#endif
			);
		control->preview_bq = buffer_queue;
#endif

#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=2
		control->preview_texture = android::sp<android::SurfaceTexture>(
				new android::SurfaceTexture(
#else
		control->preview_texture = android::sp<android::GLConsumer>(
				new android::GLConsumer(
#endif
#if ANDROID_VERSION_MAJOR>=5
					consumer,
					texture_id,
					GL_TEXTURE_EXTERNAL_OES,
					true,
					is_controlled_by_app));
#elif ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
					texture_id,
					allow_synchronous_mode,
					GL_TEXTURE_EXTERNAL_OES,
					true,
					buffer_queue));
#else
					buffer_queue,
					texture_id,
					GL_TEXTURE_EXTERNAL_OES,
					true,
					is_controlled_by_app));
#endif

		control->preview_texture_id = texture_id;
	}

	control->preview_texture->setFrameAvailableListener(
#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=2
			android::sp<android::SurfaceTexture::FrameAvailableListener>(control));
#else
			android::sp<android::GLConsumer::FrameAvailableListener>(control));
#endif

#if ANDROID_VERSION_MAJOR>=5
	control->camera->setPreviewTarget(control->preview_bq);
#elif ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=3
	control->camera->setPreviewTexture(control->preview_texture->getBufferQueue());
#else
	control->camera->setPreviewTarget(control->preview_bq);
#endif
}

#if ANDROID_VERSION_MAJOR==4 && ANDROID_VERSION_MINOR<=2
void android_camera_set_preview_surface(CameraControl* control, SfSurface* surface)
{
	REPORT_FUNCTION();
	assert(control);
	assert(surface);

	android::Mutex::Autolock al(control->guard);
	control->camera->setPreviewDisplay(surface->surface);
}
#endif

void android_camera_start_preview(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera->startPreview();
}

void android_camera_stop_preview(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera->stopPreview();
}

void android_camera_start_autofocus(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera->autoFocus();
}

void android_camera_stop_autofocus(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera->cancelAutoFocus();
}

void android_camera_start_zoom(CameraControl* control, int32_t zoom)
{
	REPORT_FUNCTION();
	assert(control);

	static const int ignored_argument = 0;

	android::Mutex::Autolock al(control->guard);
	control->camera->sendCommand(CAMERA_CMD_START_SMOOTH_ZOOM,
			zoom,
			ignored_argument);
}

// Adjust the zoom level immediately as opposed to smoothly zoomin gin.
void android_camera_set_zoom(CameraControl* control, int32_t zoom)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_ZOOM,
			zoom);

	setParameters_resilient(control);
}

void android_camera_stop_zoom(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);

	static const int ignored_argument = 0;

	android::Mutex::Autolock al(control->guard);
	control->camera->sendCommand(CAMERA_CMD_STOP_SMOOTH_ZOOM,
			ignored_argument,
			ignored_argument);
}

void android_camera_take_snapshot(CameraControl* control)
{
	REPORT_FUNCTION();
	assert(control);
	android::Mutex::Autolock al(control->guard);
	control->camera->takePicture(CAMERA_MSG_SHUTTER | CAMERA_MSG_COMPRESSED_IMAGE);
}

int android_camera_set_preview_callback_mode(CameraControl* control, PreviewCallbackMode mode)
{
	REPORT_FUNCTION();

	if (!control)
		return android::BAD_VALUE;

	android::Mutex::Autolock al(control->guard);

	control->camera->setPreviewCallbackFlags(
		mode == PREVIEW_CALLBACK_ENABLED ?
			CAMERA_FRAME_CALLBACK_FLAG_CAMCORDER : CAMERA_FRAME_CALLBACK_FLAG_NOOP);

	return android::OK;
}

void android_camera_set_preview_format(CameraControl* control, CameraPixelFormat pf)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.set(
			android::CameraParameters::KEY_PREVIEW_FORMAT,
			camera_pixel_formats[pf]);

	setParameters_resilient(control);
}

void android_camera_get_preview_format(CameraControl* control, CameraPixelFormat* pf)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	*pf = pixel_formats_lut.valueFor(
			android::String8(
				control->camera_parameters.get(
					android::CameraParameters::KEY_PREVIEW_FORMAT)));
}

void android_camera_set_focus_region(
		CameraControl* control,
		FocusRegion* region)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	static const char* focus_region_pattern = "(%d,%d,%d,%d,%d)";
	static char focus_region[256];
	snprintf(focus_region,
			sizeof(focus_region),
			focus_region_pattern,
			region->left,
			region->top,
			region->right,
			region->bottom,
			region->weight);

	control->camera_parameters.set(
			android::CameraParameters::KEY_FOCUS_AREAS,
			focus_region);

	setParameters_resilient(control);
}

void android_camera_reset_focus_region(CameraControl* control)
{
	static FocusRegion region = { 0, 0, 0, 0, 0 };

	android_camera_set_focus_region(control, &region);
}

void android_camera_set_metering_region(
                CameraControl* control,
                MeteringRegion* region)
{
        REPORT_FUNCTION();
        assert(control);

        android::Mutex::Autolock al(control->guard);
        static const char* metering_region_pattern = "(%d,%d,%d,%d,%d)";
        static char metering_region[256];
        snprintf(metering_region,
                        sizeof(metering_region),
                        metering_region_pattern,
                        region->left,
                        region->top,
                        region->right,
                        region->bottom,
                        region->weight);

        control->camera_parameters.set(
                        android::CameraParameters::KEY_METERING_AREAS,
                        metering_region);

        setParameters_resilient(control);
}

void android_camera_reset_metering_region(CameraControl* control)
{
        static FocusRegion region = { 0, 0, 0, 0, 0 };

        android_camera_set_metering_region(control, &region);
}

void android_camera_set_rotation(CameraControl* control, int rotation)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera_parameters.set(
			android::CameraParameters::KEY_ROTATION,
			rotation);
	setParameters_resilient(control);
}

void android_camera_set_location(CameraControl* control, const float* latitude, const float* longitude, const float* altitude, int timestamp, const char* method)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera_parameters.setFloat(
			android::CameraParameters::KEY_GPS_LATITUDE,
			*latitude);
	control->camera_parameters.setFloat(
			android::CameraParameters::KEY_GPS_LONGITUDE,
			*longitude);
	control->camera_parameters.setFloat(
			android::CameraParameters::KEY_GPS_ALTITUDE,
			*altitude);
	control->camera_parameters.set(
			android::CameraParameters::KEY_GPS_TIMESTAMP,
			timestamp);
	control->camera_parameters.set(
			android::CameraParameters::KEY_GPS_PROCESSING_METHOD,
			method);
	setParameters_resilient(control);
}

void android_camera_enumerate_supported_video_sizes(CameraControl* control, size_callback cb, void* ctx)
{
	REPORT_FUNCTION();
	assert(control);
	assert(cb);

	android::Mutex::Autolock al(control->guard);
	android::Vector<android::Size> sizes;
	control->camera_parameters.getSupportedVideoSizes(sizes);

	for (unsigned int i = 0; i < sizes.size(); i++) {
		cb(ctx, sizes[i].width, sizes[i].height);
	}
}

void android_camera_get_video_size(CameraControl* control, int* width, int* height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.getVideoSize(width, height);
}

void android_camera_set_video_size(CameraControl* control, int width, int height)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);

	control->camera_parameters.setVideoSize(width, height);
	setParameters_resilient(control);
}

void android_camera_set_jpeg_quality(CameraControl* control, int quality)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	control->camera_parameters.set(
			android::CameraParameters::KEY_JPEG_QUALITY,
			quality);
	setParameters_resilient(control);
}

void android_camera_get_jpeg_quality(CameraControl* control, int* quality)
{
	REPORT_FUNCTION();
	assert(control);

	android::Mutex::Autolock al(control->guard);
	*quality = atoi(control->camera_parameters.get(
            android::CameraParameters::KEY_JPEG_QUALITY));
}
