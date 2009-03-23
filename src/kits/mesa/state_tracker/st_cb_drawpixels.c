/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
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

 /*
  * Authors:
  *   Brian Paul
  */

#include "main/imports.h"
#include "main/image.h"
#include "main/bufferobj.h"
#include "main/macros.h"
#include "main/texformat.h"
#include "main/state.h"
#include "shader/program.h"
#include "shader/prog_parameter.h"
#include "shader/prog_print.h"

#include "st_context.h"
#include "st_atom.h"
#include "st_atom_constbuf.h"
#include "st_draw.h"
#include "st_program.h"
#include "st_cb_drawpixels.h"
#include "st_cb_readpixels.h"
#include "st_cb_fbo.h"
#include "st_cb_texture.h"
#include "st_draw.h"
#include "st_format.h"
#include "st_mesa_to_tgsi.h"
#include "st_texture.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_inlines.h"
#include "util/u_tile.h"
#include "util/u_draw_quad.h"
#include "shader/prog_instruction.h"
#include "cso_cache/cso_context.h"


/**
 * Check if the given program is:
 * 0: MOVE result.color, fragment.color;
 * 1: END;
 */
static GLboolean
is_passthrough_program(const struct gl_fragment_program *prog)
{
   if (prog->Base.NumInstructions == 2) {
      const struct prog_instruction *inst = prog->Base.Instructions;
      if (inst[0].Opcode == OPCODE_MOV &&
          inst[1].Opcode == OPCODE_END &&
          inst[0].DstReg.File == PROGRAM_OUTPUT &&
          inst[0].DstReg.Index == FRAG_RESULT_COLOR &&
          inst[0].DstReg.WriteMask == WRITEMASK_XYZW &&
          inst[0].SrcReg[0].File == PROGRAM_INPUT &&
          inst[0].SrcReg[0].Index == FRAG_ATTRIB_COL0 &&
          inst[0].SrcReg[0].Swizzle == SWIZZLE_XYZW) {
         return GL_TRUE;
      }
   }
   return GL_FALSE;
}



/**
 * Make fragment shader for glDraw/CopyPixels.  This shader is made
 * by combining the pixel transfer shader with the user-defined shader.
 */
static struct st_fragment_program *
combined_drawpix_fragment_program(GLcontext *ctx)
{
   struct st_context *st = ctx->st;
   struct st_fragment_program *stfp;

   if (st->pixel_xfer.program->serialNo == st->pixel_xfer.xfer_prog_sn
       && st->fp->serialNo == st->pixel_xfer.user_prog_sn) {
      /* the pixel tranfer program has not changed and the user-defined
       * program has not changed, so re-use the combined program.
       */
      stfp = st->pixel_xfer.combined_prog;
   }
   else {
      /* Concatenate the pixel transfer program with the current user-
       * defined program.
       */
      if (is_passthrough_program(&st->fp->Base)) {
         stfp = (struct st_fragment_program *)
            _mesa_clone_program(ctx, &st->pixel_xfer.program->Base.Base);
      }
      else {
#if 0
         printf("Base program:\n");
         _mesa_print_program(&st->fp->Base.Base);
         printf("DrawPix program:\n");
         _mesa_print_program(&st->pixel_xfer.program->Base.Base);
#endif
         stfp = (struct st_fragment_program *)
            _mesa_combine_programs(ctx,
                                   &st->pixel_xfer.program->Base.Base,
                                   &st->fp->Base.Base);
      }

#if 0
      {
         struct gl_program *p = &stfp->Base.Base;
         printf("Combined DrawPixels program:\n");
         _mesa_print_program(p);
         printf("InputsRead: 0x%x\n", p->InputsRead);
         printf("OutputsWritten: 0x%x\n", p->OutputsWritten);
         _mesa_print_parameter_list(p->Parameters);
      }
#endif

      /* translate to TGSI tokens */
      st_translate_fragment_program(st, stfp, NULL);

      /* save new program, update serial numbers */
      st->pixel_xfer.xfer_prog_sn = st->pixel_xfer.program->serialNo;
      st->pixel_xfer.user_prog_sn = st->fp->serialNo;
      st->pixel_xfer.combined_prog_sn = stfp->serialNo;
      st->pixel_xfer.combined_prog = stfp;
   }

   /* Ideally we'd have updated the pipe constants during the normal
    * st/atom mechanism.  But we can't since this is specific to glDrawPixels.
    */
   st_upload_constants(st, stfp->Base.Base.Parameters, PIPE_SHADER_FRAGMENT);

   return stfp;
}


