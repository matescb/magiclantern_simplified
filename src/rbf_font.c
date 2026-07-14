
#include "dryos.h"
#include "bmp.h"
#include "beep.h"
#include "rbf_font.h"

extern uint32_t ml_refresh_display_needed;

/* here some macros to wrap the CHDK functions for ML code */
#define FG_COLOR(f) FONT_FG(f)
#define BG_COLOR(f) FONT_BG(f)
#define MAKE_COLOR(fg,bg) FONT(0,fg,bg)

#define FONT_CONDENSED 0x00100000 /* this bit is not (yet) used, so we use it as internal flag */

#define draw_char(x,y,c,h) do{}while(0)

static int rbf_font_load(char *file, font* f, int maxchar);
static inline int rbf_font_height(font *rbf_font);
static inline int rbf_char_width(font *rbf_font, int ch);

//-------------------------------------------------------------------
static int RBF_HDR_MAGIC1 = 0x0DF00EE0;
static int RBF_HDR_MAGIC2 = 0x00000003;

struct font font_dynamic[MAX_DYN_FONTS+1];
static char *dyn_font_name[MAX_DYN_FONTS+1];
uint32_t dyn_fonts = 0;

#ifdef CONFIG_NO_BFNT
/* kitor: Those replace entries in platform consts.h for DIGIC6+
 *        that does not use BMP fonts anymore.  */
uint8_t *BFNT_CHAR_CODES;
uint8_t *BFNT_BITMAP_OFFSET;
uint8_t *BFNT_BITMAP_DATA;

bfnt_font* BFNT_FONT;
#endif

//-------------------------------------------------------------------

static font *new_font() {
    // allocate font from cached memory
    font *f = malloc(sizeof(font));
    if (f) {
        memset(f,0,sizeof(font));      // wipe memory
        // return address in cached memory
        return f;
    }

    // memory not allocated ! should probably do something else in this case ?
    return 0;
}

uint32_t font_by_name(char *file, uint32_t fg_color, uint32_t bg_color)
{
#if defined(CONFIG_M50mk2)
    /* [M50II 2026-07-11] The bmp_printf(FONT_CANON,...) error paths below draw via Canon's
     * BFNT backend, which calls GraphicsConvertPosType with a pos_type M50 II can't map (0xD)
     * -> the converter returns 0x8000000D and the caller branches to pc=0x2 -> abort. That's
     * the big.5 crash: on HW a font can fail FIO_GetFileSize (card not ready as early as it is
     * instant in QEMU), taking the draw path. Route the diagnostics to UART instead of drawing,
     * and log which font is being loaded so the boot log pins any straggler. */
    extern void m50ii_uart_str(const char *);
    #define FONT_LOAD_ERR(msg, f) do { m50ii_uart_str("[ML] font_by_name ERR "); \
        m50ii_uart_str(msg); m50ii_uart_str(": "); m50ii_uart_str(f); m50ii_uart_str("\n"); } while (0)
#else
    #define FONT_LOAD_ERR(msg, f) bmp_printf(FONT_CANON, 0, 0, "%s: %s", msg, f)
#endif

    /* check if this font was already loaded */
    for(int pos = 0; pos < (int)dyn_fonts; pos++)
    {
        if(!strcmp(file, dyn_font_name[pos]))
        {   
            /* yeah, so return a new specifier for this id */
            return FONT_DYN(pos, fg_color, bg_color);
        }
    }
    
    /* too many fonts loaded, return default font */
    if(dyn_fonts >= MAX_DYN_FONTS)
    {
        beep();
        FONT_LOAD_ERR("too many fonts", file);
        return FONT_CANON;
    }
    
    /* was not loaded, try to load */
    char filename[128];
    snprintf(filename, sizeof(filename), "ML/FONTS/%s.RBF", file);
    
    uint32_t size;
    int fio_ret = FIO_GetFileSize( filename, &size );
    if((fio_ret != 0) || (size == 0))
    {
        /* failed to load, return default font */
        beep();
        FONT_LOAD_ERR("not found", file);
        return FONT_CANON;
    }

    void *font = new_font();

    /* load the font here */
    if(!rbf_font_load(filename, font, 0))
    {
        FONT_LOAD_ERR("not loaded", file);
        free(font);
        return FONT_CANON;
    }
