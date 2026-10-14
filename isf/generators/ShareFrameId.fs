/*{
    "DESCRIPTION": "GPU sharing diagnostic: identical frame number encoded as RGB bytes in every interior pixel; border alternates each frame. Capture the receiving Syphon/Spout pixels to detect torn or stale frames.",
    "ISFVSN": "2",
    "CATEGORIES": ["Alignment", "Diagnostics"],
    "INPUTS": []
}*/

// FRAMEINDEX is assigned by Pinol's ISF renderer each generator frame.
// Within a single published image, all interior pixels must decode to the
// same 24-bit frame number. The alternating border is an independent phase
// marker; sample it separately in the receiver.
void main()
{
    int n = FRAMEINDEX;
    vec3 id = vec3(float(n & 255), float((n >> 8) & 255),
                   float((n >> 16) & 255)) / 255.0;
    vec2 p = isf_FragNormCoord;
    bool border = p.x < 0.035 || p.x > 0.965 || p.y < 0.035 || p.y > 0.965;
    vec3 rgb = border ? ((n & 1) == 0 ? vec3(0.0) : vec3(1.0)) : id;
    gl_FragColor = vec4(rgb, 1.0);
}