/**
 * Create fragment shader that does a TEX() instruction to get a Z
 * value, then writes to FRAG_RESULT_DEPTH.
 * Pass fragment color through as-is.
 */
static struct st_fragment_program *
make_fragment_shader_z(struct st_context *st)
{
   GLcontext *ctx = st->ctx;
   struct gl_program *p;
   GLuint ic = 0;

   if (st->drawpix.z_shader) {
      return st->drawpix.z_shader;
   }

   /*
    * Create shader now
    */
   p = ctx->Driver.NewProgram(ctx, GL_FRAGMENT_PROGRAM_ARB, 0);
   if (!p)
      return NULL;

   p->NumInstructions = 3;

   p->Instructions = _mesa_alloc_instructions(p->NumInstructions);
   if (!p->Instructions) {
      ctx->Driver.DeleteProgram(ctx, p);
      return NULL;
   }
   _mesa_init_instructions(p->Instructions, p->NumInstructions);

   /* TEX result.depth, fragment.texcoord[0], texture[0], 2D; */
   p->Instructions[ic].Opcode = OPCODE_TEX;
   p->Instructions[ic].DstReg.File = PROGRAM_OUTPUT;
   p->Instructions[ic].DstReg.Index = FRAG_RESULT_DEPTH;
   p->Instructions[ic].DstReg.WriteMask = WRITEMASK_Z;
   p->Instructions[ic].SrcReg[0].File = PROGRAM_INPUT;
   p->Instructions[ic].SrcReg[0].Index = FRAG_ATTRIB_TEX0;
   p->Instructions[ic].TexSrcUnit = 0;
   p->Instructions[ic].TexSrcTarget = TEXTURE_2D_INDEX;
   ic++;

   /* MOV result.color, fragment.color */
   p->Instructions[ic].Opcode = OPCODE_MOV;
   p->Instructions[ic].DstReg.File = PROGRAM_OUTPUT;
   p->Instructions[ic].DstReg.Index = FRAG_RESULT_COLOR;
   p->Instructions[ic].SrcReg[0].File = PROGRAM_INPUT;
   p->Instructions[ic].SrcReg[0].Index = FRAG_ATTRIB_COL0;
   ic++;

   /* END; */
   p->Instructions[ic++].Opcode = OPCODE_END;

   assert(ic == p->NumInstructions);

   p->InputsRead = FRAG_BIT_TEX0 | FRAG_BIT_COL0;
   p->OutputsWritten = (1 << FRAG_RESULT_COLOR) | (1 << FRAG_RESULT_DEPTH);
   p->SamplersUsed = 0x1;  /* sampler 0 (bit 0) is used */

   st->drawpix.z_shader = (struct st_fragment_program *) p;
   st_translate_fragment_program(st, st->drawpix.z_shader, NULL);

   return st->drawpix.z_shader;
}



/**
 * Create a simple vertex shader that just passes through the
 * vertex position and texcoord (and optionally, color).
 */
