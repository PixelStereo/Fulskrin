#!/usr/bin/env python3
"""Build a self-contained PINOL show. User audio is copied locally, never fetched or uploaded."""
import argparse
import copy
import json
import math
from pathlib import Path
import shutil
import subprocess

BASE_DURATION=116.088125
# Structural changes measured from Calculated_Rupture; scaled only when another recording is supplied.
MOMENTS=[
    (0,"01 · Poussière conductrice",[.70,.12,.04],[.85,.9,1],0,.8),
    (16,"02 · La masse prend corps",[.85,.22,.18],[.24,.65,1],.12,1.5),
    (32,"03 · Fissures et membranes",[.65,.66,.08],[.65,.36,1],.28,.7),
    (48,"04 · Friction blanche",[.65,.38,.35],[.8,.94,1],.42,1.8),
    (64,"05 · Courants profonds",[.22,.28,.92],[.05,.58,1],.58,3.0),
    (80,"06 · Orage intérieur",[.42,.85,.32],[1,.28,.06],.72,1.2),
    (85.8,"07 · Rupture / noyau blanc",[.94,.9,.18],[.88,1,.93],.82,.22),
    (88,"08 · Retombée magnétique",[.66,.35,.16],[.5,.24,.9],.88,.8),
    (98.3,"09 · Seconde faille",[.52,.86,.55],[.18,.82,1],.94,.35),
    (102.8,"10 · Décharge finale",[.96,.82,.64],[.8,.95,1],1,.28),
    (108,"11 · Cendres / extinction",[0,0,0],[.28,.4,.58],.3,8.0),
]
MACROS=[("density",.57,.2,37),("turbulence",.48,.24,53),("drift",.35,.16,71),("scale",1.05,.28,97),("bend",0,.6,43),("phase",.5,.48,127)]

