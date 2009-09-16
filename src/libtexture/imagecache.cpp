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


#include <string>
#include <sstream>
#include <vector>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/tr1/memory.hpp>
using namespace std::tr1;

#include <ImathMatrix.h>

#include "dassert.h"
#include "typedesc.h"
#include "varyingref.h"
#include "ustring.h"
#include "filesystem.h"
#include "hash.h"
#include "thread.h"
#include "fmath.h"
#include "strutil.h"
#include "sysutil.h"
#include "timer.h"
#include "imageio.h"
#include "imagebuf.h"
#include "imagecache.h"
#include "texture.h"
#include "imagecache_pvt.h"
using namespace OpenImageIO;
using namespace OpenImageIO::pvt;


namespace {  // anonymous

static shared_ptr<ImageCacheImpl> shared_image_cache;
static mutex shared_image_cache_mutex;


// Functor to compare filenames
static bool
filename_compare (const ImageCacheFileRef &a, const ImageCacheFileRef &b)
{
    return a->filename() < b->filename();
}


// Functor to compare read bytes, sort in descending order
static bool
bytesread_compare (const ImageCacheFileRef &a, const ImageCacheFileRef &b)
{
    return a->bytesread() > b->bytesread();
}


// Functor to compare read times, sort in descending order
static bool
iotime_compare (const ImageCacheFileRef &a, const ImageCacheFileRef &b)
{
    return a->iotime() > b->iotime();
}


// Functor to compare read rate (MB/s), sort in ascending order
static bool
iorate_compare (const ImageCacheFileRef &a, const ImageCacheFileRef &b)
{
    double arate = a->bytesread()/(1024.0*1024.0) / a->iotime();
    double brate = b->bytesread()/(1024.0*1024.0) / b->iotime();
    return arate < brate;
}


};  // end anonymous namespace



