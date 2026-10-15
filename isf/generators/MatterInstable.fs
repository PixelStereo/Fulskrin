/*{
  "ISFVSN":"2", "DESCRIPTION":"Matières instables — particulate membranes, filaments and blue currents. Program audio drives energy, bass, brilliance and attack through Internal interactions.",
  "CREDIT":"PINOL", "CATEGORIES":["Particles","Audio Reactive"],
  "INPUTS":[
    {"NAME":"mode","TYPE":"long","DEFAULT":0,"VALUES":[0,1,2],"LABELS":["Particulate membrane","Filaments","Currents"]},
    {"NAME":"density","TYPE":"float","DEFAULT":0.55,"MIN":0,"MAX":1},
    {"NAME":"turbulence","TYPE":"float","DEFAULT":0.4,"MIN":0,"MAX":1},
    {"NAME":"drift","TYPE":"float","DEFAULT":0.3,"MIN":0,"MAX":1},
    {"NAME":"scale","TYPE":"float","DEFAULT":1,"MIN":0.25,"MAX":3},
    {"NAME":"bend","TYPE":"float","DEFAULT":0.2,"MIN":-1,"MAX":1},
    {"NAME":"phase","TYPE":"float","DEFAULT":0.3,"MIN":0,"MAX":1},
    {"NAME":"scene","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
    {"NAME":"energy","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
    {"NAME":"bass","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
    {"NAME":"brilliance","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
    {"NAME":"attack","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
    {"NAME":"tint","TYPE":"color","DEFAULT":[0.35,0.8,1,1]}
  ]
}*/
float hash21(vec2 p){ p=fract(p*vec2(123.34,345.45));p+=dot(p,p+34.345);return fract(p.x*p.y); }
float matterNoise(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);return mix(mix(hash21(i),hash21(i+vec2(1,0)),f.x),mix(hash21(i+vec2(0,1)),hash21(i+vec2(1)),f.x),f.y);}
float fbm(vec2 p){float n=0.0,a=0.5;for(int i=0;i<4;++i){n+=a*matterNoise(p);p=mat2(1.6,1.2,-1.2,1.6)*p+7.1;a*=0.5;}return n;}
mat2 rotate2(float a){return mat2(cos(a),-sin(a),sin(a),cos(a));}
void main(){
    vec2 uv=(isf_FragNormCoord-0.5)*vec2(RENDERSIZE.x/RENDERSIZE.y,1.0);
    float t=TIME*(0.035+drift*0.11),e=clamp(energy,0.0,1.0),b=clamp(bass,0.0,1.0),hit=clamp(attack,0.0,1.0);
    vec2 p=rotate2((phase-0.5)*2.0+bend*0.5)*uv*(2.0/max(scale,0.25));
    float radius=length(p),angle=atan(p.y,p.x);
    vec2 warp=vec2(fbm(p*1.6+vec2(t,-t*0.7)),fbm(p*1.6+vec2(-t*0.6,t)+13.0));
    vec2 q=p+(warp-0.5)*(0.4+2.2*turbulence+0.8*b)+vec2(sin(t+phase*6.28),cos(t*0.73))*0.18;
    float veil=fbm(q*2.2+vec2(t,phase*5.0));
    float envelope=exp(-radius*radius*(0.4+0.35*(1.0-density)));
    float lum=0.0,core=0.0;
    if(mode==0){
        float fold=sin(q.x*4.2+sin(q.y*3.7+t)*1.8+veil*7.0+bend*3.0);
        float membrane=pow(max(0.0,1.0-abs(fold)),12.0+brilliance*20.0);
        float cells=hash21(floor(isf_FragNormCoord*RENDERSIZE/(1.0+0.8*(1.0-density))));
        float grains=pow(cells,12.0-7.0*density);
        float body=0.10+0.7*membrane+0.13*veil;
        lum=grains*body*(0.6+1.8*e)+membrane*0.17;
        core=pow(membrane,2.0)*(0.2+hit*0.7);
        lum*=envelope;
    }else if(mode==1){
        float radial=angle*(6.0+3.0*b)+sin(radius*4.0-t*2.0)*bend*3.0+veil*(6.0+7.0*turbulence);
        float filaments=pow(max(0.0,1.0-abs(sin(radial+radius*7.0))),24.0+brilliance*35.0);
        float threads=pow(max(0.0,1.0-abs(sin(q.x*19.0+q.y*5.0+veil*17.0))),35.0);
        float hollow=1.0-exp(-radius*radius*7.0);
        lum=(filaments*(0.35+e)+threads*0.19)*envelope*hollow;
        core=exp(-radius*radius*(32.0-18.0*b))*(0.25+0.6*hit);
    }else{
        float flow=q.x*8.0+sin(q.y*3.0+veil*5.0-t)*3.5+veil*13.0+bend*p.y*7.0;
        float ridges=pow(0.5+0.5*sin(flow),8.0+brilliance*12.0);
        float fine=pow(0.5+0.5*sin(flow*3.1+fbm(q*5.0)*8.0),22.0);
        lum=(ridges*(0.25+e*0.7)+fine*(0.1+brilliance*0.5))*envelope;
        core=pow(ridges,2.0)*0.3;
    }
    float breath=0.82+0.18*sin(t*1.7+scene*6.28);
    vec3 pigment=mix(vec3(0.74,0.79,0.86),tint.rgb,0.45+0.5*scene);
    vec3 color=pigment*lum*breath+mix(pigment,vec3(0.9,0.97,1.0),0.8)*core*envelope;
    color*=1.0+hit*0.45;
    // Gentle shoulder keeps bright attacks textured instead of clipping to flat white.
    color=1.0-exp(-color*1.8);
    gl_FragColor=vec4(color,1.0);
}
