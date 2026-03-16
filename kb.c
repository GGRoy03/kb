#define KB_TEXT_SHAPE_IMPLEMENTATION
#include "kb_text_shape.h"

#include <stdint.h>
#include <assert.h>
#include <malloc.h>
#include <stdbool.h>
#include <math.h>

typedef struct
{
    char    *Data;
    uint64_t Size;
} byte_string;


#define ByteStringLit(String) ByteString(String, (sizeof(String) - 1))


static byte_string
ByteString(char *Data, uint64_t Size)
{
    byte_string Result =
    {
        .Data = Data,
        .Size = Size,
    };

    return Result;
}


int main()
{
    kbts_shape_context *Context = kbts_CreateShapeContext(0, 0);
    kbts_font *Font = kbts_ShapePushFontFromSystem(Context, "Consolas (TrueType)", 0);
    byte_string String0 = ByteStringLit("ABCDEF");
    for (;;)
    {


        kbts_ShapeBegin(Context, KBTS_DIRECTION_LTR, KBTS_LANGUAGE_ENGLISH);
        kbts_ShapeUtf8(Context, String0.Data, String0.Size, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
        kbts_ShapeEnd(Context);

        kbts_run Run;
        while (kbts_ShapeRun(Context, &Run))
        {
            // No-Op?
        }

    }
}