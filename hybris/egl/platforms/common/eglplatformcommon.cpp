/*
 * Copyright (c) 2013-2022 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <android-config.h>
#include <ws.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "config.h"
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "logging.h"

#ifdef WANT_WAYLAND
#include <wayland-client.h>
#include "server_wlegl.h"
#include "server_wlegl_buffer.h"
#endif

#include "platformcommon.h"
#include "windowbuffer.h"

#include <hybris/gralloc/gralloc.h>

static struct ws_egl_interface *my_egl_interface;

extern "C" void eglplatformcommon_init(struct ws_egl_interface *egl_iface)
{
	my_egl_interface = egl_iface;
}

extern "C" void *hybris_android_egl_dlsym(const char *symbol)
{
	return (*my_egl_interface->android_egl_dlsym)(symbol);
}

extern "C" int hybris_egl_has_mapping(EGLSurface surface)
{
	return (*my_egl_interface->has_mapping)(surface);
}

EGLNativeWindowType hybris_egl_get_mapping(EGLSurface surface)
{
	return (*my_egl_interface->get_mapping)(surface);
}

#ifdef WANT_WAYLAND

extern "C" EGLBoolean eglplatformcommon_eglBindWaylandDisplayWL(EGLDisplay dpy, struct wl_display *display)
{
	server_wlegl_create(display);
	return EGL_TRUE;
}

extern "C" EGLBoolean eglplatformcommon_eglUnbindWaylandDisplayWL(EGLDisplay dpy, struct wl_display *display)
{
	return EGL_TRUE;
}

extern "C" EGLBoolean eglplatformcommon_eglQueryWaylandBufferWL(EGLDisplay dpy,
	struct wl_resource *buffer, EGLint attribute, EGLint *value)
{
	server_wlegl_buffer *buf  = server_wlegl_buffer_from(buffer);
	if (!buf || !buf->buf)
		return EGL_FALSE;

	ANativeWindowBuffer* anwb = (ANativeWindowBuffer *) buf->buf;

	if (attribute == EGL_TEXTURE_FORMAT) {
		switch(anwb->format) {
		case HAL_PIXEL_FORMAT_RGB_565:
			*value = EGL_TEXTURE_RGB;
			break;
		case HAL_PIXEL_FORMAT_RGBA_8888:
		case HAL_PIXEL_FORMAT_BGRA_8888:
			*value = EGL_TEXTURE_RGBA;
			break;
		default:
			*value =  EGL_TEXTURE_EXTERNAL_WL;
		}
		return EGL_TRUE;
	}
	if (attribute == EGL_WIDTH) {
		*value = anwb->width;
		return EGL_TRUE;
	}
	if (attribute == EGL_HEIGHT) {
		*value = anwb->height;
		return EGL_TRUE;
	}
	return EGL_FALSE ;
}

// Added as part of EGL_HYBRIS_WL_acquire_native_buffer. Buffers are released
// and decRef'ed using eglHybrisReleaseNativeBuffer
extern "C" EGLBoolean eglplatformcommon_eglHybrisAcquireNativeBufferWL(EGLDisplay dpy, struct wl_resource *wlBuffer, EGLClientBuffer *buffer)
 {
     if (!buffer)
         return EGL_FALSE;
    server_wlegl_buffer *buf  = server_wlegl_buffer_from(wlBuffer);
    if (!buf || !buf->buf || !buf->buf->isAllocated()) {
        // We only return the handles from buffers which are allocated server side. This is because some
        // hardware compositors have problems with client-side allocated buffers.
        *buffer = 0;
        return EGL_FALSE;
    }
    ANativeWindowBuffer* anwb = (ANativeWindowBuffer *) buf->buf;
    anwb->common.incRef(&anwb->common);
    *buffer = (EGLClientBuffer *) anwb;
    return EGL_TRUE;
}

#endif

// Added as part of EGL_HYBRIS_native_buffer2
extern "C" EGLBoolean eglplatformcommon_eglHybrisNativeBufferHandle(EGLDisplay dpy, EGLClientBuffer buffer, void **handle)
{
	if (!buffer || !handle)
		return EGL_FALSE;
	ANativeWindowBuffer *anwb = (ANativeWindowBuffer *) buffer;
	*handle = (void *) anwb->handle;
	return EGL_TRUE;
}

extern "C" void eglplatformcommon_eglHybrisGetNativeBufferInfo(EGLClientBuffer buffer, int *num_ints, int *num_fds)
{
	RemoteWindowBuffer *buf = static_cast<RemoteWindowBuffer *>((ANativeWindowBuffer *) buffer);
	*num_ints = buf->handle->numInts;
	*num_fds = buf->handle->numFds;
}

extern "C" void eglplatformcommon_eglHybrisSerializeNativeBuffer(EGLClientBuffer buffer, int *ints, int *fds)
{
	RemoteWindowBuffer *buf = static_cast<RemoteWindowBuffer *>((ANativeWindowBuffer *) buffer);
	memcpy(ints, buf->handle->data + buf->handle->numFds, buf->handle->numInts * sizeof(int));
	memcpy(fds, buf->handle->data, buf->handle->numFds * sizeof(int));
}

extern "C" EGLBoolean eglplatformcommon_eglHybrisCreateRemoteBuffer(EGLint width, EGLint height, EGLint usage, EGLint format, EGLint stride,
                                                                    int num_ints, int *ints, int num_fds, int *fds, EGLClientBuffer *buffer)
{
	native_handle_t *native = native_handle_create(num_fds, num_ints);
	memcpy(&native->data[0], fds, num_fds * sizeof(int));
	memcpy(&native->data[num_fds], ints, num_ints * sizeof(int));

	const native_handle_t* out_handle = NULL;
	int ret = hybris_gralloc_import_buffer(native, &out_handle);
	native_handle_close(native);
	native_handle_delete(native);

	if (ret == 0)
	{
		RemoteWindowBuffer *buf = new RemoteWindowBuffer(width, height, stride, format, usage, (buffer_handle_t)out_handle);
		buf->common.incRef(&buf->common);
		*buffer = (EGLClientBuffer) static_cast<ANativeWindowBuffer *>(buf);
		return EGL_TRUE;
	}
	else
		return EGL_FALSE;
}

extern "C" EGLBoolean eglplatformcommon_eglHybrisCreateNativeBuffer(EGLint width, EGLint height, EGLint usage, EGLint format, EGLint *stride, EGLClientBuffer *buffer)
{
	int ret;
	buffer_handle_t _handle;
	int _stride;

	ret = hybris_gralloc_allocate(width, height, format, usage, &_handle, (uint32_t*)&_stride);

	if (ret == 0)
	{
		RemoteWindowBuffer *buf = new RemoteWindowBuffer(width, height, _stride, format, usage, _handle);
		buf->common.incRef(&buf->common);
		buf->setAllocated(true);
		*buffer = (EGLClientBuffer) static_cast<ANativeWindowBuffer *>(buf);
		*stride = _stride;
		return EGL_TRUE;
	}
	else
		return EGL_FALSE;
}

extern "C" EGLBoolean eglplatformcommon_eglHybrisLockNativeBuffer(EGLClientBuffer buffer, EGLint usage, EGLint l, EGLint t, EGLint w, EGLint h, void **vaddr)
{
	int ret;
	RemoteWindowBuffer *buf = static_cast<RemoteWindowBuffer *>((ANativeWindowBuffer *) buffer);

	ret = hybris_gralloc_lock(buf->handle, usage, l, t, w, h, vaddr);
	if (ret == 0)
		return EGL_TRUE;
	else
		return EGL_FALSE;
}

extern "C" EGLBoolean eglplatformcommon_eglHybrisUnlockNativeBuffer(EGLClientBuffer buffer)
{
	int ret;
	RemoteWindowBuffer *buf = static_cast<RemoteWindowBuffer *>((ANativeWindowBuffer *) buffer);

	ret = hybris_gralloc_unlock(buf->handle);
	if (ret == 0)
		return EGL_TRUE;
	else
		return EGL_FALSE;
}


extern "C" EGLBoolean eglplatformcommon_eglHybrisReleaseNativeBuffer(EGLClientBuffer buffer)
{
	RemoteWindowBuffer *buf = static_cast<RemoteWindowBuffer *>((ANativeWindowBuffer *) buffer);

	buf->common.decRef(&buf->common);
	return EGL_TRUE;
}



/*
 * EGL_EXT_image_dma_buf_import for the PowerVR SGX's gralloc. The dma-buf (one plane at offset 0,
 * linear, with the pitch IMG's gralloc gives the width: its own buffers, hybris_gbm.so's) is
 * imported as a gralloc buffer, with a handle in IMG's layout (hal_public.h, IMG_native_handle_t:
 * 1 fd, 13 ints): the allocation's own in the process that allocated it, else rebuilt. IMG's
 * gralloc maps a rebuilt one by the fd; the stamp only keys its per-process table of mappings, so
 * the dma-buf's inode stands in for it: unique per buffer, the same for each import of one.
 *
 * NV12 (a V4L2 decoder's frames) goes in as IMG's TI_NV12, which the SGX samples as an external
 * texture (BT.601). IMG's gralloc maps it from two fds (TilerMapNV12: luma, chroma), each with its
 * size, as one range: the first uiAllocSize[0] bytes of fd[0], then the first uiAllocSize[1] of
 * fd[1]. The GPU takes the chroma plane at pitch * height from the start, so it has to be there:
 * in the same dma-buf (passed as both fds: the range is then that buffer's start) or at the start
 * of its own, after a luma plane of whole pages.
 */
