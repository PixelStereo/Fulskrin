/*{
    "DESCRIPTION": "Solid color (background, light mask, projector test).",
    "ISFVSN": "2",
    "CATEGORIES": ["Generative"],
    "INPUTS": [
        { "NAME": "color", "LABEL": "Color", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0] }
    ]
}*/

void main()
{
    gl_FragColor = color;
}
