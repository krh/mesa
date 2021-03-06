/**************************************************************************
 * 
 * Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

#include <errno.h>
#include "main/glheader.h"
#include "main/context.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"
#include "main/hash.h"
#include "main/fbobject.h"
#include "main/mfeatures.h"
#include "main/version.h"
#include "swrast/s_renderbuffer.h"

#include "utils.h"
#include "xmlpool.h"

PUBLIC const char __driConfigOptions[] =
   DRI_CONF_BEGIN
   DRI_CONF_SECTION_PERFORMANCE
      DRI_CONF_VBLANK_MODE(DRI_CONF_VBLANK_ALWAYS_SYNC)
      /* Options correspond to DRI_CONF_BO_REUSE_DISABLED,
       * DRI_CONF_BO_REUSE_ALL
       */
      DRI_CONF_OPT_BEGIN_V(bo_reuse, enum, 1, "0:1")
	 DRI_CONF_DESC_BEGIN(en, "Buffer object reuse")
	    DRI_CONF_ENUM(0, "Disable buffer object reuse")
	    DRI_CONF_ENUM(1, "Enable reuse of all sizes of buffer objects")
	 DRI_CONF_DESC_END
      DRI_CONF_OPT_END

      DRI_CONF_OPT_BEGIN(texture_tiling, bool, true)
	 DRI_CONF_DESC(en, "Enable texture tiling")
      DRI_CONF_OPT_END

      DRI_CONF_OPT_BEGIN(hiz, bool, true)
	 DRI_CONF_DESC(en, "Enable Hierarchical Z on gen6+")
      DRI_CONF_OPT_END

      DRI_CONF_OPT_BEGIN(early_z, bool, false)
	 DRI_CONF_DESC(en, "Enable early Z in classic mode (unstable, 945-only).")
      DRI_CONF_OPT_END

      DRI_CONF_OPT_BEGIN(fragment_shader, bool, true)
	 DRI_CONF_DESC(en, "Enable limited ARB_fragment_shader support on 915/945.")
      DRI_CONF_OPT_END

   DRI_CONF_SECTION_END
   DRI_CONF_SECTION_QUALITY
      DRI_CONF_FORCE_S3TC_ENABLE(false)
      DRI_CONF_ALLOW_LARGE_TEXTURES(2)
   DRI_CONF_SECTION_END
   DRI_CONF_SECTION_DEBUG
     DRI_CONF_NO_RAST(false)
     DRI_CONF_ALWAYS_FLUSH_BATCH(false)
     DRI_CONF_ALWAYS_FLUSH_CACHE(false)
     DRI_CONF_FORCE_GLSL_EXTENSIONS_WARN(false)
     DRI_CONF_DISABLE_BLEND_FUNC_EXTENDED(false)

      DRI_CONF_OPT_BEGIN(stub_occlusion_query, bool, false)
	 DRI_CONF_DESC(en, "Enable stub ARB_occlusion_query support on 915/945.")
      DRI_CONF_OPT_END

      DRI_CONF_OPT_BEGIN(shader_precompile, bool, false)
	 DRI_CONF_DESC(en, "Perform code generation at shader link time.")
      DRI_CONF_OPT_END
   DRI_CONF_SECTION_END
DRI_CONF_END;

const GLuint __driNConfigOptions = 15;

#include "intel_batchbuffer.h"
#include "intel_buffers.h"
#include "intel_bufmgr.h"
#include "intel_chipset.h"
#include "intel_fbo.h"
#include "intel_mipmap_tree.h"
#include "intel_screen.h"
#include "intel_tex.h"
#include "intel_regions.h"

#include "i915_drm.h"

#ifdef USE_NEW_INTERFACE
static PFNGLXCREATECONTEXTMODES create_context_modes = NULL;
#endif /*USE_NEW_INTERFACE */

void
aub_dump_bmp(struct gl_context *ctx)
{
   struct gl_framebuffer *fb = ctx->DrawBuffer;

   for (int i = 0; i < fb->_NumColorDrawBuffers; i++) {
      struct intel_renderbuffer *irb =
	 intel_renderbuffer(fb->_ColorDrawBuffers[i]);

      if (irb && irb->mt) {
	 enum aub_dump_bmp_format format;

	 switch (irb->Base.Base.Format) {
	 case MESA_FORMAT_ARGB8888:
	 case MESA_FORMAT_XRGB8888:
	    format = AUB_DUMP_BMP_FORMAT_ARGB_8888;
	    break;
	 default:
	    continue;
	 }

	 drm_intel_gem_bo_aub_dump_bmp(irb->mt->region->bo,
				       irb->draw_x,
				       irb->draw_y,
				       irb->Base.Base.Width,
				       irb->Base.Base.Height,
				       format,
				       irb->mt->region->pitch *
				       irb->mt->region->cpp,
				       0);
      }
   }
}