#undef FONT_LOAD_ERR

    /* now updated cached font name (not filename) */
    dyn_font_name[dyn_fonts] = malloc(strlen(filename) + 1);
    strcpy(dyn_font_name[dyn_fonts], file);
    
    /* and measure font sizes */
    font_dynamic[dyn_fonts].bitmap = font;
    font_dynamic[dyn_fonts].height = rbf_font_height((void*)font_dynamic[dyn_fonts].bitmap);
    font_dynamic[dyn_fonts].width = rbf_char_width((void*)font_dynamic[dyn_fonts].bitmap, '0');
    dyn_fonts++;

    return FONT_DYN(dyn_fonts - 1, fg_color, bg_color);
}

static void alloc_cTable(font *f) {

    // Calculate additional values for font
    f->width = 8 * f->hdr.charSize / f->hdr.height;
    f->charCount = f->hdr.charLast - f->hdr.charFirst + 1;

    // set width table to default value
    memset(f->wTable, f->width, 256);
    
    // allocate cTable memory

    // If existing data has been allocated then we are re-using the font data
    // See if it the existing cTable data is large enough to hold the new font data
    // If not free it so new memory will be allocated
    if ((f->cTable != NULL) && (f->cTableSizeMax < (f->charCount*f->hdr.charSize))) {
        free(f->cTable);  // free the memory
        f->cTable = NULL; // clear pointer so new memory is allocated
        f->cTableSizeMax = 0;
    }

    // Allocated memory if needed
    if (f->cTable == NULL) {
        // Allocate memory from cached pool
        int size = f->charCount*f->hdr.charSize;
        f->cTable = malloc(size);

        // save size
        f->cTableSize = f->charCount*f->hdr.charSize;
        if (f->cTableSizeMax == 0) f->cTableSizeMax = f->cTableSize;    // Save actual size allocated
    }
}

