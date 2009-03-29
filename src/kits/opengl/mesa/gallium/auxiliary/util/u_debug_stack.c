/**************************************************************************
 * 
 * Copyright 2009 VMware, Inc.
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/**
 * @file
 * Stack backtracing.
 * 
 * @author Jose Fonseca <jfonseca@vmware.com>
 */

#include "u_debug.h"
#include "u_debug_stack.h"


void
debug_backtrace_capture(struct debug_stack_frame *backtrace,
                        unsigned start_frame, 
                        unsigned nr_frames)
{
   const void **frame_pointer = NULL;
   unsigned i = 0;

   if(!nr_frames)
      return;

#if defined(PIPE_CC_GCC)
   frame_pointer = ((const void **)__builtin_frame_address(1));
#elif defined(PIPE_CC_MSVC)
   __asm {
      mov frame_pointer, ebp
   }
   frame_pointer = (const void **)frame_pointer[0];
#else
   frame_pointer = NULL;
#endif
  
   
#ifdef PIPE_ARCH_X86
   while(nr_frames) {
      if(!frame_pointer)
         break;
      
      if(start_frame)
         --start_frame;
      else {
         backtrace[i++].function = frame_pointer[1];
         --nr_frames;
      }
      
      frame_pointer = (const void **)frame_pointer[0];
   }
#endif
   
   while(nr_frames) {
      backtrace[i++].function = NULL;
      --nr_frames;
   }
}
   

void
debug_backtrace_dump(const struct debug_stack_frame *backtrace, 
                     unsigned nr_frames)
{
   unsigned i;
   
   for(i = 0; i < nr_frames; ++i) {
      if(!backtrace[i].function)
         break;
      debug_printf("\t%p\n", backtrace[i].function);
   }
}