static const __DRItexBufferExtension intelTexBufferExtension = {
    { __DRI_TEX_BUFFER, __DRI_TEX_BUFFER_VERSION },
   intelSetTexBuffer,
   intelSetTexBuffer2,
};

static void
intelDRI2Flush(__DRIdrawable *drawable)
{
   GET_CURRENT_CONTEXT(ctx);
   struct intel_context *intel = intel_context(ctx);
   if (intel == NULL)
      return;

   if (intel->gen < 4)
      INTEL_FIREVERTICES(intel);

   intel->need_throttle = true;

   if (intel->batch.used)
      intel_batchbuffer_flush(intel);

   if (INTEL_DEBUG & DEBUG_AUB) {
      aub_dump_bmp(ctx);
   }
}

static const struct __DRI2flushExtensionRec intelFlushExtension = {
    { __DRI2_FLUSH, __DRI2_FLUSH_VERSION },
    intelDRI2Flush,
    dri2InvalidateDrawable,
};

static __DRIimage *
intel_allocate_image(int dri_format, void *loaderPrivate)
{
    __DRIimage *image;

    image = CALLOC(sizeof *image);
    if (image == NULL)
	return NULL;

    image->dri_format = dri_format;
    image->offset = 0;

    switch (dri_format) {
    case __DRI_IMAGE_FORMAT_RGB565:
       image->format = MESA_FORMAT_RGB565;
       break;
    case __DRI_IMAGE_FORMAT_XRGB8888:
       image->format = MESA_FORMAT_XRGB8888;
       break;
    case __DRI_IMAGE_FORMAT_ARGB8888:
       image->format = MESA_FORMAT_ARGB8888;
       break;
    case __DRI_IMAGE_FORMAT_ABGR8888:
       image->format = MESA_FORMAT_RGBA8888_REV;
       break;
    case __DRI_IMAGE_FORMAT_XBGR8888:
       image->format = MESA_FORMAT_RGBX8888_REV;
       break;
    case __DRI_IMAGE_FORMAT_R8:
       image->format = MESA_FORMAT_R8;
       break;
    case __DRI_IMAGE_FORMAT_GR88:
       image->format = MESA_FORMAT_GR88;
       break;
    case __DRI_IMAGE_FORMAT_NONE:
       image->format = MESA_FORMAT_NONE;
       break;
    default:
       free(image);
       return NULL;
    }

    image->internal_format = _mesa_get_format_base_format(image->format);
    image->data = loaderPrivate;

    return image;
}

static __DRIimage *
intel_create_image_from_name(__DRIscreen *screen,
			     int width, int height, int format,
			     int name, int pitch, void *loaderPrivate)
{
    struct intel_screen *intelScreen = screen->driverPrivate;
    __DRIimage *image;
    int cpp;

    image = intel_allocate_image(format, loaderPrivate);
    if (image->format == MESA_FORMAT_NONE)
       cpp = 0;
    else
       cpp = _mesa_get_format_bytes(image->format);
    image->region = intel_region_alloc_for_handle(intelScreen,
						  cpp, width, height,
						  pitch, name, "image");
    if (image->region == NULL) {
       FREE(image);
       return NULL;
    }

    return image;	
}

static __DRIimage *
intel_create_image_from_renderbuffer(__DRIcontext *context,
				     int renderbuffer, void *loaderPrivate)
{
   __DRIimage *image;
   struct intel_context *intel = context->driverPrivate;
   struct gl_renderbuffer *rb;
   struct intel_renderbuffer *irb;

   rb = _mesa_lookup_renderbuffer(&intel->ctx, renderbuffer);
   if (!rb) {
      _mesa_error(&intel->ctx,
		  GL_INVALID_OPERATION, "glRenderbufferExternalMESA");
      return NULL;
   }

   irb = intel_renderbuffer(rb);
   image = CALLOC(sizeof *image);
   if (image == NULL)
      return NULL;

   image->internal_format = rb->InternalFormat;
   image->format = rb->Format;
   image->offset = 0;
   image->data = loaderPrivate;
   intel_region_reference(&image->region, irb->mt->region);

   switch (image->format) {
   case MESA_FORMAT_RGB565:
      image->dri_format = __DRI_IMAGE_FORMAT_RGB565;
      break;
   case MESA_FORMAT_XRGB8888:
      image->dri_format = __DRI_IMAGE_FORMAT_XRGB8888;
      break;
   case MESA_FORMAT_ARGB8888:
      image->dri_format = __DRI_IMAGE_FORMAT_ARGB8888;
      break;
   case MESA_FORMAT_RGBA8888_REV:
      image->dri_format = __DRI_IMAGE_FORMAT_ABGR8888;
      break;
   case MESA_FORMAT_R8:
      image->dri_format = __DRI_IMAGE_FORMAT_R8;
      break;
   case MESA_FORMAT_RG88:
      image->dri_format = __DRI_IMAGE_FORMAT_GR88;
      break;
   }

   return image;
}

