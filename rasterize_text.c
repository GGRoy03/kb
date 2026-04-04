#include "kb_text_shape.h"
#include <assert.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment (lib, "dxguid")

#pragma comment(lib, "user32.lib")

#include "text_vertex_shader.h"
#include "text_pixel_shader.h"

// -------------------------------------------------------
// Globals
// -------------------------------------------------------

static HWND                    Window           = 0;
static ID3D11Device           *Device           = 0;
static ID3D11DeviceContext    *Context          = 0;
static IDXGISwapChain         *SwapChain        = 0;
static ID3D11RenderTargetView *BackbufferRTV    = 0;
static ID3D11VertexShader     *TextVS           = 0;
static ID3D11PixelShader      *TextPS           = 0;
static ID3D11InputLayout      *TextInputLayout  = 0;
static ID3D11Buffer           *TextVertexBuffer = 0;
static ID3D11Buffer           *PerPassCB        = 0;
static ID3D11RasterizerState  *RasterState      = 0;
static kbts_shape_context     *ShapeContext     = 0;
static kbts_curve_texture      CurveTexture     = {0};

static int WindowWidth  = 1920;
static int WindowHeight = 1080;
static int Running      = 1;


// -------------------------------------------------------
// Structs
// -------------------------------------------------------


typedef struct
{
    float PosX , PosY;
    float NormX, NormY;
} text_vertex;


typedef struct
{
	float c0r0, c0r1, c0r2, c0r3;
	float c1r0, c1r1, c1r2, c1r3;
	float c2r0, c2r1, c2r2, c2r3;
	float c3r0, c3r1, c3r2, c3r3;
} mat4x4;



// -------------------------------------------------------
// Helpers
// -------------------------------------------------------


mat4x4
OrthographicProjection(float Left, float Right, float Top, float Bot, float Near, float Far)
{
    assert(Right > Left);
    assert(Bot   > Top);
    assert(Far   > Near);

    mat4x4 Result =
    {
        .c0r0 = 2.0f / (Right - Left),
        .c1r1 = -2.0f / (Bot   - Top),
        .c2r2 = 1.0f / (Far   - Near),

        .c3r0 = -(Right + Left) / (Right - Left),
        .c3r1 = (Bot   + Top)  / (Bot   - Top),
        .c3r2 = -Near / (Far  - Near),

        .c3r3 = 1.0f,
    };

    return Result;
}


// -------------------------------------------------------
// Window Procedure
// -------------------------------------------------------

static LRESULT CALLBACK
WindowProc(HWND Hwnd, UINT Message, WPARAM WParam, LPARAM LParam)
{
    LRESULT Result = 0;

    switch(Message)
    {
        case WM_CLOSE:
        case WM_DESTROY:
        {
            Running = 0;
        } break;

        case WM_SIZE:
        {
            WindowWidth  = LOWORD(LParam);
            WindowHeight = HIWORD(LParam);
        } break;

        default:
        {
            Result = DefWindowProcA(Hwnd, Message, WParam, LParam);
        } break;
    }

    return Result;
}

// -------------------------------------------------------
// D3D11 Init
// -------------------------------------------------------