static struct st_vertex_program *
st_make_passthrough_vertex_shader(struct st_context *st, GLboolean passColor)
{
   GLcontext *ctx = st->ctx;
   struct st_vertex_program *stvp;
   struct gl_program *p;
   GLuint ic = 0;

   if (st->drawpix.vert_shaders[passColor])
      return st->drawpix.vert_shaders[passColor];

   /*
    * Create shader now
    */
   p = ctx->Driver.NewProgram(ctx, GL_VERTEX_PROGRAM_ARB, 0);
   if (!p)
      return NULL;

   if (passColor)
      p->NumInstructions = 4;
   else
      p->NumInstructions = 3;

   p->Instructions = _mesa_alloc_instructions(p->NumInstructions);
   if (!p->Instructions) {
      ctx->Driver.DeleteProgram(ctx, p);
      return NULL;
   }
   _mesa_init_instructions(p->Instructions, p->NumInstructions);
   /* MOV result.pos, vertex.pos; */
   p->Instructions[0].Opcode = OPCODE_MOV;
   p->Instructions[0].DstReg.File = PROGRAM_OUTPUT;
   p->Instructions[0].DstReg.Index = VERT_RESULT_HPOS;
   p->Instructions[0].SrcReg[0].File = PROGRAM_INPUT;
   p->Instructions[0].SrcReg[0].Index = VERT_ATTRIB_POS;
   /* MOV result.texcoord0, vertex.texcoord0; */
   p->Instructions[1].Opcode = OPCODE_MOV;
   p->Instructions[1].DstReg.File = PROGRAM_OUTPUT;
   p->Instructions[1].DstReg.Index = VERT_RESULT_TEX0;
   p->Instructions[1].SrcReg[0].File = PROGRAM_INPUT;
   p->Instructions[1].SrcReg[0].Index = VERT_ATTRIB_TEX0;
   ic = 2;
   if (passColor) {
      /* MOV result.color0, vertex.color0; */
      p->Instructions[ic].Opcode = OPCODE_MOV;
      p->Instructions[ic].DstReg.File = PROGRAM_OUTPUT;
      p->Instructions[ic].DstReg.Index = VERT_RESULT_COL0;
      p->Instructions[ic].SrcReg[0].File = PROGRAM_INPUT;
      p->Instructions[ic].SrcReg[0].Index = VERT_ATTRIB_COLOR0;
      ic++;
   }

   /* END; */
   p->Instructions[ic].Opcode = OPCODE_END;
   ic++;

   assert(ic == p->NumInstructions);

   p->InputsRead = VERT_BIT_POS | VERT_BIT_TEX0;
   p->OutputsWritten = ((1 << VERT_RESULT_TEX0) |
                        (1 << VERT_RESULT_HPOS));
   if (passColor) {
      p->InputsRead |= VERT_BIT_COLOR0;
      p->OutputsWritten |= (1 << VERT_RESULT_COL0);
   }

   stvp = (struct st_vertex_program *) p;
   st_translate_vertex_program(st, stvp, NULL, NULL, NULL);

   st->drawpix.vert_shaders[passColor] = stvp;

   return stvp;
}


static GLenum
_mesa_base_format(GLenum format)
{
   switch (format) {
   case GL_DEPTH_COMPONENT:
      return GL_DEPTH_COMPONENT;
   case GL_STENCIL_INDEX:
      return GL_STENCIL_INDEX;
   default:
      return GL_RGBA;
   }
}


/**
 * Make texture containing an image for glDrawPixels image.
 * If 'pixels' is NULL, leave the texture image data undefined.
 */
static struct pipe_texture *
make_texture(struct st_context *st,
	     GLsizei width, GLsizei height, GLenum format, GLenum type,
	     const struct gl_pixelstore_attrib *unpack,
	     const GLvoid *pixels)
{
   GLcontext *ctx = st->ctx;
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;
   const struct gl_texture_format *mformat;
   struct pipe_texture *pt;
   enum pipe_format pipeFormat;
   GLuint cpp;
   GLenum baseFormat;

   baseFormat = _mesa_base_format(format);

   mformat = st_ChooseTextureFormat(ctx, baseFormat, format, type);
   assert(mformat);

   pipeFormat = st_mesa_format_to_pipe_format(mformat->MesaFormat);
   assert(pipeFormat);
   cpp = st_sizeof_format(pipeFormat);

   pixels = _mesa_map_drawpix_pbo(ctx, unpack, pixels);
   if (!pixels)
      return NULL;

   pt = st_texture_create(st, PIPE_TEXTURE_2D, pipeFormat, 0, width, height,
			  1, 0,
                          PIPE_TEXTURE_USAGE_SAMPLER);
   if (!pt) {
      _mesa_unmap_drawpix_pbo(ctx, unpack);
      return NULL;
   }

   {
      struct pipe_transfer *transfer;
      static const GLuint dstImageOffsets = 0;
      GLboolean success;
      GLubyte *dest;
      const GLbitfield imageTransferStateSave = ctx->_ImageTransferState;

      /* we'll do pixel transfer in a fragment shader */
      ctx->_ImageTransferState = 0x0;

      transfer = screen->get_tex_transfer(screen, pt, 0, 0, 0,
                                          PIPE_TRANSFER_WRITE, 0, 0,
                                          width, height);

      /* map texture transfer */
      dest = screen->transfer_map(screen, transfer);

      /* Put image into texture transfer.
       * Note that the image is actually going to be upside down in
       * the texture.  We deal with that with texcoords.
       */
      success = mformat->StoreImage(ctx, 2,           /* dims */
                                    baseFormat,       /* baseInternalFormat */
                                    mformat,          /* gl_texture_format */
                                    dest,             /* dest */
                                    0, 0, 0,          /* dstX/Y/Zoffset */
                                    transfer->stride, /* dstRowStride, bytes */
                                    &dstImageOffsets, /* dstImageOffsets */
                                    width, height, 1, /* size */
                                    format, type,     /* src format/type */
                                    pixels,           /* data source */
                                    unpack);

      /* unmap */
      screen->transfer_unmap(screen, transfer);
      screen->tex_transfer_destroy(transfer);

      assert(success);

      /* restore */
      ctx->_ImageTransferState = imageTransferStateSave;
   }

   _mesa_unmap_drawpix_pbo(ctx, unpack);

   return pt;
}


