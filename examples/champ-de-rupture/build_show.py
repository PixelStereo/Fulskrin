#!/usr/bin/env python3
"""Build the original Champ de rupture FM audiovisual performance."""
import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess

BASE_DURATION=180.0
# Composed score landmarks; scaled only when another recording is supplied.
MOMENTS=[
 (0,"01 · Émergence / chambre noire",[.65,0,0],[.8,.86,1],0,1.5),
 (12,"02 · Objets suspendus",[.82,.08,0],[.85,.92,1],.06,1.1),
 (28,"03 · La poussière se souvient",[.76,.28,.08],[.6,.72,1],.15,1.8),
 (44,"04 · Peaux de résonance",[.36,.84,.12],[.86,.93,1],.25,1.2),
 (60,"05 · Première chambre / vide",[.2,.22,.85],[.62,.82,.94],.35,.15),
 (76,"06 · Compression",[.78,.55,.2],[.72,.9,.83],.42,1.7),
 (91,"07 · Grêle métallique",[.86,.35,.65],[.92,.96,1],.52,.6),
 (104,"08 · Fracture / silence",[.24,.24,.95],[.9,.96,1],.62,.12),
 (116,"09 · Mémoire oblique",[.65,.62,.12],[.68,.6,.9],.70,2.2),
 (128,"10 · Masse critique",[.75,.82,.45],[.83,.9,1],.80,.8),
 (144,"11 · Le champ cède",[.9,.42,.92],[.94,1,.96],.90,.12),
 (160,"12 · Débris lents",[.55,.38,.12],[.62,.75,.89],.98,2.8),
 (172,"13 · Retour à la chambre noire",[0,0,0],[.46,.54,.7],.4,8.),
]
MACROS=[("aperture",.58,.24,47),("fold",.47,.30,61),("drift",.28,.18,83),("depth",.55,.32,107),("shear",0,.65,53),("phase",.5,.48,139)]

def build(out,audio=None,duration=BASE_DURATION,timeline_count=18):
    out=Path(out).resolve();out.mkdir(parents=True,exist_ok=True)
    nearby=Path(__file__).resolve().parent/"shaders"/"ChampRupture.fs"
    shader=nearby if nearby.exists() else Path(__file__).resolve().parents[2]/"isf"/"generators"/"ChampRupture.fs"
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
    for i,name in enumerate(["Poussière / gravitation","Peaux / interférences","Faisceaux / fractures"]):
        params={"opacity":[.7,.12,.04][i],"blend_mode":"add","param/mode":i,"param/scene":0,"position/x":640,"position/y":360}
        for macro,center,amplitude,period in MACROS:params[f"param/{macro}"]=center
        for component,value in zip("rgba",[.3,.75,1,1]):params[f"param/tint/{component}"]=value
        layer={"id":str(101+i),"name":name,"enable":True,"source":{"type":"isf","path":"shaders/ChampRupture.fs","relative_path":"shaders/ChampRupture.fs","width":1280,"height":720},"params":params,"fx":[],"viewports":{},"color_models":1}
        layers.append(layer)
        for macro,center,amplitude,period in MACROS:
            n=len(timelines)+1
            timelines.append({"id":str(n),"name":f"{name.split(' /')[0]} · {macro}","duration":period+(i*11),"loop":"loop","tracks":[{"layer":str(101+i),"param":f"param/{macro}","oscillator":{"wave":"sine","period":period+i*11,"center":center,"amplitude":amplitude,"phase":i*.23,"seed":n},"keys":[]}]})
        for source,param,lo,hi,feature in [("level","energy",20,20000,0),("sub","bass",28,120,0),("metal","metal",700,3200,0),("air","air",6000,16000,0),("onset","hit",20,20000,2)]:
            interactions.append({"id":str(len(interactions)+1),"name":f"Audio {source} → {name.split(' /')[0]}","enabled":True,"source":"audio","audio_low":lo,"audio_high":hi,"audio_feature":feature,"layer":str(101+i),"param":f"param/{param}","source_range":[0,.55 if source!="onset" else 1],"target_range":[0,1],"master":1,"filter_on":True,"filter":"attack_release","filter_settings":[.008 if source=="onset" else .025,.09 if source=="onset" else .24],"takeover":"jump"})
    if timeline_count==1:
        timelines=[{"id":"1","name":"Champ de rupture · 18 mouvements","duration":161,"loop":"loop","tracks":[track for timeline in timelines for track in timeline["tracks"]]}]
    audio_layer={"id":"104","name":"Champ de rupture · bande sonore","enable":True,"source":{"type":"audio","path":audio_path,"relative_path":audio_path} if audio_path else {"type":"none"},"params":{"play":False,"play_mode":"oneshot","volume":1,"mute":False},"fx":[],"viewports":{}}
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
    project={"app":"Pinol","format_version":1,"composition":{"width":1280,"height":720,"fps":60,"speed":1},"audio":{"volume":1},"layers":layers,"interactions":interactions,"timelines":timelines,"snapshots":snapshots,"snapshot_tree":[snap["id"] for snap in snapshots],"sequences":[{"name":"Champ de rupture · FM / 3 minutes","loop":False,"steps":steps}],"current_sequence":0,"startup_sequence":-1,"ui":{"selected_layer":0}}
    (out/"Champ_de_rupture.pinol").write_text(json.dumps(project,ensure_ascii=False,indent=2)+"\n")
    (out/"cuebook.json").write_text(json.dumps({"audio":audio_path,"duration":duration,"cues":cuebook},ensure_ascii=False,indent=2)+"\n")
    return project, cuebook

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--audio",type=Path);parser.add_argument("--out",type=Path,required=True);parser.add_argument("--duration",type=float,default=BASE_DURATION);parser.add_argument("--timelines",type=int,choices=[1,18],default=18)
    args=parser.parse_args();project,cues=build(args.out,args.audio,args.duration,args.timelines)
    print(f"Show ready: {args.out.resolve()/'Champ_de_rupture.pinol'} ({len(project['timelines'])} timelines, {len(cues)} cues)")
