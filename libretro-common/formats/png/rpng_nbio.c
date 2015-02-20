/* Copyright  (C) 2010-2015 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (rpng.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <formats/rpng.h>

#include <zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <file/nbio.h>

#ifdef GEKKO
#include <malloc.h>
#endif

#include "rpng_common.h"
#include "rpng_decode_common.h"

static bool read_chunk_header(uint8_t *buf, struct png_chunk *chunk)
{
   unsigned i;
   uint8_t dword[4] = {0};

   for (i = 0; i < 4; i++)
      dword[i] = buf[i];

   buf += 4;

   chunk->size = dword_be(dword);

   for (i = 0; i < 4; i++)
      chunk->type[i] = buf[i];

   buf += 4;

   return true;
}

static bool png_parse_ihdr(uint8_t *buf,
      struct png_ihdr *ihdr)
{
   unsigned i;
   bool ret = true;

   buf += 4 + 4;

   ihdr->width       = dword_be(buf + 0);
   ihdr->height      = dword_be(buf + 4);
   ihdr->depth       = buf[8];
   ihdr->color_type  = buf[9];
   ihdr->compression = buf[10];
   ihdr->filter      = buf[11];
   ihdr->interlace   = buf[12];

   if (ihdr->width == 0 || ihdr->height == 0)
      GOTO_END_ERROR();

   if (ihdr->color_type == 2 || 
         ihdr->color_type == 4 || ihdr->color_type == 6)
   {
      if (ihdr->depth != 8 && ihdr->depth != 16)
         GOTO_END_ERROR();
   }
   else if (ihdr->color_type == 0)
   {
      static const unsigned valid_bpp[] = { 1, 2, 4, 8, 16 };
      bool correct_bpp = false;

      for (i = 0; i < ARRAY_SIZE(valid_bpp); i++)
      {
         if (valid_bpp[i] == ihdr->depth)
         {
            correct_bpp = true;
            break;
         }
      }

      if (!correct_bpp)
         GOTO_END_ERROR();
   }
   else if (ihdr->color_type == 3)
   {
      static const unsigned valid_bpp[] = { 1, 2, 4, 8 };
      bool correct_bpp = false;

      for (i = 0; i < ARRAY_SIZE(valid_bpp); i++)
      {
         if (valid_bpp[i] == ihdr->depth)
         {
            correct_bpp = true;
            break;
         }
      }

      if (!correct_bpp)
         GOTO_END_ERROR();
   }
   else
      GOTO_END_ERROR();

#ifdef RPNG_TEST
   fprintf(stderr, "IHDR: (%u x %u), bpc = %u, palette = %s, color = %s, alpha = %s, adam7 = %s.\n",
         ihdr->width, ihdr->height,
         ihdr->depth, ihdr->color_type == 3 ? "yes" : "no",
         ihdr->color_type & 2 ? "yes" : "no",
         ihdr->color_type & 4 ? "yes" : "no",
         ihdr->interlace == 1 ? "yes" : "no");
#endif

   if (ihdr->compression != 0)
      GOTO_END_ERROR();

#if 0
   if (ihdr->interlace != 0) /* No Adam7 supported. */
      GOTO_END_ERROR();
#endif

end:
   return ret;
}

static bool png_realloc_idat(const struct png_chunk *chunk, struct idat_buffer *buf)
{
   uint8_t *new_buffer = (uint8_t*)realloc(buf->data, buf->size + chunk->size);

   if (!new_buffer)
      return false;

   buf->data  = new_buffer;
   return true;
}

static bool png_read_plte_into_buf(uint32_t *buffer, unsigned entries)
{
   unsigned i;
   uint8_t buf[256 * 3];

   for (i = 0; i < entries; i++)
   {
      uint32_t r = buf[3 * i + 0];
      uint32_t g = buf[3 * i + 1];
      uint32_t b = buf[3 * i + 2];
      buffer[i] = (r << 16) | (g << 8) | (b << 0) | (0xffu << 24);
   }

   return true;
}

bool rpng_nbio_load_image_argb_iterate(uint8_t *buf,
      struct png_chunk *chunk,
      uint32_t *palette,
      struct png_ihdr *ihdr, struct idat_buffer *idat_buf,
      bool *has_ihdr, bool *has_idat,
      bool *has_iend, bool *has_plte, size_t *increment_size)
{
   unsigned i;

#if 0
   for (i = 0; i < 4; i++)
   {
      fprintf(stderr, "chunktype: %c\n", chunk->type[i]);
   }
#endif

   switch (png_chunk_type(chunk))
   {
      case PNG_CHUNK_NOOP:
      default:
         break;

      case PNG_CHUNK_ERROR:
         return false;

      case PNG_CHUNK_IHDR:
         if (*has_ihdr || *has_idat || *has_iend)
            return false;

         if (chunk->size != 13)
            return false;

         if (!png_parse_ihdr(buf, ihdr))
            return false;

         *has_ihdr = true;
         break;

      case PNG_CHUNK_PLTE:
         {
            unsigned entries = chunk->size / 3;

            if (!*has_ihdr || *has_plte || *has_iend || *has_idat)
               return false;

            if (chunk->size % 3)
               return false;

            if (entries > 256)
               return false;

            buf += 8;

            for (i = 0; i < entries; i++)
               palette[i] = buf[i];

            if (!png_read_plte_into_buf(palette, entries))
               return false;

            *has_plte = true;
         }
         break;

      case PNG_CHUNK_IDAT:
         if (!(*has_ihdr) || *has_iend || (ihdr->color_type == 3 && !(*has_plte)))
            return false;

         if (!png_realloc_idat(chunk, idat_buf))
            return false;

         buf += 8;

         for (i = 0; i < chunk->size; i++)
            idat_buf->data[i + idat_buf->size] = buf[i];

         idat_buf->size += chunk->size;

         *has_idat = true;
         break;

      case PNG_CHUNK_IEND:
         if (!(*has_ihdr) || !(*has_idat))
            return false;

         *has_iend = true;
         return false;
   }

   return true;
}