static void
intel_destroy_image(__DRIimage *image)
{
    intel_region_release(&image->region);
    FREE(image);
}

static __DRIimage *
intel_create_image(__DRIscreen *screen,
		   int width, int height, int format,
		   unsigned int use,
		   void *loaderPrivate)
{
   __DRIimage *image;
   struct intel_screen *intelScreen = screen->driverPrivate;
   uint32_t tiling;
   int cpp;

   tiling = I915_TILING_X;
   if (use & __DRI_IMAGE_USE_CURSOR) {
      if (width != 64 || height != 64)
	 return NULL;
      tiling = I915_TILING_NONE;
   }

   /* We only support write for cursor drm images */
   if ((use & __DRI_IMAGE_USE_WRITE) &&
       use != (__DRI_IMAGE_USE_WRITE | __DRI_IMAGE_USE_CURSOR))
      return NULL;

   image = intel_allocate_image(format, loaderPrivate);
   image->usage = use;
   cpp = _mesa_get_format_bytes(image->format);
   image->region =
      intel_region_alloc(intelScreen, tiling, cpp, width, height, true);
   if (image->region == NULL) {
      FREE(image);
      return NULL;
   }
   
   return image;
}

static GLboolean
intel_query_image(__DRIimage *image, int attrib, int *value)
{
   switch (attrib) {
   case __DRI_IMAGE_ATTRIB_STRIDE:
      *value = image->region->pitch * image->region->cpp;
      return true;
   case __DRI_IMAGE_ATTRIB_HANDLE:
      *value = image->region->bo->handle;
      return true;
   case __DRI_IMAGE_ATTRIB_NAME:
      return intel_region_flink(image->region, (uint32_t *) value);
   case __DRI_IMAGE_ATTRIB_FORMAT:
      *value = image->dri_format;
      return true;
   case __DRI_IMAGE_ATTRIB_WIDTH:
      *value = image->region->width;
      return true;
   case __DRI_IMAGE_ATTRIB_HEIGHT:
      *value = image->region->height;
      return true;
  default:
      return false;
   }
}

static __DRIimage *
intel_dup_image(__DRIimage *orig_image, void *loaderPrivate)
{
   __DRIimage *image;

   image = CALLOC(sizeof *image);
   if (image == NULL)
      return NULL;

   intel_region_reference(&image->region, orig_image->region);
   if (image->region == NULL) {
      FREE(image);
      return NULL;
   }

   image->internal_format = orig_image->internal_format;
   image->usage           = orig_image->usage;
   image->dri_format      = orig_image->dri_format;
   image->format          = orig_image->format;
   image->offset          = orig_image->offset;
   image->data            = loaderPrivate;
   
   return image;
}

static GLboolean
intel_validate_usage(__DRIimage *image, unsigned int use)
{
   if (use & __DRI_IMAGE_USE_CURSOR) {
      if (image->region->width != 64 || image->region->height != 64)
	 return GL_FALSE;
   }

   /* We only support write for cursor drm images */
   if ((use & __DRI_IMAGE_USE_WRITE) &&
       use != (__DRI_IMAGE_USE_WRITE | __DRI_IMAGE_USE_CURSOR))
      return GL_FALSE;

   return GL_TRUE;
}

static int
intel_image_write(__DRIimage *image, const void *buf, size_t count)
{
   if (image->region->map_refcount)
      return -1;
   if (!(image->usage & __DRI_IMAGE_USE_WRITE))
      return -1;

   drm_intel_bo_map(image->region->bo, true);
   memcpy(image->region->bo->virtual, buf, count);
   drm_intel_bo_unmap(image->region->bo);

   return 0;
}

