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


/// \file
/// Implementation of ImageBuf class.


#include <iostream>

#include <ImathFun.h>
#include <half.h>
#include <boost/scoped_ptr.hpp>

#include "imageio.h"
#define DLL_EXPORT_PUBLIC /* Because we are implementing ImageBuf */
#include "imagebuf.h"
#undef DLL_EXPORT_PUBLIC
#include "dassert.h"
#include "fmath.h"


namespace OpenImageIO {



ImageBuf::ImageBuf (const std::string &filename)
    : m_name(filename), m_nsubimages(0), m_current_subimage(0),
      m_spec_valid(false), m_pixels_valid(false),
      m_badfile(false), m_orientation(1), m_pixelaspect(1)
{
}



ImageBuf::ImageBuf (const std::string &filename, const ImageSpec &spec)
    : m_name(filename), m_nsubimages(0), m_current_subimage(0),
      m_spec_valid(false), m_pixels_valid(false),
      m_badfile(false), m_orientation(1), m_pixelaspect(1)
{
    alloc (spec);
}



ImageBuf::~ImageBuf ()
{
}



void
ImageBuf::realloc ()
{
    size_t newsize = spec().image_bytes ();
    if (((int)newsize - (int)m_pixels.size()) > 1024*1024) {
        // If we are substantially shrinking, try to actually free
        // memory, which std::vector::resize does not do!
        std::vector<char> tmp;      // vector with 0 memory
        std::swap (tmp, m_pixels);  // Now tmp holds the mem, not m_pixels
        // As tmp leaves scope, it frees m_pixels's old memory
    }
    m_pixels.resize (newsize);
}



void
ImageBuf::alloc (const ImageSpec &spec)
{
    m_spec = spec;
    m_spec_valid = true;
    realloc ();
}



bool
ImageBuf::init_spec (const std::string &filename)
{
    m_name = filename;
    boost::scoped_ptr<ImageInput> in (ImageInput::create (filename.c_str(), "" /* searchpath */));
    if (! in) {
        std::cerr << OpenImageIO::error_message() << "\n";
    }
    if (in && in->open (filename.c_str(), m_spec)) {
        m_fileformat = in->format_name ();
        ImageSpec tempspec;
        m_nsubimages = 1;
        while (in->seek_subimage (m_nsubimages, tempspec))
            ++m_nsubimages;
//        std::cerr << filename << " has " << m_nsubimages << " subimages\n";
        m_current_subimage = 0;
        in->close ();
        m_badfile = false;
        m_spec_valid = true;
    } else {
        m_badfile = true;
        m_spec_valid = false;
    }
    return !m_badfile;
}



bool
ImageBuf::read (int subimage, bool force, TypeDesc convert,
               OpenImageIO::ProgressCallback progress_callback,
               void *progress_callback_data)
{
    if (pixels_valid() && !force)
        return true;

    // Find an ImageIO plugin that can open the input file, and open it.
    boost::scoped_ptr<ImageInput> in (ImageInput::create (m_name.c_str(), "" /* searchpath */));
    if (! in) {
        m_err = OpenImageIO::error_message();
        return false;
    }
    if (in->open (m_name.c_str(), m_spec)) {
        m_fileformat = in->format_name ();
        ImageSpec tempspec;
        m_nsubimages = 1;
        while (in->seek_subimage (m_nsubimages, tempspec))
            ++m_nsubimages;
        m_current_subimage = 0;
        in->seek_subimage (0, m_spec);
        m_badfile = false;
        m_spec_valid = true;
    } else {
        m_badfile = true;
        m_spec_valid = false;
        m_err = in->error_message();
        return false;
    }

    if (subimage > 0 &&  in->seek_subimage (subimage, m_spec))
        m_current_subimage = subimage;
    else
        m_current_subimage = 0;

    if (convert != TypeDesc::UNKNOWN)
        m_spec.format = convert;

    ImageIOParameter *orient = m_spec.find_attribute ("orientation", TypeDesc::UINT);
    m_orientation = orient ? *(unsigned int *)orient->data() : 1;

    ImageIOParameter *aspect = m_spec.find_attribute ("pixelaspectratio", TypeDesc::FLOAT);
    m_pixelaspect = aspect ? *(float *)aspect->data() : 1.0f;

    realloc ();
    const OpenImageIO::stride_t as = OpenImageIO::AutoStride;
    m_pixels_valid = in->read_image (m_spec.format, &m_pixels[0], as, as, as,
                                     progress_callback, progress_callback_data);
    if (! m_pixels_valid)
        m_err = in->error_message();
    in->close ();
    if (progress_callback)
        progress_callback (progress_callback_data, 0);
    return m_pixels_valid;
}



bool
ImageBuf::save (const std::string &_filename, const std::string &_fileformat,
                OpenImageIO::ProgressCallback progress_callback,
                void *progress_callback_data)
{
    std::string filename = _filename.size() ? _filename : name();
    std::string fileformat = _fileformat.size() ? _fileformat : filename;
    boost::scoped_ptr<ImageOutput> out (ImageOutput::create (fileformat.c_str(), "" /* searchpath */));
    if (! out) {
        m_err = OpenImageIO::error_message();
        return false;
    }
    if (! out->open (filename.c_str(), m_spec)) {
        m_err = out->error_message();
        return false;
    }
    OpenImageIO::stride_t as = OpenImageIO::AutoStride;
    if (! out->write_image (m_spec.format, &m_pixels[0], as, as, as,
                            progress_callback, progress_callback_data)) {
        m_err = out->error_message();
        return false;
    }
    out->close ();
    if (progress_callback)
        progress_callback (progress_callback_data, 0);
    return true;
}



float
ImageBuf::getchannel (int x, int y, int c) const
{
    if (c < 0 || c >= spec().nchannels)
        return 0.0f;
    const void *pixel = pixeladdr(x,y);
    switch (spec().format.basetype) {
    case TypeDesc::FLOAT:
        return ((float *)pixel)[c];
    case TypeDesc::HALF:
        return ((half *)pixel)[c];
    case TypeDesc::DOUBLE:
        return ((double *)pixel)[c];
    case TypeDesc::INT8:
        return ((char *)pixel)[c] / (float)std::numeric_limits<char>::max();
    case TypeDesc::UINT8:
        return ((unsigned char *)pixel)[c] / (float)std::numeric_limits<unsigned char>::max();
    case TypeDesc::INT16:
        return ((short *)pixel)[c] / (float)std::numeric_limits<short>::max();
    case TypeDesc::UINT16:
        return ((unsigned short *)pixel)[c] / 
                (float)std::numeric_limits<unsigned short>::max();
    default:
        ASSERT (0);
        return 0.0f;
    }
}



void
ImageBuf::getpixel (int x, int y, float *pixel, int maxchannels) const
{
    int n = std::min (spec().nchannels, maxchannels);
    OpenImageIO::convert_types (spec().format, pixeladdr(x,y),
                                TypeDesc::FLOAT, pixel, n);
}



void
ImageBuf::getpixel (int i, float *pixel, int maxchannels) const
{
    int n = std::min (spec().nchannels, maxchannels);
    OpenImageIO::convert_types (spec().format, &m_pixels[i*m_spec.pixel_bytes()],
                                TypeDesc::FLOAT, pixel, n);
}



void
ImageBuf::interppixel (float x, float y, float *pixel) const
{
    const int maxchannels = 64;  // Reasonable guess
    float p[4][maxchannels];
    int n = std::min (spec().nchannels, maxchannels);
    x -= 0.5f;
    y -= 0.5f;
    int xtexel, ytexel;
    float xfrac, yfrac;
    xfrac = floorfrac (x, &xtexel);
    yfrac = floorfrac (y, &ytexel);
    int xtexel0 = Imath::clamp (xtexel, xmin(), xmax());
    int ytexel0 = Imath::clamp (ytexel, ymin(), ymax());
    int xtexel1 = Imath::clamp (xtexel+1, xmin(), xmax());
    int ytexel1 = Imath::clamp (ytexel+1, ymin(), ymax());
    getpixel (xtexel0, ytexel0, p[0], n);
    getpixel (xtexel1, ytexel0, p[1], n);
    getpixel (xtexel0, ytexel1, p[2], n);
    getpixel (xtexel1, ytexel1, p[3], n);
    bilerp (p[0], p[1], p[2], p[3], xfrac, yfrac, n, pixel);
}



void
ImageBuf::setpixel (int x, int y, const float *pixel, int maxchannels)
{
    int n = std::min (spec().nchannels, maxchannels);
    OpenImageIO::convert_types (TypeDesc::FLOAT, pixel, 
                                spec().format, pixeladdr(x,y), n);
}



void
ImageBuf::setpixel (int i, const float *pixel, int maxchannels)
{
    int n = std::min (spec().nchannels, maxchannels);
    OpenImageIO::convert_types (TypeDesc::FLOAT, pixel, spec().format,
                                &m_pixels[i*m_spec.pixel_bytes()], n);
}



int
ImageBuf::oriented_width () const
{
    return m_orientation <= 4 ? m_spec.width : m_spec.height;
}



int
ImageBuf::oriented_height () const
{
    return m_orientation <= 4 ? m_spec.height : m_spec.width;
}



int
ImageBuf::oriented_x () const
{
    return m_orientation <= 4 ? m_spec.x : m_spec.y;
}



int
ImageBuf::oriented_y () const
{
    return m_orientation <= 4 ? m_spec.y : m_spec.x;
}



int
ImageBuf::oriented_full_width () const
{
    return m_orientation <= 4 ? m_spec.full_width : m_spec.full_height;
}



int
ImageBuf::oriented_full_height () const
{
    return m_orientation <= 4 ? m_spec.full_height : m_spec.full_width;
}



int
ImageBuf::oriented_full_x () const
{
    return m_orientation <= 4 ? m_spec.full_x : m_spec.full_y;
}



int
ImageBuf::oriented_full_y () const
{
    return m_orientation <= 4 ? m_spec.full_y : m_spec.full_x;
}





void
ImageBuf::zero ()
{
    memset (&m_pixels[0], 0, m_pixels.size());
}


};  // end namespace OpenImageIO
