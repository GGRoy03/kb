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

//
// Rasterizing implementation
//


typedef struct
{
    float X;
    float Y;
    bool  OnCurve; // Lazy, maybe don't need (another struct?)
} point;


typedef struct
{
    point Start;
    point End;
} segment;


typedef struct
{
    uint8_t R;
    uint8_t G;
    uint8_t B;
    uint8_t A;
} pixel;


typedef struct
{
    short NumberOfContours;
    short MinX;
    short MinY;
    short MaxX;
    short MaxY;
} glyf_header;


typedef struct
{
    uint16_t *EndPointsOfContours;
    uint16_t  InstructionLength;
    uint16_t *Instructions;
    uint8_t  *Flags;
    short    *XCoordinates;
    short    *YCoordinates;
} glyf_data;


typedef struct
{
    point   *Points;
    uint32_t PointCount;
} contour;


typedef struct contour_node contour_node;
struct contour_node
{
    contour_node *Next;
    contour       Value;
};


typedef struct contour_list
{
    contour_node *First;
    contour_node *Last;
} contour_list;


#define COLOR_BLACK {.R =   0, G =   0, B =   0, A = 255}
#define COLOR_WHITE {.R = 255, G = 255, B = 255, A = 255}


static float
LinearInterpolate(float A, float B, float T)
{
    float Result = A + (T * (B - A));
    return Result;
}


static point
LinearInterpolatePoint(point A, point B, float T)
{
    point Result =
    {
        .X = A.X + (T * (B.X - A.X)),
        .Y = A.Y + (T * (B.Y - A.Y)),
    };

    return Result;
}


static point
GetMidwayPoint(point A, point B)
{
    point Result =
    {
        .X = (A.X + B.X) / 2,
        .Y = (A.Y + B.Y) / 2,
    };

    return Result;
}


static void
WriteBMP(const char *Filename, pixel *Buffer, int Width, int Height)
{
    int PixelDataSize = Width * Height * 4;
    int FileSize = 54 + PixelDataSize;

    uint8_t Header[54] = { 0 };

    // File header
    Header[0] = 'B'; Header[1] = 'M';
    *(int *)(Header + 2) = FileSize;
    *(int *)(Header + 10) = 54; // Pixel data offset

    // DIB header
    *(int *)(Header + 14) = 40;     // DIB header size
    *(int *)(Header + 18) = Width;
    *(int *)(Header + 22) = -Height; // Negative = top-down
    *(short *)(Header + 26) = 1;      // Color planes
    *(short *)(Header + 28) = 32;     // Bits per pixel
    *(int *)(Header + 34) = PixelDataSize;

    FILE *File = fopen(Filename, "wb");
    assert(File);
    fwrite(Header, 1, 54, File);
    fwrite(Buffer, 1, PixelDataSize, File);
    fclose(File);
}