/**
 * Draw quad with texcoords and optional color.
 * Coords are window coords with y=0=bottom.
 * \param color  may be null
 * \param invertTex  if true, flip texcoords vertically
 */
static void
draw_quad(GLcontext *ctx, GLfloat x0, GLfloat y0, GLfloat z,
          GLfloat x1, GLfloat y1, const GLfloat *color,
          GLboolean invertTex)
{
   struct st_context *st = ctx->st;
   struct pipe_context *pipe = ctx->st->pipe;
   GLfloat verts[4][3][4]; /* four verts, three attribs, XYZW */

   /* setup vertex data */
   {
      const struct gl_framebuffer *fb = st->ctx->DrawBuffer;
      const GLfloat fb_width = (GLfloat) fb->Width;
      const GLfloat fb_height = (GLfloat) fb->Height;
      const GLfloat clip_x0 = x0 / fb_width * 2.0f - 1.0f;
      const GLfloat clip_y0 = y0 / fb_height * 2.0f - 1.0f;
      const GLfloat clip_x1 = x1 / fb_width * 2.0f - 1.0f;
      const GLfloat clip_y1 = y1 / fb_height * 2.0f - 1.0f;
      const GLfloat sLeft = 0.0f, sRight = 1.0f;
      const GLfloat tTop = invertTex, tBot = 1.0f - tTop;
      GLuint tex, i;

      /* upper-left */
      verts[0][0][0] = clip_x0;    /* v[0].attr[0].x */
      verts[0][0][1] = clip_y0;    /* v[0].attr[0].y */

      /* upper-right */
      verts[1][0][0] = clip_x1;
      verts[1][0][1] = clip_y0;

      /* lower-right */
      verts[2][0][0] = clip_x1;
      verts[2][0][1] = clip_y1;

      /* lower-left */
      verts[3][0][0] = clip_x0;
      verts[3][0][1] = clip_y1;

      tex = color ? 2 : 1;
      verts[0][tex][0] = sLeft; /* v[0].attr[tex].s */
      verts[0][tex][1] = tTop;  /* v[0].attr[tex].t */
      verts[1][tex][0] = sRight;
      verts[1][tex][1] = tTop;
      verts[2][tex][0] = sRight;
      verts[2][tex][1] = tBot;
      verts[3][tex][0] = sLeft;
      verts[3][tex][1] = tBot;

      /* same for all verts: */
      if (color) {
         for (i = 0; i < 4; i++) {
            verts[i][0][2] = z;   /*Z*/
            verts[i][0][3] = 1.0f; /*W*/
            verts[i][1][0] = color[0];
            verts[i][1][1] = color[1];
            verts[i][1][2] = color[2];
            verts[i][1][3] = color[3];
            verts[i][2][2] = 0.0f; /*R*/
            verts[i][2][3] = 1.0f; /*Q*/
         }
      }
      else {
         for (i = 0; i < 4; i++) {
            verts[i][0][2] = z;   /*Z*/
            verts[i][0][3] = 1.0f; /*W*/
            verts[i][1][2] = 0.0f; /*R*/
            verts[i][1][3] = 1.0f; /*Q*/
         }
      }
   }

   {
      struct pipe_buffer *buf;

      /* allocate/load buffer object with vertex data */
      buf = pipe_buffer_create(pipe->screen, 32, PIPE_BUFFER_USAGE_VERTEX,
                               sizeof(verts));
      pipe_buffer_write(pipe->screen, buf, 0, sizeof(verts), verts);

      util_draw_vertex_buffer(pipe, buf, 0,
                              PIPE_PRIM_QUADS,
                              4,  /* verts */
                              3); /* attribs/vert */
      pipe_buffer_reference(&buf, NULL);
   }
}



