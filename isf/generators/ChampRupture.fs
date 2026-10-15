/*{
 "ISFVSN":"2","DESCRIPTION":"Champ de rupture — projected grain, interference sheets and fracture beams. Original audiovisual performance generator.","CREDIT":"PINOL","CATEGORIES":["Particles","Audio Reactive"],
 "INPUTS":[
 {"NAME":"mode","TYPE":"long","DEFAULT":0,"VALUES":[0,1,2],"LABELS":["Gravitational dust","Interference sheets","Fracture beams"]},
 {"NAME":"energy","TYPE":"float","DEFAULT":0.3,"MIN":0,"MAX":1},
 {"NAME":"bass","TYPE":"float","DEFAULT":0.2,"MIN":0,"MAX":1},
 {"NAME":"metal","TYPE":"float","DEFAULT":0.2,"MIN":0,"MAX":1},
 {"NAME":"air","TYPE":"float","DEFAULT":0.2,"MIN":0,"MAX":1},
 {"NAME":"hit","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
 {"NAME":"phase","TYPE":"float","DEFAULT":0.3,"MIN":0,"MAX":1},
 {"NAME":"fold","TYPE":"float","DEFAULT":0.4,"MIN":0,"MAX":1},
 {"NAME":"depth","TYPE":"float","DEFAULT":0.5,"MIN":0,"MAX":1},
 {"NAME":"drift","TYPE":"float","DEFAULT":0.3,"MIN":0,"MAX":1},
 {"NAME":"shear","TYPE":"float","DEFAULT":0,"MIN":-1,"MAX":1},
 {"NAME":"aperture","TYPE":"float","DEFAULT":0.6,"MIN":0,"MAX":1},
 {"NAME":"scene","TYPE":"float","DEFAULT":0,"MIN":0,"MAX":1},
 {"NAME":"tint","TYPE":"color","DEFAULT":[0.75,0.88,1,1]}
 ]
}*/
float crHash(vec2 p){p=fract(p*vec2(123.34,456.21));p+=dot(p,p+45.32);return fract(p.x*p.y);}
float crNoise(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(crHash(i),crHash(i+vec2(1,0)),f.x),mix(crHash(i+vec2(0,1)),crHash(i+1.),f.x),f.y);}
float crFbm(vec2 p){float n=0.,a=.5;for(int i=0;i<4;i++){n+=a*crNoise(p);p=mat2(1.6,1.2,-1.2,1.6)*p+9.7;a*=.5;}return n;}
mat2 crRot(float a){return mat2(cos(a),-sin(a),sin(a),cos(a));}
void main(){
 vec2 uv=(isf_FragNormCoord-.5)*vec2(RENDERSIZE.x/RENDERSIZE.y,1.);
 float t=TIME*(.018+.10*drift),pulse=clamp(hit,0.,1.);
 vec2 p=crRot((phase-.5)*1.7)*uv; p.x+=p.y*shear*.6; p*=1.6-.8*aperture;
 float lum=0.,halo=0.;
 if(mode==0){
   // Six projected depth planes. Fine stipple is anchored to the warped
   // field, rather than screen-space noise flickering independently.
   for(int z=0;z<6;z++){
     float d=float(z)/6.,zoom=.65+d*(1.8+depth*1.5);
     vec2 q=p*zoom+vec2(t*(.35+d),-t*.3)+d*9.;
     float n=crFbm(q*2.+vec2(t*.5,-t*.3));
     q+=vec2(sin(q.y*3.+n*4.+t),cos(q.x*2.-n*3.))*(.18+fold*.65+bass*.35);
     float membrane=pow(max(0.,1.-abs(sin(q.x*4.+q.y*1.7+n*7.+d*3.))),18.+metal*22.);
     vec2 grid=q*(180.+air*160.);vec2 cell=floor(grid);vec2 f=fract(grid)-.5;
     float seed=crHash(cell+d*29.);float dotShape=1.-smoothstep(.08,.42,length(f));
     float grains=dotShape*smoothstep(.38-.22*energy,.95,seed);
     float field=exp(-dot(p,p)*(.55+d*.8));
     lum+=(membrane*2.0+pow(n,2.)*.5)*grains*field*(.7+energy*3.)/(1.+d*2.);
     halo+=membrane*field*.013;
   }
 }else if(mode==1){
   vec2 q=p*(1.4+depth*1.6);float n=crFbm(q*1.5+vec2(t,-t*.7));
   float wave=q.x*6.+sin(q.y*3.+t)*(.6+fold*4.)+n*(5.+bass*6.);
   float strands=pow(.5+.5*sin(wave*5.),32.+air*30.);
   float sheet=pow(max(0.,1.-abs(sin(wave+metal*2.))),10.);
   float tears=smoothstep(.23,.55,crFbm(q*3.+t*.3));
   float volume=exp(-abs(p.x)*.9-abs(p.y)*.3);
   lum=(strands*.8+sheet*.11)*tears*volume*(.3+energy*1.3);
   halo=sheet*volume*.04;
 }else{
   vec2 q=crRot(.3*sin(t)+fold)*p;
   float distortion=crFbm(q*2.+t);q.y+=sin(q.x*4.+distortion*5.)*(.06+bass*.25);
   for(int i=0;i<9;i++){
     float k=float(i),line=(k-4.)*.055;
     float slope=sin(k*1.7+phase*6.28)*(.06+depth*.2);
     float distanceToLine=abs(q.y-line-q.x*slope);
     float beam=exp(-distanceToLine*(600.-metal*350.));
     float fragments=step(.33,crNoise(vec2(floor(q.x*(17.+k*3.)),k+t*.7)));
     lum+=beam*fragments*exp(-abs(q.x)*.9)*(.1+.45*energy+pulse*.8);
   }
   halo=exp(-abs(q.y)*18.)*exp(-abs(q.x))*.035*energy;
 }
 float edge=1.-smoothstep(.5,1.25,length(uv));
 vec3 pigment=mix(vec3(.85,.9,1.),tint.rgb,scene*.75+.15);
 vec3 color=pigment*(lum+halo)*(1.+pulse*.7)*edge;
 color=1.-exp(-color*2.1);
 gl_FragColor=vec4(color,1.);
}