static __DRIimage *
intel_create_sub_image(__DRIimage *parent,
                       int width, int height, int dri_format,
                       int offset, int pitch, void *loaderPrivate)
{
    __DRIimage *image;
    int cpp;
    uint32_t mask_x, mask_y;

    image = intel_allocate_image(dri_format, loaderPrivate);
    cpp = _mesa_get_format_bytes(image->format);
    if (offset + height * cpp * pitch > parent->region->bo->size) {
       _mesa_warning(NULL, "intel_create_sub_image: subimage out of bounds");
       FREE(image);
       return NULL;
    }

    image->region = calloc(sizeof(*image->region), 1);
    if (image->region == NULL) {
       FREE(image);
       return NULL;
    }

    image->region->cpp = _mesa_get_format_bytes(image->format);
    image->region->width = width;
    image->region->height = height;
    image->region->pitch = pitch;
    image->region->refcount = 1;
    image->region->bo = parent->region->bo;
    drm_intel_bo_reference(image->region->bo);
    image->region->tiling = parent->region->tiling;
    image->region->screen = parent->region->screen;
    image->offset = offset;

    intel_region_get_tile_masks(image->region, &mask_x, &mask_y);
    if (offset & mask_x)
       _mesa_warning(NULL,
                     "intel_create_sub_image: offset not on tile boundary");

    return image;
}

static struct __DRIimageExtensionRec intelImageExtension = {
    { __DRI_IMAGE, 5 },
    intel_create_image_from_name,
    intel_create_image_from_renderbuffer,
    intel_destroy_image,
    intel_create_image,
    intel_query_image,
    intel_dup_image,
    intel_validate_usage,
    intel_image_write,
    intel_create_sub_image
};

static const __DRIextension *intelScreenExtensions[] = {
    &intelTexBufferExtension.base,
    &intelFlushExtension.base,
    &intelImageExtension.base,
    &dri2ConfigQueryExtension.base,
    NULL
};

static bool
intel_get_param(__DRIscreen *psp, int param, int *value)
{
   int ret;
   struct drm_i915_getparam gp;

   memset(&gp, 0, sizeof(gp));
   gp.param = param;
   gp.value = value;

   ret = drmCommandWriteRead(psp->fd, DRM_I915_GETPARAM, &gp, sizeof(gp));
   if (ret) {
      if (ret != -EINVAL)
	 _mesa_warning(NULL, "drm_i915_getparam: %d", ret);
      return false;
   }

   return true;
}

static bool
intel_get_boolean(__DRIscreen *psp, int param)
{
   int value = 0;
   return intel_get_param(psp, param, &value) && value;
}

static void
nop_callback(GLuint key, void *data, void *userData)
{
}

static void
intelDestroyScreen(__DRIscreen * sPriv)
{
   struct intel_screen *intelScreen = sPriv->driverPrivate;

   dri_bufmgr_destroy(intelScreen->bufmgr);
   driDestroyOptionInfo(&intelScreen->optionCache);

   /* Some regions may still have references to them at this point, so
    * flush the hash table to prevent _mesa_DeleteHashTable() from
    * complaining about the hash not being empty; */
   _mesa_HashDeleteAll(intelScreen->named_regions, nop_callback, NULL);
   _mesa_DeleteHashTable(intelScreen->named_regions);

   FREE(intelScreen);
   sPriv->driverPrivate = NULL;
}


/**
 * This is called when we need to set up GL rendering to a new X window.
 */