//-------------------------------------------------------------------
// Return address of 'character' data for specified font & char
static inline char* FAST rbf_font_char(font* f, int ch)
{
    if (f && (ch >= f->hdr.charFirst) && (ch <= f->hdr.charLast))
    {
        return &f->cTable[(ch-f->hdr.charFirst)*f->hdr.charSize];
    }

    return 0;
}
//-------------------------------------------------------------------
// Read data from SD file using uncached buffer and copy to cached
// font memory
static int font_read(FILE* fd, unsigned char *dest, int len)
{
    // Return actual bytes read
    int bytes_read = 0;

    /* [M50II 2026-07-13] Canon's _FIO_ReadFile DMAs in whole 512-byte sectors and can write
     * PAST the requested length -- HW showed a memcheck "overflow 527/512" here (rbf_font.c:178),
     * i.e. 15 bytes past a 512-byte buffer, into the fio_malloc guard. mem.c already rounds DMA
     * allocations up to a sector, but the overshoot still reached past the rounded end, corrupting
     * the adjacent heap (mem.c notes this same corruption "broke button input" -> the flaky menu
     * nav). Over-allocate the uncached buffer to the next sector boundary PLUS one spare sector so
     * any sector-granular overshoot lands in slack, not the guard. Only `len` bytes are copied out. */
    /* HW: Canon's _FIO_ReadFile DMAs whole SD CLUSTERS, overrunning a small buffer by up to a
     * cluster -- observed 25755 bytes actually written for a 48-byte header read. Sector-margin
     * pads (+0x200/+0x800) were nowhere near enough. Size the scratch buffer to hold a full
     * cluster: at least 64KB, which exceeds any FAT cluster this card uses. Only `len` bytes are
     * copied to dest; the buffer is freed right after, so the transient 64KB is fine. */
    int alloc_len = ((len + 0x1FF) & ~0x1FF) + 0x800;
    if (alloc_len < 0x10000) alloc_len = 0x10000;
    unsigned char *ubuffer = fio_malloc(alloc_len);
    if (ubuffer)
    {
        // Read block and copy to dest (read only the real length; buffer has sector slack)
        bytes_read += FIO_ReadFile(fd, ubuffer, len);
        memcpy(dest, ubuffer, len);
        fio_free(ubuffer);
    }

    return bytes_read;
}
//-------------------------------------------------------------------
// Load from from file. If maxchar != 0 limit charLast (for symbols)
static int rbf_font_load(char *file, font* f, int maxchar)
{
    int i;

    // make sure the font has been allocated
    if (f == 0)
    {
        return 0;
    }

    // open file (can't use fopen here due to potential conflict FsIoNotify crash)
    FILE *fd = FIO_OpenFile(file, O_RDONLY | O_SYNC);
    if (!fd)
    {
        return 0;
    }

    // read header
    i = font_read(fd, (unsigned char*)&(f->hdr), sizeof(font_hdr));

    // check size read is correct and magic numbers are valid
    if ((i != sizeof(font_hdr)) || (f->hdr.magic1 != RBF_HDR_MAGIC1) || (f->hdr.magic2 != RBF_HDR_MAGIC2))
    {
        return 0;
    }
    
    if (maxchar != 0)
    {
        f->hdr.charLast = maxchar;
    }

    alloc_cTable(f);

    // read width table (using uncached buffer)
    memset(&f->wTable[0], 0, sizeof(f->wTable));
    FIO_SeekSkipFile(fd, f->hdr._wmapAddr, SEEK_SET);
    font_read(fd, (unsigned char*)&f->wTable[f->hdr.charFirst], f->charCount);

    // read cTable data (using uncached buffer)
    FIO_SeekSkipFile(fd, f->hdr._cmapAddr, SEEK_SET);
    font_read(fd, (unsigned char*)f->cTable, f->charCount*f->hdr.charSize);

    FIO_CloseFile(fd);

    /* hardcoded tab width: 4 spaces */
    f->wTable['\t'] = f->wTable[' '] * 4;

    return 1;
}

//-------------------------------------------------------------------
static inline int rbf_font_height(font *rbf_font) {
    if (!rbf_font) return 0;   /* [M50II] bring-up: draw before fonts loaded (FIO not ready) */
    return rbf_font->hdr.height;
}
//-------------------------------------------------------------------
static inline int rbf_char_width(font *rbf_font, int ch) {
    if (!rbf_font) return 0;   /* [M50II] guards rbf_str_width/clipped_width/strlen_clipped */
    return rbf_font->wTable[ch];
}

//-------------------------------------------------------------------
int rbf_str_width(font *rbf_font, const char *str) {
    int l=0;
    int maxl = 0;

    // Calculate how long the string is in pixels
    // and return the length of the longest line
    while (*str)
    {
        if (*str == '\n')
        {
            maxl = MAX(l, maxl);
            l = 0;
        }
        else
        {
            l += rbf_char_width(rbf_font, *str);
        }
        str++;
    }
    maxl = MAX(l, maxl);

    return maxl;
}

int rbf_str_clipped_width(font *rbf_font, const char *str, int maxlen) {
    int l = 0;
    // Calculate how long the string is in pixels (possibly clipped to 'maxlen')
    while (*str && l+rbf_char_width(rbf_font, *str)<=maxlen)
        l+=rbf_char_width(rbf_font, *str++);

    return l;
}

