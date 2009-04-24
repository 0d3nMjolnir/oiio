/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <iostream>
#include <vector>

#include "dassert.h"
#include "typedesc.h"
#include "filesystem.h"
#include "plugin.h"
#include "thread.h"
#include "strutil.h"

#define DLL_EXPORT_PUBLIC /* Because we are implementing ImageIO */
#include "imageio.h"
#include "imageio_pvt.h"
#undef DLL_EXPORT_PUBLIC

using namespace OpenImageIO;
using namespace OpenImageIO::pvt;



int
ImageOutput::send_to_output (const char *format, ...)
{
    // FIXME -- I can't remember how this is supposed to work
    return 0;
}



int
ImageOutput::send_to_client (const char *format, ...)
{
    // FIXME -- I can't remember how this is supposed to work
    return 0;
}



void
ImageOutput::error (const char *format, ...)
{
    va_list ap;
    va_start (ap, format);
    m_errmessage = Strutil::vformat (format, ap);
    va_end (ap);
}



const void *
ImageOutput::to_native_scanline (TypeDesc format,
                                 const void *data, stride_t xstride,
                                 std::vector<unsigned char> &scratch)
{
    return to_native_rectangle (0, m_spec.width-1, 0, 0, 0, 0, format, data,
                                xstride, 0, 0, scratch);
}



const void *
ImageOutput::to_native_tile (TypeDesc format, const void *data,
                             stride_t xstride, stride_t ystride, stride_t zstride,
                             std::vector<unsigned char> &scratch)
{
    return to_native_rectangle (0, m_spec.tile_width-1, 0, m_spec.tile_height-1,
                                0, std::max(0,m_spec.tile_depth-1), format, data,
                                xstride, ystride, zstride, scratch);
}



const void *
ImageOutput::to_native_rectangle (int xmin, int xmax, int ymin, int ymax,
                                  int zmin, int zmax, 
                                  TypeDesc format, const void *data,
                                  stride_t xstride, stride_t ystride, stride_t zstride,
                                  std::vector<unsigned char> &scratch)
{
    m_spec.auto_stride (xstride, ystride, zstride, format, m_spec.nchannels,
                        xmax-xmin+1, ymax-ymin+1);

    // Compute width and height from the rectangle extents
    int width = xmax - xmin + 1;
    int height = ymax - ymin + 1;
    int depth = zmax - zmin + 1;

    // Do the strides indicate that the data are already contiguous?
    bool contiguous = (xstride == m_spec.nchannels*(int)format.size() &&
                       (ystride == xstride*width || height == 1) &&
                       (zstride == ystride*height || depth == 1));
    // Is the only conversion we are doing that of data format?
    bool data_conversion_only =  (contiguous && m_spec.gamma == 1.0f);

    if (format == m_spec.format && data_conversion_only) {
        // Data are already in the native format, contiguous, and need
        // no gamma correction -- just return a ptr to the original data.
        return data;
    }

    int rectangle_pixels = width * height * depth;
    int rectangle_values = rectangle_pixels * m_spec.nchannels;
    int contiguoussize = contiguous ? 0 
                             : rectangle_values * format.size();
    contiguoussize = (contiguoussize+3) & (~3); // Round up to 4-byte boundary
    DASSERT ((contiguoussize & 3) == 0);
    int rectangle_bytes = rectangle_pixels * m_spec.pixel_bytes();
    int floatsize = rectangle_values * sizeof(float);
    scratch.resize (contiguoussize + floatsize + rectangle_bytes);

    // Force contiguity if not already present
    if (! contiguous) {
        data = contiguize (data, m_spec.nchannels, xstride, ystride, zstride,
                           (void *)&scratch[0], width, height, depth, format);
    }

    // Rather than implement the entire cross-product of possible
    // conversions, use float as an intermediate format, which generally
    // will always preserve enough precision.
    const float *buf;
    if (format == TypeDesc::FLOAT && m_spec.gamma == 1.0f) {
        // Already in float format and no gamma correction is needed --
        // leave it as-is.
        buf = (float *)data;
    } else {
        // Convert to from 'format' to float.
        buf = convert_to_float (data, (float *)&scratch[contiguoussize],
                                rectangle_values, format);
        // Now buf points to float
        if (m_spec.gamma != 1) {
            float invgamma = 1.0 / m_spec.gamma;
            float *f = (float *)buf;
            for (int p = 0;  p < rectangle_pixels;  ++p)
                for (int c = 0;  c < m_spec.nchannels;  ++c, ++f)
                    if (c != m_spec.alpha_channel)
                        *f = powf (*f, invgamma);
            // FIXME: we should really move the gamma correction to
            // happen immediately after contiguization.  That way,
            // byte->byte with gamma can use a table shortcut instead
            // of having to go through float just for gamma.
        }
        // Now buf points to gamma-corrected float
    }
    // Convert from float to native format.
    return convert_from_float (buf, &scratch[contiguoussize+floatsize], 
                       rectangle_values, m_spec.quant_black, m_spec.quant_white,
                       m_spec.quant_min, m_spec.quant_max, m_spec.quant_dither,
                       m_spec.format);
}