static void
draw_textured_quad(GLcontext *ctx, GLint x, GLint y, GLfloat z,
                   GLsizei width, GLsizei height,
                   GLfloat zoomX, GLfloat zoomY,
                   struct pipe_texture *pt,
                   struct st_vertex_program *stvp,
                   struct st_fragment_program *stfp,
                   const GLfloat *color,
                   GLboolean invertTex)
{
   struct st_context *st = ctx->st;
   struct pipe_context *pipe = ctx->st->pipe;
   struct cso_context *cso = ctx->st->cso_context;
   GLfloat x0, y0, x1, y1;
   GLsizei maxSize;

   /* limit checks */
   /* XXX if DrawPixels image is larger than max texture size, break
    * it up into chunks.
    */
   maxSize = 1 << (pipe->screen->get_param(pipe->screen, PIPE_CAP_MAX_TEXTURE_2D_LEVELS) - 1);
   assert(width <= maxSize);
   assert(height <= maxSize);

   cso_save_rasterizer(cso);
   cso_save_viewport(cso);
   cso_save_samplers(cso);
   cso_save_sampler_textures(cso);
   cso_save_fragment_shader(cso);
   cso_save_vertex_shader(cso);

   /* rasterizer state: just scissor */
   {
      struct pipe_rasterizer_state rasterizer;
      memset(&rasterizer, 0, sizeof(rasterizer));
      rasterizer.gl_rasterization_rules = 1;
      rasterizer.scissor = ctx->Scissor.Enabled;
      cso_set_rasterizer(cso, &rasterizer);
   }

   /* fragment shader state: TEX lookup program */
   cso_set_fragment_shader_handle(cso, stfp->driver_shader);

   /* vertex shader state: position + texcoord pass-through */
   cso_set_vertex_shader_handle(cso, stvp->driver_shader);


   /* texture sampling state: */
   {
      struct pipe_sampler_state sampler;
      memset(&sampler, 0, sizeof(sampler));
      sampler.wrap_s = PIPE_TEX_WRAP_CLAMP;
      sampler.wrap_t = PIPE_TEX_WRAP_CLAMP;
      sampler.wrap_r = PIPE_TEX_WRAP_CLAMP;
      sampler.min_img_filter = PIPE_TEX_FILTER_NEAREST;
      sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
      sampler.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
      sampler.normalized_coords = 1;

      cso_single_sampler(cso, 0, &sampler);
      if (st->pixel_xfer.pixelmap_enabled) {
         cso_single_sampler(cso, 1, &sampler);
      }
      cso_single_sampler_done(cso);
   }

   /* viewport state: viewport matching window dims */
   {
      const float width = (float) ctx->DrawBuffer->Width;
      const float height = (float) ctx->DrawBuffer->Height;
      struct pipe_viewport_state vp;
      vp.scale[0] =  0.5f * width;
      vp.scale[1] = -0.5f * height;
      vp.scale[2] = 1.0f;
      vp.scale[3] = 1.0f;
      vp.translate[0] = 0.5f * width;
      vp.translate[1] = 0.5f * height;
      vp.translate[2] = 0.0f;
      vp.translate[3] = 0.0f;
      cso_set_viewport(cso, &vp);
   }

   /* texture state: */
   if (st->pixel_xfer.pixelmap_enabled) {
      struct pipe_texture *textures[2];
      textures[0] = pt;
      textures[1] = st->pixel_xfer.pixelmap_texture;
      pipe->set_sampler_textures(pipe, 2, textures);
   }
   else {
      pipe->set_sampler_textures(pipe, 1, &pt);
   }

   /* Compute window coords (y=0=bottom) with pixel zoom.
    * Recall that these coords are transformed by the current
    * vertex shader and viewport transformation.
    */
   x0 = (GLfloat) x;
   x1 = x + width * ctx->Pixel.ZoomX;
   y0 = (GLfloat) y;
   y1 = y + height * ctx->Pixel.ZoomY;

   draw_quad(ctx, x0, y0, z, x1, y1, color, invertTex);

   /* restore state */
   cso_restore_rasterizer(cso);
   cso_restore_viewport(cso);
   cso_restore_samplers(cso);
   cso_restore_sampler_textures(cso);
   cso_restore_fragment_shader(cso);
   cso_restore_vertex_shader(cso);
}