#define IMG_DMABUF_USAGE (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER | \
	GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN)
#define IMG_DMABUF_FLAGS 0x10001003	/* what IMG's gralloc sets for that usage */
#define HYBRIS_FOURCC(a, b, c, d) \
	((EGLint)(a) | ((EGLint)(b) << 8) | ((EGLint)(c) << 16) | ((EGLint)(d) << 24))

static const struct { EGLint fourcc; int hal_format; } dmabuf_formats[] = {
	{ HYBRIS_FOURCC('X', 'R', '2', '4'), 0x1FF },	/* IMG's BGRX_8888 */
	{ HYBRIS_FOURCC('A', 'R', '2', '4'), HAL_PIXEL_FORMAT_BGRA_8888 },
	{ HYBRIS_FOURCC('X', 'B', '2', '4'), HAL_PIXEL_FORMAT_RGBX_8888 },
	{ HYBRIS_FOURCC('A', 'B', '2', '4'), HAL_PIXEL_FORMAT_RGBA_8888 },
};
#define N_DMABUF_FORMATS (sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]))
#define DMABUF_NV12 HYBRIS_FOURCC('N', 'V', '1', '2')
#ifndef HAL_PIXEL_FORMAT_TI_NV12
#define HAL_PIXEL_FORMAT_TI_NV12 0x100	/* IMG's hal_public.h */
#endif
#define IMG_NV12_USAGE (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_SW_READ_OFTEN)
#define IMG_PAGE 4096UL