bool
ImageOutput::write_image (TypeDesc format, const void *data,
                          stride_t xstride, stride_t ystride, stride_t zstride,
                          OpenImageIO::ProgressCallback progress_callback,
                          void *progress_callback_data)
{
    m_spec.auto_stride (xstride, ystride, zstride, format, m_spec.nchannels,
                        m_spec.width, m_spec.height);
    if (supports ("rectangles")) {
        // Use a rectangle if we can
        return write_rectangle (0, m_spec.width-1, 0, m_spec.height-1, 0, m_spec.depth-1,
                                format, data, xstride, ystride, zstride);
    }

    bool ok = true;
    if (progress_callback)
        if (progress_callback (progress_callback_data, 0.0f))
            return ok;
    if (m_spec.tile_width && supports ("tiles")) {
        // Tiled image

        // FIXME: what happens if the image dimensions are smaller than
        // the tile dimensions?  Or if one of the tiles runs past the
        // right or bottom edge?  Do we need to allocate a full tile and
        // copy into it before calling write_tile?  That's probably the
        // safe thing to do.  Or should that handling be pushed all the
        // way into write_tile itself?

        // Locally allocate a single tile to gracefully deal with image
        // dimensions smaller than a tile, or if one of the tiles runs
        // past the right or bottom edge.  Then we copy from our tile to
        // the user data, only copying valid pixel ranges.
        size_t tilexstride = m_spec.nchannels * format.size();
        size_t tileystride = tilexstride * m_spec.tile_width;
        size_t tilezstride = tileystride * m_spec.tile_height;
        size_t tile_values = (size_t)m_spec.tile_width * (size_t)m_spec.tile_height *
            (size_t)std::max(1,m_spec.tile_depth) * m_spec.nchannels;
        std::vector<char> pels (tile_values * format.size());

        for (int z = 0;  z < m_spec.depth;  z += m_spec.tile_depth)
            for (int y = 0;  y < m_spec.height;  y += m_spec.tile_height) {
                for (int x = 0;  x < m_spec.width && ok;  x += m_spec.tile_width) {
                    // Now copy out the scanlines
                    // FIXME -- can we do less work for the tiles that
                    // don't overlap image boundaries?
                    int ntz = std::min (z+m_spec.tile_depth, m_spec.depth) - z;
                    int nty = std::min (y+m_spec.tile_height, m_spec.height) - y;
                    int ntx = std::min (x+m_spec.tile_width, m_spec.width) - x;
                    for (int tz = 0;  tz < ntz;  ++tz) {
                        for (int ty = 0;  ty < nty;  ++ty) {
                            // FIXME -- doesn't work for non-contiguous scanlines
                            memcpy (&pels[ty*tileystride+tz*tilezstride],
                                    (char *)data + x*xstride + (y+ty)*ystride + (z+tz)*zstride,
                                    ntx*tilexstride);
                        }
                    }

                    ok &= write_tile (x+m_spec.x, y+m_spec.y, z+m_spec.z,
                                      format, &pels[0]);
                }
                if (progress_callback)
                    if (progress_callback (progress_callback_data, (float)y/m_spec.height))
                        return ok;
            }
    } else {
        // Scanline image
        for (int z = 0;  z < m_spec.depth;  ++z)
            for (int y = 0;  y < m_spec.height && ok;  ++y) {
                ok &= write_scanline (y+m_spec.y, z+m_spec.z, format,
                                      (const char *)data + z*zstride + y*ystride,
                                      xstride);
                if (progress_callback && !(y & 0x0f))
                    if (progress_callback (progress_callback_data, (float)y/m_spec.height))
                        return ok;
            }
    }
    if (progress_callback)
        progress_callback (progress_callback_data, 1.0f);

    return ok;
}



bool
ImageOutput::copy_image (ImageInput *in)
{
    if (! in) {
        error ("copy_image: no input supplied");
        return false;
    }

    // Make sure the images are compatible in size
    const ImageSpec &inspec (in->spec());
    if (inspec.width != spec().width || inspec.height != spec().height || 
        inspec.depth != spec().depth || inspec.nchannels != spec().nchannels) {
        error ("Could not copy %d x %d x %d channels to %d x %d x %d channels",
               inspec.width, inspec.height, inspec.nchannels,
               spec().width, spec().height, spec().nchannels);
        return false;
    }

    // Naive implementation -- read the whole image and write it back out.
    // FIXME -- a smarter implementation would read scanlines or tiles at
    // a time, to minimize mem footprint.
    std::vector<char> pixels (spec().image_bytes());
    bool ok = in->read_image (spec().format, &pixels[0]);
    if (!ok)
        error ("%s", in->error_message().c_str());  // copy err from in to out
    if (ok)
        ok = write_image (spec().format, &pixels[0]);
    return ok;
}