static void
RasterizePointList(contour_list List, uint32_t GlyphSizeX, uint32_t GlyphSizeY)
{
    //
    // Don't use malloc.
    //

    pixel *OutputBuffer = malloc(GlyphSizeX * GlyphSizeY * sizeof(point));
    assert(OutputBuffer);
    memset(OutputBuffer, 0, GlyphSizeX * GlyphSizeY * sizeof(point));

    //
    // Don't want to use malloc here.
    //

    uint32_t ScanlineY    = 0;
    uint32_t SegmentCount = 0;
    segment *Segments     = malloc(100 * sizeof(segment)); // This will overflow at some point. Need more meta-data to bound?

    assert(Segments);

    uint32_t StepCount    = 10;
    float    StepAmount   = 1.0f / StepCount;
    float    StepFraction = StepAmount;

    for (contour_node *Node = List.First; Node != 0; Node = Node->Next)
    {
        contour  Contour  = Node->Value;
        uint32_t PointIdx = 0;

        while(PointIdx < Contour.PointCount)
        {
            point Current = Contour.Points[PointIdx];
            point Next    = Contour.Points[(PointIdx + 1) % Contour.PointCount];

            if (Current.OnCurve && Next.OnCurve)
            {
                Segments[SegmentCount++] = (segment){ .Start = Current, .End = Next };
                PointIdx += 1;
            }
            else
            {
                point Control = Next;
                point End     = Contour.Points[(PointIdx + 2) % Contour.PointCount];

                point PreviousPoint = Current;
                float StepFraction  = StepAmount;

                while (StepFraction <= 1.0f)
                {
                    point A = LinearInterpolatePoint(Current, Control, StepFraction);
                    point B = LinearInterpolatePoint(Control, End, StepFraction);
                    point C = LinearInterpolatePoint(A, B, StepFraction);

                    Segments[SegmentCount++] = (segment){ .Start = PreviousPoint, .End = C };

                    PreviousPoint = C;
                    StepFraction += StepAmount;
                }

                PointIdx += 2;
            }
        }
    }



    //
    // Enabling this line somehow produces better results. This doesn't make any sense as it is
    // a copy of our last segment anyways. Maybe not better, but slightly different.
    //

    //Segments[SegmentCount++] = (segment){ .Start = Points[PointCount - 1], .End = Points[0] };


    //
    // Don't want to use malloc here.
    //

    uint32_t IntersectionCount = 0;
    float   *Intersections     = malloc(100 * sizeof(float)); // This will overflow at some point. Need more meta-data to bound?

    assert(Intersections);
    memset(Intersections, 0, 100 * sizeof(float));


    while (ScanlineY < GlyphSizeY)
    {
        IntersectionCount = 0;


        //
        // Something in this code is wrong. Scanline of 5 Y on the A should not produce
        // 2 intersections but 4.
        //

        for (uint32_t SegmentIdx = 0; SegmentIdx < SegmentCount; ++SegmentIdx)
        {
            segment Segment = Segments[SegmentIdx];

            float HeightDifference = Segment.End.Y - Segment.Start.Y;
            if (HeightDifference != 0.0f)
            {
                float HeightDistance = ScanlineY - Segment.Start.Y;
                float HeightFraction = HeightDistance / HeightDifference;

                if (HeightFraction >= 0.0f && HeightFraction <= 1.0f)
                {
                    Intersections[IntersectionCount++] = LinearInterpolate(Segment.Start.X, Segment.End.X, HeightFraction);
                }
            }
        }

        // assert(IntersectionCount % 2 == 0);

        //
        // Sort the X Intersection buffer such that we can fill lines.
        //


        for (uint32_t I = 0; I < IntersectionCount; ++I)
        {
            for (uint32_t J = 0; J < IntersectionCount - 1; ++J)
            {
                if (Intersections[J] > Intersections[J + 1])
                {
                    float Temp = Intersections[J];
                    Intersections[J]     = Intersections[J + 1];
                    Intersections[J + 1] = Temp;
                }
            }
        }

        //
        // Fill the output buffer given the sorted buffer.
        //

        for (uint32_t IntersectIdx = 0; IntersectIdx < IntersectionCount; IntersectIdx += 2)
        {
            uint32_t XStart = (uint32_t)(ceilf(Intersections[IntersectIdx]));
            uint32_t XEnd   = (uint32_t)(ceilf(Intersections[IntersectIdx + 1]));

            for (uint32_t X = XStart; X < XEnd; ++X)
            {
                pixel *Pixel = &OutputBuffer[ScanlineY * GlyphSizeX + X];
                Pixel->R = 255;
                Pixel->G = 255;
                Pixel->B = 255;
                Pixel->A = 255;
            }
        }

        ScanlineY += 1;
    }

    WriteBMP("test_raster_first_glyph.bmp", OutputBuffer, GlyphSizeX, GlyphSizeY);
}


//
// 
//