static void
draw_stencil_pixels(GLcontext *ctx, GLint x, GLint y,
                    GLsizei width, GLsizei height, GLenum type,
                    const struct gl_pixelstore_attrib *unpack,
                    const GLvoid *pixels)
{
   struct st_context *st = ctx->st;
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;
   struct st_renderbuffer *strb;
   struct pipe_transfer *pt;
   const GLboolean zoom = ctx->Pixel.ZoomX != 1.0 || ctx->Pixel.ZoomY != 1.0;
   GLint skipPixels;
   ubyte *stmap;

   pipe->flush(pipe, PIPE_FLUSH_RENDER_CACHE, NULL);

   strb = st_renderbuffer(ctx->DrawBuffer->
                          Attachment[BUFFER_STENCIL].Renderbuffer);
   pt = screen->get_tex_transfer(screen, strb->texture, 0, 0, 0,
                                 PIPE_TRANSFER_WRITE, x, y,
                                 width, height);

   stmap = screen->transfer_map(screen, pt);

   /* if width > MAX_WIDTH, have to process image in chunks */
   skipPixels = 0;
   while (skipPixels < width) {
      const GLint spanX = skipPixels;
      const GLint spanWidth = MIN2(width - skipPixels, MAX_WIDTH);
      GLint row;
      for (row = 0; row < height; row++) {
         GLint spanY = row;
         GLubyte values[MAX_WIDTH];
         GLenum destType = GL_UNSIGNED_BYTE;
         const GLvoid *source = _mesa_image_address2d(unpack, pixels,
                                                      width, height,
                                                      GL_COLOR_INDEX, type,
                                                      row, skipPixels);
         _mesa_unpack_stencil_span(ctx, spanWidth, destType, values,
                                   type, source, unpack,
                                   ctx->_ImageTransferState);
         if (zoom) {
            /*
            _swrast_write_zoomed_stencil_span(ctx, 0, 0, spanWidth,
                                              spanX, spanY, values);
            */
         }
         else {
            if (st_fb_orientation(ctx->DrawBuffer) == Y_0_TOP) {
               spanY = height - spanY - 1;
            }

            switch (pt->format) {
            case PIPE_FORMAT_S8_UNORM:
               {
                  ubyte *dest = stmap + spanY * pt->stride + spanX;
                  memcpy(dest, values, spanWidth);
               }
               break;
            case PIPE_FORMAT_S8Z24_UNORM:
               {
                  uint *dest = (uint *) (stmap + spanY * pt->stride + spanX*4);
                  GLint k;
                  for (k = 0; k < spanWidth; k++) {
                     uint p = dest[k];
                     p = (p & 0xffffff) | (values[k] << 24);
                     dest[k] = p;
                  }
               }
               break;
            default:
               assert(0);
            }
         }
      }
      skipPixels += spanWidth;
   }

   /* unmap the stencil buffer */
   screen->transfer_unmap(screen, pt);
   screen->tex_transfer_destroy(pt);
}


/**
 * Called via ctx->Driver.DrawPixels()
 */
static void
st_DrawPixels(GLcontext *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
              GLenum format, GLenum type,
              const struct gl_pixelstore_attrib *unpack, const GLvoid *pixels)
{
   struct st_fragment_program *stfp;
   struct st_vertex_program *stvp;
   struct st_context *st = ctx->st;
   struct pipe_surface *ps;
   GLuint bufferFormat;
   const GLfloat *color;

   if (format == GL_STENCIL_INDEX) {
      draw_stencil_pixels(ctx, x, y, width, height, type, unpack, pixels);
      return;
   }

   _mesa_set_vp_override( ctx, TRUE );
   _mesa_update_state( ctx );

   st_validate_state(st);

   if (format == GL_DEPTH_COMPONENT) {
      ps = st->state.framebuffer.zsbuf;
      stfp = make_fragment_shader_z(ctx->st);
      stvp = st_make_passthrough_vertex_shader(ctx->st, GL_TRUE);
      color = ctx->Current.RasterColor;
   }
   else if (format == GL_STENCIL_INDEX) {
      ps = st->state.framebuffer.zsbuf;
      /* XXX special case - can't use texture map */
      color = NULL;
   }
   else {
      ps = st->state.framebuffer.cbufs[0];
      stfp = combined_drawpix_fragment_program(ctx);
      stvp = st_make_passthrough_vertex_shader(ctx->st, GL_FALSE);
      color = NULL;
   }

   bufferFormat = ps->format;

   /* draw with textured quad */
   {
      struct pipe_texture *pt
         = make_texture(ctx->st, width, height, format, type, unpack, pixels);
      if (pt) {
         draw_textured_quad(ctx, x, y, ctx->Current.RasterPos[2],
                            width, height, ctx->Pixel.ZoomX, ctx->Pixel.ZoomY,
                            pt, stvp, stfp, color, GL_FALSE);
         pipe_texture_reference(&pt, NULL);
      }
   }

   _mesa_set_vp_override( ctx, FALSE );
}