static GLboolean
intelCreateBuffer(__DRIscreen * driScrnPriv,
                  __DRIdrawable * driDrawPriv,
                  const struct gl_config * mesaVis, GLboolean isPixmap)
{
   struct intel_renderbuffer *rb;
   struct intel_screen *screen = (struct intel_screen*) driScrnPriv->driverPrivate;

   if (isPixmap) {
      return false;          /* not implemented */
   }
   else {
      gl_format rgbFormat;

      struct gl_framebuffer *fb = CALLOC_STRUCT(gl_framebuffer);

      if (!fb)
	 return false;

      _mesa_initialize_window_framebuffer(fb, mesaVis);

      if (mesaVis->redBits == 5)
	 rgbFormat = MESA_FORMAT_RGB565;
      else if (mesaVis->alphaBits == 0)
	 rgbFormat = MESA_FORMAT_XRGB8888;
      else
	 rgbFormat = MESA_FORMAT_ARGB8888;

      /* setup the hardware-based renderbuffers */
      rb = intel_create_renderbuffer(rgbFormat);
      _mesa_add_renderbuffer(fb, BUFFER_FRONT_LEFT, &rb->Base.Base);

      if (mesaVis->doubleBufferMode) {
	 rb = intel_create_renderbuffer(rgbFormat);
         _mesa_add_renderbuffer(fb, BUFFER_BACK_LEFT, &rb->Base.Base);
      }

      /*
       * Assert here that the gl_config has an expected depth/stencil bit
       * combination: one of d24/s8, d16/s0, d0/s0. (See intelInitScreen2(),
       * which constructs the advertised configs.)
       */
      if (mesaVis->depthBits == 24) {
	 assert(mesaVis->stencilBits == 8);

	 if (screen->hw_has_separate_stencil) {
	    rb = intel_create_private_renderbuffer(MESA_FORMAT_X8_Z24);
	    _mesa_add_renderbuffer(fb, BUFFER_DEPTH, &rb->Base.Base);
	    rb = intel_create_private_renderbuffer(MESA_FORMAT_S8);
	    _mesa_add_renderbuffer(fb, BUFFER_STENCIL, &rb->Base.Base);
	 } else {
	    /*
	     * Use combined depth/stencil. Note that the renderbuffer is
	     * attached to two attachment points.
	     */
            rb = intel_create_private_renderbuffer(MESA_FORMAT_S8_Z24);
	    _mesa_add_renderbuffer(fb, BUFFER_DEPTH, &rb->Base.Base);
	    _mesa_add_renderbuffer(fb, BUFFER_STENCIL, &rb->Base.Base);
	 }
      }
      else if (mesaVis->depthBits == 16) {
	 assert(mesaVis->stencilBits == 0);
         /* just 16-bit depth buffer, no hw stencil */
         struct intel_renderbuffer *depthRb
	    = intel_create_private_renderbuffer(MESA_FORMAT_Z16);
         _mesa_add_renderbuffer(fb, BUFFER_DEPTH, &depthRb->Base.Base);
      }
      else {
	 assert(mesaVis->depthBits == 0);
	 assert(mesaVis->stencilBits == 0);
      }

      /* now add any/all software-based renderbuffers we may need */
      _swrast_add_soft_renderbuffers(fb,
                                     false, /* never sw color */
                                     false, /* never sw depth */
                                     false, /* never sw stencil */
                                     mesaVis->accumRedBits > 0,
                                     false, /* never sw alpha */
                                     false  /* never sw aux */ );
      driDrawPriv->driverPrivate = fb;

      return true;
   }
}

static void
intelDestroyBuffer(__DRIdrawable * driDrawPriv)
{
    struct gl_framebuffer *fb = driDrawPriv->driverPrivate;
  
    _mesa_reference_framebuffer(&fb, NULL);
}

/* There are probably better ways to do this, such as an
 * init-designated function to register chipids and createcontext
 * functions.
 */
extern bool
i830CreateContext(const struct gl_config *mesaVis,
		  __DRIcontext *driContextPriv,
		  void *sharedContextPrivate);

extern bool
i915CreateContext(int api,
		  const struct gl_config *mesaVis,
		  __DRIcontext *driContextPriv,
		  void *sharedContextPrivate);
extern bool
brwCreateContext(int api,
	         const struct gl_config *mesaVis,
	         __DRIcontext *driContextPriv,
		 void *sharedContextPrivate);

static GLboolean
intelCreateContext(gl_api api,
		   const struct gl_config * mesaVis,
                   __DRIcontext * driContextPriv,
		   unsigned major_version,
		   unsigned minor_version,
		   uint32_t flags,
		   unsigned *error,
                   void *sharedContextPrivate)
{
   __DRIscreen *sPriv = driContextPriv->driScreenPriv;
   struct intel_screen *intelScreen = sPriv->driverPrivate;
   bool success = false;

#ifdef I915
   if (IS_9XX(intelScreen->deviceID)) {
      if (!IS_965(intelScreen->deviceID)) {
	 success = i915CreateContext(api, mesaVis, driContextPriv,
				     sharedContextPrivate);
      }
   } else {
      intelScreen->no_vbo = true;
      success = i830CreateContext(mesaVis, driContextPriv,
				  sharedContextPrivate);
   }
#else
   if (IS_965(intelScreen->deviceID))
      success = brwCreateContext(api, mesaVis,
			      driContextPriv,
			      sharedContextPrivate);
#endif

   if (success) {
      struct gl_context *ctx =
	 (struct gl_context *) driContextPriv->driverPrivate;

      _mesa_compute_version(ctx);
      if (ctx->VersionMajor > major_version
	  || (ctx->VersionMajor == major_version
	      && ctx->VersionMinor >= minor_version)) {
	 return true;
      }

      *error = __DRI_CTX_ERROR_BAD_VERSION;
      intelDestroyContext(driContextPriv);
   } else {
      *error = __DRI_CTX_ERROR_NO_MEMORY;
      fprintf(stderr, "Unrecognized deviceID 0x%x\n", intelScreen->deviceID);
   }

   return false;
}

