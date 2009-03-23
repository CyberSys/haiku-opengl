/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * Copyright 2008 VMware, Inc.  All rights reserved.
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

/* Author:
 *    Keith Whitwell <keith@tungstengraphics.com>
 */

//#define ST_CONTEXT_PERF
#ifdef ST_CONTEXT_PERF
#	define TRACEP(x...)	fprintf(stderr, x);
#else
#	define TRACEP(x...)
#endif

#include <OS.h>
#include <sys/time.h>
#include "draw/draw_context.h"
#include "pipe/p_defines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "sp_clear.h"
#include "sp_context.h"
#include "sp_flush.h"
#include "sp_prim_setup.h"
#include "sp_prim_vbuf.h"
#include "sp_state.h"
#include "sp_surface.h"
#include "sp_tile_cache.h"
#include "sp_texture.h"
#include "sp_winsys.h"
#include "sp_query.h"



/**
 * Map any drawing surfaces which aren't already mapped
 */
void
softpipe_map_transfers(struct softpipe_context *sp)
{
   unsigned i;

   for (i = 0; i < sp->framebuffer.nr_cbufs; i++) {
      sp_tile_cache_map_transfers(sp->cbuf_cache[i]);
   }

   sp_tile_cache_map_transfers(sp->zsbuf_cache);
}


/**
 * Unmap any mapped drawing surfaces
 */
void
softpipe_unmap_transfers(struct softpipe_context *sp)
{
   uint i;

   for (i = 0; i < sp->framebuffer.nr_cbufs; i++)
      sp_flush_tile_cache(sp, sp->cbuf_cache[i]);
   sp_flush_tile_cache(sp, sp->zsbuf_cache);

   for (i = 0; i < sp->framebuffer.nr_cbufs; i++) {
      sp_tile_cache_unmap_transfers(sp->cbuf_cache[i]);
   }
   sp_tile_cache_unmap_transfers(sp->zsbuf_cache);
}


static void softpipe_destroy( struct pipe_context *pipe )
{
   struct softpipe_context *softpipe = softpipe_context( pipe );
   uint i;

   if (softpipe->draw)
      draw_destroy( softpipe->draw );

   for (i = 0; i < SP_NUM_QUAD_THREADS; i++) {
      softpipe->quad[i].polygon_stipple->destroy( softpipe->quad[i].polygon_stipple );
      softpipe->quad[i].earlyz->destroy( softpipe->quad[i].earlyz );
      softpipe->quad[i].shade->destroy( softpipe->quad[i].shade );
      softpipe->quad[i].alpha_test->destroy( softpipe->quad[i].alpha_test );
      softpipe->quad[i].depth_test->destroy( softpipe->quad[i].depth_test );
      softpipe->quad[i].stencil_test->destroy( softpipe->quad[i].stencil_test );
      softpipe->quad[i].occlusion->destroy( softpipe->quad[i].occlusion );
      softpipe->quad[i].coverage->destroy( softpipe->quad[i].coverage );
      softpipe->quad[i].blend->destroy( softpipe->quad[i].blend );
      softpipe->quad[i].colormask->destroy( softpipe->quad[i].colormask );
      softpipe->quad[i].output->destroy( softpipe->quad[i].output );
   }

   for (i = 0; i < PIPE_MAX_COLOR_BUFS; i++)
      sp_destroy_tile_cache(softpipe->cbuf_cache[i]);
   sp_destroy_tile_cache(softpipe->zsbuf_cache);

   for (i = 0; i < PIPE_MAX_SAMPLERS; i++)
      sp_destroy_tile_cache(softpipe->tex_cache[i]);

   for (i = 0; i < Elements(softpipe->constants); i++) {
      if (softpipe->constants[i].buffer) {
         pipe_buffer_reference(&softpipe->constants[i].buffer, NULL);
      }
   }

   FREE( softpipe );
}


