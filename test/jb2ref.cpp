/* jb2ref.cpp -- decode a raw Sjbz stream with DjVuLibre's JB2Image and either
 * dump its blits or render the mask. Used to verify the JB2 codec in isolation.
 *   jb2ref in.sjbz                 -> dump blits (left bottom shape w h)
 *   jb2ref in.sjbz -mask out.pgm   -> render the mask bitmap as PGM
 *   jb2ref in.sjbz <N>             -> dump shape N as ascii
 */
#include "libdjvu/JB2Image.h"
#include "libdjvu/ByteStream.h"
#include "libdjvu/GBitmap.h"
#include "libdjvu/GURL.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
using namespace DJVU;

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: jb2ref in.sjbz [-mask out.pgm | <shapeN>]\n"); return 2; }
    GP<ByteStream> bs = ByteStream::create(GURL::Filename::UTF8(argv[1]), "rb");
    GP<JB2Image> img = JB2Image::create();
    img->decode(bs);

    if (argc >= 4 && !strcmp(argv[2], "-mask")) {
        GP<GBitmap> bm = img->get_bitmap();
        int w = bm->columns(), h = bm->rows();
        GP<ByteStream> out = ByteStream::create(GURL::Filename::UTF8(argv[3]), "wb");
        char hd[64]; int n = sprintf(hd, "P5\n%d %d\n255\n", w, h);
        out->writall(hd, n);
        for (int y = h - 1; y >= 0; y--) {
            const unsigned char *row = (*bm)[y];
            for (int x = 0; x < w; x++) { unsigned char v = row[x] ? 0 : 255; out->write(&v, 1); }
        }
        return 0;
    }
    if (argc >= 3) {   /* dump shape N */
        int sn = atoi(argv[2]);
        const JB2Shape &s = img->get_shape(sn);
        if (s.bits) {
            int w = s.bits->columns(), h = s.bits->rows();
            printf("SHAPE %d %dx%d:\n", sn, w, h);
            for (int r = h - 1; r >= 0; r--) {
                const unsigned char *row = (*s.bits)[r];
                for (int x = 0; x < w; x++) putchar(row[x] ? '#' : '.');
                putchar('\n');
            }
        }
        return 0;
    }
    for (int i = 0; i < img->get_blit_count(); i++) {
        const JB2Blit *b = img->get_blit(i);
        const JB2Shape &s = img->get_shape(b->shapeno);
        int w = 0, h = 0;
        if (s.bits) { w = s.bits->columns(); h = s.bits->rows(); }
        printf("BLIT %d left=%d bottom=%d shape=%d w=%d h=%d\n",
               i, b->left, b->bottom, (int)b->shapeno, w, h);
    }
    return 0;
}
