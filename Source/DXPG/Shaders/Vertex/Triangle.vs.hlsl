struct ModelViewProjection
{
    matrix MVP;
};
 
ConstantBuffer<ModelViewProjection> ModelViewProjectionCB : register(b0);

struct VSIn
{
    float3 Pos : POSITION;
    float3 Color : COLOR;
};

struct VSOut
{
    float4 Pos : SV_POSITION;
    float3 Color : COLOR;
};

VSOut main(VSIn IN)
{
    VSOut output;
    output.Pos = mul(float4(IN.Pos, 1.0f), ModelViewProjectionCB.MVP);
    output.Color = IN.Color;
    return output;
}