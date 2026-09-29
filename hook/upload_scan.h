// Read the renderer's per-frame constant upload instead of hunting for the
// SceneDynamicBuffer in the heap.
//
// WHAT IS MEASURED: the disassembly of endRender shows the game passing
// renderer+0x1130 with length 0x800 to an uploader immediately before Present.
// The offset, the length and the per-frame call are real.
//
// WHAT IS NOT: an earlier revision of this file claimed the 0x800 region holds
// a constant buffer containing a SceneDynamicBuffer whose entries are 0x40-byte
// float4x4 matrices, and used that claim as the justification for scanning it
// every byte offset. A live 2048-byte dump of the region (logs/upload_v29.bin)
// refutes it: 0 of 24 slots pass a matrix test and the values present are small
// integers and heap pointers. There is no matrix data at this offset. See the
// long version of this correction at the TD_OFF_CONST_UPLOAD definition in
// teardown_vr.c, which keeps the reasoning that was wrong.
//
// So this scan is a DIAGNOSTIC, not a working path. It has never reported a
// hit. It is retained because "we looked here and it is empty" is worth having
// written down, and because if the region ever does get filled the dump below
// is how that would be noticed. Nothing in the runtime depends on a result from
// it, and if it is ever retired, nothing else in the hook needs to change.
//
// The original reasoning, kept because the mistake is the lesson:
//
//   "A SceneDynamicBuffer is 0x1F4 = 500 bytes, so it fits inside one of these
//    with room to spare. That makes this buffer the ground truth."
//
// 0x1F4 fitting inside 0x800 is a size coincidence. It says nothing about
// whether the upload contains an SDB, and treating the two as equivalent is
// what produced the false claim.
//
// Scan cost: 2048 offsets times roughly 130 floats for the cheap shape gate,
// about 270k float reads every 5 seconds. That is nothing, and the read is
// done through td_read, so an unreadable buffer is refused rather than faulting.

#include "memscan.h"

// Where the constant upload sits inside the render object.
#ifndef TD_OFF_CONST_UPLOAD
#define TD_OFF_CONST_UPLOAD  0x1130
#endif
#ifndef TD_CONST_UPLOAD_SZ
#define TD_CONST_UPLOAD_SZ  0x800
#endif

#define TD_UPLOAD_SCAN_EVERY  300      // frames; about every 5 seconds
#define TD_UPLOAD_SCAN_AFTER  60

static unsigned long long g_upload_seq = 0;

// A byte offset is a plausible SDB start if an SDB would fit from there to the
// end of the buffer.
static int td_upload_offset_fits(size_t off) {
    return off + 0x1F4 <= TD_CONST_UPLOAD_SZ;
}

static void td_upload_scan(void* self) {
    static unsigned char buf[TD_CONST_UPLOAD_SZ];
    static unsigned char hit[0x1F4];

    uint64_t at = (uint64_t)(uintptr_t)((unsigned char*)self + TD_OFF_CONST_UPLOAD);

    uint32_t got = td_read(at, buf, TD_CONST_UPLOAD_SZ);
    if (got != TD_CONST_UPLOAD_SZ) {
        vr_log("upload: refused %u of %u bytes at %p", got, TD_CONST_UPLOAD_SZ,
               (void*)(uintptr_t)at);
        return;
    }

    int   found   = 0;
    int   shape    = 0;
    size_t first   = 0;

    for (size_t off = 0; td_upload_offset_fits(off); ++off) {
        td_scan_hit h;
        memset(&h, 0, sizeof h);
        h.addr = at + off;

        // The cheap gate first. Almost every offset fails it, and there is no
        // point running the algebraic test on a block that is obviously not a
        // matrix.
        uint32_t f = td_test_candidate(buf + off, &h);
        if (!f) continue;

        ++shape;
        if (shape == 1) first = off;

        if (f & TD_SCAN_F_FORMULA) {
            ++found;
            if (found <= 4) {
                const float* m  = (const float*)(buf + off);
                const float* pd = (const float*)(buf + off + 0x070);
                vr_log("upload SDB at renderer+0x%X (offset %u) "
                       "m0=%.6f m5=%.6f m8=%.6f m9=%.6f | "
                       "pd=(%.5f %.5f %.5f %.5f) aspect=%.4f",
                       TD_OFF_CONST_UPLOAD + (unsigned)off, (unsigned)off,
                       m[0], m[5], m[8], m[9],
                       pd[0], pd[1], pd[2], pd[3], h.aspect);
            }
            // Keep the payload so it can be dumped and compared frame to frame.
            memcpy(hit, buf + off, 0x1F4);
        }
    }

    g_upload_seq++;
    vr_log("upload: seq=%llu read=%u shape=%d formula=%d%s",
           g_upload_seq, got, shape, found,
           (shape > 0 && found == 0) ? "  <- shapes but no SDB" : "");

    if (shape == 0 || found == 0) {
        // The most useful single fact when nothing passes: show what the shapes
        // actually contained, so the next filter can be aimed at real numbers
        // instead of guessed ranges.
        const float* pd = (const float*)(buf + first + 0x070);
        const float* m  = (const float*)(buf + first);
        vr_log("upload: first shape at offset %u  m0=%.6f m5=%.6f "
               "pd=(%.5f %.5f %.5f %.5f)",
               (unsigned)first, m[0], m[5], pd[0], pd[1], pd[2], pd[3]);
    }

    // Write the raw 2 KB out once. Everything above is a summary of this buffer,
    // and summaries are where the wrong turn has to be caught. The file can be
    // mapped offline row by row, which is how a wrong offset gets told apart
    // from a buffer that was simply not filled yet at this point in the frame.
    if (g_upload_seq == 1) {
        // Relative path, same as renderer_dump.h. The game's working directory
        // is where its own files land, which is where the offline tools look.
        FILE* f = NULL;
        if (fopen_s(&f, "upload_v29.bin", "wb") != 0 || !f) {
            vr_log("upload: could not write upload_v29.bin (err %lu)",
                   GetLastError());
        } else {
            DWORD wrote = 0;
            fwrite(buf, 1, got, f);
            fclose(f);
            vr_log("upload: wrote upload_v29.bin (%u bytes)", got);
            (void)wrote;
        }
    }
}