bool rpng_nbio_load_image_argb_process(uint8_t *inflate_buf,
      struct png_ihdr *ihdr,
      struct idat_buffer *idat_buf, uint32_t **data,
      uint32_t *palette,
      unsigned *width, unsigned *height)
{
   z_stream stream = {0};
   size_t inflate_buf_size = 0;

   if (inflateInit(&stream) != Z_OK)
      return false;

   png_pass_geom(ihdr, ihdr->width,
         ihdr->height, NULL, NULL, &inflate_buf_size);
   if (ihdr->interlace == 1) /* To be sure. */
      inflate_buf_size *= 2;

   inflate_buf = (uint8_t*)malloc(inflate_buf_size);
   if (!inflate_buf)
      return false;

   stream.next_in   = idat_buf->data;
   stream.avail_in  = idat_buf->size;
   stream.avail_out = inflate_buf_size;
   stream.next_out  = inflate_buf;

   if (inflate(&stream, Z_FINISH) != Z_STREAM_END)
   {
      inflateEnd(&stream);
      return false;
   }
   inflateEnd(&stream);

   *width  = ihdr->width;
   *height = ihdr->height;
#ifdef GEKKO
   /* we often use these in textures, make sure they're 32-byte aligned */
   *data = (uint32_t*)memalign(32, ihdr->width * ihdr->height * sizeof(uint32_t));
#else
   *data = (uint32_t*)malloc(ihdr->width * ihdr->height * sizeof(uint32_t));
#endif
   if (!*data)
      return false;

   if (ihdr->interlace == 1)
   {
      if (!png_reverse_filter_adam7(*data,
               ihdr, inflate_buf, stream.total_out, palette))
         return false;
   }
   else if (!png_reverse_filter(*data,
            ihdr, inflate_buf, stream.total_out, palette))
      return false;

   return true;
}

bool rpng_nbio_load_image_argb(const char *path, uint32_t **data,
      unsigned *width, unsigned *height)
{
   size_t file_len;
   uint8_t *buff_data = NULL;
   struct nbio_t* nbread = NULL;
   uint8_t *inflate_buf = NULL;
   struct idat_buffer idat_buf = {0};
   struct png_ihdr ihdr = {0};
   uint32_t palette[256] = {0};
   bool has_ihdr = false;
   bool has_idat = false;
   bool has_iend = false;
   bool has_plte = false;
   bool ret      = true;
   void* ptr = NULL;
   size_t increment = 0;

   {
      bool looped = false;
      nbread = nbio_open(path, NBIO_READ);
      ptr  = nbio_get_ptr(nbread, &file_len);
      nbio_begin_read(nbread);

      while (!nbio_iterate(nbread)) looped=true;
      ptr = nbio_get_ptr(nbread, &file_len);
      (void)ptr;
      (void)looped;

      buff_data = (uint8_t*)ptr;
   }

   {
      unsigned i;
      char header[8];

      for (i = 0; i < 8; i++)
         header[i] = buff_data[i];

      if (memcmp(header, png_magic, sizeof(png_magic)) != 0)
         return false;

      buff_data += 8;
   }

   while (1)
   {
      struct png_chunk chunk = {0};

      if (!read_chunk_header(buff_data, &chunk))
         GOTO_END_ERROR();

      if (!rpng_nbio_load_image_argb_iterate(
            buff_data, &chunk, palette, &ihdr, &idat_buf,
            &has_ihdr, &has_idat, &has_iend, &has_plte,
            &increment))
         break;

      buff_data += 4 + 4 + chunk.size + 4;
   }

#if 0
   fprintf(stderr, "has_ihdr: %d\n", has_ihdr);
   fprintf(stderr, "has_idat: %d\n", has_idat);
   fprintf(stderr, "has_iend: %d\n", has_iend);
#endif

   if (!has_ihdr || !has_idat || !has_iend)
      GOTO_END_ERROR();
   
   rpng_nbio_load_image_argb_process(inflate_buf,
         &ihdr, &idat_buf, data, palette,
         width, height);

end:
   nbio_free(nbread);
   if (!ret)
      free(*data);
   if (idat_buf.data)
      free(idat_buf.data);
   if (inflate_buf)
      free(inflate_buf);
   return ret;
}
