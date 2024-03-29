/*
 * Copyright (C) 2013 Canonical Ltd
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
 * Authored by: Thomas Voss <thomas.voss@canonical.com>
 *              Ricardo Salveti de Araujo <ricardo.salveti@canonical.com>
 */

#include <hybris/surface_flinger/surface_flinger_compatibility_layer.h>
#include <hybris/internal/surface_flinger_compatibility_layer_internal.h>

#include <utils/misc.h>

#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayInfo.h>
#include <ui/PixelFormat.h>
#include <ui/Region.h>
#include <ui/Rect.h>
#if ANDROID_VERSION_MAJOR>=5
#include <hardware/hwcomposer_defs.h>
#endif

#include <cassert>
#include <cstdio>

namespace
{
void report_failed_to_allocate_surface_flinger_composer_client_on_creation()
{
	printf("Problem allocating an object of type SurfaceComposerClient during client creation");
}

void report_failed_to_get_egl_default_display_on_creation()
{
	printf("Problem accessing default egl display during client creation");
}

void report_failed_to_initialize_egl_on_creation()
{
	printf("Problem initializing egl during client creation");
}

void report_failed_to_choose_egl_config_on_creation()
{
	printf("Problem choosing egl config on creation");
}

void report_surface_control_is_null_during_creation()
{
	printf("Could not acquire surface control object during surface creation");
}

void report_surface_is_null_during_creation()
{
	printf("Could not acquire surface from surface control during surface creation");
}
}

void sf_blank(size_t display_id)
{
	android::sp<android::IBinder> display;

	if (display_id == 0) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdMain);
	} else if (display_id == 1) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdHdmi);
	} else {
		fprintf(stderr, "Warning: sf_blank invalid display_id (0 || 1)\n");
		return;
	}

#if ANDROID_VERSION_MAJOR<=4
	android::SurfaceComposerClient::blankDisplay(display);
#elif ANDROID_VERSION_MAJOR>=5
	android::SurfaceComposerClient::setDisplayPowerMode(display, HWC_POWER_MODE_OFF);
#endif
}

void sf_unblank(size_t display_id)
{
	android::sp<android::IBinder> display;

	if (display_id == 0) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdMain);
	} else if (display_id == 1) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdHdmi);
	} else {
		fprintf(stderr, "Warning: sf_unblank invalid display_id (0 || 1)\n");
		return;
	}

#if ANDROID_VERSION_MAJOR<=4
	android::SurfaceComposerClient::unblankDisplay(display);
#elif ANDROID_VERSION_MAJOR>=5
	android::SurfaceComposerClient::setDisplayPowerMode(display, HWC_POWER_MODE_NORMAL);
#endif
}

size_t sf_get_display_width(size_t display_id)
{
	android::sp<android::IBinder> display;

	if (display_id == 0) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdMain);
	} else if (display_id == 1) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdHdmi);
	} else {
		fprintf(stderr, "Warning: sf_get_display_width invalid display_id (0 || 1)\n");
		return -1;
	}

	android::DisplayInfo info;
	android::SurfaceComposerClient::getDisplayInfo(display, &info);
	return info.w;
}

size_t sf_get_display_height(size_t display_id)
{
	android::sp<android::IBinder> display;

	if (display_id == 0) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdMain);
	} else if (display_id == 1) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdHdmi);
	} else {
		fprintf(stderr, "Warning: sf_get_display_height invalid display_id (0 || 1)\n");
		return -1;
	}

	android::DisplayInfo info;
	android::SurfaceComposerClient::getDisplayInfo(display, &info);
	return info.h;
}

size_t sf_get_display_info(size_t display_id, SfDisplayInfo* display_info)
{
	android::sp<android::IBinder> display;

	if (display_id == 0) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdMain);
	} else if (display_id == 1) {
		display = android::SurfaceComposerClient::getBuiltInDisplay(
				android::ISurfaceComposer::eDisplayIdHdmi);
	} else {
		fprintf(stderr, "Warning: sf_get_display_info invalid display_id (0 || 1)\n");
		return -1;
	}

	android::DisplayInfo info;
	android::SurfaceComposerClient::getDisplayInfo(display, &info);

	if (display_info == NULL) {
		fprintf(stderr, "Warning: sf_get_display_info: display_info is null! Please allocate it before \n");
		return -2;
	}

	// partial copy
	display_info->w = info.w;
	display_info->h = info.h;
	display_info->xdpi = info.xdpi;
	display_info->ydpi = info.ydpi;
	display_info->fps = info.fps;
	display_info->density = info.density;
	display_info->orientation = info.orientation;

	return 0;
}

SfClient* sf_client_create_full(bool egl_support)
{
	SfClient* client = new SfClient();

	client->client = new android::SurfaceComposerClient();
	if (client->client == NULL) {
		report_failed_to_allocate_surface_flinger_composer_client_on_creation();
		delete client;
		return NULL;
	}

	client->egl_support = egl_support;
	if (egl_support) {
		client->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (client->egl_display == EGL_NO_DISPLAY) {
			report_failed_to_get_egl_default_display_on_creation();
			delete client;
			return NULL;
		}

		int major, minor;
		int rc = eglInitialize(client->egl_display, &major, &minor);
		if (rc == EGL_FALSE) {
			report_failed_to_initialize_egl_on_creation();
			delete client;
			return NULL;
		}

		EGLint attribs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
		};

		int n;
		if (eglChooseConfig(client->egl_display,
					attribs,
					&client->egl_config, 1,
					&n) == EGL_FALSE) {
			report_failed_to_choose_egl_config_on_creation();
			delete client;
			return NULL;
		}

		EGLint context_attribs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		};

		client->egl_context = eglCreateContext(
				client->egl_display,
				client->egl_config,
				EGL_NO_CONTEXT,
				context_attribs);
	}

	return client;
}

