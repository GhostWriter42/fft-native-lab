#ifndef CARD_H
#define CARD_H
/* A virtual PlayStation memory card for the platform layer: the BIOS memory-card file API the game uses (open / read / write / lseek / close / erase /
 * firstfile / nextfile / format on "bu00:" paths), the low-level card calls (_card_info / _card_load / _card_clear / _card_read / _card_write ...) and the
 * card events (SwCARD / HwCARD) they deliver. The card is a 128 KiB image in the standard raw ".mcr" layout (block 0 = header + directory, blocks 1..15 =
 * 8 KiB of file data each), the format emulators use, so a save file can be moved between this port and an emulator.
 *
 * Every machine of the lockstep owns one (hle_t.card); both start from the same image, so they stay identical. Only slot 0 ("bu00:") holds a card; slot 1
 * reports no card, as before. Freestanding C. */

#define CARD_BYTES (128 * 1024)
#define CARD_FILES 4                                                             /* open descriptors */

typedef struct mcard {
    unsigned char img[CARD_BYTES];
    int dirty;                                                                  /* changed since the driver last saved it to the host */
    struct { int used; unsigned first, pos, size; } fd[CARD_FILES];
    char find_pattern[24];                                                      /* firstfile / nextfile: the pattern and the next directory entry to look at */
    int find_next;
    unsigned writes;                                                            /* statistics */
} mcard_t;

struct hle;
void card_format(mcard_t* c);                                                   /* an empty, formatted card */
int card_valid(const mcard_t* c);                                               /* the image starts with the "MC" header */
/* The card calls: returns 1 when `name` was handled (result in *ret). */
int card_hle_call(struct hle* h, mcard_t* c, const char* name, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned* ret);
#endif