int rbf_strlen_clipped(font *rbf_font, const char *str, int maxlen) {
    int l = 0;
    // Calculate how long the string is in characters (possibly clipped to 'maxlen')
    char* str0 = (char*) str;
    while (*str && l+rbf_char_width(rbf_font, *str)<=maxlen)
        l+=rbf_char_width(rbf_font, *str++);

    return str - str0;
}

//-------------------------------------------------------------------
static void FAST font_draw_char(font *rbf_font, int x, int y, char *cdata, int width, int height, int pixel_width, int fontspec) {
    int xx, yy;
    uint8_t * bmp = bmp_vram();
    int fg = FG_COLOR(fontspec);
    int bg = BG_COLOR(fontspec);
    int x0 = fontspec & FONT_CONDENSED ? 1 : 0;
    
    // draw pixels for font character
    if (cdata)
    {
        if (bg != NO_BG_ERASE)
        {
            bmp_fill(bg, x, y, pixel_width, height);
        }

        for (yy=0; yy<height; ++yy)
        {
            if (y+yy <= BMP_H_MINUS || y+yy >= BMP_H_PLUS)
            {
                break;
            }
            for (xx=x0; xx<pixel_width; ++xx)
            {
                if(cdata[yy*width/8+xx/8] & (1<<(xx%8)))
                {
                    bmp_putpixel_fast(bmp, x+xx, y+yy, fg);
                }
            }
        }
    }
}

static void FAST font_draw_char_shadow(font *rbf_font, int x, int y, char *cdata, int width, int height, int pixel_width, int fontspec) {
    int xx, yy;
    uint8_t * bmp = bmp_vram();
    int fg = FG_COLOR(fontspec);
    int bg = BG_COLOR(fontspec);
    
    // draw pixels for font character
    if (cdata)
    {
        for (yy=0; yy<height; ++yy)
        {
            if (y+yy <= BMP_H_MINUS || y+yy >= BMP_H_PLUS)
            {
                break;
            }
            for (xx=0; xx<pixel_width; ++xx)
            {
                int px = (cdata[yy*width/8+xx/8] & (1<<(xx%8)));
                if (px)
                {
                    bmp_putpixel_fast(bmp, x+xx, y+yy, fg);
                    
                    /* shadow: background pixels are only drawn near a foreground pixel */
                    /* heuristic: usually there are much less fg pixels than bg, so it makes sense to look for neighbours here */
                    for (int xxx = MAX(xx-1, 0); xxx <= MIN(xx+1, pixel_width-1); xxx++)
                    {
                        for (int yyy = MAX(yy-1, 0); yyy <= MIN(yy+1, height-1); yyy++)
                        {
                            int pxx = (cdata[yyy*width/8+xxx/8] & (1<<(xxx%8)));
                            if (!pxx)
                            {
                                bmp_putpixel_fast(bmp, x+xxx, y+yyy, bg);
                            }
                        }
                    }
                }
            }
        }
    }
}

//-------------------------------------------------------------------
static int FAST rbf_draw_char(font *rbf_font, int x, int y, int ch, int fontspec) {
    /* [M50II 2026-07-11] Defensive NULL-font guard: during early bring-up a draw can be requested
     * before fonts are loaded (e.g. startup_warning -> _load_fonts fails because FIO/card isn't
     * mounted yet), leaving rbf_font NULL -> deref of rbf_font->cTable (+0x58) hard-aborts. A NULL
     * font is always an error state; skip drawing instead of crashing. */
    if (!rbf_font) return 0;
    // Get char data pointer
    char* cdata = rbf_font_char(rbf_font, ch);
    
    if (!rbf_font->cTable)
        bfnt_draw_char(ch, x, y, FG_COLOR(fontspec), BG_COLOR(fontspec));
    else if (fontspec & SHADOW_MASK)
        font_draw_char_shadow(rbf_font, x, y, cdata, rbf_font->width, rbf_font->hdr.height, rbf_font->wTable[ch], fontspec);
    else
        font_draw_char(rbf_font, x, y, cdata, rbf_font->width, rbf_font->hdr.height, rbf_font->wTable[ch], fontspec);

#if 0   /* fixme: breaks cursor in editor.lua */
    if (ch == '\t')
    {
        int tab_width = rbf_font->wTable[' '] * 4;
        return (x + tab_width) / tab_width * tab_width - x;
    }
#endif
    ml_refresh_display_needed = 1;
    return rbf_font->wTable[ch];
}


