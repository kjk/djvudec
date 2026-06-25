/* iw44ref.cpp -- decode a FORM:PM44 file with DjVuLibre's IW44Image directly
 * (no ddjvu gamma/color-correction pipeline) and write a raw PPM.
 * Usage: iw44ref in_pm44.djvu out.ppm */
#include "libdjvu/IW44Image.h"
#include "libdjvu/IFFByteStream.h"
#include "libdjvu/ByteStream.h"
#include "libdjvu/GPixmap.h"
#include "libdjvu/GString.h"
#include "libdjvu/GURL.h"
#include <stdio.h>

using namespace DJVU;

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: iw44ref in.djvu out.ppm\n"); return 2; }
    GP<ByteStream> bs = ByteStream::create(GURL::Filename::UTF8(argv[1]), "rb");
    GP<IFFByteStream> iff = IFFByteStream::create(bs);
    GUTF8String chkid;
    GP<IW44Image> iw = IW44Image::create_decode(IW44Image::COLOR);
    iff->get_chunk(chkid);   /* FORM:PM44 */
    while (iff->get_chunk(chkid)) {
        if (chkid == "PM44" || chkid == "BM44")
            iw->decode_chunk(iff->get_bytestream());
        iff->close_chunk();
    }
    GP<GPixmap> pm = iw->get_pixmap();
    if (!pm) { fprintf(stderr, "no pixmap\n"); return 1; }
    GP<ByteStream> out = ByteStream::create(GURL::Filename::UTF8(argv[2]), "wb");
    pm->save_ppm(*out);
    return 0;
}