struct pipe_context *
softpipe_create( struct pipe_screen *screen,
                 struct pipe_winsys *pipe_winsys,
                 void *unused )
{
   struct softpipe_context *softpipe = CALLOC_STRUCT(softpipe_context);
   uint i;
   bigtime_t beg, end;

//   beg = time(NULL);
   util_init_math();
//   end = time(NULL);
//   TRACEP("%s> util_init_math time: %f\n", __FUNCTION__, difftime(end, beg));

#ifdef PIPE_ARCH_X86
   softpipe->use_sse = !debug_get_bool_option( "GALLIUM_NOSSE", FALSE );
#else
   softpipe->use_sse = FALSE;
#endif

   softpipe->dump_fs = debug_get_bool_option( "GALLIUM_DUMP_FS", FALSE );

   softpipe->pipe.winsys = pipe_winsys;
   softpipe->pipe.screen = screen;
   softpipe->pipe.destroy = softpipe_destroy;

   /* state setters */
   softpipe->pipe.create_blend_state = softpipe_create_blend_state;
   softpipe->pipe.bind_blend_state   = softpipe_bind_blend_state;
   softpipe->pipe.delete_blend_state = softpipe_delete_blend_state;

   softpipe->pipe.create_sampler_state = softpipe_create_sampler_state;
   softpipe->pipe.bind_sampler_states  = softpipe_bind_sampler_states;
   softpipe->pipe.delete_sampler_state = softpipe_delete_sampler_state;

   softpipe->pipe.create_depth_stencil_alpha_state = softpipe_create_depth_stencil_state;
   softpipe->pipe.bind_depth_stencil_alpha_state   = softpipe_bind_depth_stencil_state;
   softpipe->pipe.delete_depth_stencil_alpha_state = softpipe_delete_depth_stencil_state;

   softpipe->pipe.create_rasterizer_state = softpipe_create_rasterizer_state;
   softpipe->pipe.bind_rasterizer_state   = softpipe_bind_rasterizer_state;
   softpipe->pipe.delete_rasterizer_state = softpipe_delete_rasterizer_state;

   softpipe->pipe.create_fs_state = softpipe_create_fs_state;
   softpipe->pipe.bind_fs_state   = softpipe_bind_fs_state;
   softpipe->pipe.delete_fs_state = softpipe_delete_fs_state;

   softpipe->pipe.create_vs_state = softpipe_create_vs_state;
   softpipe->pipe.bind_vs_state   = softpipe_bind_vs_state;
   softpipe->pipe.delete_vs_state = softpipe_delete_vs_state;

   softpipe->pipe.set_blend_color = softpipe_set_blend_color;
   softpipe->pipe.set_clip_state = softpipe_set_clip_state;
   softpipe->pipe.set_constant_buffer = softpipe_set_constant_buffer;
   softpipe->pipe.set_framebuffer_state = softpipe_set_framebuffer_state;
   softpipe->pipe.set_polygon_stipple = softpipe_set_polygon_stipple;
   softpipe->pipe.set_scissor_state = softpipe_set_scissor_state;
   softpipe->pipe.set_sampler_textures = softpipe_set_sampler_textures;
   softpipe->pipe.set_viewport_state = softpipe_set_viewport_state;

   softpipe->pipe.set_vertex_buffers = softpipe_set_vertex_buffers;
   softpipe->pipe.set_vertex_elements = softpipe_set_vertex_elements;

   softpipe->pipe.draw_arrays = softpipe_draw_arrays;
   softpipe->pipe.draw_elements = softpipe_draw_elements;
   softpipe->pipe.draw_range_elements = softpipe_draw_range_elements;
   softpipe->pipe.set_edgeflags = softpipe_set_edgeflags;


   softpipe->pipe.clear = softpipe_clear;
   softpipe->pipe.flush = softpipe_flush;

//   beg = time(NULL);
   softpipe_init_query_funcs( softpipe );
//   end = time(NULL);
//   TRACEP("%s> softpipe_init_query_funcs time: %f\n", __FUNCTION__,
//	difftime(end, beg));

//   beg = time(NULL);
   softpipe_init_texture_funcs( softpipe );
//   end = time(NULL);
//   TRACEP("%s> softpipe_init_texture_funcs time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   /*
    * Alloc caches for accessing drawing surfaces and textures.
    * Must be before quad stage setup!
    */
   beg = system_time();
   for (i = 0; i < PIPE_MAX_COLOR_BUFS; i++)
      softpipe->cbuf_cache[i] = sp_create_tile_cache( screen );
   end = system_time();
   TRACEP("%s> for(cbuf_cache) sp_create_tile_cache time: %Ld\n", __FUNCTION__, end - beg);

//   beg = time(NULL);
   softpipe->zsbuf_cache = sp_create_tile_cache( screen );
//   end = time(NULL);
//   TRACEP("%s> sp_create_tile_cache time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   beg = system_time();
   for (i = 0; i < PIPE_MAX_SAMPLERS; i++)
      softpipe->tex_cache[i] = sp_create_tile_cache( screen );
   end = system_time();
   TRACEP("%s> for(tex_cache) sp_create_tile_cache time: %Ld\n", __FUNCTION__, end - beg);


   /* setup quad rendering stages */
//   beg = time(NULL);
   for (i = 0; i < SP_NUM_QUAD_THREADS; i++) {
      softpipe->quad[i].polygon_stipple = sp_quad_polygon_stipple_stage(softpipe);
      softpipe->quad[i].earlyz = sp_quad_earlyz_stage(softpipe);
      softpipe->quad[i].shade = sp_quad_shade_stage(softpipe);
      softpipe->quad[i].alpha_test = sp_quad_alpha_test_stage(softpipe);
      softpipe->quad[i].depth_test = sp_quad_depth_test_stage(softpipe);
      softpipe->quad[i].stencil_test = sp_quad_stencil_test_stage(softpipe);
      softpipe->quad[i].occlusion = sp_quad_occlusion_stage(softpipe);
      softpipe->quad[i].coverage = sp_quad_coverage_stage(softpipe);
      softpipe->quad[i].blend = sp_quad_blend_stage(softpipe);
      softpipe->quad[i].colormask = sp_quad_colormask_stage(softpipe);
      softpipe->quad[i].output = sp_quad_output_stage(softpipe);
   }
//   end = time(NULL);
//   TRACEP("%s> setup quad rendering stages time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   /* vertex shader samplers */
//   beg = time(NULL);
   for (i = 0; i < PIPE_MAX_SAMPLERS; i++) {
      softpipe->tgsi.vert_samplers[i].base.get_samples = sp_get_samples_vertex;
      softpipe->tgsi.vert_samplers[i].unit = i;
      softpipe->tgsi.vert_samplers[i].sp = softpipe;
      softpipe->tgsi.vert_samplers[i].cache = softpipe->tex_cache[i];
      softpipe->tgsi.vert_samplers_list[i] = &softpipe->tgsi.vert_samplers[i];
   }
//   end = time(NULL);
//   TRACEP("%s> vertex shader samplers time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   /* fragment shader samplers */
//   beg = time(NULL);
   for (i = 0; i < PIPE_MAX_SAMPLERS; i++) {
      softpipe->tgsi.frag_samplers[i].base.get_samples = sp_get_samples_fragment;
      softpipe->tgsi.frag_samplers[i].unit = i;
      softpipe->tgsi.frag_samplers[i].sp = softpipe;
      softpipe->tgsi.frag_samplers[i].cache = softpipe->tex_cache[i];
      softpipe->tgsi.frag_samplers_list[i] = &softpipe->tgsi.frag_samplers[i];
   }
//   end = time(NULL);
//   TRACEP("%s> fragment shader samplers time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   /*
    * Create drawing context and plug our rendering stage into it.
    */
//   beg = time(NULL);
   softpipe->draw = draw_create();
//   end = time(NULL);
//   TRACEP("%s> draw_create time: %f\n", __FUNCTION__, difftime(end, beg));
   if (!softpipe->draw) 
      goto fail;

//   beg = time(NULL);
   draw_texture_samplers(softpipe->draw,
                         PIPE_MAX_SAMPLERS,
                         (struct tgsi_sampler **)
                            softpipe->tgsi.vert_samplers_list);
//   end = time(NULL);
//   TRACEP("%s> draw_texture_samplers time: %f\n", __FUNCTION__,
//	difftime(end, beg));

//   beg = time(NULL);
   softpipe->setup = sp_draw_render_stage(softpipe);
//   end = time(NULL);
//   TRACEP("%s> sp_draw_render_stage time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   if (!softpipe->setup)
      goto fail;

   if (debug_get_bool_option( "SP_NO_RAST", FALSE ))
      softpipe->no_rast = TRUE;

   if (debug_get_bool_option( "SP_NO_VBUF", FALSE )) {
      /* Deprecated path -- vbuf is the intended interface to the draw module:
       */
      draw_set_rasterize_stage(softpipe->draw, softpipe->setup);
   }
   else {
      sp_init_vbuf(softpipe);
   }

   /* plug in AA line/point stages */
//   beg = time(NULL);
   draw_install_aaline_stage(softpipe->draw, &softpipe->pipe);
   draw_install_aapoint_stage(softpipe->draw, &softpipe->pipe);
//   end = time(NULL);
//   TRACEP("%s> AA line/point stages time: %f\n", __FUNCTION__,
//	difftime(end, beg));

#if USE_DRAW_STAGE_PSTIPPLE
   /* Do polygon stipple w/ texture map + frag prog? */
//   beg = time(NULL);
   draw_install_pstipple_stage(softpipe->draw, &softpipe->pipe);
//   end = time(NULL);
//   TRACEP("%s> draw_install_pstipple_stage time: %f\n", __FUNCTION__,
//	difftime(end, beg));
#endif

//   beg = time(NULL);
   sp_init_surface_functions(softpipe);
//   end = time(NULL);
//   TRACEP("%s> sp_init_surface_functions time: %f\n", __FUNCTION__,
//	difftime(end, beg));

   return &softpipe->pipe;

 fail:
   softpipe_destroy(&softpipe->pipe);
   return NULL;
}

