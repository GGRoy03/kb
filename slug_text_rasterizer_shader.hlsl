cbuffer PerPass : register(b0)
{
    float4x4 ViewProj;
};


cbuffer PerGlyph : register(b1)
{
    uint CurveTexelCount;
};


struct VS_INPUT
{
    float2 ScreenPosInPixel    : POSITION;
    float2 PixelCoordInEmSpace : TEXCOORD;
};

struct PS_INPUT
{
    float4 Position          : SV_Position;
    float2 PixelCoordInEmSpace : EMTX;
};


Texture2D CurveTexture : register(t0); // Control Point Texture


PS_INPUT VS(VS_INPUT Input)
{
    PS_INPUT Output;
    
    Output.Position            = mul(ViewProj, float4(Input.ScreenPosInPixel, 0, 1));
    Output.PixelCoordInEmSpace = Input.PixelCoordInEmSpace;
    
    return Output;
}


float2 FindHorizontalHit(float2 P0, float2 P1, float2 P2)
{
    //
    // The general idea is to do ray hit-testing code along the position Y axis and find where the ray crosses the curve.
    // We want to find where Y = 0 given a curve described by three points.
    //
    // LERP        : A + ((B - A) * T)
    // LERP        : A * (1 - T) + B * T
    // LERP(P0, P1): (P0 * (1 - T) + P1 * T)
    // LERP(P1, P2): (P1 * (1 - T) + P2 * T)
    //
    // QUADRATIC BEZIER(P0, P1, P2): LERP(LERP(P0, P1), LERP(P1, P2))
    // QUADRATIC BEZIER(P0, P1, P2): (P0 * (1 - T) + P1 * T) * (1 - T) + (P1 * (1 - t) + P2 * T) * (T)
    //
    // POLYNOMIAL: (P0 * (1 - T)^2 + P1 * 2 * T * (1 - T) + P2 * T^2
    // POLYNOMIAL: (P0 * (1 - 2T + T^2)) + (P1 * (2T - 2 * T^2)) + P2 * T^2
    // POLYNOMIAL: (P0 - (P0 * 2T) + (P0 * T^2)) + ((P1 * 2T) - (P1 * 2 * T^2)) + (P2 * T^2)
    //
    // QUADRATIC A: (P0 - (2P1) + P2) -> T^2
    // QUADRATIC B: (-2P0) + (2P1)    -> T
    // QUADRATIC C: P0                -> Constant
    //
    // (2(P0-P1) +- sqrt((4(P0-P1)^2 - 4(P0-2P1+P2)P0))) / 2(P0-2P1+P2)
    // ((P0-P1) +- sqrt(((P0-P1)^2 - (P0-2P1+P2)P0))) / (P0-2P1+P2)
    // A: P0-2P1+P2
    // B: P0-P1
    // C: P0
    //
    
    float2 CoeffA       = P0 - (2.0f * P1) + P2;
    float2 CoeffB       = P0 - P1;
    float2 CoeffC       = P0;
    float  Discriminant = sqrt(max(CoeffB.y * CoeffB.y - CoeffA.y * CoeffC.y, 0.0f));
    
    float T0 = (CoeffB.y - Discriminant) / (CoeffA.y);
    float T1 = (CoeffB.y + Discriminant) / (CoeffA.y);
    
    //
    // If A is close to 0, then the expression is nearly linear.
    // Solve: -2bt + c = 0, -2bt = -c, -2t = -c/b, t = -c/b/-2, t = C/(B/2)
    //
    
    if(abs(CoeffA.y) < 1.0f / 65536.0f)
    {
        T0 = CoeffC.y * (0.5f / CoeffB.y);
        T1 = CoeffC.y * (0.5f / CoeffB.y);
    }
    
    //
    // We want to return the Xs where Y = 0, we have our Ts which represents where along that
    // curve Y = 0, we simply have to solve the quadratic again : At^2 + 2Bt + C
    // B = (P0-P1) -> (-2P0 + 2p2) = -2 * (P0-P1)
    //
    
    float Root0 = ((CoeffA.x * T0 - CoeffB.x * 2.0f) * T0) + CoeffC.x;
    float Root1 = ((CoeffA.x * T1 - CoeffB.x * 2.0f) * T1) + CoeffC.x;
    
    return float2(Root0, Root1);
}


uint ComputeRootCode(float y0, float y1, float y2)
{
    //
    // TODO: Can probably be optimized, looking at the reference shader code.
    // TODO: Explain what the fuck this is computing.
    //
    
    uint Sign0 = (y0 > 0.0f) ? 2U : 0U;
    uint Sign1 = (y1 > 0.0f) ? 4U : 0U;
    uint Sign2 = (y2 > 0.0f) ? 8U : 0U;
    uint Shift = Sign2 | Sign1 | Sign0;
    
    uint Table = 0x2E74U;
    uint Entry = Table >> Shift;
    uint Code  = Entry & 0x3U;
    
    return Code;
}


float4 PS(PS_INPUT Input) : SV_TARGET
{
    int WindingNumber = 0;
    
    for (uint TexelIndex = 0; TexelIndex < CurveTexelCount - 1; ++TexelIndex)
    {
        //
        // The CurveTexture contains the control points that form the outlines of the glyphs represented as quadratic Bezier curves.
        // Texel0 contains the first and second point (xy | zw)
        // Texel1 contains the third point (xy)
        //
        
        float4 Texel0 = CurveTexture.Load(int3(TexelIndex    , 0, 0));
        float4 Texel1 = CurveTexture.Load(int3(TexelIndex + 1, 0, 0));
        
        //
        // Move the points as if the pixel we are rendering was at the origin.
        //
        
        float2 Point0 = Texel0.xy - Input.PixelCoordInEmSpace;
        float2 Point1 = Texel0.zw - Input.PixelCoordInEmSpace;
        float2 Point2 = Texel1.xy - Input.PixelCoordInEmSpace;
        
        //
        // Compute the root code, the root code tells us what to do with our root values
        //
        
        uint RootCode = ComputeRootCode(Point0.y, Point1.y, Point2.y);
        if (RootCode != 0U)
        {
            float2 Roots = FindHorizontalHit(Point0, Point1, Point2);

            if ((RootCode & 1U) != 0U && Roots.x >= 0.0f)
            {
                WindingNumber += 1;
            }

            if ((RootCode & 2U) != 0U && Roots.y >= 0.0f)
            {
                WindingNumber -= 1;
            }
        }
    }
    
    if (WindingNumber != 0)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    else
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
}