def build(out,audio=None,duration=BASE_DURATION,timeline_count=18):
    out=Path(out).resolve();out.mkdir(parents=True,exist_ok=True)
    nearby=Path(__file__).resolve().parent/"shaders"/"MatterInstable.fs"
    shader=nearby if nearby.exists() else Path(__file__).resolve().parents[2]/"isf"/"generators"/"MatterInstable.fs"
    (out/"shaders").mkdir(exist_ok=True);destination=out/"shaders"/shader.name
    if shader.resolve()!=destination.resolve():shutil.copy2(shader,destination)
    if audio:
        audio=Path(audio).resolve()
        if not audio.is_file():raise ValueError(f"Audio file not found: {audio}")
        duration=float(subprocess.check_output(["ffprobe","-v","error","-show_entries","format=duration","-of","default=noprint_wrappers=1:nokey=1",str(audio)],text=True))
        (out/"audio").mkdir(exist_ok=True);target=out/"audio"/audio.name
        if audio!=target.resolve():shutil.copy2(audio,target)
        audio_path=f"audio/{audio.name}"
    else:audio_path=None
    ratio=duration/BASE_DURATION;layers=[];interactions=[];timelines=[]
    for i,name in enumerate(["Poussière / membranes","Filaments / noyaux","Courants / strates"]):
        params={"opacity":[.7,.12,.04][i],"blend_mode":"add","param/mode":i,"param/scene":0,"position/x":640,"position/y":360}
        for macro,center,amplitude,period in MACROS:params[f"param/{macro}"]=center
        for component,value in zip("rgba",[.3,.75,1,1]):params[f"param/tint/{component}"]=value
        layer={"id":str(101+i),"name":name,"enable":True,"source":{"type":"isf","path":"shaders/MatterInstable.fs","relative_path":"shaders/MatterInstable.fs","width":1280,"height":720},"params":params,"fx":[],"viewports":{},"color_models":1}
        layers.append(layer)
        for macro,center,amplitude,period in MACROS:
            n=len(timelines)+1
            timelines.append({"id":str(n),"name":f"{name.split(' /')[0]} · {macro}","duration":period+(i*11),"loop":"loop","tracks":[{"layer":str(101+i),"param":f"param/{macro}","oscillator":{"wave":"sine","period":period+i*11,"center":center,"amplitude":amplitude,"phase":i*.23,"seed":n},"keys":[]}]})
        for source,param in [("level","energy"),("bass","bass"),("high","brilliance"),("onset","attack")]:
            interactions.append({"id":str(len(interactions)+1),"name":f"Audio {source} → {name.split(' /')[0]}","enabled":True,"source":"internal","source_layer":"0","source_param":f"audio/{source}","layer":str(101+i),"param":f"param/{param}","source_range":[0,1],"target_range":[0,1],"master":1,"filter_on":False,"takeover":"jump"})
    if timeline_count==1:
        timelines=[{"id":"1","name":"Matières instables · 18 mouvements","duration":127,"loop":"loop","tracks":[track for timeline in timelines for track in timeline["tracks"]]}]
    audio_layer={"id":"104","name":"Calculated Rupture · bande sonore","enable":True,"source":{"type":"audio","path":audio_path,"relative_path":audio_path} if audio_path else {"type":"none"},"params":{"play":False,"play_mode":"oneshot","volume":1,"mute":False},"fx":[],"viewports":{}}
    layers.append(audio_layer)
    steps=[{"timeline":timeline["id"],"action":"play","continue":"follow","text":"Armement des mouvements"} for timeline in timelines]
    snapshots=[];cuebook=[]
    for index,(time,name,opacities,tint,scene,fade) in enumerate(MOMENTS):
        states=copy.deepcopy(layers)
        for i in range(3):
            states[i]["included"]=True
            # Motion and audio own separate parameters; cues change only the art direction.
            states[i]["params"]={"opacity":opacities[i],"blend_mode":"add","param/mode":i,"param/scene":scene,**{f"param/tint/{component}":value for component,value in zip("rgba",tint+[1])}}
        states[3]["included"]=index==0
        if index==0:states[3]["params"].update({"restart":True,"play":True})
        span=((MOMENTS[index+1][0] if index+1<len(MOMENTS) else BASE_DURATION)-time)*ratio
        fade=min(fade*ratio,span)
        snapshot={"id":str(index+1),"name":name,"fade":fade,"layers":states,"composition":{"included":True,"opacity":1,"volume":1},"interactions":[{"id":it["id"],"included":False} for it in interactions]}
        snapshots.append(snapshot)
        steps.append({"snapshot":str(index+1),"continue":"auto_follow","post_wait":max(0,span-fade),"text":name,"description":f"{time*ratio:.2f} s · {span:.2f} s"})
        cuebook.append({"at":round(time*ratio,3),"duration":round(span,3),"name":name,"fade":round(fade,3)})
    for timeline in timelines:steps.append({"timeline":timeline["id"],"action":"stop","continue":"follow","text":"Fin des mouvements"})
    # Fade the picture and sound together across the recording's final tail.
    snapshots[-1]["composition"]["volume"]=0
    project={"app":"Pinol","format_version":1,"composition":{"width":1280,"height":720,"fps":60,"speed":1},"audio":{"volume":1},"layers":layers,"interactions":interactions,"timelines":timelines,"snapshots":snapshots,"snapshot_tree":[snap["id"] for snap in snapshots],"sequences":[{"name":"Calculated Rupture · Matières instables","loop":False,"steps":steps}],"current_sequence":0,"startup_sequence":-1,"ui":{"selected_layer":0}}
    (out/"Matieres_instables.pinol").write_text(json.dumps(project,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    (out/"cuebook.json").write_text(json.dumps({"audio":audio_path,"duration":duration,"cues":cuebook},ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return project, cuebook

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--audio",type=Path);parser.add_argument("--out",type=Path,required=True);parser.add_argument("--duration",type=float,default=BASE_DURATION);parser.add_argument("--timelines",type=int,choices=[1,18],default=18)
    args=parser.parse_args();project,cues=build(args.out,args.audio,args.duration,args.timelines)
    print(f"Show ready: {args.out.resolve()/'Matieres_instables.pinol'} ({len(project['timelines'])} timelines, {len(cues)} cues)")