static void
copy_stencil_pixels(GLcontext *ctx, GLint srcx, GLint srcy,
                    GLsizei width, GLsizei height,
                    GLint dstx, GLint dsty)
{
   struct st_renderbuffer *rbDraw = st_renderbuffer(ctx->DrawBuffer->_StencilBuffer);
   struct pipe_screen *screen = ctx->st->pipe->screen;
   struct pipe_transfer *ptDraw;
   ubyte *drawMap;
   ubyte *buffer;
   int i;

   buffer = _mesa_malloc(width * height * sizeof(ubyte));
   if (!buffer) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "glCopyPixels(stencil)");
      return;
   }

   /* this will do stencil pixel transfer ops */
   st_read_stencil_pixels(ctx, srcx, srcy, width, height, GL_UNSIGNED_BYTE,
                          &ctx->DefaultPacking, buffer);

   ptDraw = screen->get_tex_transfer(screen, rbDraw->texture, 0, 0, 0,
                                     PIPE_TRANSFER_WRITE, dstx, dsty,
                                     width, height);

   assert(ptDraw->block.width == 1);
   assert(ptDraw->block.height == 1);
   
   /* map the stencil buffer */
   drawMap = screen->transfer_map(screen, ptDraw);

   /* draw */
   /* XXX PixelZoom not handled yet */
   for (i = 0; i < height; i++) {
      ubyte *dst;
      const ubyte *src;
      int y;

      y = i;

      if (st_fb_orientation(ctx->DrawBuffer) == Y_0_TOP) {
         y = height - y - 1;
      }

      dst = drawMap + y * ptDraw->stride;
      src = buffer + i * width;

      switch (ptDraw->format) {
      case PIPE_FORMAT_S8Z24_UNORM:
         {
            uint *dst4 = (uint *) dst;
            int j;
            for (j = 0; j < width; j++) {
               *dst4 = (*dst4 & 0xffffff) | (src[j] << 24);
               dst4++;
            }
         }
         break;
      case PIPE_FORMAT_S8_UNORM:
         memcpy(dst, src, width);
         break;
      default:
         assert(0);
      }
   }

   _mesa_free(buffer);

   /* unmap the stencil buffer */
   screen->transfer_unmap(screen, ptDraw);
   screen->tex_transfer_destroy(ptDraw);
}


