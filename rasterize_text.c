#include <assert.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment (lib, "dxguid")
#pragma comment(lib, "user32.lib")

#include "kb_text_shape.h"

#include "text_vertex_shader.h"
#include "text_pixel_shader.h"

// -------------------------------------------------------
// Globals
// -------------------------------------------------------

static HWND                     Window           = 0;
static ID3D11Device            *Device           = 0;
static ID3D11DeviceContext     *Context          = 0;
static IDXGISwapChain          *SwapChain        = 0;
static ID3D11RenderTargetView  *BackbufferRTV    = 0;
static ID3D11VertexShader      *TextVS           = 0;
static ID3D11PixelShader       *TextPS           = 0;
static ID3D11InputLayout       *TextInputLayout  = 0;
static ID3D11Buffer            *TextVertexBuffer = 0;
static ID3D11Buffer            *PerPassCB        = 0;
static ID3D11Buffer            *PerGlyphCB       = 0;
static ID3D11RasterizerState   *RasterState      = 0;
static ID3D11Texture2D         *CurveTexture2D   = 0;
static ID3D11ShaderResourceView*CurveSRV         = 0;
static kbts_shape_context      *ShapeContext     = 0;
static kbts_curve_texture       CurveTexture     = {0};
static int                      CurveUploaded    = 0;

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

//
// A lot of padding.
//

typedef struct 
{
    uint32_t Count;
    uint32_t Start;
    float   _Pad0, _Pad1;
} glyph_contour;


typedef struct
{
    uint32_t      ContourCount; float _Pad0, _Pad1, _Pad2;
    glyph_contour Contours[4];
} per_glyph_cb;


// -------------------------------------------------------
// Helpers
// -------------------------------------------------------


