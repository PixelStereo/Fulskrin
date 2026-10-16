#!/usr/bin/env python3
"""Original deterministic FM/electroacoustic score. NumPy + ffmpeg; no samples or downloads."""
from pathlib import Path
import argparse,json,subprocess,wave
import numpy as np
RATE=48000
DURATION=180
SEED=274601

def compose(out):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    rng=np.random.default_rng(SEED)
    mix=np.zeros((RATE*DURATION,2),np.float64)
    events=[]
    def place(at,sound,pan=0,gain=1,kind='FM'):
        start=int(at*RATE);count=min(len(sound),len(mix)-start)
        if start<0 or count<=0:return
        a=np.asarray(sound[:count])*gain
        mix[start:start+count,0]+=a*np.cos((pan+1)*np.pi/4)
        mix[start:start+count,1]+=a*np.sin((pan+1)*np.pi/4)
        events.append({'at':round(at,4),'kind':kind,'duration':round(count/RATE,3)})
    def fm(length,carrier,ratio,index,decay,attack=.003,glide=0,roughness=0):
        t=np.arange(int(length*RATE))/RATE
        env=(1-np.exp(-t/max(.0005,attack)))*np.exp(-t/decay)
        env*=np.minimum(1,(length-t)/.05)
        # A decaying index follows the strike; nested FM leaves a complex
        # inharmonic tail, with slow frequency drift in the carrier.
        mod=np.sin(2*np.pi*carrier*ratio*t+roughness*np.sin(2*np.pi*carrier*.371*t))
        signal=np.sin(2*np.pi*carrier*(t+glide*t*t/(2*max(length,.1)))+index*np.exp(-t/(decay*.75))*mod)
        return signal*env
    # Overture: isolated glass flecks, then dense grains around three ruptures.
    sections=[(0,12,.45),(12,28,1.8),(28,44,3.5),(44,60,6.),(60,76,1.2),(76,91,4.5),(91,104,9.),(104,116,.9),(116,128,5.),(128,144,11.),(144,160,3.),(160,172,7.),(172,180,.8)]
    for start,end,density in sections:
        t=start+.3
        while t<end-1:
            # Non-periodic spacing: stochastic texture rather than a beat loop.
            t+=rng.exponential(1/density)
            if t>=end-1:break
            dur=float(rng.uniform(.05,1.9));fc=float(rng.choice([73.4,109.3,181.7,267.9,421.1,637.3,977.7,1451.]))
            ratio=float(rng.choice([1.41421356,2.71828,3.14159,5.173,7.07]))
            tone=fm(dur,fc,ratio,float(rng.uniform(.7,10)),dur*.22,roughness=1.5)
            place(t,tone,float(rng.uniform(-.95,.95)),float(rng.uniform(.025,.10)),kind='FM shards')
    # Slow suspended sheets: detuned four-operator FM clusters.
    for at,length,base,level in [(10,24,46,.09),(29,26,69,.11),(62,21,38,.12),(78,26,55,.13),(108,18,82,.07),(118,28,41,.15),(146,25,62,.11),(166,14,35,.09)]:
        t=np.arange(int(length*RATE))/RATE
        envelope=np.sin(np.pi*t/length)**2
        for voice in range(3):
            f=base*(1+voice*.0061)
            index=(2.5+2*np.sin(2*np.pi*t/(length*.83)))
            nested=np.sin(2*np.pi*f*1.414*t+.9*np.sin(2*np.pi*f*.371*t))
            signal=np.sin(2*np.pi*f*t+index*nested)*envelope
            place(at,signal,[-.65,0,.65][voice],level/3,'FM pressure sheet')
    # Metallic resonant bodies: independent inharmonic partials plus FM bite.
    for at in [12,27.6,43.3,59.7,76,90.8,96.1,103.6,116,127.6,132.9,139.7,159.8,169.4]:
        length=8.;t=np.arange(int(length*RATE))/RATE;metal=np.zeros_like(t)
        base=float(rng.choice([88.2,113.7,161.3]))
        for k,ratio in enumerate([1,1.483,2.097,2.712,4.371,6.913,9.127]):
            metal+=np.sin(2*np.pi*base*ratio*t+rng.uniform(0,6.28))*np.exp(-t/(4.2/(1+k*.6)))/(1+k)
        metal*=np.minimum(t/.002,1)*np.minimum((length-t)/.1,1)
        metal+=fm(length,base*.5,2.413,8,1.1)*.6
        place(at,metal,float(rng.uniform(-.5,.5)),.12,'Resonant metal / FM')
    # Concrete-like friction synthesized from a deterministic noise exciter.
    for at,length,level in [(32,11,.05),(46,13,.06),(82,8,.07),(92,10,.08),(120,7,.07),(129,14,.1),(161,10,.06)]:
        t=np.arange(int(length*RATE))/RATE
        noise=rng.normal(0,1,len(t))
        # Differentiated exciter emphasizes scrape and air without external samples.
        scrape=(noise-np.roll(noise,1))*.35
        envelope=np.sin(np.pi*t/length)**2*(.25+.75*np.sin(2*np.pi*(3.4*t+.07*t*t))**8)
        place(at,scrape*envelope,-.65,level,'Friction exciter')
        place(at+.037,scrape[::-1]*envelope,.65,level*.85,'Reversed friction')
    # Three crescendos contract to a deep inharmonic strike and a brief vacuum.
    for hit,start in [(60,55.5),(104,99),(144,137)]:
        length=hit-start;t=np.arange(int(length*RATE))/RATE
        noise=rng.normal(0,1,len(t));env=(t/length)**3
        climb=np.sin(2*np.pi*(39*t+22*t*t)+9*np.sin(2*np.pi*71.3*t))
        place(start,(climb*.8+noise*.08)*env,0,.18,'FM ascent')
        place(hit,fm(9,36.7,1.618,11,1.6,glide=-.18,roughness=3),0,.36,'FM fracture')
        # 160 ms acoustic void after the strike transient, then its tail returns.
        a=int((hit+.07)*RATE);b=int((hit+.23)*RATE)
        mix[a:b]*=.035
    # Stereo multi-tap diffusion. Feed-forward, fixed time: no evolving runaway feedback.
    dry=mix.copy()
    for delay,gain in [(.173,.17),(.311,.12),(.529,.075),(.887,.045),(1.339,.025)]:
        n=int(delay*RATE);mix[n:,0]+=dry[:-n,1]*gain;mix[n:,1]+=dry[:-n,0]*gain
    # Final breath and silence, no abrupt endpoint. DC removed; global normalization
    # keeps dynamics between sections rather than normalizing each event.
    mix-=mix.mean(axis=0)
    t=np.arange(len(mix))/RATE
    mix*=np.minimum(t/1.5,1)[:,None]*np.clip((DURATION-t)/8,0,1)[:,None]
    mix=np.tanh(mix*1.35)
    mix*=.89/max(float(np.max(np.abs(mix))),1e-9)
    wav=out/'Champ_de_rupture.wav'
    with wave.open(str(wav),'wb') as f:
        f.setnchannels(2);f.setsampwidth(2);f.setframerate(RATE);f.writeframes((mix*32767).astype('<i2').tobytes())
    mp3=out/'Champ_de_rupture.mp3'
    subprocess.run(['ffmpeg','-v','error','-y','-i',str(wav),'-codec:a','libmp3lame','-b:a','256k',str(mp3)],check=True)
    (out/'Composition.json').write_text(json.dumps({'title':'Champ de rupture','duration':DURATION,'seed':SEED,'rate':RATE,'original':True,'external_samples':False,'events':sorted(events,key=lambda e:e['at'])},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(mp3)
    return mp3
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);compose(p.parse_args().out)