static int dmabuf_hal_format(EGLint fourcc)
{
	for (unsigned int i = 0; i < N_DMABUF_FORMATS; i++)
		if (dmabuf_formats[i].fourcc == fourcc)
			return dmabuf_formats[i].hal_format;
	return 0;
}

/* DRM_FORMAT_MOD_LINEAR is 0, DRM_FORMAT_MOD_INVALID means "implicit": linear here too */
static bool dmabuf_linear(EGLint lo, EGLint hi)
{
	return (lo == 0 && hi == 0) || (lo == -1 && hi == 0x00ffffff);
}

/* the NV12 frame's handle (TI_NV12, two fds), or NULL if the SGX can't sample its layout */
static native_handle_t *nv12_handle(int width, int height, const int fd[2], const int offset[2],
		const int pitch[2])
{
	struct stat st[2];
	off_t size[2];
	unsigned long luma = (unsigned long)pitch[0] * height, alloc[2];

	if (width <= 0 || height <= 0 || width % 2 || height % 2 || pitch[0] < width ||
			pitch[1] != pitch[0] || offset[0] || fd[0] < 0 || fd[1] < 0)
		return NULL;
	for (int i = 0; i < 2; i++)
		if (fstat(fd[i], &st[i]) || (size[i] = lseek(fd[i], 0, SEEK_END)) <= 0 ||
				size[i] % IMG_PAGE)
			return NULL;
	if (st[0].st_ino == st[1].st_ino && st[0].st_dev == st[1].st_dev) {
		/* one buffer: the range is its start, as far as the chroma plane's end */
		unsigned long end = (luma + luma / 2 + IMG_PAGE - 1) & ~(IMG_PAGE - 1);
		if ((unsigned long)offset[1] != luma || end > (unsigned long)size[0])
			return NULL;
		alloc[0] = luma & ~(IMG_PAGE - 1);
		alloc[1] = end - alloc[0];
	} else {
		if (offset[1] || luma % IMG_PAGE || luma > (unsigned long)size[0] ||
				luma / 2 > (unsigned long)size[1])
			return NULL;
		alloc[0] = luma;
		alloc[1] = size[1];
	}

	native_handle_t *h = native_handle_create(2, 12);
	unsigned long long stamp = (1ULL << 63) | st[0].st_ino;
	h->data[0] = fd[0];
	h->data[1] = fd[1];
	h->data[2] = -1;				/* fd[2] */
	memcpy(&h->data[3], &stamp, sizeof(stamp));	/* ui64Stamp */
	h->data[5] = IMG_NV12_USAGE;
	h->data[6] = width;
	h->data[7] = height;
	h->data[8] = HAL_PIXEL_FORMAT_TI_NV12;
	h->data[9] = 12;				/* uiBpp */
	h->data[10] = alloc[0];				/* uiAllocSize[0..2] */
	h->data[11] = alloc[1];
	h->data[12] = 0;
	h->data[13] = IMG_DMABUF_FLAGS;
	return h;
}