static int
D3D11Init(void)
{
    // Swap chain + device
    DXGI_SWAP_CHAIN_DESC SwapDesc =
    {
        .BufferDesc.Width            = WindowWidth,
        .BufferDesc.Height           = WindowHeight,
        .BufferDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM,
        .BufferDesc.RefreshRate.Numerator   = 60,
        .BufferDesc.RefreshRate.Denominator = 1,
        .SampleDesc.Count            = 1,
        .SampleDesc.Quality          = 0,
        .BufferUsage                 = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount                 = 2,
        .OutputWindow                = Window,
        .Windowed                    = TRUE,
        .SwapEffect                  = DXGI_SWAP_EFFECT_FLIP_DISCARD,
    };

    D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT              DeviceFlags  = D3D11_CREATE_DEVICE_DEBUG;

    HRESULT HR = D3D11CreateDeviceAndSwapChain(
        0,
        D3D_DRIVER_TYPE_HARDWARE,
        0,
        DeviceFlags,
        &FeatureLevel, 1,
        D3D11_SDK_VERSION,
        &SwapDesc,
        &SwapChain,
        &Device,
        0,
        &Context
    );

    if(FAILED(HR)) return 0;

    // Backbuffer RTV
    ID3D11Texture2D *Backbuffer = 0;
    SwapChain->lpVtbl->GetBuffer(SwapChain, 0, &IID_ID3D11Texture2D, (void **)&Backbuffer);
    Device->lpVtbl->CreateRenderTargetView(Device, (ID3D11Resource *)Backbuffer, 0, &BackbufferRTV);
    Backbuffer->lpVtbl->Release(Backbuffer);

    return 1;
}