//-------------------------------------------------------------------
// Draw a string colored 'c1' with the character at string-position 'c' colored 'c2'.
static int rbf_draw_string_c(font *rbf_font, int x, int y, const char *str, int fontspec1, int c, int fontspec2) {
     if (!rbf_font) return 0;   /* early boot: drawn before fonts loaded (FIO not ready) */
     int l=0, i=0;

     while (*str) {
          if (*str == '\n')
          {
              l = 0;
              y += rbf_font->hdr.height;
              str++;
              i++;
              continue;
          }
          l+=rbf_draw_char(rbf_font, x+l, y, *str++, (i==c)?fontspec1:fontspec2);
          ++i;
     }
     return l;
}

//-------------------------------------------------------------------
static int rbf_draw_string_simple(font *rbf_font, int x, int y, const char *str, int fontspec) {
    return rbf_draw_string_c(rbf_font, x, y, str, fontspec, -1, fontspec);
}

//-------------------------------------------------------------------
static int rbf_draw_clipped_string(font *rbf_font, int x, int y, const char *str, int fontspec, int maxlen)
{
    if (!rbf_font) return 0;   /* early boot: drawn before fonts loaded (FIO not ready) */
    int i = 0;
    int l = 0;
    
    int justified = (fontspec & FONT_ALIGN_MASK) == FONT_ALIGN_JUSTIFIED;
    
    if (justified)
    {
        int should_fill = !(fontspec & SHADOW_MASK);
        int bg = FONT_BG(fontspec);
        int len = maxlen;
        int space = len - rbf_str_width(rbf_font, str);
        int is_mono = rbf_char_width(rbf_font, 'm') == rbf_char_width(rbf_font, 'i');
        
        /* divide the space across the chars: a space character can accept 5 times more stretch space than a regular letter */
        /* first non-letter (indent) can't be stretched (to render bullet points correctly) */
        /* monospaced fonts are condensed uniformly */
        int bins = 0;
        char* c = (char*) str;
        int indent = 1;
        while (*c && *(c+1))    /* note: last char should not be stretched */
        {
            if (*c != ' ' && *c != '*') indent = 0;
            bins += is_mono ? 3 : indent ? 0 : space < 0 ? 1 : *c == ' ' ? 10 : 2;
            c++;
        }
        
        /* use Bresenham line-drawing algorithm to divide space with integer-only math */
        /* http://en.wikipedia.org/wiki/Bresenham%27s_line_algorithm#Algorithm_with_Integer_Arithmetic */
        /* x: from 0 to bins */
        /* y: space accumulated (from 0 to space) */
        int dx = bins;
        int dy = ABS(space);
        
        /* if it requires too much stretching, just stretch as much as we can */
        dy = MIN(dy, dx);
        
        int D = 2*dy - dx;

        // Draw chars from string up to max pixel length
        indent = 1;
        int condensed = 0;
        while (*str && l+rbf_char_width(rbf_font, *str)<=maxlen)
        {
            l += rbf_draw_char(rbf_font, x+l, y, *str, fontspec | (condensed ? FONT_CONDENSED : 0) );
            int l0 = l;

            if (*(str+1))
            {
                /* Bresenham step */
                if (*str != ' ' && *str != '*') indent = 0;
                int repeat = is_mono ? 3 : indent ? 0 : space < 0 ? 1 : *c == ' ' ? 10 : 2;
                condensed = 0;
                for (int i = 0; i < repeat; i++)
                {
                    if (D > 0)
                    {
                        l += SGN(space);
                        D = D + (2*dy - 2*dx);
                        if (space < 0) condensed = 1;
                    }
                    else
                    {
                        D = D + 2*dy;
                    }
                }
            }
            
            if (should_fill && l > l0)
            {
                bmp_fill(bg, x+l0, y, l-l0, rbf_font->hdr.height);
            }
            
            i++; str++;
        }
    }
    else
    {
        // Draw chars from string up to max pixel length
        while (*str && l+rbf_char_width(rbf_font, *str)<=maxlen)
        {
            l+=rbf_draw_char(rbf_font, x+l, y, *str++, fontspec);
            i++;
        }
    }
    return l;
}