static void
st_CopyPixels(GLcontext *ctx, GLint srcx, GLint srcy,
              GLsizei width, GLsizei height,
              GLint dstx, GLint dsty, GLenum type)
{
   struct st_context *st = ctx->st;
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;
   struct st_renderbuffer *rbRead;
   struct st_vertex_program *stvp;
   struct st_fragment_program *stfp;
   struct pipe_texture *pt;
   GLfloat *color;
   enum pipe_format srcFormat, texFormat;

   /* make sure rendering has completed */
   pipe->flush(pipe, PIPE_FLUSH_RENDER_CACHE, NULL);

   st_validate_state(st);

   if (type == GL_STENCIL) {
      /* can't use texturing to do stencil */
      copy_stencil_pixels(ctx, srcx, srcy, width, height, dstx, dsty);
      return;
   }

   if (type == GL_COLOR) {
      rbRead = st_get_color_read_renderbuffer(ctx);
      color = NULL;
      stfp = combined_drawpix_fragment_program(ctx);
      stvp = st_make_passthrough_vertex_shader(ctx->st, GL_FALSE);
   }
   else {
      assert(type == GL_DEPTH);
      rbRead = st_renderbuffer(ctx->ReadBuffer->_DepthBuffer);
      color = ctx->Current.Attrib[VERT_ATTRIB_COLOR0];
      stfp = make_fragment_shader_z(ctx->st);
      stvp = st_make_passthrough_vertex_shader(ctx->st, GL_TRUE);
   }

   srcFormat = rbRead->texture->format;

   if (screen->is_format_supported(screen, srcFormat, PIPE_TEXTURE_2D, 
                                   PIPE_TEXTURE_USAGE_SAMPLER, 0)) {
      texFormat = srcFormat;
   }
   else {
      /* srcFormat can't be used as a texture format */
      if (type == GL_DEPTH) {
         texFormat = st_choose_format(pipe, GL_DEPTH_COMPONENT, PIPE_TEXTURE_2D, 
                                      PIPE_TEXTURE_USAGE_DEPTH_STENCIL);
         assert(texFormat != PIPE_FORMAT_NONE); /* XXX no depth texture formats??? */
      }
      else {
         /* default color format */
         texFormat = st_choose_format(pipe, GL_RGBA, PIPE_TEXTURE_2D, 
                                      PIPE_TEXTURE_USAGE_SAMPLER);
         assert(texFormat != PIPE_FORMAT_NONE);
      }
   }

   pt = st_texture_create(ctx->st, PIPE_TEXTURE_2D, texFormat, 0,
                          width, height, 1, 0,
                          PIPE_TEXTURE_USAGE_SAMPLER);
   if (!pt)
      return;

   if (st_fb_orientation(ctx->DrawBuffer) == Y_0_TOP) {
      srcy = ctx->DrawBuffer->Height - srcy - height;
   }

   if (srcFormat == texFormat) {
      /* copy source framebuffer surface into mipmap/texture */
      struct pipe_surface *psRead = screen->get_tex_surface(screen,
                                       rbRead->texture, 0, 0, 0,
                                       PIPE_BUFFER_USAGE_GPU_READ);
      struct pipe_surface *psTex = screen->get_tex_surface(screen, pt, 0, 0, 0, 
                                      PIPE_BUFFER_USAGE_GPU_WRITE );
      pipe->surface_copy(pipe,
			 psTex, /* dest */
			 0, 0, /* destx/y */
			 psRead,
			 srcx, srcy, width, height);
      pipe_surface_reference(&psRead, NULL); 
      pipe_surface_reference(&psTex, NULL);
   }
   else {
      /* CPU-based fallback/conversion */
      struct pipe_transfer *ptRead =
         screen->get_tex_transfer(screen, rbRead->texture, 0, 0, 0,
                                  PIPE_TRANSFER_READ, srcx, srcy, width,
                                  height);

      struct pipe_transfer *ptTex =
         screen->get_tex_transfer(screen, pt, 0, 0, 0, PIPE_TRANSFER_WRITE,
                                  0, 0, width, height);

      if (type == GL_COLOR) {
         /* alternate path using get/put_tile() */
         GLfloat *buf = (GLfloat *) _mesa_malloc(width * height * 4 * sizeof(GLfloat));

         pipe_get_tile_rgba(ptRead, 0, 0, width, height, buf);
         pipe_put_tile_rgba(ptTex, 0, 0, width, height, buf);

         _mesa_free(buf);
      }
      else {
         /* GL_DEPTH */
         GLuint *buf = (GLuint *) _mesa_malloc(width * height * sizeof(GLuint));
         pipe_get_tile_z(ptRead, 0, 0, width, height, buf);
         pipe_put_tile_z(ptTex, 0, 0, width, height, buf);
         _mesa_free(buf);
      }

      screen->tex_transfer_destroy(ptRead);
      screen->tex_transfer_destroy(ptTex);
   }

   /* draw textured quad */
   draw_textured_quad(ctx, dstx, dsty, ctx->Current.RasterPos[2],
                      width, height, ctx->Pixel.ZoomX, ctx->Pixel.ZoomY,
                      pt, stvp, stfp, color, GL_TRUE);

   pipe_texture_reference(&pt, NULL);
}



void st_init_drawpixels_functions(struct dd_function_table *functions)
{
   functions->DrawPixels = st_DrawPixels;
   functions->CopyPixels = st_CopyPixels;
}


void
st_destroy_drawpix(struct st_context *st)
{
   st_reference_fragprog(st, &st->drawpix.z_shader, NULL);
   st_reference_fragprog(st, &st->pixel_xfer.combined_prog, NULL);
   st_reference_vertprog(st, &st->drawpix.vert_shaders[0], NULL);
   st_reference_vertprog(st, &st->drawpix.vert_shaders[1], NULL);
}