int main()
{
    kbts_shape_context *Context = kbts_CreateShapeContext(0, 0);
    kbts_font *Font = kbts_ShapePushFontFromSystem(Context, "Consolas (TrueType)", 0);

    kbts_ShapeBegin(Context, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);

    byte_string String0 = ByteStringLit("A");
    byte_string String1 = ByteStringLit("B");

    kbts_ShapeUtf8(Context, String0.Data, String0.Size, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
    kbts_ShapeUtf8(Context, String1.Data, String1.Size, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);

    kbts_ShapeEnd(Context);

    kbts_run Run;
    int CursorX = 0, CursorY = 0;
    while (kbts_ShapeRun(Context, &Run))
    {
        kbts_glyph *Glyph;
        while (kbts_GlyphIteratorNext(&Run.Glyphs, &Glyph))
        {
            //
            // Most of this code is pure hot garbage, but just trying to figure stuff out.
            //

            kbts_blob_table HeadTable = Font->Blob->Tables[KBTS_BLOB_TABLE_ID_HEAD];
            kbts_blob_table GlyfTable = Font->Blob->Tables[KBTS_BLOB_TABLE_ID_GLYF];
            kbts_blob_table LocaTable = Font->Blob->Tables[KBTS_BLOB_TABLE_ID_LOCA];

            if (HeadTable.Length && GlyfTable.Length && LocaTable.Length)
            {
                kbts__head *HeadTableData = (kbts__head *)((uint8_t *)Font->Blob + HeadTable.OffsetFromStartOfFile);
                void       *LocaTableData = (uint8_t *)Font->Blob + LocaTable.OffsetFromStartOfFile;
                void       *GlyfTableData = (uint8_t *)Font->Blob + GlyfTable.OffsetFromStartOfFile;

                float Scale = 32.0f / HeadTableData->UnitsPerEm;

                uint32_t ByteOffset = 0;
                if (HeadTableData->IndexToLocFormat == 0)
                {
                    ByteOffset = (*((uint16_t *)LocaTableData + Glyph->Id)) * 2;
                }
                else if (HeadTableData->IndexToLocFormat == 1)
                {
                    ByteOffset = *((uint32_t *)LocaTableData + Glyph->Id);
                }

                assert(ByteOffset < GlyfTable.Length);

                glyf_header *Header              = (glyf_header *)((uint8_t *)GlyfTableData + ByteOffset);
                uint16_t    *EndPointsOfContours = (uint16_t *)(Header + 1);
                uint16_t    *InstructionLength   = EndPointsOfContours + Header->NumberOfContours;
                uint8_t     *Instructions        = (uint8_t *)(InstructionLength + 1);
                uint8_t     *Flags               = (uint8_t *)(Instructions + *InstructionLength);

                //
                // This is where this becomes annoying. Because the amount of coodinates is not known.
                // Okay. We already parsed that. Should we just store it in the header or something?
                // It's just so annoying to deal with. Doing it once should be enough right?
                //

                uint16_t LogicalPointCount        = EndPointsOfContours[Header->NumberOfContours - 1] + 1;
                uint32_t LogicalPointIndex        = 0;
                uint32_t PhysicalFlagIndex        = 0;
                uint32_t XCoordinatePhysicalCount = 0;
                uint32_t YCoordinatePhysicalCount = 0;

                while (LogicalPointIndex < LogicalPointCount)
                {
                    uint8_t  BitFlag    = Flags[PhysicalFlagIndex++];
                    uint8_t  XByteCount = (BitFlag & 0x02) ? 1 : ((BitFlag & 0x10) ? 0 : 2);
                    uint8_t  YByteCount = (BitFlag & 0x04) ? 1 : ((BitFlag & 0x20) ? 0 : 2);
                    uint16_t Repeat     = (BitFlag & 0x08) ? Flags[PhysicalFlagIndex++] + 1 : 1;

                    XCoordinatePhysicalCount += Repeat * XByteCount;
                    YCoordinatePhysicalCount += Repeat * YByteCount;
                    LogicalPointIndex        += Repeat;
                }

                uint8_t *XCoordinate = (Flags + PhysicalFlagIndex);
                uint8_t *YCoordinate = ((uint8_t *)XCoordinate + XCoordinatePhysicalCount);

                LogicalPointIndex = 0;
                PhysicalFlagIndex = 0;


                //
                // Something like that is more useful.
                //

                contour_list List = {};

                //
                // Have to add another pass, maybe it's possible to do it in a single pass. I don't really know.
                // And I don't really care right now. Just want something that works.
                // 
                // Also, don't use malloc.
                //


                uint32_t ExpandedPointCount = 0;
                point   *ExpandedPoints     = malloc(LogicalPointCount * 2 * sizeof(point));
                assert(ExpandedPoints);


                uint32_t CurrentX    = 0;
                uint32_t CurrentY    = 0;
                uint32_t XByteOffset = 0;
                uint32_t YByteOffset = 0;

                uint32_t ContourStart = 0;
                for (uint32_t ContourIdx = 0; ContourIdx < Header->NumberOfContours; ++ContourIdx)
                {
                    int32_t  ContourEnd   = EndPointsOfContours[ContourIdx];
                    uint32_t ContourCount = ContourEnd - ContourStart + 1;

                    contour_node *Node = malloc(sizeof(contour_node));
                    Node->Value.Points     = malloc(ContourCount * 2 * sizeof(point));
                    Node->Value.PointCount = 0;
                    Node->Next             = 0;

                    contour *Contour = &Node->Value;

                    if (!List.First)
                    {
                        List.First = Node;
                        List.Last  = Node;
                    }
                    else
                    {
                        List.Last->Next = Node;
                        List.Last       = Node;
                    }

                    for (uint32_t _ = 0; _ < ContourCount; _++)
                    {
                        uint8_t BitFlag = Flags[PhysicalFlagIndex++];

                        //
                        // Extract all of the flags that we need.
                        // 
                        // TODO: Give names to these.
                        //

                        bool IsPointOnCurve = BitFlag & 0x01;
                        bool IsXShortVector = BitFlag & 0x02;
                        bool IsYShortVector = BitFlag & 0x04;
                        bool IsRepeated = BitFlag & 0x08;
                        bool IsXSameOrPositiveShort = BitFlag & 0x10;
                        bool IsYSameOrPositiveShort = BitFlag & 0x20;

                        //
                        // We need to fill this data depending on which flags are set
                        //

                        uint8_t  XByteCount = 0;
                        uint8_t  YByteCount = 0;
                        int      XSign = 1;
                        int      YSign = 1;
                        uint16_t Repeat = 1;

                        if (IsXShortVector)
                        {
                            XByteCount = 1;

                            if (!IsXSameOrPositiveShort)
                            {
                                XSign = -1;
                            }
                        }
                        else if (!IsXSameOrPositiveShort)
                        {
                            XByteCount = 2;
                        }

                        if (IsYShortVector)
                        {
                            YByteCount = 1;

                            if (!IsYSameOrPositiveShort)
                            {
                                YSign = -1;
                            }
                        }
                        else if (!IsYSameOrPositiveShort)
                        {
                            YByteCount = 2;
                        }

                        if (IsRepeated)
                        {
                            Repeat = Flags[PhysicalFlagIndex++] + 1;
                        }


                        //
                        // Maybe flip the Y since the bitmap Y is downwards?
                        //


                        for (uint32_t RepeatIdx = 0; RepeatIdx < Repeat; ++RepeatIdx)
                        {
                            short DeltaX = 0;
                            short DeltaY = 0;

                            if (XByteCount == 1)
                            {
                                DeltaX = XSign * (*(XCoordinate + XByteOffset));
                            }
                            else if (XByteCount == 2)
                            {
                                DeltaX = *(short *)(XCoordinate + XByteOffset);
                            }

                            if (YByteCount == 1)
                            {
                                DeltaY = YSign * (*(YCoordinate + YByteOffset));
                            }
                            else if (YByteCount == 2)
                            {
                                DeltaY = *(short *)(YCoordinate + YByteOffset);
                            }

                            //
                            // This can't underflow right? Assuming parsing is correct. Maybe just use signed stuff and assert
                            // just to be sure.
                            //

                            CurrentX += DeltaX;
                            CurrentY += DeltaY;

                            point *Point = Contour->Points + (RepeatIdx + Contour->PointCount++);
                            Point->X       = (CurrentX - Header->MinX) * Scale;
                            Point->Y       = (CurrentY - Header->MinY) * Scale;
                            Point->OnCurve = IsPointOnCurve;

                            XByteOffset += XByteCount;
                            YByteOffset += YByteCount;
                        }

                        LogicalPointIndex += Repeat;
                    }


                    //
                    // This needs to rewrite points in place maybe? Kind of annoying to deal with.
                    // Could also replace the pointer.
                    //

                    // point Start = Contour->Points[0];
                    // point End   = Contour->Points[Contour->PointCount - 1];
                    // if (!Start.OnCurve && !End.OnCurve)
                    // {
                    //     ExpandedPoints[ExpandedPointCount++] = GetMidwayPoint(End, Start);
                    // }
                    // ExpandedPoints[ExpandedPointCount++] = Start;
                       
                    // for (uint32_t PointIdx = 1; PointIdx < ContourCount; ++PointIdx)
                    // {
                    //     point Current = Points[PointIdx + ContourStart];
                    //     point Last    = Points[PointIdx + ContourStart - 1];
                       
                    //     if (!Current.OnCurve && !Last.OnCurve)
                    //     {
                    //         ExpandedPoints[ExpandedPointCount++] = GetMidwayPoint(Last, Current);
                    //     }
                       
                    //     ExpandedPoints[ExpandedPointCount++] = Current;
                    // }


                    ContourStart = ContourEnd + 1;
                }

                uint32_t GlyphSizeX = (uint32_t)(ceilf(Scale * (Header->MaxX - Header->MinX)));
                uint32_t GlyphSizeY = (uint32_t)(ceilf(Scale * (Header->MaxY - Header->MinY)));


                //for (uint32_t Idx = 0; Idx < ExpandedPointCount; ++Idx)
                //{
                //    printf("Point: (%f, %f)", ExpandedPoints[Idx].X, ExpandedPoints[Idx].Y);
                //}

                RasterizePointList(List, GlyphSizeX, GlyphSizeY);
            }
        }
    }
}