static EGLClientBuffer import_dmabuf(const EGLint *attrib_list)
{
	int fd[2] = { -1, -1 }, offset[2] = { 0, 0 }, pitch[2] = { 0, 0 };
	int width = 0, height = 0, hal = 0;
	EGLint fourcc = 0, mod_lo[2] = { 0, 0 }, mod_hi[2] = { 0, 0 };
	bool yuv_hints = false;

	for (const EGLint *a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
		switch (a[0]) {
		case EGL_WIDTH: width = a[1]; break;
		case EGL_HEIGHT: height = a[1]; break;
		case EGL_LINUX_DRM_FOURCC_EXT: fourcc = a[1]; break;
		case EGL_DMA_BUF_PLANE0_FD_EXT: fd[0] = a[1]; break;
		case EGL_DMA_BUF_PLANE0_OFFSET_EXT: offset[0] = a[1]; break;
		case EGL_DMA_BUF_PLANE0_PITCH_EXT: pitch[0] = a[1]; break;
		case EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT: mod_lo[0] = a[1]; break;
		case EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT: mod_hi[0] = a[1]; break;
		case EGL_DMA_BUF_PLANE1_FD_EXT: fd[1] = a[1]; break;
		case EGL_DMA_BUF_PLANE1_OFFSET_EXT: offset[1] = a[1]; break;
		case EGL_DMA_BUF_PLANE1_PITCH_EXT: pitch[1] = a[1]; break;
		case EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT: mod_lo[1] = a[1]; break;
		case EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT: mod_hi[1] = a[1]; break;
		case EGL_IMAGE_PRESERVED_KHR: break;
		/* the SGX's YUV conversion is its own (BT.601, limited range): hints accepted */
		case EGL_YUV_COLOR_SPACE_HINT_EXT:
		case EGL_SAMPLE_RANGE_HINT_EXT:
		case EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT:
		case EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT: yuv_hints = true; break;
		default: return NULL;	/* a third plane */
		}
	}
	if (!dmabuf_linear(mod_lo[0], mod_hi[0]) || (yuv_hints && fourcc != DMABUF_NV12))
		return NULL;

	if (fourcc == DMABUF_NV12) {
		if (!dmabuf_linear(mod_lo[1], mod_hi[1]))
			return NULL;
		native_handle_t *h = nv12_handle(width, height, fd, offset, pitch);
		if (!h)
			return NULL;
		const native_handle_t *out = NULL;
		int ret = hybris_gralloc_import_buffer(h, &out);	/* a clone: the fds stay the caller's */
		native_handle_delete(h);
		if (ret)
			return NULL;
		RemoteWindowBuffer *buf = new RemoteWindowBuffer(width, height, pitch[0],
			HAL_PIXEL_FORMAT_TI_NV12, IMG_NV12_USAGE, (buffer_handle_t)out);
		buf->common.incRef(&buf->common);
		return (EGLClientBuffer) static_cast<ANativeWindowBuffer *>(buf);
	}

	hal = dmabuf_hal_format(fourcc);
	struct stat st;
	off_t size;
	if (fd[0] < 0 || fd[1] >= 0 || !hal || width <= 0 || height <= 0 || offset[0] ||
			pitch[0] < width * 4 || pitch[0] % 4 || fstat(fd[0], &st) ||
			(size = lseek(fd[0], 0, SEEK_END)) <= 0)
		return NULL;
	int pitch0 = pitch[0];

	/*
	 * The process that allocated the buffer (a GBM bo from hybris_gbm) has to use the allocation's
	 * handle: IMG's gralloc maps a rebuilt one with another stamp to other memory there. Other
	 * processes get it rebuilt from the fd, any stamp works (the kernel finds the buffer by the fd).
	 */
	native_handle_t *h = native_handle_create(1, 13);
	int num_fds, usage = IMG_DMABUF_USAGE;
	h->data[0] = fd[0];
	if (hybris_gralloc_allocated_ints(fd[0], &num_fds, &h->data[1], 13) == 13 && num_fds == 1) {
		usage = h->data[5];
	} else {
		unsigned long long stamp = (1ULL << 63) | st.st_ino;
		h->data[1] = h->data[2] = -1;			/* fd[1], fd[2]: no more planes */
		memcpy(&h->data[3], &stamp, sizeof(stamp));	/* ui64Stamp */
		h->data[5] = usage;
		h->data[6] = width;
		h->data[7] = height;
		h->data[8] = hal;
		h->data[9] = 32;				/* uiBpp */
		h->data[10] = size;				/* uiAllocSize[0..2] */
		h->data[11] = h->data[12] = 0;
		h->data[13] = IMG_DMABUF_FLAGS;
	}

	const native_handle_t *out = NULL;
	int ret = hybris_gralloc_import_buffer(h, &out);	/* a clone: the fd stays the caller's */
	native_handle_delete(h);
	if (ret)
		return NULL;
	RemoteWindowBuffer *buf = new RemoteWindowBuffer(width, height, pitch0 / 4, hal,
		usage, (buffer_handle_t)out);
	buf->common.incRef(&buf->common);
	return (EGLClientBuffer) static_cast<ANativeWindowBuffer *>(buf);
}