static bool
intel_init_bufmgr(struct intel_screen *intelScreen)
{
   __DRIscreen *spriv = intelScreen->driScrnPriv;
   int num_fences = 0;

   intelScreen->no_hw = getenv("INTEL_NO_HW") != NULL;

   intelScreen->bufmgr = intel_bufmgr_gem_init(spriv->fd, BATCH_SZ);
   if (intelScreen->bufmgr == NULL) {
      fprintf(stderr, "[%s:%u] Error initializing buffer manager.\n",
	      __func__, __LINE__);
      return false;
   }

   if (!intel_get_param(spriv, I915_PARAM_NUM_FENCES_AVAIL, &num_fences) ||
       num_fences == 0) {
      fprintf(stderr, "[%s: %u] Kernel 2.6.29 required.\n", __func__, __LINE__);
      return false;
   }

   drm_intel_bufmgr_gem_enable_fenced_relocs(intelScreen->bufmgr);

   intelScreen->named_regions = _mesa_NewHashTable();

   intelScreen->relaxed_relocations = 0;
   intelScreen->relaxed_relocations |=
      intel_get_boolean(spriv, I915_PARAM_HAS_RELAXED_DELTA) << 0;

   return true;
}

/**
 * Override intel_screen.hw_has_separate_stencil with environment variable
 * INTEL_SEPARATE_STENCIL.
 *
 * Valid values for INTEL_SEPARATE_STENCIL are "0" and "1". If an invalid
 * valid value is encountered, a warning is emitted and INTEL_SEPARATE_STENCIL
 * is ignored.
 */
static void
intel_override_separate_stencil(struct intel_screen *screen)
{
   const char *s = getenv("INTEL_SEPARATE_STENCIL");
   if (!s) {
      return;
   } else if (!strncmp("0", s, 2)) {
      screen->hw_has_separate_stencil = false;
   } else if (!strncmp("1", s, 2)) {
      screen->hw_has_separate_stencil = true;
   } else {
      fprintf(stderr,
	      "warning: env variable INTEL_SEPARATE_STENCIL=\"%s\" has "
	      "invalid value and is ignored", s);
   }
}

static bool
intel_detect_swizzling(struct intel_screen *screen)
{
   drm_intel_bo *buffer;
   unsigned long flags = 0;
   unsigned long aligned_pitch;
   uint32_t tiling = I915_TILING_X;
   uint32_t swizzle_mode = 0;

   buffer = drm_intel_bo_alloc_tiled(screen->bufmgr, "swizzle test",
				     64, 64, 4,
				     &tiling, &aligned_pitch, flags);
   if (buffer == NULL)
      return false;

   drm_intel_bo_get_tiling(buffer, &tiling, &swizzle_mode);
   drm_intel_bo_unreference(buffer);

   if (swizzle_mode == I915_BIT_6_SWIZZLE_NONE)
      return false;
   else
      return true;
}

/**
 * This is the driver specific part of the createNewScreen entry point.
 * Called when using DRI2.
 *
 * \return the struct gl_config supported by this driver
 */