//-------------------------------------------------------------------
static int rbf_draw_string_single_line(font *rbf_font, int x, int y, const char *str, int fontspec, int maxlen) {
    if (!rbf_font) return 0;   /* early boot: drawn before fonts loaded (FIO not ready) */

    /* how much space do we have to draw the string? */
    int len = maxlen;
    
    // Calulate amount of padding needed
    int padding = len - rbf_str_clipped_width(rbf_font, str, len);

    /* is that padding on the left? on the right? or both? */
    int padding_left = 0;
    int padding_right = 0;
    
    switch (fontspec & FONT_ALIGN_MASK)
    {
        case FONT_ALIGN_LEFT:
            padding_right = padding;
            break;
        
        case FONT_ALIGN_CENTER:
            x -= len/2;
            padding_left = padding/2;
            padding_right = padding - padding/2;
            break;
        
        case FONT_ALIGN_RIGHT:
            padding_left = padding;
            x -= len;
            break;
        
        case FONT_ALIGN_JUSTIFIED:
            padding_left = 0;
            padding_right = 0;
    }
    
    int bg = FONT_BG(fontspec);
    int should_fill = fontspec & FONT_ALIGN_FILL;

    // Fill left padding with background color
    if (should_fill && padding_left)
        bmp_fill(bg, x, y, padding_left, rbf_font->hdr.height);

    // Draw chars
    rbf_draw_clipped_string(rbf_font, x + padding_left, y, str, fontspec, len - padding_right - padding_left);

    // Fill right padding with background color
    if (should_fill && padding_right)
        bmp_fill(bg, x+len-padding_right, y, padding_right, rbf_font->hdr.height);

    return len;
}

int rbf_draw_string(font *rbf_font, int x, int y, const char *str, int fontspec) {
    if (!rbf_font) return 0;   /* early boot: drawn before fonts loaded (FIO not ready) */

    /* how much space do we have to draw the string? */
    int len = FONT_GET_TEXT_WIDTH(fontspec);

    /* choose some reasonable defaults for text width, if it's not specified */
    if (len == 0)
    {
        if ((fontspec & FONT_ALIGN_MASK) == FONT_ALIGN_LEFT && (fontspec & FONT_ALIGN_FILL) == 0)
        {
            /* no fancy alignment */
            return rbf_draw_string_simple(rbf_font, x, y, str, fontspec);
        }
        else
        {
            /* use natural string length */
            len = rbf_str_width(rbf_font, str);
        }
    }

    char* start = (char*) str;
    char* end = start;

    /* for each line in string */
    while (*start)
    {
        /* where does this line end? */
        while (*end && *end != '\n')
            end++;
        
        /* chop the string here */
        char old = *end;
        *end = 0;
        
        /* draw this line */
        rbf_draw_string_single_line(rbf_font, x, y, start, fontspec, len);
        
        /* finished? */
        if (old == 0)
            break;
        
        /* undo chopping */
        *end = old;
        
        /* okay, let's go to next line */
        y += rbf_font->hdr.height;
        start = end = end+1;
    }
    
    return len;
}


/* for compatibility with existing code */
struct font font_small;
struct font font_med;
struct font font_med_large;
struct font font_large;
struct font font_canon;