SfClient* sf_client_create()
{
	return sf_client_create_full(true);
}

EGLDisplay sf_client_get_egl_display(SfClient* client)
{
	assert(client);

	if (client->egl_support)
		return client->egl_display;
	else {
		fprintf(stderr, "Warning: sf_client_get_egl_display not supported, EGL "
				"support disabled\n");
		return NULL;
	}
}

EGLConfig sf_client_get_egl_config(SfClient* client)
{
	assert(client);

	if (client->egl_support)
		return client->egl_config;
	else {
		fprintf(stderr, "Warning: sf_client_get_egl_config not supported, EGL "
				"support disabled\n");
		return NULL;
	}
}

EGLContext sf_client_get_egl_context(SfClient* client)
{
	assert(client);

	if (client->egl_support)
		return client->egl_context;
	else {
		fprintf(stderr, "Warning: sf_client_get_egl_context not supported, EGL "
				"support disabled\n");
		return NULL;
	}
}

#if ANDROID_VERSION_MAJOR<8
void sf_client_begin_transaction(SfClient* client)
{
	assert(client);
	client->client->openGlobalTransaction();
}

void sf_client_end_transaction(SfClient* client)
{
	assert(client);
	client->client->closeGlobalTransaction();
}
#endif

SfSurface* sf_surface_create(SfClient* client, SfSurfaceCreationParameters* params)
{
	assert(client);
	assert(params);

	SfSurface* surface = new SfSurface();
	surface->client = client;
	surface->surface_control = surface->client->client->createSurface(
					android::String8(params->name),
					params->w,
					params->h,
					android::PIXEL_FORMAT_RGBA_8888,
					0x300);

	if (surface->surface_control == NULL) {
		report_surface_control_is_null_during_creation();
		delete(surface);
		return NULL;
	}

	surface->surface = surface->surface_control->getSurface();

	if (surface->surface == NULL) {
		report_surface_is_null_during_creation();
		delete(surface);
		return NULL;
	}

#if ANDROID_VERSION_MAJOR>=8
	uint32_t mDisplayLayerStack = 0;
	android::SurfaceComposerClient::Transaction()
		.setLayerStack(surface->surface_control, mDisplayLayerStack)
		.setLayer(surface->surface_control, params->layer)
		.setAlpha(surface->surface_control, params->alpha)
		.setPosition(surface->surface_control, params->x, params->y)
		.apply();
#else
	sf_client_begin_transaction(client);
	{
		surface->surface_control->setPosition(params->x, params->y);
		surface->surface_control->setLayer(params->layer);
		surface->surface_control->setAlpha(params->alpha);
	}
	sf_client_end_transaction(client);
#endif

	if (params->create_egl_window_surface) {
		if (client->egl_support) {
			android::sp<ANativeWindow> anw(surface->surface);
			surface->egl_surface = eglCreateWindowSurface(
						surface->client->egl_display,
						surface->client->egl_config,
						anw.get(),
						NULL);
		} else
			fprintf(stderr, "Warning: params->create_egl_window_surface not "
					"supported, EGL support disabled\n");
	}

	return surface;
}

EGLSurface sf_surface_get_egl_surface(SfSurface* surface)
{
	assert(surface);

	if (surface->client->egl_support)
		return surface->egl_surface;
	else {
		fprintf(stderr, "Warning: sf_surface_get_egl_surface not supported, "
				"EGL support disabled\n");
		return NULL;
	}
}

EGLNativeWindowType sf_surface_get_egl_native_window(SfSurface* surface)
{
	assert(surface);
	return surface->surface.get();
}

void sf_surface_make_current(SfSurface* surface)
{
	assert(surface);

	if (surface->client->egl_support) {
		eglMakeCurrent(
				surface->client->egl_display,
				surface->egl_surface,
				surface->egl_surface,
				surface->client->egl_context);
	} else {
		fprintf(stderr, "Warning: sf_surface_make_current not supported, EGL "
				"support disabled\n");
	}
}

void sf_surface_move_to(SfSurface* surface, int x, int y)
{
	assert(surface);
#if ANDROID_VERSION_MAJOR>=8
	android::SurfaceComposerClient::Transaction()
		.setPosition(surface->surface_control, x, y)
		.apply(true);
#else
	surface->surface_control->setPosition(x, y);
#endif
}

void sf_surface_set_size(SfSurface* surface, int w, int h)
{
	assert(surface);
#if ANDROID_VERSION_MAJOR>=8
	android::SurfaceComposerClient::Transaction()
		.setSize(surface->surface_control, w, h)
		.apply(true);
#else
	surface->surface_control->setSize(w, h);
#endif
}

void sf_surface_set_layer(SfSurface* surface, int layer)
{
	assert(surface);
#if ANDROID_VERSION_MAJOR>=8
	android::SurfaceComposerClient::Transaction()
		.setLayer(surface->surface_control, layer)
		.apply(true);
#else
	surface->surface_control->setLayer(layer);
#endif
}

void sf_surface_set_alpha(SfSurface* surface, float alpha)
{
	assert(surface);
#if ANDROID_VERSION_MAJOR>=8
	android::SurfaceComposerClient::Transaction()
		.setAlpha(surface->surface_control, alpha)
		.apply(true);
#else
	surface->surface_control->setAlpha(alpha);
#endif
}