static const
__DRIconfig **intelInitScreen2(__DRIscreen *psp)
{
   struct intel_screen *intelScreen;
   GLenum fb_format[3];
   GLenum fb_type[3];
   unsigned int api_mask;

   static const GLenum back_buffer_modes[] = {
       GLX_NONE, GLX_SWAP_UNDEFINED_OML, GLX_SWAP_COPY_OML
   };
   uint8_t depth_bits[4], stencil_bits[4], msaa_samples_array[1];
   int color;
   __DRIconfig **configs = NULL;

   if (psp->dri2.loader->base.version <= 2 ||
       psp->dri2.loader->getBuffersWithFormat == NULL) {
      fprintf(stderr,
	      "\nERROR!  DRI2 loader with getBuffersWithFormat() "
	      "support required\n");
      return false;
   }

   /* Allocate the private area */
   intelScreen = CALLOC(sizeof *intelScreen);
   if (!intelScreen) {
      fprintf(stderr, "\nERROR!  Allocating private area failed\n");
      return false;
   }
   /* parse information in __driConfigOptions */
   driParseOptionInfo(&intelScreen->optionCache,
                      __driConfigOptions, __driNConfigOptions);

   intelScreen->driScrnPriv = psp;
   psp->driverPrivate = (void *) intelScreen;

   if (!intel_init_bufmgr(intelScreen))
       return false;

   intelScreen->deviceID = drm_intel_bufmgr_gem_get_devid(intelScreen->bufmgr);

   intelScreen->kernel_has_gen7_sol_reset =
      intel_get_boolean(intelScreen->driScrnPriv,
			I915_PARAM_HAS_GEN7_SOL_RESET);

   if (IS_GEN7(intelScreen->deviceID)) {
      intelScreen->gen = 7;
   } else if (IS_GEN6(intelScreen->deviceID)) {
      intelScreen->gen = 6;
   } else if (IS_GEN5(intelScreen->deviceID)) {
      intelScreen->gen = 5;
   } else if (IS_965(intelScreen->deviceID)) {
      intelScreen->gen = 4;
   } else if (IS_9XX(intelScreen->deviceID)) {
      intelScreen->gen = 3;
   } else {
      intelScreen->gen = 2;
   }

   intelScreen->hw_has_separate_stencil = intelScreen->gen >= 6;
   intelScreen->hw_must_use_separate_stencil = intelScreen->gen >= 7;

   int has_llc = 0;
   bool success = intel_get_param(intelScreen->driScrnPriv, I915_PARAM_HAS_LLC,
				  &has_llc);
   if (success && has_llc)
      intelScreen->hw_has_llc = true;
   else if (!success && intelScreen->gen >= 6)
      intelScreen->hw_has_llc = true;

   intel_override_separate_stencil(intelScreen);

   api_mask = (1 << __DRI_API_OPENGL);
#if FEATURE_ES1
   api_mask |= (1 << __DRI_API_GLES);
#endif
#if FEATURE_ES2
   api_mask |= (1 << __DRI_API_GLES2);
#endif

   if (IS_9XX(intelScreen->deviceID) || IS_965(intelScreen->deviceID))
      psp->api_mask = api_mask;

   intelScreen->hw_has_swizzling = intel_detect_swizzling(intelScreen);

   psp->extensions = intelScreenExtensions;

   msaa_samples_array[0] = 0;

   fb_format[0] = GL_RGB;
   fb_type[0] = GL_UNSIGNED_SHORT_5_6_5;

   fb_format[1] = GL_BGR;
   fb_type[1] = GL_UNSIGNED_INT_8_8_8_8_REV;

   fb_format[2] = GL_BGRA;
   fb_type[2] = GL_UNSIGNED_INT_8_8_8_8_REV;

   depth_bits[0] = 0;
   stencil_bits[0] = 0;

   /* Generate a rich set of useful configs that do not include an
    * accumulation buffer.
    */
   for (color = 0; color < ARRAY_SIZE(fb_format); color++) {
      __DRIconfig **new_configs;
      int depth_factor;

      /* Starting with DRI2 protocol version 1.1 we can request a depth/stencil
       * buffer that has a diffferent number of bits per pixel than the color
       * buffer.  This isn't yet supported here.
       */
      if (fb_type[color] == GL_UNSIGNED_SHORT_5_6_5) {
	 depth_bits[1] = 16;
	 stencil_bits[1] = 0;
      } else {
	 depth_bits[1] = 24;
	 stencil_bits[1] = 8;
      }

      depth_factor = 2;

      new_configs = driCreateConfigs(fb_format[color], fb_type[color],
				     depth_bits,
				     stencil_bits,
				     depth_factor,
				     back_buffer_modes,
				     ARRAY_SIZE(back_buffer_modes),
				     msaa_samples_array,
				     ARRAY_SIZE(msaa_samples_array),
				     false);
      if (configs == NULL)
	 configs = new_configs;
      else
	 configs = driConcatConfigs(configs, new_configs);
   }

   /* Generate the minimum possible set of configs that include an
    * accumulation buffer.
    */
   for (color = 0; color < ARRAY_SIZE(fb_format); color++) {
      __DRIconfig **new_configs;

      if (fb_type[color] == GL_UNSIGNED_SHORT_5_6_5) {
	 depth_bits[0] = 16;
	 stencil_bits[0] = 0;
      } else {
	 depth_bits[0] = 24;
	 stencil_bits[0] = 8;
      }

      new_configs = driCreateConfigs(fb_format[color], fb_type[color],
				     depth_bits, stencil_bits, 1,
				     back_buffer_modes + 1, 1,
				     msaa_samples_array, 1,
				     true);
      if (configs == NULL)
	 configs = new_configs;
      else
	 configs = driConcatConfigs(configs, new_configs);
   }

   if (configs == NULL) {
      fprintf(stderr, "[%s:%u] Error creating FBConfig!\n", __func__,
              __LINE__);
      return NULL;
   }

   return (const __DRIconfig **)configs;
}

struct intel_buffer {
   __DRIbuffer base;
   struct intel_region *region;
};

/**
 * \brief Get tiling format for a DRI buffer.
 *
 * \param attachment is the buffer's attachmet point, such as
 *        __DRI_BUFFER_DEPTH.
 * \param out_tiling is the returned tiling format for buffer.
 * \return false if attachment is unrecognized or is incompatible with screen.
 */
