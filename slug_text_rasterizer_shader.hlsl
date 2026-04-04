cbuffer PerPass : register(b0)
{
    float4x4 ViewProj;
};

struct VS_INPUT
{
    float2 ScreenPosInPixel  : POSITION;
    float2 TexCoordInEmSpace : TEXCOORD;
};

struct PS_INPUT
{
    float4 Position          : SV_Position;
    float2 TexCoordInEmSpace : EMTX;
};


PS_INPUT VS(VS_INPUT Input)
{
    PS_INPUT Output;
    
    Output.Position          = mul(ViewProj, float4(Input.ScreenPosInPixel, 0, 1));
    Output.TexCoordInEmSpace = Input.TexCoordInEmSpace;
    
    return Output;
}

float4 PS(PS_INPUT Input) : SV_TARGET
{
    return float4(1, 1, 1, 1);
}