/* bench_ddjvu.cpp -- DjVuLibre decode timing, exposed to djvu_test (-bench).
 *
 * Provides a high-resolution clock and a "decode+render one page" routine that
 * uses DjVuLibre's high-level DjVuDocument/DjVuImage path (the same compositing
 * our djvu_page_render performs), so the C harness can time the two side by
 * side. Linked into djvu_test.exe via the cached libdjvu static lib. */
#include "libdjvu/DjVuDocument.h"
#include "libdjvu/DjVuImage.h"
#include "libdjvu/GPixmap.h"
#include "libdjvu/GBitmap.h"
#include "libdjvu/GRect.h"
#include "libdjvu/GURL.h"
#include "libdjvu/GException.h"
#include <chrono>

using namespace DJVU;

extern "C" {

/* Monotonic high-resolution timestamp in milliseconds (same clock both sides). */
double bench_now_ms(void)
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* Decode + render page `page0` (0-based) of `path` with DjVuLibre and return the
 * elapsed milliseconds, or -1 on error. Bilevel pages render via get_bitmap,
 * everything else via get_pixmap -- mirroring djvu_page_render's gray/color
 * output. Document open is excluded from the timing; decode+composite is what we
 * measure. */
double bench_ddjvu_page_ms(const char *path, int page0)
{
    double ms = -1.0;
    G_TRY
    {
        GURL url = GURL::Filename::UTF8(path);
        GP<DjVuDocument> doc = DjVuDocument::create_wait(url);
        if (doc) {
            double t0 = bench_now_ms();
            GP<DjVuImage> dimg = doc->get_page(page0, true);
            if (dimg) {
                GRect rect(0, 0, dimg->get_width(), dimg->get_height());
                if (dimg->is_legal_bilevel())
                    (void)dimg->get_bitmap(rect, 1);
                else
                    (void)dimg->get_pixmap(rect, 1, 0);
                ms = bench_now_ms() - t0;
            }
        }
    }
    G_CATCH(ex)
    {
        (void)ex;
        ms = -1.0;
    }
    G_ENDCATCH;
    return ms;
}

} /* extern "C" */