namespace OpenImageIO {

namespace pvt {   // namespace OpenImageIO::pvt



void
ImageCacheStatistics::init ()
{
    // ImageCache stats:
    find_tile_calls = 0;
    find_tile_microcache_misses = 0;
    find_tile_cache_misses = 0;
//    tiles_created = 0;
//    tiles_current = 0;
//    tiles_peak = 0;
    files_totalsize = 0;
    bytes_read = 0;
//    open_files_created = 0;
//    open_files_current = 0;
//    open_files_peak = 0;
    unique_files = 0;
    fileio_time = 0;
    fileopen_time = 0;
    file_locking_time = 0;
    tile_locking_time = 0;
    find_file_time = 0;
    find_tile_time = 0;

    // TextureSystem stats:
    texture_queries = 0;
    texture_batches = 0;
    texture3d_queries = 0;
    texture3d_batches = 0;
    shadow_queries = 0;
    shadow_batches = 0;
    environment_queries = 0;
    environment_batches = 0;
    aniso_queries = 0;
    aniso_probes = 0;
    max_aniso = 1;
    closest_interps = 0;
    bilinear_interps = 0;
    cubic_interps = 0;
}



void
ImageCacheStatistics::merge (const ImageCacheStatistics &s)
{
    // ImageCache stats:
    find_tile_calls += s.find_tile_calls;
    find_tile_microcache_misses += s.find_tile_microcache_misses;
    find_tile_cache_misses += s.find_tile_cache_misses;
//    tiles_created += s.tiles_created;
//    tiles_current += s.tiles_current;
//    tiles_peak += s.tiles_peak;
    files_totalsize += s.files_totalsize;
    bytes_read += s.bytes_read;
//    open_files_created += s.open_files_created;
//    open_files_current += s.open_files_current;
//    open_files_peak += s.open_files_peak;
    unique_files += s.unique_files;
    fileio_time += s.fileio_time;
    fileopen_time += s.fileopen_time;
    file_locking_time += s.file_locking_time;
    tile_locking_time += s.tile_locking_time;
    find_file_time += s.find_file_time;
    find_tile_time += s.find_tile_time;

    // TextureSystem stats:
    texture_queries += s.texture_queries;
    texture_batches += s.texture_batches;
    texture3d_queries += s.texture3d_queries;
    texture3d_batches += s.texture3d_batches;
    shadow_queries += s.shadow_queries;
    shadow_batches += s.shadow_batches;
    environment_queries += s.environment_queries;
    environment_batches += s.environment_batches;
    aniso_queries += s.aniso_queries;
    aniso_probes += s.aniso_probes;
    max_aniso = std::max (max_aniso, s.max_aniso);
    closest_interps += s.closest_interps;
    bilinear_interps += s.bilinear_interps;
    cubic_interps += s.cubic_interps;
}



ImageCacheFile::ImageCacheFile (ImageCacheImpl &imagecache,
                                ImageCachePerThreadInfo *thread_info,
                                ustring filename)
    : m_filename(filename), m_used(true), m_broken(false),
      m_untiled(false), m_unmipped(false),
      m_texformat(TexFormatTexture),
      m_swrap(TextureOptions::WrapBlack), m_twrap(TextureOptions::WrapBlack),
      m_cubelayout(CubeUnknown), m_y_up(false),
      m_tilesread(0), m_bytesread(0), m_timesopened(0), m_iotime(0),
      m_mipused(false), m_imagecache(imagecache), m_duplicate(NULL)
{
    m_spec.clear ();
    m_filename = imagecache.resolve_filename (m_filename.string());
    recursive_lock_guard guard (m_input_mutex);
    open (thread_info);
    if (! broken())
        m_mod_time = boost::filesystem::last_write_time (m_filename.string());
#if 0
    static int x=0;
    if ((++x % 16) == 0) {
    std::cerr << "Opened " << filename ;
    std::cerr << ", now mem is "
              << Strutil::memformat (Sysutil::memory_used())
              << " virtual, resident = "
              << Strutil::memformat (Sysutil::memory_used(true))
              << "\n";
    }
#endif
}



ImageCacheFile::~ImageCacheFile ()
{
    close ();
}



bool
ImageCacheFile::open (ImageCachePerThreadInfo *thread_info)
{
    // N.B. open() does not need to lock the m_input_mutex, because open()
    // itself is only called by routines that hold the lock.
    // recursive_lock_guard_t guard (m_input_mutex);

    if (m_input)         // Already opened
        return !m_broken;
    if (m_broken)        // Already failed an open -- it's broken
        return false;

    m_input.reset (ImageInput::create (m_filename.c_str(),
                                       m_imagecache.searchpath().c_str()));
    if (! m_input) {
        imagecache().error ("%s", OpenImageIO::geterror().c_str());
        m_broken = true;
        return false;
    }

    ImageSpec tempspec;
    if (! m_input->open (m_filename.c_str(), tempspec)) {
        imagecache().error ("%s", m_input->geterror().c_str());
        m_broken = true;
        m_input.reset ();
        return false;
    }
    m_fileformat = ustring (m_input->format_name());
    ++m_timesopened;
    m_imagecache.incr_open_files ();
    use ();

    // If m_spec has already been filled out, we've opened this file
    // before, read the spec, and filled in all the fields.  So now that
    // we've re-opened it, we're done.
    if (m_spec.size())
        return true;

    // From here on, we know that we've opened this file for the very
    // first time.  So read all the subimages, fill out all the fields
    // of the ImageCacheFile.
    m_spec.reserve (16);
    int nsubimages = 0;
    do {
        if (nsubimages > 1 && tempspec.nchannels != m_spec[0].nchannels) {
            // No idea what to do with a subimage that doesn't have the
            // same number of channels as the others, so just skip it.
            close ();
            m_broken = true;
            return false;
        }
        if (tempspec.tile_width == 0 || tempspec.tile_height == 0) {
            m_untiled = true;
            if (imagecache().autotile()) {
                // Automatically make it appear as if it's tiled
                tempspec.tile_width = imagecache().autotile();
                tempspec.tile_height = imagecache().autotile();
                tempspec.tile_depth = 1;
            } else {
                // Don't auto-tile -- which really means, make it look like
                // a single tile that's as big as the whole image
                tempspec.tile_width = pow2roundup (tempspec.width);
                tempspec.tile_height = pow2roundup (tempspec.height);
                tempspec.tile_depth = 1;
            }
        }
        ++nsubimages;
        m_spec.push_back (tempspec);
        thread_info->m_stats.files_totalsize += tempspec.image_bytes();
    } while (m_input->seek_subimage (nsubimages, tempspec));
    ASSERT ((size_t)nsubimages == m_spec.size());

    // Special work for non-MIPmapped images -- but only if "automip" is
    // on, it's a non-mipmapped image, and it doesn't have a "textureformat"
    // attribute (because that would indicate somebody constructed it as
    // texture and specifically wants it un-mipmapped).
    if (nsubimages == 1)
        m_unmipped = true;
    if (m_untiled && m_unmipped && imagecache().automip() &&
            ! spec().find_attribute ("textureformat", TypeDesc::TypeString)) {
        int w = spec().full_width;
        int h = spec().full_height;
        while (w > 1 || h > 1) {
            w = std::max (1, w/2);
            h = std::max (1, h/2);
            ImageSpec s = spec();
            s.width = w;
            s.height = h;
            s.full_width = w;
            s.full_height = h;
            if (imagecache().autotile()) {
                s.tile_width = std::min (imagecache().autotile(), w);
                s.tile_height = std::min (imagecache().autotile(), h);
            } else {
                s.tile_width = w;
                s.tile_height = h;
            }
            // Texture system requires pow2 tile sizes
            s.tile_width = pow2roundup (s.tile_width);
            s.tile_height = pow2roundup (s.tile_height);
            ++nsubimages;
            m_spec.push_back (s);
        }
    }

    if (m_untiled && ! imagecache().accept_untiled()) {
        imagecache().error ("%s was untiled, rejecting", m_filename.c_str());
        m_broken = true;
        m_input.reset ();
        return false;
    }

    const ImageSpec &spec (m_spec[0]);
    const ImageIOParameter *p;

    m_texformat = TexFormatTexture;
    if ((p = spec.find_attribute ("textureformat", TypeDesc::STRING))) {
        const char *textureformat = *(const char **)p->data();
        for (int i = 0;  i < TexFormatLast;  ++i)
            if (! strcmp (textureformat, texture_format_name((TexFormat)i))) {
                m_texformat = (TexFormat) i;
                break;
            }
        // For textures marked as such, doctor the full_width/full_height to
        // not be non-sensical.
        if (m_texformat == TexFormatTexture) {
            for (size_t i = 0;  i < m_spec.size();  ++i) {
                ImageSpec &spec (m_spec[i]);
                if (spec.full_width > spec.width)
                    spec.full_width = spec.width;
                if (spec.full_height > spec.height)
                    spec.full_height = spec.height;
            }
        }
    }

    if ((p = spec.find_attribute ("wrapmodes", TypeDesc::STRING))) {
        const char *wrapmodes = (const char *)p->data();
        TextureOptions::parse_wrapmodes (wrapmodes, m_swrap, m_twrap);
    }

    m_y_up = false;
    if (m_texformat == TexFormatCubeFaceEnv) {
        if (! strcmp (m_input->format_name(), "openexr"))
            m_y_up = true;
        int w = std::max (spec.full_width, spec.tile_width);
        int h = std::max (spec.full_height, spec.tile_height);
        if (spec.width == 3*w && spec.height == 2*h)
            m_cubelayout = CubeThreeByTwo;
        else if (spec.width == w && spec.height == 6*h)
            m_cubelayout = CubeOneBySix;
        else
            m_cubelayout = CubeLast;
    }

    Imath::M44f c2w;
    m_imagecache.get_commontoworld (c2w);
    if ((p = spec.find_attribute ("worldtocamera", PT_MATRIX))) {
        const Imath::M44f *m = (const Imath::M44f *)p->data();
        m_Mlocal = c2w * (*m);
    }
    if ((p = spec.find_attribute ("worldtoscreen", PT_MATRIX))) {
        const Imath::M44f *m = (const Imath::M44f *)p->data();
        m_Mproj = c2w * (*m);
    }
    // FIXME -- compute Mtex, Mras

    // See if there's a SHA-1 hash in the image description
    std::string desc = spec.get_string_attribute ("ImageDescription");
    const char *prefix = "SHA-1=";
    size_t found = desc.rfind (prefix);
    if (found != std::string::npos)
        m_fingerprint = ustring (desc, found+strlen(prefix), 40);

    m_datatype = TypeDesc::FLOAT;
    if (! m_imagecache.forcefloat()) {
        // If we aren't forcing everything to be float internally, then 
        // there are a few other types we allow.
        if (spec.format == TypeDesc::UINT8)
            m_datatype = spec.format;
    }

    m_channelsize = m_datatype.size();
    m_pixelsize = m_channelsize * spec.nchannels;
    m_eightbit = (m_datatype == TypeDesc::UINT8);

    return !m_broken;
}



bool
ImageCacheFile::read_tile (ImageCachePerThreadInfo *thread_info,
                           int subimage, int x, int y, int z,
                           TypeDesc format, void *data)
{
    recursive_lock_guard guard (m_input_mutex);

    bool ok = open (thread_info);
    if (! ok)
        return false;

    // Mark if we ever use a subimage that's not the first
    if (subimage > 0)
        m_mipused = true;

    // Special case for un-MIP-mapped
    if (m_unmipped && subimage != 0)
        return read_unmipped (thread_info, subimage, x, y, z, format, data);

    // Special case for untiled
    if (m_untiled)
        return read_untiled (thread_info, subimage, x, y, z, format, data);

    // Ordinary tiled
    ImageSpec tmp;
    if (m_input->current_subimage() != subimage)
        ok = m_input->seek_subimage (subimage, tmp);
    if (ok) {
        ok = m_input->read_tile (x, y, z, format, data);
        if (! ok)
            imagecache().error ("%s", m_input->error_message().c_str());
    }
    if (ok) {
        size_t b = spec(subimage).tile_bytes();
        thread_info->m_stats.bytes_read += b;
        m_bytesread += b;
        ++m_tilesread;
    }
    return ok;
}



bool
ImageCacheFile::read_unmipped (ImageCachePerThreadInfo *thread_info,
                               int subimage, int x, int y, int z,
                               TypeDesc format, void *data)
{
    // We need a tile from an unmipmapped file, and it doesn't really
    // exist.  So generate it out of thin air by interpolating pixels
    // from the next higher-res subimage.  Of course, that may also not
    // exist, but it will be generated recursively, since we call
    // imagecache->get_pixels(), and it will ask for other tiles, which
    // will again call read_unmipped... eventually it will hit a subimage 0
    // tile that actually exists.

    // N.B. No need to lock the mutex, since this is only called
    // from read_tile, which already holds the lock.

    // Figure out the size and strides for a single tile, make an ImageBuf
    // to hold it temporarily.
    const ImageSpec &spec (this->spec(subimage));
    int tw = spec.tile_width;
    int th = spec.tile_height;
    stride_t xstride=AutoStride, ystride=AutoStride, zstride=AutoStride;
    spec.auto_stride(xstride, ystride, zstride, format, spec.nchannels, tw, th);
    ImageSpec lospec (tw, th, spec.nchannels, TypeDesc::FLOAT);
    ImageBuf lores ("tmp", lospec);

    // Figure out the range of texels we need for this tile
    x -= spec.x;
    y -= spec.y;
    z -= spec.z;
    int x0 = x - (x % spec.tile_width);
    int x1 = std::min (x0+spec.tile_width-1, spec.full_width-1);
    int y0 = y - (y % spec.tile_height);
    int y1 = std::min (y0+spec.tile_height-1, spec.full_height-1);
//    int z0 = z - (z % spec.tile_depth);
//    int z1 = std::min (z0+spec.tile_depth, spec.full_depth-1);

    // Texel by texel, generate the values by interpolating filtered
    // lookups form the next finer subimage.
    const ImageSpec &upspec (this->spec(subimage-1));  // next higher subimage
    float *bilerppels = (float *) alloca (4 * spec.nchannels * sizeof(float));
    float *resultpel = (float *) alloca (spec.nchannels * sizeof(float));
    bool ok = true;
    for (int j = y0;  j <= y1;  ++j) {
        float yf = (j+0.5f) / spec.full_height;
        int ylow;
        float yfrac = floorfrac (yf * upspec.full_height - 0.5, &ylow);
        for (int i = x0;  i <= x1;  ++i) {
            float xf = (i+0.5f) / spec.full_width;
            int xlow;
            float xfrac = floorfrac (xf * upspec.full_width - 0.5, &xlow);
            ok &= imagecache().get_pixels (this, thread_info, subimage-1,
                                           xlow, xlow+2, ylow, ylow+2,
                                           0, 1, TypeDesc::FLOAT, bilerppels);
            bilerp (bilerppels+0, bilerppels+spec.nchannels,
                    bilerppels+2*spec.nchannels, bilerppels+3*spec.nchannels,
                    xfrac, yfrac, spec.nchannels, resultpel);
            lores.setpixel (i-x0, j-y0, resultpel);
        }
    }

    // Now convert and copy those values out to the caller's buffer
    lores.copy_pixels (0, tw, 0, th, format, data);
    return ok;
}



// Helper routine for read_tile that handles the rare (but tricky) case
// of reading a "tile" from a file that's scanline-oriented.
bool
ImageCacheFile::read_untiled (ImageCachePerThreadInfo *thread_info,
                              int subimage, int x, int y, int z,
                              TypeDesc format, void *data)
{
    // N.B. No need to lock the mutex, since this is only called
    // from read_tile, which already holds the lock.

    if (m_input->current_subimage() != subimage) {
        ImageSpec tmp;
        if (! m_input->seek_subimage (subimage, tmp))
            return false;
    }

    // Strides for a single tile
    int tw = spec(subimage).tile_width;
    int th = spec(subimage).tile_height;
    stride_t xstride=AutoStride, ystride=AutoStride, zstride=AutoStride;
    spec(subimage).auto_stride (xstride, ystride, zstride, format,
                                spec(subimage).nchannels, tw, th);

    bool ok = true;
    if (imagecache().autotile()) {
        // Auto-tile is on, with a tile size that isn't the whole image.
        // We're only being asked for one tile, but since it's a
        // scanline image, we are forced to read (at the very least) a
        // whole row of tiles.  So we add all those tiles to the cache,
        // if not already present, on the assumption that it's highly
        // likely that they will also soon be requested.
        // FIXME -- I don't think this works properly for 3D images
        int pixelsize = spec(subimage).nchannels * format.size();
        // Because of the way we copy below, we need to allocate the
        // buffer to be an even multiple of the tile width, so round up.
        stride_t scanlinesize = tw * ((spec(subimage).width+tw-1)/tw);
        scanlinesize *= pixelsize;
        std::vector<char> buf (scanlinesize * th); // a whole tile-row size
        int yy = y - spec(subimage).y;   // counting from top scanline
        // [y0,y1] is the range of scanlines to read for a tile-row
        int y0 = yy - (yy % th);
        int y1 = std::min (y0 + th - 1, spec(subimage).height - 1);
        y0 += spec(subimage).y;
        y1 += spec(subimage).y;
        // Read the whole tile-row worth of scanlines
        for (int scanline = y0, i = 0; scanline <= y1 && ok; ++scanline, ++i) {
            ok = m_input->read_scanline (scanline, z, format, (void *)&buf[scanlinesize*i]);
            if (! ok)
                imagecache().error ("%s", m_input->error_message().c_str());
        }
        size_t b = (y1-y0+1) * spec(subimage).scanline_bytes();
        thread_info->m_stats.bytes_read += b;
        m_bytesread += b;
        ++m_tilesread;

        // For all tiles in the tile-row, enter them into the cache if not
        // already there.  Special case for the tile we're actually being
        // asked for -- save it in 'data' rather than adding a tile.
        int xx = x - spec(subimage).x;   // counting from left row
        int x0 = xx - (xx % tw); // start of the tile we are retrieving
        for (int i = 0;  i < spec(subimage).width;  i += tw) {
            if (i == xx) {
                // This is the tile we've been asked for
                convert_image (spec(subimage).nchannels, tw, th, 1,
                               &buf[x0 * pixelsize], format, pixelsize,
                               scanlinesize, scanlinesize*th, data, format,
                               xstride, ystride, zstride);
            } else {
                // Not the tile we asked for, but it's in the same
                // tile-row, so let's put it in the cache anyway so
                // it'll be there when asked for.
                TileID id (*this, subimage, i+spec(subimage).x, y0, z);
                if (! imagecache().tile_in_cache (id)) {
                    ImageCacheTileRef tile;
                    tile = new ImageCacheTile (id, &buf[i*pixelsize],
                                            format, pixelsize,
                                            scanlinesize, scanlinesize*th);
                    ok &= tile->valid ();
                    imagecache().incr_tiles (tile->memsize());
                    imagecache().add_tile_to_cache (tile, thread_info);
                }
            }
        }
    } else {
        // No auto-tile -- the tile is the whole image
        ok = m_input->read_image (format, data, xstride, ystride, zstride);
        if (! ok)
            imagecache().error ("%s", m_input->error_message().c_str());
        size_t b = spec(subimage).image_bytes();
        thread_info->m_stats.bytes_read += b;
        m_bytesread += b;
        ++m_tilesread;
    }

    return ok;
}



void
ImageCacheFile::close ()
{
    // N.B. close() does not need to lock the m_input_mutex, because close()
    // itself is only called by routines that hold the lock.
    if (opened()) {
        m_input->close ();
        m_input.reset ();
        m_imagecache.decr_open_files ();
    }
}



void
ImageCacheFile::release ()
{
    recursive_lock_guard guard (m_input_mutex);
    if (m_used)
        m_used = false;
    else
        close ();
}



void
ImageCacheFile::invalidate ()
{
    recursive_lock_guard guard (m_input_mutex);
    close ();
    m_spec.clear();
    m_broken = false;
    m_fingerprint.clear ();
    duplicate (NULL);
    open (imagecache().get_perthread_info());  // Force reload of spec
}



ImageCacheFile *
ImageCacheImpl::find_file (ustring filename,
                           ImageCachePerThreadInfo *thread_info)
{
    ImageCacheStatistics &stats (thread_info->m_stats);
    {
#if IMAGECACHE_TIME_STATS
        Timer timer;
#endif
        ic_read_lock readguard (m_filemutex);
#if IMAGECACHE_TIME_STATS
        double donelocking = timer();
        stats.file_locking_time += donelocking;
#endif
        FilenameMap::iterator found = m_files.find (filename);

#if IMAGECACHE_TIME_STATS
        stats.find_file_time += timer() - donelocking;
#endif

        if (found != m_files.end()) {
            ImageCacheFile *tf = found->second.get();
            // if this is a duplicate texture, switch to the canonical copy
            if (tf->duplicate())
                tf = tf->duplicate();
            tf->use ();
            return tf;
        }
    }

    // We don't already have this file in the table.  Try to
    // open it and create a record.

    // Yes, we're creating an ImageCacheFile with no lock -- this is to
    // prevent all the other threads from blocking because of our
    // expensive disk read.  We believe this is safe, since underneath
    // the ImageCacheFile will lock itself for the open and there are
    // no other non-threadsafe side effects.
    Timer timer;
    ImageCacheFile *tf = new ImageCacheFile (*this, thread_info, filename);
    double createtime = timer();
    stats.fileio_time += createtime;
    stats.fileopen_time += createtime;
    tf->iotime() += createtime;

    ic_write_lock writeguard (m_filemutex);
#if IMAGECACHE_TIME_STATS
    double donelocking = timer();
    stats.file_locking_time += donelocking-createtime;
#endif

    // Another thread may have created and added the file earlier while
    // we were unlocked.
    if (m_files.find (filename) != m_files.end()) {
        delete tf;   // Don't need that one after all
        tf = m_files[filename].get();
        // if this is a duplicate texture, switch to the canonical copy
        if (tf->duplicate())
            tf = tf->duplicate();
        tf->use();
        return tf;
    }

    // What if we've opened another file, with a different name, but the
    // SAME pixels?  It can happen!  Bad user, bad!  But let's save them
    // from their own foolishness.
    if (tf->fingerprint ()) {
        // std::cerr << filename << " hash=" << tf->fingerprint() << "\n";
        FilenameMap::iterator fingerfound = m_fingerprints.find (tf->fingerprint());
        if (fingerfound == m_fingerprints.end()) {
            // Not already in the fingerprint list -- add it
            m_fingerprints[tf->fingerprint()] = tf;
        } else {
            // Already in fingerprints -- mark this one as a duplicate, but
            // ONLY if we don't have other reasons not to consider them true
            // duplicates (the fingerprint only considers source image 
            // pixel values).
            // FIXME -- be sure to add extra tests here if more metadata
            // have significance later!
            ImageCacheFile *dup = fingerfound->second.get();
            if (tf->m_swrap == dup->m_swrap && tf->m_twrap == dup->m_twrap &&
                  tf->m_datatype == dup->m_datatype && 
                  tf->m_cubelayout == dup->m_cubelayout &&
                  tf->m_y_up == dup->m_y_up) {
                tf->duplicate (dup);
                tf->close ();
                // std::cerr << "  duplicates " 
                //           << fingerfound->second.get()->filename() << "\n";
            }
        }
    }

    check_max_files ();
    m_files[filename] = tf;
    if (tf->duplicate())
        tf = tf->duplicate();
    else
        ++stats.unique_files;
    tf->use ();

#if IMAGECACHE_TIME_STATS
    stats.find_file_time += timer()-donelocking;
#endif

    return tf;
}



void
ImageCacheImpl::check_max_files ()
{
#ifdef DEBUG
    if (! (m_stat_open_files_created % 16) || m_stat_open_files_current >= m_max_open_files) {
        std::cerr << "open files " << m_stat_open_files_current << ", max = " << m_max_open_files << "\n";
    std::cout << "    ImageInputs : " << m_stat_open_files_created << " created, " << m_stat_open_files_current << " current, " << m_stat_open_files_peak << " peak\n";
    }
#endif
    while (m_stat_open_files_current >= m_max_open_files) {
        if (m_file_sweep == m_files.end())   // If at the end of list,
            m_file_sweep = m_files.begin();  //     loop back to beginning
        if (m_file_sweep == m_files.end())   // If STILL at the end,
            break;                           //     it must be empty, done
        DASSERT (m_file_sweep->second);
        m_file_sweep->second->release ();  // May reduce open files
        ++m_file_sweep;
    }
}



ImageCacheTile::ImageCacheTile (const TileID &id,
                                ImageCachePerThreadInfo *thread_info)
    : m_id (id), m_used(true)
{
    ImageCacheFile &file (m_id.file ());
    size_t size = memsize();
    m_pixels.resize (size);
    m_valid = file.read_tile (thread_info, m_id.subimage(), m_id.x(), m_id.y(), m_id.z(),
                              file.datatype(), &m_pixels[0]);
    if (! m_valid) {
        m_used = false;  // Don't let it hold mem if invalid
#if 0
        std::cerr << "(1) error reading tile " << m_id.x() << ' ' << m_id.y()
                  << " lev=" << m_id.subimage() << " from " << file.filename() << "\n";
#endif
    }
    // FIXME -- for shadow, fill in mindepth, maxdepth
}



ImageCacheTile::ImageCacheTile (const TileID &id, void *pels, TypeDesc format,
                    stride_t xstride, stride_t ystride, stride_t zstride)
    : m_id (id), m_used(true)
{
    ImageCacheFile &file (m_id.file ());
    const ImageSpec &spec (file.spec(id.subimage()));
    size_t size = memsize();
    m_pixels.resize (size);
    size_t dst_pelsize = spec.nchannels * file.datatype().size();
    m_valid = convert_image (spec.nchannels, spec.tile_width, spec.tile_height,
                             spec.tile_depth, pels, format, xstride, ystride,
                             zstride, &m_pixels[0], file.datatype(),
                             dst_pelsize, dst_pelsize * spec.tile_width,
                             dst_pelsize * spec.tile_pixels());
    // FIXME -- for shadow, fill in mindepth, maxdepth
}



ImageCacheTile::~ImageCacheTile ()
{
    DASSERT (memsize() == m_pixels.size());
    m_id.file().imagecache().decr_tiles (memsize ());
}



const void *
ImageCacheTile::data (int x, int y, int z) const
{
    const ImageSpec &spec = m_id.file().spec (m_id.subimage());
    size_t w = spec.tile_width;
    size_t h = spec.tile_height;
    size_t d = std::max (1, spec.tile_depth);
    x -= m_id.x();
    y -= m_id.y();
    z -= m_id.z();
    if (x < 0 || x >= (int)w || y < 0 || y >= (int)h || z < 0 || z >= (int)d)
        return NULL;
    size_t pixelsize = spec.nchannels * m_id.file().datatype().size();
    size_t offset = ((z * h + y) * w + x) * pixelsize;
    return (const void *)&m_pixels[offset];
}



mutex ImageCacheImpl::m_perthread_info_mutex;



ImageCacheImpl::ImageCacheImpl ()
    : m_perthread_info (&cleanup_perthread_info),
      m_file_sweep(m_files.end()),
      m_tile_sweep(m_tilecache.end()), m_mem_used(0)
{
    init ();
}



void
ImageCacheImpl::init ()
{
    m_max_open_files = 100;
    m_max_memory_MB = 50;
    m_max_memory_bytes = (int) (m_max_memory_MB * 1024 * 1024);
    m_autotile = 0;
    m_automip = false;
    m_forcefloat = false;
    m_accept_untiled = true;
    m_Mw2c.makeIdentity();
    m_mem_used = 0;
    m_statslevel = 0;
    m_stat_tiles_created = 0;
    m_stat_tiles_current = 0;
    m_stat_tiles_peak = 0;
    m_stat_open_files_created = 0;
    m_stat_open_files_current = 0;
    m_stat_open_files_peak = 0;
}



ImageCacheImpl::~ImageCacheImpl ()
{
    printstats ();
    erase_perthread_info ();
}



void
ImageCacheImpl::mergestats (ImageCacheStatistics &stats) const
{
    stats.init ();
    lock_guard lock (m_perthread_info_mutex);
    for (size_t i = 0;  i < m_all_perthread_info.size();  ++i)
        stats.merge (m_all_perthread_info[i]->m_stats);
}



std::string
ImageCacheImpl::onefile_stat_line (const ImageCacheFileRef &file,
                                   int i, bool includestats) const
{
    std::ostringstream out;
    const ImageSpec &spec (file->spec());
    const char *formatcode = "u8";
    switch (spec.format.basetype) {
    case TypeDesc::UINT8  : formatcode = "u8";  break;
    case TypeDesc::INT8   : formatcode = "i8";  break;
    case TypeDesc::UINT16 : formatcode = "u16"; break;
    case TypeDesc::INT16  : formatcode = "i16"; break;
    case TypeDesc::UINT   : formatcode = "u32"; break;
    case TypeDesc::INT    : formatcode = "i32"; break;
    case TypeDesc::HALF   : formatcode = "f16"; break;
    case TypeDesc::FLOAT  : formatcode = "f32"; break;
    case TypeDesc::DOUBLE : formatcode = "f64"; break;
    default: break;
    }
    if (i >= 0)
        out << Strutil::format ("%7lu ", i);
    if (includestats)
        out << Strutil::format ("%4lu    %5lu   %6.1f %9s  ",
                                file->timesopened(), file->tilesread(),
                                file->bytesread()/1024.0/1024.0,
                                Strutil::timeintervalformat(file->iotime()).c_str());
    out << Strutil::format ("%4dx%4dx%d.%s", spec.width, spec.height,
                            spec.nchannels, formatcode);
    out << "  " << file->filename();
    if (file->duplicate()) {
        out << " DUPLICATES " << file->duplicate()->filename();
        return out.str();
    }
    if (file->untiled())
        out << " UNTILED";
    if (file->unmipped() && automip())
        out << " UNMIPPED";
    if (! file->unmipped() && ! file->mipused())
        out << " MIP-UNUSED";
    return out.str ();
}



std::string
ImageCacheImpl::getstats (int level) const
{
    // Merge all the threads
    ImageCacheStatistics stats;
    mergestats (stats);

    std::ostringstream out;
    if (level > 0) {
        out << "OpenImageIO ImageCache statistics (" << (void*)this << ")\n";
        if (stats.unique_files) {
            out << "  Images : " << stats.unique_files << " unique\n";
            out << "    ImageInputs : " << m_stat_open_files_created << " created, " << m_stat_open_files_current << " current, " << m_stat_open_files_peak << " peak\n";
            out << "    Total size of all images referenced : " << Strutil::memformat (stats.files_totalsize) << "\n";
            out << "    Read from disk : " << Strutil::memformat (stats.bytes_read) << "\n";
        } else {
            out << "  No images opened\n";
        }
        if (stats.find_file_time > 0.001)
            out << "    Find file time : " << Strutil::timeintervalformat (stats.find_file_time) << "\n";
        if (stats.fileio_time > 0.001) {
            out << "    File I/O time : " 
                << Strutil::timeintervalformat (stats.fileio_time) << "\n";
            out << "    File open time only : " 
                << Strutil::timeintervalformat (stats.fileopen_time) << "\n";
        }
        if (stats.file_locking_time > 0.001)
            out << "    File mutex locking time : " << Strutil::timeintervalformat (stats.file_locking_time) << "\n";
        if (m_stat_tiles_created > 0) {
            out << "  Tiles: " << m_stat_tiles_created << " created, " << m_stat_tiles_current << " current, " << m_stat_tiles_peak << " peak\n";
            out << "    total tile requests : " << stats.find_tile_calls << "\n";
            out << "    micro-cache misses : " << stats.find_tile_microcache_misses << " (" << 100.0*(double)stats.find_tile_microcache_misses/(double)stats.find_tile_calls << "%)\n";
            out << "    main cache misses : " << stats.find_tile_cache_misses << " (" << 100.0*(double)stats.find_tile_cache_misses/(double)stats.find_tile_calls << "%)\n";
        }
        out << "    Peak cache memory : " << Strutil::memformat (m_mem_used) << "\n";
        if (stats.tile_locking_time > 0.001)
            out << "    Tile mutex locking time : " << Strutil::timeintervalformat (stats.tile_locking_time) << "\n";
        if (stats.find_tile_time > 0.001)
            out << "    Find tile time : " << Strutil::timeintervalformat (stats.find_tile_time) << "\n";
    }

    // Gather file list and statistics
    size_t total_opens = 0, total_tiles = 0;
    imagesize_t total_bytes = 0;
    size_t total_untiled = 0, total_unmipped = 0, total_duplicates = 0;
    double total_iotime = 0;
    std::vector<ImageCacheFileRef> files;
    {
        ic_read_lock fileguard (m_filemutex);
        for (FilenameMap::const_iterator f = m_files.begin(); f != m_files.end(); ++f) {
            const ImageCacheFileRef &file (f->second);
            files.push_back (file);
            total_opens += file->timesopened();
            total_tiles += file->tilesread();
            total_bytes += file->bytesread();
            total_iotime += file->iotime();
            if (file->duplicate()) {
                ++total_duplicates;
                continue;
            }
            if (file->untiled())
                ++total_untiled;
            if (file->unmipped() && automip())
                ++total_unmipped;
        }
    }

    if (level >= 2 && files.size()) {
        out << "  Image file statistics:\n";
        out << "        opens   tiles  MB read  I/O time  res             File\n";
        std::sort (files.begin(), files.end(), filename_compare);
        for (size_t i = 0;  i < files.size();  ++i) {
            const ImageCacheFileRef &file (files[i]);
            ASSERT (file);
            if (file->broken()) {
                out << "BROKEN    " << file->filename() << "\n";
                continue;
            }
            out << onefile_stat_line (file, i+1) << "\n";
        }
        out << Strutil::format ("\n  Tot:  %4lu    %5lu   %6.1f %9s\n",
                                total_opens, total_tiles,
                                total_bytes/1024.0/1024.0,
                                Strutil::timeintervalformat(total_iotime).c_str());
    }

    // Try to point out hot spots
    if (level > 0) {
        if (total_duplicates)
            out << "  " << total_duplicates << " were exact duplicates of other images\n";
        if (total_untiled || (total_unmipped && automip())) {
            out << "  " << total_untiled << " not tiled, "
                << total_unmipped << " not MIP-mapped\n";
#if 0
            if (files.size() >= 50) {
                out << "  Untiled/unmipped files were:\n";
                for (size_t i = 0;  i < files.size();  ++i) {
                    const ImageCacheFileRef &file (files[i]);
                    if (file->untiled() || (file->unmipped() && automip()))
                        out << onefile_stat_line (file, -1) << "\n";
                }
            }
#endif
        }
        if (files.size() >= 50) {
            const size_t topN = 3;
            std::sort (files.begin(), files.end(), bytesread_compare);
            out << "  Top files by bytes read:\n";
            for (size_t i = 0;  i < std::min (topN, files.size());  ++i) {
                if (files[i]->broken())
                    continue;
                out << Strutil::format ("    %d   %6.1f MB (%4.1f%%)  ", i+1,
                                        files[i]->bytesread()/1024.0/1024.0,
                                        100.0 * (files[i]->bytesread() / (double)total_bytes));
                out << onefile_stat_line (files[i], -1, false) << "\n";
            }
            std::sort (files.begin(), files.end(), iotime_compare);
            out << "  Top files by I/O time:\n";
            for (size_t i = 0;  i < std::min (topN, files.size());  ++i) {
                if (files[i]->broken())
                    continue;
                out << Strutil::format ("    %d   %9s (%4.1f%%)   ", i+1,
                                        Strutil::timeintervalformat (files[i]->iotime()).c_str(),
                                        100.0 * files[i]->iotime() / total_iotime);
                out << onefile_stat_line (files[i], -1, false) << "\n";
            }
            std::sort (files.begin(), files.end(), iorate_compare);
            out << "  Files with slowest I/O rates:\n";
            size_t n = 0;
            BOOST_FOREACH (const ImageCacheFileRef &file, files) {
                if (file->broken())
                    continue;
                if (file->iotime() < 0.25)
                    continue;
                double mb = file->bytesread()/(1024.0*1024.0);
                double r = mb / file->iotime();
                out << Strutil::format ("    %d   %6.2f MB/s (%.2fMB/%.2fs)   ", n+1, r, mb, file->iotime());
                out << onefile_stat_line (file, -1, false) << "\n";
                if (++n >= topN)
                    break;
            }
            if (n == 0)
                out << "    (nothing took more than 0.25s)\n";
            double fast = files.back()->bytesread()/(1024.0*1024.0) / files.back()->iotime();
            out << Strutil::format ("    (fastest was %.1f MB/s)\n", fast);
        }
    }

    return out.str();
}



void
ImageCacheImpl::printstats () const
{
    if (m_statslevel == 0)
        return;
    std::cout << getstats (m_statslevel) << "\n\n";
}



bool
ImageCacheImpl::attribute (const std::string &name, TypeDesc type,
                           const void *val)
{
    if (name == "max_open_files" && type == TypeDesc::INT) {
        m_max_open_files = *(const int *)val;
        return true;
    }
    if (name == "max_memory_MB" && type == TypeDesc::FLOAT) {
        float size = *(const float *)val;
        m_max_memory_MB = size;
        m_max_memory_bytes = (int)(size * 1024 * 1024);
        return true;
    }
    if (name == "max_memory_MB" && type == TypeDesc::INT) {
        float size = *(const int *)val;
        m_max_memory_MB = size;
        m_max_memory_bytes = (int)(size * 1024 * 1024);
        return true;
    }
    if (name == "searchpath" && type == TypeDesc::STRING) {
        m_searchpath = std::string (*(const char **)val);
        Filesystem::searchpath_split (m_searchpath, m_searchdirs, true);
        return true;
    }
    if (name == "statistics:level" && type == TypeDesc::INT) {
        m_statslevel = *(const int *)val;
        return true;
    }
    if (name == "autotile" && type == TypeDesc::INT) {
        m_autotile = pow2roundup (*(const int *)val);  // guarantee pow2
        // Clamp to minimum 8x8 tiles to protect against stupid user who
        // think this is a boolean rather than the tile size.  Unless
        // we're in DEBUG mode, then allow developers to play with fire.
#ifndef DEBUG
        if (m_autotile > 0 && m_autotile < 8)
            m_autotile = 8;
#endif
        return true;
    }
    if (name == "automip" && type == TypeDesc::INT) {
        m_automip = *(const int *)val;
        return true;
    }
    if (name == "forcefloat" && type == TypeDesc::INT) {
        m_forcefloat = *(const int *)val;
        return true;
    }
    if (name == "accept_untiled" && type == TypeDesc::INT) {
        m_accept_untiled = *(const int *)val;
        return true;
    }
    return false;
}



bool
ImageCacheImpl::getattribute (const std::string &name, TypeDesc type,
                              void *val)
{
    if (name == "max_open_files" && type == TypeDesc::INT) {
        *(int *)val = m_max_open_files;
        return true;
    }
    if (name == "max_memory_MB" && type == TypeDesc::FLOAT) {
        *(float *)val = m_max_memory_MB;
        return true;
    }
    if (name == "searchpath" && type == TypeDesc::STRING) {
        *(ustring *)val = m_searchpath;
        return true;
    }
    if (name == "statistics:level" && type == TypeDesc::INT) {
        *(int *)val = m_statslevel;
        return true;
    }
    if (name == "autotile" && type == TypeDesc::INT) {
        *(int *)val = m_autotile;
        return true;
    }
    if (name == "automip" && type == TypeDesc::INT) {
        *(int *)val = (int)m_automip;
        return true;
    }
    if (name == "forcefloat" && type == TypeDesc::INT) {
        *(int *)val = (int)m_forcefloat;
        return true;
    }
    if (name == "accept_untiled" && type == TypeDesc::INT) {
        *(int *)val = (int)m_accept_untiled;
        return true;
    }
    if (name == "worldtocommon" && (type == TypeDesc::PT_MATRIX ||
                                    type == TypeDesc(TypeDesc::FLOAT,16))) {
        *(Imath::M44f *)val = m_Mw2c;
        return true;
    }
    if (name == "commontoworld" && (type == TypeDesc::PT_MATRIX ||
                                    type == TypeDesc(TypeDesc::FLOAT,16))) {
        *(Imath::M44f *)val = m_Mc2w;
        return true;
    }
    return false;
}



bool
ImageCacheImpl::find_tile_main_cache (const TileID &id, ImageCacheTileRef &tile,
                           ImageCachePerThreadInfo *thread_info)
{
    DASSERT (! id.file().broken());
    ImageCacheStatistics &stats (thread_info->m_stats);

    ++stats.find_tile_microcache_misses;

    {
#if IMAGECACHE_TIME_STATS
        Timer timer;
#endif
        ic_read_lock readguard (m_tilemutex);
#if IMAGECACHE_TIME_STATS
        stats.tile_locking_time += timer();
#endif

        TileCache::iterator found = m_tilecache.find (id);
#if IMAGECACHE_TIME_STATS
        stats.find_tile_time += timer();
#endif
        if (found != m_tilecache.end()) {
            tile = found->second;
            tile->use ();
            DASSERT (id == tile->id() && !memcmp(&id, &tile->id(), sizeof(TileID)));
            DASSERT (tile);
            return true;
        }
    }

    // The tile was not found in cache.

    ++stats.find_tile_cache_misses;

    // Yes, we're creating and reading a tile with no lock -- this is to
    // prevent all the other threads from blocking because of our
    // expensive disk read.  We believe this is safe, since underneath
    // the ImageCacheFile will lock itself for the read_tile and there are
    // no other non-threadsafe side effects.
    Timer timer;
    tile = new ImageCacheTile (id, thread_info);
    DASSERT (tile);
    DASSERT (id == tile->id() && !memcmp(&id, &tile->id(), sizeof(TileID)));
    incr_tiles (tile->memsize());
    double readtime = timer();
    stats.fileio_time += readtime;
    id.file().iotime() += readtime;

    add_tile_to_cache (tile, thread_info);
    return tile->valid();
}



void
ImageCacheImpl::add_tile_to_cache (ImageCacheTileRef &tile,
                                   ImageCachePerThreadInfo *thread_info)
{
#if IMAGECACHE_TIME_STATS
    Timer timer;
#endif
    ic_write_lock writeguard (m_tilemutex);
#if IMAGECACHE_TIME_STATS
    thread_info->m_stats.tile_locking_time += timer();
#endif
    check_max_mem ();
    m_tilecache[tile->id()] = tile;
}



void
ImageCacheImpl::check_max_mem ()
{
#ifdef DEBUG
    static atomic_int n;
    if (! (n++ % 64) || m_mem_used >= m_max_memory_bytes)
        std::cerr << "mem used: " << m_mem_used << ", max = " << m_max_memory_bytes << "\n";
#endif
    if (m_tilecache.empty())
        return;
    while (m_mem_used >= m_max_memory_bytes) {
        if (m_tile_sweep == m_tilecache.end())  // If at the end of list,
            m_tile_sweep = m_tilecache.begin(); //     loop back to beginning
        if (m_tile_sweep == m_tilecache.end())  // If STILL at the end,
            break;                              //      it must be empty, done
        if (! m_tile_sweep->second->release ()) {
            TileCache::iterator todelete = m_tile_sweep;
            ++m_tile_sweep;
            size_t size = todelete->second->memsize();
            ASSERT (m_mem_used >= size);
#ifdef DEBUG
            std::cerr << "  Freeing tile, recovering " << size << "\n";
#endif
            m_tilecache.erase (todelete);
        } else {
            ++m_tile_sweep;
        }
    }
}



std::string
ImageCacheImpl::resolve_filename (const std::string &filename) const
{
    std::string s = Filesystem::searchpath_find (filename, m_searchdirs, true);
    return s.empty() ? filename : s;
}



bool
ImageCacheImpl::get_image_info (ustring filename, ustring dataname,
                                TypeDesc datatype, void *data)
{
    ImageCachePerThreadInfo *thread_info = get_perthread_info ();
    ImageCacheFile *file = find_file (filename, thread_info);
    if (! file) {
        error ("Image file \"%s\" not found", filename.c_str());
        return false;
    }
    if (file->broken()) {
        error ("Invalid image file \"%s\"", filename.c_str());
        return false;
    }
    const ImageSpec &spec (file->spec());
    if (dataname == "resolution" && datatype==TypeDesc(TypeDesc::INT,2)) {
        int *d = (int *)data;
        d[0] = spec.width;
        d[1] = spec.height;
        return true;
    }
    if (dataname == "texturetype" && datatype == TypeDesc::TypeString) {
        ustring s (texture_type_name (file->textureformat()));
        *(const char **)data = s.c_str();
        return true;
    }
    if (dataname == "textureformat" && datatype == TypeDesc::TypeString) {
        ustring s (texture_format_name (file->textureformat()));
        *(const char **)data = s.c_str();
        return true;
    }
    if (dataname == "fileformat" && datatype == TypeDesc::TypeString) {
        *(const char **)data = file->fileformat().c_str();
        return true;
    }
    if (dataname == "channels" && datatype == TypeDesc::TypeInt) {
        *(int *)data = spec.nchannels;
        return true;
    }
    if (dataname == "channels" && datatype == TypeDesc::TypeFloat) {
        *(float *)data = spec.nchannels;
        return true;
    }
    if (dataname == "format" && datatype == TypeDesc::TypeInt) {
        *(int *)data = (int) spec.format.basetype;
        return true;
    }
    if ((dataname == "cachedformat" || dataname == "cachedpixeltype") &&
            datatype == TypeDesc::TypeInt) {
        *(int *)data = (int) file->m_datatype.basetype;
        return true;
    }
    // FIXME - "viewingmatrix"
    // FIXME - "projectionmatrix"

    // general case -- handle anything else that's able to be found by
    // spec.find_attribute().
    const ImageIOParameter *p = spec.find_attribute (dataname.string());
    if (p && p->type().arraylen == datatype.arraylen) {
        // First test for exact type match
        if (p->type() == datatype) {
            memcpy (data, p->data(), datatype.size());
            return true;
        }
        // If the real data is int but user asks for float, translate it
        if (p->type().basetype == TypeDesc::FLOAT &&
                datatype.basetype == TypeDesc::INT) {
            for (int i = 0;  i < p->type().arraylen;  ++i)
                ((float *)data)[i] = ((int *)p->data())[i];
            return true;
        }
    }

    return false;
}



bool
ImageCacheImpl::get_imagespec (ustring filename, ImageSpec &spec, int subimage)
{
    ImageCachePerThreadInfo *thread_info = get_perthread_info ();
    ImageCacheFile *file = find_file (filename, thread_info);
    if (! file) {
        error ("Image file \"%s\" not found", filename.c_str());
        return false;
    }
    if (file->broken()) {
        error ("Invalid image file \"%s\"", filename.c_str());
        return false;
    }
    if (subimage < 0 || subimage >= file->subimages()) {
        error ("Unknown subimage %d (out of %d)", subimage, file->subimages());
        return false;
    }
    spec = file->spec (subimage);
    return true;
}



bool
ImageCacheImpl::get_pixels (ustring filename, int subimage,
                            int xbegin, int xend, int ybegin, int yend,
                            int zbegin, int zend,
                            TypeDesc format, void *result)
{
    ImageCachePerThreadInfo *thread_info = get_perthread_info ();
    ImageCacheFile *file = find_file (filename, thread_info);
    if (! file) {
        error ("Image file \"%s\" not found", filename.c_str());
        return false;
    }
    if (file->broken()) {
        error ("Invalid image file \"%s\"", filename.c_str());
        return false;
    }
    if (subimage < 0 || subimage >= file->subimages()) {
        error ("get_pixels asked for nonexistant subimage %d of \"%s\"",
               subimage, filename.c_str());
        return false;
    }

    return get_pixels (file, thread_info, subimage, xbegin, xend, 
                       ybegin, yend, zbegin, zend, format, result);
}



bool
ImageCacheImpl::get_pixels (ImageCacheFile *file, 
                            ImageCachePerThreadInfo *thread_info,
                            int subimage,
                            int xbegin, int xend, int ybegin, int yend,
                            int zbegin, int zend, 
                            TypeDesc format, void *result)
{
    const ImageSpec &spec (file->spec());
    bool ok = true;

    // FIXME -- this could be WAY more efficient than starting from
    // scratch for each pixel within the rectangle.  Instead, we should
    // grab a whole tile at a time and memcpy it rapidly.  But no point
    // doing anything more complicated (not to mention bug-prone) until
    // somebody reports this routine as being a bottleneck.
    int nc = file->spec().nchannels;
    size_t formatpixelsize = nc * format.size();
    for (int z = zbegin;  z < zend;  ++z) {
        int tz = z - (z % std::max (1, spec.tile_depth));
        for (int y = ybegin;  y < yend;  ++y) {
            int ty = y - (y % spec.tile_height);
            for (int x = xbegin;  x < xend;  ++x) {
                int tx = x - (x % spec.tile_width);
                TileID tileid (*file, subimage, tx, ty, tz);
                ok &= find_tile (tileid, thread_info);
                ImageCacheTileRef &tile (thread_info->tile);
                const char *data;
                if (tile && (data = (const char *)tile->data (x, y, z))) {
                    convert_types (file->datatype(), data, format, result, nc);
                } else {
                    memset (result, 0, formatpixelsize);
                }
                result = (void *) ((char *) result + formatpixelsize);
            }
        }
    }

    return ok;
}



ImageCache::Tile *
ImageCacheImpl::get_tile (ustring filename, int subimage, int x, int y, int z)
{
    ImageCachePerThreadInfo *thread_info = get_perthread_info ();
    ImageCacheFile *file = find_file (filename, thread_info);
    if (! file || file->broken())
        return NULL;
    const ImageSpec &spec (file->spec());
    // Snap x,y,z to the corner of the tile
    int xtile = (x-spec.x) / spec.tile_width;
    int ytile = (y-spec.y) / spec.tile_height;
    int ztile = (z-spec.z) / spec.tile_depth;
    x = spec.x + xtile * spec.tile_width;
    y = spec.y + ytile * spec.tile_height;
    z = spec.z + ztile * spec.tile_depth;
    TileID id (*file, subimage, x, y, z);
    ImageCacheTileRef tile;
    if (find_tile_main_cache (id, tile, thread_info)) {
        tile->_incref();   // Fake an extra reference count
        tile->use ();
        return (ImageCache::Tile *) tile.get();
    } else {
        return NULL;
    }
}



void
ImageCacheImpl::release_tile (ImageCache::Tile *tile) const
{
    if (! tile)
        return;
    ImageCacheTileRef tileref((ImageCacheTile *)tile);
    tileref->use ();
    tileref->_decref();  // Reduce ref count that we bumped in get_tile
    // when we exit scope, tileref will do the final dereference
}



const void *
ImageCacheImpl::tile_pixels (ImageCache::Tile *tile, TypeDesc &format) const
{
    if (! tile)
        return NULL;
    ImageCacheTile * t = (ImageCacheTile *)tile;
    format = t->file().datatype();
    return t->data ();
}



void
ImageCacheImpl::invalidate (ustring filename)
{
    ImageCacheFile *file = NULL;
    {
        ic_read_lock fileguard (m_filemutex);
        FilenameMap::iterator fileit = m_files.find (filename);
        if (fileit != m_files.end())
            file = fileit->second.get();
        else
            return;  // no such file
    }

    {
        ic_write_lock tileguard (m_tilemutex);
        for (TileCache::iterator tci = m_tilecache.begin();  tci != m_tilecache.end();  ) {
            TileCache::iterator todel (tci);
            ++tci;
            if (&todel->second->file() == file)
                m_tilecache.erase (todel);
        }
    }

    {
        ic_write_lock fileguard (m_filemutex);
        file->invalidate ();
    }

    // Mark the per-thread microcaches as invalid
    lock_guard lock (m_perthread_info_mutex);
    for (size_t i = 0;  i < m_all_perthread_info.size();  ++i)
        if (m_all_perthread_info[i])
            m_all_perthread_info[i]->purge = 1;
}



void
ImageCacheImpl::invalidate_all (bool force)
{
    // Make a list of all files that need to be invalidated
    std::vector<ustring> all_files;
    {
        ic_read_lock fileguard (m_filemutex);
        for (FilenameMap::iterator fileit = m_files.begin();
                 fileit != m_files.end();  ++fileit) {
            ustring name = fileit->second->filename();
            if (fileit->second->broken()) {
                all_files.push_back (name);
                continue;
            }
            std::time_t t = boost::filesystem::last_write_time (name.string());
            if (force || (t != fileit->second->mod_time()))
                all_files.push_back (name);
        }
    }

    BOOST_FOREACH (ustring f, all_files) {
        // fprintf (stderr, "Invalidating %s\n", f.c_str());
        invalidate (f);
    }

    {
        ic_write_lock fileguard (m_filemutex);
        m_fingerprints.clear ();  // Clear fingerprint database
    }

    // Mark the per-thread microcaches as invalid
    lock_guard lock (m_perthread_info_mutex);
    for (size_t i = 0;  i < m_all_perthread_info.size();  ++i)
        if (m_all_perthread_info[i])
            m_all_perthread_info[i]->purge = 1;
}



ImageCachePerThreadInfo *
ImageCacheImpl::get_perthread_info ()
{
    ImageCachePerThreadInfo *p = m_perthread_info.get();
    if (! p) {
        p = new ImageCachePerThreadInfo;
        m_perthread_info.reset (p);
        // printf ("New perthread %p\n", (void *)p);
        lock_guard lock (m_perthread_info_mutex);
        m_all_perthread_info.push_back (p);
        p->shared = true;  // both the IC and the thread point to it
    }
    if (p->purge) {  // has somebody requested a tile purge?
        // This is safe, because it's our thread.
        lock_guard lock (m_perthread_info_mutex);
        p->tile = NULL;
        p->lasttile = NULL;
        p->purge = 0;
    }
    return p;
}



void
ImageCacheImpl::erase_perthread_info ()
{
    lock_guard lock (m_perthread_info_mutex);
    for (size_t i = 0;  i < m_all_perthread_info.size();  ++i) {
        ImageCachePerThreadInfo *p = m_all_perthread_info[i];
        if (p) {
            // Clear the microcache.
            p->tile = NULL;
            p->lasttile = NULL;
            if (p->shared) {
                // Pointed to by both thread-specific-ptr and our list.
                // Just remove from out list, then ownership is only
                // by the thread-specific-ptr.
                p->shared = false;
            } else {
                // Only pointed to by us -- delete it!
                delete p;
            }
            m_all_perthread_info[i] = NULL;
        }
    }
}



void
ImageCacheImpl::cleanup_perthread_info (ImageCachePerThreadInfo *p)
{
    lock_guard lock (m_perthread_info_mutex);
    if (p) {
        // Clear the microcache.
        p->tile = NULL;
        p->lasttile = NULL;
        if (! p->shared)  // If we own it, delete it
            delete p;
        else
            p->shared = false;  // thread disappearing, no longer shared
    }
}



std::string
ImageCacheImpl::geterror () const
{
    std::string e;
    std::string *errptr = m_errormessage.get ();
    if (errptr) {
        e = *errptr;
        errptr->clear ();
    }
    return e;
}



void
ImageCacheImpl::error (const char *message, ...)
{
    std::string *errptr = m_errormessage.get ();
    if (! errptr) {
        errptr = new std::string;
        m_errormessage.reset (errptr);
    }
    ASSERT (errptr != NULL);
    if (errptr->size())
        *errptr += '\n';
    va_list ap;
    va_start (ap, message);
    *errptr += Strutil::vformat (message, ap);
    va_end (ap);
}



};  // end namespace OpenImageIO::pvt



ImageCache *
ImageCache::create (bool shared)
{
    if (shared) {
        // They requested a shared cache.  If a shared cache already
        // exists, just return it, otherwise record the new cache.
        lock_guard guard (shared_image_cache_mutex);
        if (! shared_image_cache.get())
            shared_image_cache.reset (new ImageCacheImpl);
#ifdef DEBUG
        std::cerr << " shared ImageCache is "
                  << (void *)shared_image_cache.get() << "\n";
#endif
        return shared_image_cache.get ();
    }

    // Doesn't need a shared cache
    ImageCacheImpl *ic = new ImageCacheImpl;
#ifdef DEBUG
    std::cerr << "creating new ImageCache " << (void *)ic << "\n";
#endif
    return ic;
}



void
ImageCache::destroy (ImageCache *x)
{
    // If this is not a shared cache, delete it for real.  But if it is
    // the same as the shared cache, don't really delete it, since others
    // may be using it now, or may request a shared cache some time in
    // the future.  Don't worry that it will leak; because shared_image_cache
    // is itself a shared_ptr, when the process ends it will properly
    // destroy the shared cache.
    lock_guard guard (shared_image_cache_mutex);
    if (x != shared_image_cache.get())
        delete (ImageCacheImpl *) x;
}


};  // end namespace OpenImageIO