static bool
intel_get_dri_buffer_tiling(struct intel_screen *screen,
                            uint32_t attachment,
                            uint32_t *out_tiling)
{
   if (screen->gen < 4) {
      *out_tiling = I915_TILING_X;
      return true;
   }

   switch (attachment) {
   case __DRI_BUFFER_DEPTH:
   case __DRI_BUFFER_DEPTH_STENCIL:
   case __DRI_BUFFER_HIZ:
      *out_tiling = I915_TILING_Y;
      return true;
   case __DRI_BUFFER_ACCUM:
   case __DRI_BUFFER_FRONT_LEFT:
   case __DRI_BUFFER_FRONT_RIGHT:
   case __DRI_BUFFER_BACK_LEFT:
   case __DRI_BUFFER_BACK_RIGHT:
   case __DRI_BUFFER_FAKE_FRONT_LEFT:
   case __DRI_BUFFER_FAKE_FRONT_RIGHT:
      *out_tiling = I915_TILING_X;
      return true;
   case __DRI_BUFFER_STENCIL:
      /* The stencil buffer is W tiled. However, we request from the kernel
       * a non-tiled buffer because the GTT is incapable of W fencing.
       */
      *out_tiling = I915_TILING_NONE;
      return true;
   default:
      if(unlikely(INTEL_DEBUG & DEBUG_DRI)) {
	 fprintf(stderr, "error: %s: unrecognized DRI buffer attachment 0x%x\n",
	         __FUNCTION__, attachment);
      }
       return false;
   }
}

static __DRIbuffer *
intelAllocateBuffer(__DRIscreen *screen,
		    unsigned attachment, unsigned format,
		    int width, int height)
{
   struct intel_buffer *intelBuffer;
   struct intel_screen *intelScreen = screen->driverPrivate;

   uint32_t tiling;
   uint32_t region_width;
   uint32_t region_height;
   uint32_t region_cpp;

   bool ok = true;

   ok = intel_get_dri_buffer_tiling(intelScreen, attachment, &tiling);
   if (!ok)
      return NULL;

   intelBuffer = CALLOC(sizeof *intelBuffer);
   if (intelBuffer == NULL)
      return NULL;

   if (attachment == __DRI_BUFFER_STENCIL) {
      /* Stencil buffers use W tiling, a tiling format that the DRM functions
       * don't properly account for.  Therefore, when we allocate a stencil
       * buffer that is private to Mesa (see intel_miptree_create), we round
       * the height and width up to the next multiple of the tile size (64x64)
       * and then ask DRM to allocate an untiled buffer.  Consequently, the
       * height and the width stored in the stencil buffer's region structure
       * are always multiples of 64, even if the stencil buffer itself is
       * smaller.
       *
       * To avoid inconsistencies between how we represent private buffers and
       * buffers shared with the window system, round up the height and width
       * for window system buffers too.
       */
      region_width = ALIGN(width, 64);
      region_height = ALIGN(height, 64);
   } else {
      region_width = width;
      region_height = height;
   }

   region_cpp = format / 8;

   intelBuffer->region = intel_region_alloc(intelScreen,
                                            tiling,
                                            region_cpp,
                                            region_width,
                                            region_height,
                                            true);
   
   if (intelBuffer->region == NULL) {
	   FREE(intelBuffer);
	   return NULL;
   }
   
   intel_region_flink(intelBuffer->region, &intelBuffer->base.name);

   intelBuffer->base.attachment = attachment;
   intelBuffer->base.cpp = intelBuffer->region->cpp;
   intelBuffer->base.pitch =
         intelBuffer->region->pitch * intelBuffer->region->cpp;

   return &intelBuffer->base;
}

static void
intelReleaseBuffer(__DRIscreen *screen, __DRIbuffer *buffer)
{
   struct intel_buffer *intelBuffer = (struct intel_buffer *) buffer;

   intel_region_release(&intelBuffer->region);
   free(intelBuffer);
}


const struct __DriverAPIRec driDriverAPI = {
   .InitScreen		 = intelInitScreen2,
   .DestroyScreen	 = intelDestroyScreen,
   .CreateContext	 = intelCreateContext,
   .DestroyContext	 = intelDestroyContext,
   .CreateBuffer	 = intelCreateBuffer,
   .DestroyBuffer	 = intelDestroyBuffer,
   .MakeCurrent		 = intelMakeCurrent,
   .UnbindContext	 = intelUnbindContext,
   .AllocateBuffer       = intelAllocateBuffer,
   .ReleaseBuffer        = intelReleaseBuffer
};

/* This is the table of extensions that the loader will dlsym() for. */
PUBLIC const __DRIextension *__driDriverExtensions[] = {
    &driCoreExtension.base,
    &driDRI2Extension.base,
    NULL
};