extern "C" EGLBoolean eglplatformcommon_eglQueryDmaBufFormatsEXT(EGLDisplay dpy, EGLint max_formats,
		EGLint *formats, EGLint *num_formats)
{
	EGLint n = N_DMABUF_FORMATS + 1;	/* and NV12 */

	if (max_formats > 0 && formats) {
		if (n > max_formats)
			n = max_formats;
		for (EGLint i = 0; i < n; i++)
			formats[i] = i < (EGLint)N_DMABUF_FORMATS ? dmabuf_formats[i].fourcc : DMABUF_NV12;
	}
	*num_formats = n;
	return EGL_TRUE;
}

extern "C" EGLBoolean eglplatformcommon_eglQueryDmaBufModifiersEXT(EGLDisplay dpy, EGLint format,
		EGLint max_modifiers, EGLuint64KHR *modifiers, EGLBoolean *external_only,
		EGLint *num_modifiers)
{
	if (!dmabuf_hal_format(format) && format != DMABUF_NV12)
		return EGL_FALSE;
	if (max_modifiers > 0 && modifiers) {
		modifiers[0] = 0;	/* DRM_FORMAT_MOD_LINEAR */
		if (external_only)	/* YUV: sampled through GL_TEXTURE_EXTERNAL_OES only */
			external_only[0] = format == DMABUF_NV12;
	}
	*num_modifiers = 1;
	return EGL_TRUE;
}