#ifdef CONFIG_NO_BFNT
//-------------------------------------------------------------------
// Load bitmap.bfn from card
static int bfnt_load_from_card()
{
    char filename[128];
    //TODO? make it configurable?
    snprintf(filename, sizeof(filename), "ML/FONTS/bitmap.bfn");
    uint32_t size;
    if( FIO_GetFileSize( filename, &size ) != 0 )
        return 1;

    DryosDebugMsg(0, 15, "File '%s' size %d bytes", filename, size);

    BFNT_FONT = (bfnt_font*)read_entire_file(filename, (int*)&size);
    if( BFNT_FONT == NULL ){
        DryosDebugMsg(0, 15, "BFNT read failed.");
        return 2;
    }
    DryosDebugMsg(0, 15, "read ok");

    if( BFNT_FONT->magic != 0x00544e46) { // "FNT\0"
        DryosDebugMsg(0, 15, "Font magic incorrect: 0x%08x", BFNT_FONT->magic);
        free(BFNT_FONT);
        return 3;
    }

    return 0;
}
#endif

/* must be called before menu_init, otherwise it can't measure strings */
void _load_fonts()
{
    /* Tolerate multiple calls, but only latch as done once fonts ACTUALLY load.
     * A premature call (e.g. startup_warning -> _load_fonts before FIO/card is
     * ready on M50 II) used to set fonts_loaded=1 up front, poisoning the real
     * later call at my_big_init_task so it silently no-op'd and ML ran fontless.
     * We now latch at the end, only on success, so a failed early call retries. */
    static int fonts_loaded = 0;
    if (fonts_loaded)
        return;

    /* [M50II 2026-07-11] Direct-UART bisect of the big.5 abort (pos_type=0xD -> converter
     * returns 0x8000000D -> caller branches to pc=0x2). DryosDebugMsg (the [FDG] markers) is
     * async and won't flush before the fault, so use synchronous m50ii_uart_str. The LAST
     * big.5.N printed before [ABORT] pins the crashing segment. Also a hard skip lever:
     * flip M50II_SKIP_LOAD_FONTS to 1 to bypass _load_fonts entirely (ML runs fontless) and
     * confirm whether reaching big.8 is blocked here or by an async CBR elsewhere. */
    extern void m50ii_uart_str(const char *);
    m50ii_uart_str("[ML] big.5.0: _load_fonts ENTER\n");
#ifndef M50II_SKIP_LOAD_FONTS
#define M50II_SKIP_LOAD_FONTS 0
#endif
#if M50II_SKIP_LOAD_FONTS
    m50ii_uart_str("[ML] big.5.SKIP: _load_fonts bypassed (M50II_SKIP_LOAD_FONTS)\n");
    return;
#endif

    int bfnt_status = -1;

    #ifdef CONFIG_NO_BFNT
    //Try to load BFNT from card on newer generations
    bfnt_status = bfnt_load_from_card();
    if ( bfnt_status == 0) {
        DryosDebugMsg(0, 15, "bfnt read OK: %08x %s",  BFNT_FONT, BFNT_FONT->name);
        DryosDebugMsg(0, 15, "hdr %08x char %08x", BFNT_FONT->charmap_offset, BFNT_FONT->charmap_size);

        //setup variables that would be constants from ROM on old generations
        BFNT_CHAR_CODES    = (uint8_t *)BFNT_FONT + BFNT_FONT->charmap_offset;
        BFNT_BITMAP_OFFSET = BFNT_CHAR_CODES + BFNT_FONT->charmap_size;
        BFNT_BITMAP_DATA   = BFNT_BITMAP_OFFSET + BFNT_FONT->charmap_size;

        DryosDebugMsg(0, 15, "%08x %08x %08x", BFNT_CHAR_CODES, BFNT_BITMAP_OFFSET, BFNT_BITMAP_DATA);
    }
    else{
        DryosDebugMsg(0, 15, "bfnt read fail: %d", bfnt_status);
    }
    #else
    //If constants are not zero - assume BFNT is available from ROM
    if(BFNT_CHAR_CODES && BFNT_BITMAP_OFFSET && BFNT_BITMAP_DATA)
        bfnt_status = 0;
    #endif

    m50ii_uart_str("[ML] big.5.1: bfnt block done, building canon fake-font\n");

    /* fake font for Canon BFNT font backend, with the same metrics */
    font *canon_font = new_font();
    canon_font->hdr.height = 40;
    for (int i = 0; i < 256; i++)
        canon_font->wTable[i] = bfnt_char_get_width(i);
    m50ii_uart_str("[ML] big.5.2: canon fake-font wTable built (bfnt_char_get_width x256)\n");

    /* [M50II FDG 2026-07-10] bisect the Graphics::GraphicsConvertPosType.c:9 assert that fires in
     * _load_fonts once the display is live (post-CamIF-fix). Markers B/C/E split the function so the
     * next HW boot shows which segment stalls (or, if all print + big.5 prints, the assert is an
     * async Canon GUI task, not this ML path). Remove once localized. */
    DryosDebugMsg(0, 15, "[FDG] B: canon-bfnt fake-font built (bfnt_status=%d)", bfnt_status);

    /* Try to load some RBF fonts */
    m50ii_uart_str("[ML] big.5.3: loading RBF fonts (font_by_name block)\n");
    font_by_name("term12", COLOR_BLACK, COLOR_WHITE);
    font_by_name("term20", COLOR_BLACK, COLOR_WHITE);
    #ifdef CONFIG_LOW_RESOLUTION_DISPLAY
    font_by_name("arghlf22", COLOR_BLACK, COLOR_WHITE);
    #else
    font_by_name("argnor23", COLOR_BLACK, COLOR_WHITE);
    #endif
    font_by_name("argnor28", COLOR_BLACK, COLOR_WHITE);
    font_by_name("argnor32", COLOR_BLACK, COLOR_WHITE);

    m50ii_uart_str("[ML] big.5.4: RBF font_by_name block done, entering fallback setup\n");
    DryosDebugMsg(0, 15, "[FDG] C: RBF font_by_name block done (dyn_fonts=%d)", dyn_fonts);

    if (bfnt_status == 0) {
        /* use BFNT font as fallback */
        for (int i = dyn_fonts; i <= MAX_DYN_FONTS; i++)
        {
            font_dynamic[i].bitmap = (void*) canon_font;
            font_dynamic[i].height = 40;
            font_dynamic[i].width = rbf_char_width((void*)font_dynamic[i].bitmap, '0');
        }

        font_canon = *fontspec_font(FONT_CANON);
    }
    else {
        /* use last loaded RBF font as a fallback */
        font_by_name("argnor32", COLOR_BLACK, COLOR_WHITE);
        if(dyn_fonts)
        {
            for (int i = dyn_fonts; i <= MAX_DYN_FONTS; i++)
                font_dynamic[i] = font_dynamic[dyn_fonts - 1];
        }

        /* Replace missing FONT_CANON with FONT_LARGE */
        font_canon = *fontspec_font(FONT_LARGE);
    }

    font_small = *fontspec_font(FONT_SMALL);
    font_med = *fontspec_font(FONT_MED);
    font_med_large = *fontspec_font(FONT_MED_LARGE);
    font_large = *fontspec_font(FONT_LARGE);

    DryosDebugMsg(0, 15, "[FDG] E: fontspec block done, _load_fonts about to return");

    /* Only latch as loaded if at least one usable font really came up (a BFNT
     * backend or any dynamic RBF font). If FIO/card wasn't ready yet, leave
     * fonts_loaded=0 so a later call can retry rather than run fontless. */
    if (bfnt_status == 0 || dyn_fonts > 0)
        fonts_loaded = 1;
}
