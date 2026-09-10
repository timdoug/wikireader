#include "html.h"
#include "../zim/zim_html.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static SPARROW w;
    static SPARROW_RESULT r;
    static char html[32768];
    static unsigned char text[32768];
    size_t hn, tn;
    strcpy(w.snapshot, "synthetic-test");
    strcpy(r.answer, "Ulm & <test>");
    r.hop_count = 1;
    r.hops[0].subject.id = 1;
    strcpy(r.hops[0].subject.label, "Albert Einstein");
    r.hops[0].property = 19;
    r.hops[0].count = 1;
    r.hops[0].claims[0].object = 2;
    strcpy(r.hops[0].claims[0].value, "Ulm & <test>");
    strcpy(r.hops[0].claims[0].source, "Q1$synthetic");
    assert(!sparrow_html(&w, &r, "Einstein birthplace", html, sizeof(html), &hn));
    assert(strstr(html, "Ulm &amp; &lt;test&gt;"));
    assert(!zim_html_to_text_images((unsigned char *)html, hn, text, sizeof(text)-1, &tn));
    text[tn] = 0;
    /* Text includes binary link markers; search the complete byte span. */
    {
        size_t i; int found = 0, link = 0;
        for (i = 0; i + 15 < tn; ++i) {
            if (!memcmp(text+i, "Albert Einstein", 15)) found = 1;
            if (!memcmp(text+i, "@sparrow/", 9)) link = 1;
        }
        assert(found && link && tn > 100);
    }
    assert(sparrow_html(&w, &r, "query", html, 8, &hn) == SPARROW_LIMIT);
    puts("PASS: answer HTML survives production conversion, links and escaping");
    return 0;
}