extern "C" void
eglplatformcommon_passthroughImageKHR(EGLContext *ctx, EGLenum *target, EGLClientBuffer *buffer, const EGLint **attrib_list)
{
	if (*target == EGL_LINUX_DMA_BUF_EXT)
	{
		EGLClientBuffer buf = import_dmabuf(*attrib_list);
		if (buf)
		{
			*buffer = buf;	/* released with the image (egl.c) */
			*target = EGL_NATIVE_BUFFER_ANDROID;
			*ctx = EGL_NO_CONTEXT;
			*attrib_list = NULL;
		}
		return;
	}
#ifdef WANT_WAYLAND
	static int debugenvchecked = 0;
	if (*target == EGL_WAYLAND_BUFFER_WL)
	{
		server_wlegl_buffer *buf = server_wlegl_buffer_from((struct wl_resource *)*buffer);
		if (!buf || !buf->buf)
			return;

		HYBRIS_TRACE_BEGIN("eglplatformcommon", "Wayland_eglImageKHR", "-resource@%i", wl_resource_get_id((struct wl_resource *)*buffer));
		HYBRIS_TRACE_END("eglplatformcommon", "Wayland_eglImageKHR", "-resource@%i", wl_resource_get_id((struct wl_resource *)*buffer));
		if (debugenvchecked == 0)
		{
			if (getenv("HYBRIS_WAYLAND_KHR_DUMP_BUFFERS") != NULL)
				debugenvchecked = 2;
			else
				debugenvchecked = 1;
		} else if (debugenvchecked == 2)
		{
			hybris_dump_buffer_to_file((ANativeWindowBuffer *) buf->buf);
		}
		*buffer = (EGLClientBuffer) (ANativeWindowBuffer *) buf->buf;
		*target = EGL_NATIVE_BUFFER_ANDROID;
		*ctx = EGL_NO_CONTEXT;
		*attrib_list = NULL;
	}
#endif
}

extern "C" __eglMustCastToProperFunctionPointerType eglplatformcommon_eglGetProcAddress(const char *procname)
{
#ifdef WANT_WAYLAND
	if (strcmp(procname, "eglBindWaylandDisplayWL") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglBindWaylandDisplayWL;
	}
	else
	if (strcmp(procname, "eglUnbindWaylandDisplayWL") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglUnbindWaylandDisplayWL;
	}else
	if (strcmp(procname, "eglQueryWaylandBufferWL") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglQueryWaylandBufferWL;
	}
	else
	if (strcmp(procname, "eglHybrisAcquireNativeBufferWL") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType) eglplatformcommon_eglHybrisAcquireNativeBufferWL;
	}
	else
#endif
	if (strcmp(procname, "eglHybrisCreateNativeBuffer") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisCreateNativeBuffer;
	}
	else
	if (strcmp(procname, "eglHybrisLockNativeBuffer") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisLockNativeBuffer;
	}
	else
	if (strcmp(procname, "eglHybrisUnlockNativeBuffer") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisUnlockNativeBuffer;
	}
	else
	if (strcmp(procname, "eglHybrisReleaseNativeBuffer") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisReleaseNativeBuffer;
	}
	else
	if (strcmp(procname, "eglHybrisGetNativeBufferInfo") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisGetNativeBufferInfo;
	}
	else
	if (strcmp(procname, "eglHybrisSerializeNativeBuffer") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisSerializeNativeBuffer;
	}
	else
	if (strcmp(procname, "eglHybrisCreateRemoteBuffer") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisCreateRemoteBuffer;
	}
	else
	if (strcmp(procname, "eglHybrisNativeBufferHandle") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglHybrisNativeBufferHandle;
	}
	else
	if (strcmp(procname, "eglQueryDmaBufFormatsEXT") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglQueryDmaBufFormatsEXT;
	}
	else
	if (strcmp(procname, "eglQueryDmaBufModifiersEXT") == 0)
	{
		return (__eglMustCastToProperFunctionPointerType)eglplatformcommon_eglQueryDmaBufModifiersEXT;
	}
	return NULL;
}

extern "C" const char *eglplatformcommon_eglQueryString(EGLDisplay dpy, EGLint name, const char *(*real_eglQueryString)(EGLDisplay dpy, EGLint name))
{
	const char *ret = (*real_eglQueryString)(dpy, name);
	if (ret && name == EGL_EXTENSIONS)
	{
		static char eglextensionsbuf[2048];
		snprintf(eglextensionsbuf, 2046, "%s %s", ret,
			"EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers"
#ifdef WANT_WAYLAND
			" EGL_HYBRIS_native_buffer2 EGL_HYBRIS_WL_acquire_native_buffer EGL_WL_bind_wayland_display"
#endif
		);
		ret = eglextensionsbuf;
	}
	return ret;
}