static mat4x4
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
    DXGI_SWAP_CHAIN_DESC SwapDesc =
    {
        .BufferDesc.Width                   = WindowWidth,
        .BufferDesc.Height                  = WindowHeight,
        .BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM,
        .BufferDesc.RefreshRate.Numerator   = 60,
        .BufferDesc.RefreshRate.Denominator = 1,
        .SampleDesc.Count                   = 1,
        .SampleDesc.Quality                 = 0,
        .BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount                        = 2,
        .OutputWindow                       = Window,
        .Windowed                           = TRUE,
        .SwapEffect                         = DXGI_SWAP_EFFECT_FLIP_DISCARD,
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
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    Device->lpVtbl->CreateInputLayout(Device, Layout, 2, TextVertexShaderBytes, sizeof(TextVertexShaderBytes), &TextInputLayout);

    // Vertex buffer (6 vertices for a quad)
    D3D11_BUFFER_DESC VBDesc =
    {
        .ByteWidth      = 6 * sizeof(text_vertex),
        .Usage          = D3D11_USAGE_DYNAMIC,
        .BindFlags      = D3D11_BIND_VERTEX_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    Device->lpVtbl->CreateBuffer(Device, &VBDesc, 0, &TextVertexBuffer);

    // PerPass cbuffer
    D3D11_BUFFER_DESC CBDesc =
    {
        .ByteWidth      = sizeof(mat4x4),
        .Usage          = D3D11_USAGE_DYNAMIC,
        .BindFlags      = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    Device->lpVtbl->CreateBuffer(Device, &CBDesc, 0, &PerPassCB);

    // PerGlyph cbuffer
    D3D11_BUFFER_DESC PerGlyphCBDesc =
    {
        .ByteWidth      = sizeof(per_glyph_cb),
        .Usage          = D3D11_USAGE_DYNAMIC,
        .BindFlags      = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    Device->lpVtbl->CreateBuffer(Device, &PerGlyphCBDesc, 0, &PerGlyphCB);

    // Curve texture (1024x1024, R16G16B16A16_FLOAT, we fill only what we need)
    D3D11_TEXTURE2D_DESC TexDesc =
    {
        .Width          = 1024,
        .Height         = 1024,
        .MipLevels      = 1,
        .ArraySize      = 1,
        .Format         = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc     = { .Count = 1, .Quality = 0 },
        .Usage          = D3D11_USAGE_DYNAMIC,
        .BindFlags      = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    Device->lpVtbl->CreateTexture2D(Device, &TexDesc, 0, &CurveTexture2D);

    D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc =
    {
        .Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D.MipLevels       = 1,
        .Texture2D.MostDetailedMip = 0,
    };
    Device->lpVtbl->CreateShaderResourceView(Device, (ID3D11Resource *)CurveTexture2D, &SRVDesc, &CurveSRV);

    // Rasterizer
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

    if(!CurveTexture.Texels)
    {
        CurveTexture = kbts_LoadCurveTexture(ShapeContext);
    }

    if(!CurveUploaded && CurveTexture.Texels)
    {
        D3D11_MAPPED_SUBRESOURCE Mapped = {0};
        Context->lpVtbl->Map(Context, (ID3D11Resource *)CurveTexture2D, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        if(Mapped.pData)
        {
            // Copy row by row — texture pitch may be larger than our data width
            uint8_t  *Dst         = (uint8_t *)Mapped.pData;
            uint8_t  *Src         = (uint8_t *)CurveTexture.Texels;
            size_t    RowBytes    = CurveTexture.TexelCount * 4 * sizeof(uint16_t);
            memcpy(Dst, Src, RowBytes);
            Context->lpVtbl->Unmap(Context, (ID3D11Resource *)CurveTexture2D, 0);
        }

        CurveUploaded = 1;
    }

    // Update PerPass cbuffer
    {
        mat4x4 OrthoMatrix = OrthographicProjection(0, WindowWidth, 0, WindowHeight, 0.0f, 1.0f);
        D3D11_MAPPED_SUBRESOURCE Mapped = {0};
        Context->lpVtbl->Map(Context, (ID3D11Resource *)PerPassCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        *(mat4x4 *)Mapped.pData = OrthoMatrix;
        Context->lpVtbl->Unmap(Context, (ID3D11Resource *)PerPassCB, 0);
    }

    // Update PerGlyph cbuffer
    {
        per_glyph_cb PerGlyph =
        {
            .ContourCount      = CurveTexture.ContourCount,
            .Contours[0].Start = CurveTexture.Contours[0].StartCurve,
            .Contours[0].Count = CurveTexture.Contours[0].CurveCount,
            .Contours[1].Start = CurveTexture.Contours[1].StartCurve,
            .Contours[1].Count = CurveTexture.Contours[1].CurveCount,
        };


        D3D11_MAPPED_SUBRESOURCE Mapped = {0};
        Context->lpVtbl->Map(Context, (ID3D11Resource *)PerGlyphCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        *(per_glyph_cb *)Mapped.pData = PerGlyph;
        Context->lpVtbl->Unmap(Context, (ID3D11Resource *)PerGlyphCB, 0);
    }

    // Build quad (6 vertices, 2 triangles)
    // Screen pos: (QuadX, QuadY) to (QuadX+QuadW, QuadY+QuadH)
    // Em space:   (MinX, MinY)   to (MaxX, MaxY)
    {
        float QuadX = 100.0f, QuadY = 100.0f;
        float QuadW = 400.0f, QuadH = 400.0f;

        float MinX = (float)CurveTexture.Bounds.MinX;
        float MinY = (float)CurveTexture.Bounds.MinY;
        float MaxX = (float)CurveTexture.Bounds.MaxX;
        float MaxY = (float)CurveTexture.Bounds.MaxY;

        // Screen positions
        float L = QuadX,        R = QuadX + QuadW;
        float T = QuadY,        B = QuadY + QuadH;

        text_vertex Verts[6] =
        {
            { L, T,  MinX, MaxY },
            { R, T,  MaxX, MaxY },
            { R, B,  MaxX, MinY },

            { L, T,  MinX, MaxY },
            { R, B,  MaxX, MinY },
            { L, B,  MinX, MinY },
        };

        D3D11_MAPPED_SUBRESOURCE Mapped = {0};
        Context->lpVtbl->Map(Context, (ID3D11Resource *)TextVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        if(Mapped.pData)
        {
            memcpy(Mapped.pData, Verts, sizeof(Verts));
            Context->lpVtbl->Unmap(Context, (ID3D11Resource *)TextVertexBuffer, 0);
        }
    }

    // Draw
    UINT Stride = sizeof(text_vertex);
    UINT Offset = 0;
    Context->lpVtbl->IASetInputLayout(Context, TextInputLayout);
    Context->lpVtbl->IASetVertexBuffers(Context, 0, 1, &TextVertexBuffer, &Stride, &Offset);
    Context->lpVtbl->IASetPrimitiveTopology(Context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Context->lpVtbl->VSSetShader(Context, TextVS, 0, 0);
    Context->lpVtbl->VSSetConstantBuffers(Context, 0, 1, &PerPassCB);
    Context->lpVtbl->PSSetShader(Context, TextPS, 0, 0);
    Context->lpVtbl->PSSetConstantBuffers(Context, 1, 1, &PerGlyphCB);
    Context->lpVtbl->PSSetShaderResources(Context, 0, 1, &CurveSRV);
    Context->lpVtbl->RSSetState(Context, RasterState);
    Context->lpVtbl->Draw(Context, 6, 0);

    SwapChain->lpVtbl->Present(SwapChain, 1, 0);
}


// -------------------------------------------------------
// Entry Point
// -------------------------------------------------------

int WINAPI
WinMain(HINSTANCE Instance, HINSTANCE Prev, LPSTR CmdLine, int CmdShow)
{
    (void)Prev; (void)CmdLine; (void)CmdShow;

    WNDCLASSA WindowClass =
    {
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = WindowProc,
        .hInstance     = Instance,
        .hCursor       = LoadCursor(0, IDC_ARROW),
        .lpszClassName = "SlugTestWindow",
    };

    RegisterClassA(&WindowClass);

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

    ShapeContext = kbts_CreateShapeContext(0, 0);
    kbts_ShapePushFontFromFile(ShapeContext, "font/TrenchThin.ttf", 0);

    kbts_ShapeBegin(ShapeContext, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);
    kbts_ShapeUtf8(ShapeContext, "A", 1, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
    kbts_ShapeEnd(ShapeContext);

    kbts_run Run;
    while(kbts_ShapeRun(ShapeContext, &Run))
    {
        kbts_glyph *Glyph;
        while(kbts_GlyphIteratorNext(&Run.Glyphs, &Glyph))
        {
        }
    }

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