static int
TextRenderInit(void)
{
    // Shaders
    Device->lpVtbl->CreateVertexShader(Device, TextVertexShaderBytes, sizeof(TextVertexShaderBytes), 0, &TextVS);
    Device->lpVtbl->CreatePixelShader (Device, TextPixelShaderBytes,  sizeof(TextPixelShaderBytes),  0, &TextPS);

    // Input layout
    D3D11_INPUT_ELEMENT_DESC Layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    Device->lpVtbl->CreateInputLayout(Device, Layout, 2, TextVertexShaderBytes, sizeof(TextVertexShaderBytes), &TextInputLayout);

    D3D11_BUFFER_DESC VBDesc =
    {
        .ByteWidth      = 60 * 1024,
        .Usage          = D3D11_USAGE_DYNAMIC,
        .BindFlags      = D3D11_BIND_VERTEX_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    Device->lpVtbl->CreateBuffer(Device, &VBDesc, 0, &TextVertexBuffer);

    // Per-pass cbuffer (just allocate, we'll update each frame)
    D3D11_BUFFER_DESC CBDesc =
    {
        .ByteWidth      = sizeof(mat4x4),
        .Usage          = D3D11_USAGE_DYNAMIC,
        .BindFlags      = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    Device->lpVtbl->CreateBuffer(Device, &CBDesc, 0, &PerPassCB);

    D3D11_RASTERIZER_DESC RasterDesc =
    {
        .FillMode = D3D11_FILL_SOLID,
        .CullMode = D3D11_CULL_NONE,
    };
    Device->lpVtbl->CreateRasterizerState(Device, &RasterDesc, &RasterState);

    return 1;
}

// -------------------------------------------------------
// Render
// -------------------------------------------------------


static void
Render(void)
{
    // Viewport
    D3D11_VIEWPORT Viewport =
    {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width    = (float)WindowWidth,
        .Height   = (float)WindowHeight,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };

    Context->lpVtbl->RSSetViewports(Context, 1, &Viewport);
    Context->lpVtbl->OMSetRenderTargets(Context, 1, &BackbufferRTV, 0);

    float ClearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    Context->lpVtbl->ClearRenderTargetView(Context, BackbufferRTV, ClearColor);

    mat4x4 OrthoMatrix = OrthographicProjection(0, WindowWidth, 0, WindowHeight, 0.0f, 1.0f);

    D3D11_MAPPED_SUBRESOURCE Mapped = {0};
    Context->lpVtbl->Map(Context, (ID3D11Resource *)PerPassCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
    *(mat4x4 *)Mapped.pData = OrthoMatrix;
    Context->lpVtbl->Unmap(Context, (ID3D11Resource *)PerPassCB, 0);

    int VertIdx = 0;
    int StepsPerCurve = 20;
    {
        if(!CurveTexture.Texels)
        {
            CurveTexture = kbts_LoadCurveTexture(ShapeContext);

            //for (uint32_t TexelIdx = 0; TexelIdx < CurveTexture.TexelCount; ++TexelIdx)
            //{
            //    kbts_curve_texel Texel = CurveTexture.Texels[TexelIdx];

            //    char Buffer[256] = { 0 };
            //    snprintf(Buffer, sizeof(Buffer), "TEXEL = Point0 : [%hd, %hd] | Point1 : [%hd, %hd]\n", Texel.X, Texel.Y, Texel.Z, Texel.W);

            //    OutputDebugStringA(Buffer);
            //}
        }

        int TotalVertCount = 0;
        for (int ContourIdx = 0; ContourIdx < CurveTexture.ContourCount; ContourIdx++)
        {
            TotalVertCount += CurveTexture.Contours[ContourIdx].CurveCount * StepsPerCurve * 2;
        }

        text_vertex *CurveVerts = malloc(TotalVertCount * sizeof(text_vertex));
        int          VertIdx    = 0;

        float EmWidth  = (float)(CurveTexture.Bounds.MaxX - CurveTexture.Bounds.MinX);
        float EmHeight = (float)(CurveTexture.Bounds.MaxY - CurveTexture.Bounds.MinY);
        float QuadX    = 100.0f;
        float QuadY    = 100.0f;
        float QuadW    = 400.0f;
        float QuadH    = 400.0f;

        for (int ContourIdx = 0; ContourIdx < CurveTexture.ContourCount; ContourIdx++)
        {
            kbts_contour_range Contour   = CurveTexture.Contours[ContourIdx];
            int                CurveBase = Contour.StartCurve;

            OutputDebugStringA("Curve Start\n");

            for (int CurveIdx = 0; CurveIdx < Contour.CurveCount; CurveIdx++)
            {
                int              TexelBase = (CurveBase + CurveIdx);
                kbts_curve_texel T0        = CurveTexture.Texels[TexelBase + 0];
                kbts_curve_texel T1        = CurveTexture.Texels[TexelBase + 1];

                assert(TexelBase + 1 < CurveTexture.TexelCount);

                float P1x = (float)T0.X, P1y = (float)T0.Y;
                float P2x = (float)T0.Z, P2y = (float)T0.W;
                float P3x = (float)T1.X, P3y = (float)T1.Y;

                char Buffer0[128] = { 0 };
                char Buffer1[128] = { 0 };
                char Buffer2[128] = { 0 };
                snprintf(Buffer0, sizeof(Buffer0), "Point1: [%f, %f]\n", P1x, P1y);
                snprintf(Buffer1, sizeof(Buffer1), "Point2: [%f, %f]\n", P2x, P2y);
                snprintf(Buffer2, sizeof(Buffer2), "Point3: [%f, %f]\n", P3x, P3y);
                OutputDebugStringA(Buffer0);
                OutputDebugStringA(Buffer1);
                OutputDebugStringA(Buffer2);

                for (int Step = 0; Step < StepsPerCurve; Step++)
                {
                    float T0f = (float)(Step + 0) / (float)StepsPerCurve;
                    float T1f = (float)(Step + 1) / (float)StepsPerCurve;

                    float A0 = (1 - T0f) * (1 - T0f), B0 = 2 * T0f * (1 - T0f), C0 = T0f * T0f;
                    float A1 = (1 - T1f) * (1 - T1f), B1 = 2 * T1f * (1 - T1f), C1 = T1f * T1f;

                    float X0 = A0 * P1x + B0 * P2x + C0 * P3x;
                    float Y0 = A0 * P1y + B0 * P2y + C0 * P3y;
                    float X1 = A1 * P1x + B1 * P2x + C1 * P3x;
                    float Y1 = A1 * P1y + B1 * P2y + C1 * P3y;

                    float SX0 = QuadX + ((X0 - CurveTexture.Bounds.MinX) / EmWidth) * QuadW;
                    float SY0 = QuadY + (1.0f - (Y0 - CurveTexture.Bounds.MinY) / EmHeight) * QuadH;
                    float SX1 = QuadX + ((X1 - CurveTexture.Bounds.MinX) / EmWidth) * QuadW;
                    float SY1 = QuadY + (1.0f - (Y1 - CurveTexture.Bounds.MinY) / EmHeight) * QuadH;

                    CurveVerts[VertIdx++] = (text_vertex){ SX0, SY0, 0, 0 };
                    CurveVerts[VertIdx++] = (text_vertex){ SX1, SY1, 0, 0 };
                }
            }
        }

        D3D11_MAPPED_SUBRESOURCE Mapped = {0};
        Context->lpVtbl->Map(Context, (ID3D11Resource *)TextVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        if(Mapped.pData)
        {
            memcpy(Mapped.pData, CurveVerts, VertIdx * sizeof(text_vertex));
            Context->lpVtbl->Unmap(Context, (ID3D11Resource *)TextVertexBuffer, 0);
        }

        free(CurveVerts);    
    }

    // Draw quad
    UINT Stride = sizeof(text_vertex);
    UINT Offset = 0;
    Context->lpVtbl->IASetInputLayout(Context, TextInputLayout);
    Context->lpVtbl->IASetVertexBuffers(Context, 0, 1, &TextVertexBuffer, &Stride, &Offset);
    Context->lpVtbl->IASetPrimitiveTopology(Context, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    Context->lpVtbl->VSSetShader(Context, TextVS, 0, 0);
    Context->lpVtbl->VSSetConstantBuffers(Context, 0, 1, &PerPassCB);
    Context->lpVtbl->PSSetShader(Context, TextPS, 0, 0);
    Context->lpVtbl->RSSetState(Context, RasterState);

    for (int ContourIdx = 0; ContourIdx < CurveTexture.ContourCount; ContourIdx++)
    {
        Context->lpVtbl->Draw(Context, CurveTexture.Contours[ContourIdx].CurveCount * StepsPerCurve * 2, CurveTexture.Contours[ContourIdx].StartCurve * StepsPerCurve * 2);
    }

    SwapChain->lpVtbl->Present(SwapChain, 1, 0);
}


// -------------------------------------------------------
// Entry Point
// -------------------------------------------------------


int WINAPI
WinMain(HINSTANCE Instance, HINSTANCE Prev, LPSTR CmdLine, int CmdShow)
{
    (void)Prev; (void)CmdLine; (void)CmdShow;

    // Register window class
    WNDCLASSA WindowClass =
    {
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = WindowProc,
        .hInstance     = Instance,
        .hCursor       = LoadCursor(0, IDC_ARROW),
        .lpszClassName = "SlugTestWindow",
    };

    RegisterClassA(&WindowClass);

    // Create window
    Window = CreateWindowExA(
        0,
        "SlugTestWindow",
        "Slug Test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WindowWidth, WindowHeight,
        0, 0, Instance, 0
    );

    if(!Window) return 1;

    if(!D3D11Init()) return 1;
    if(!TextRenderInit()) return 1;

    //
    // Text Setup
    //

    ShapeContext = kbts_CreateShapeContext(0, 0);
    kbts_ShapePushFontFromFile(ShapeContext, "font/TrenchThin.ttf", 0);

    kbts_ShapeBegin(ShapeContext, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);
    kbts_ShapeUtf8(ShapeContext, "A", 1, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
    kbts_ShapeEnd(ShapeContext);

    kbts_run Run;
    int CursorX = 0, CursorY = 0;
    while(kbts_ShapeRun(ShapeContext, &Run))
    {
        kbts_glyph *Glyph;
        while(kbts_GlyphIteratorNext(&Run.Glyphs, &Glyph))
        {
        }
    }

    // Game loop
    while(Running)
    {
        MSG Message;
        while(PeekMessageA(&Message, 0, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Message);
            DispatchMessageA(&Message);
        }

        Render();
    }

    return 0;
}
