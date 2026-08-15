import { describe, it } from "vitest";
import { AudioEngine } from "@/audio/engine";
import { MockCtx, makeBuffer, SR } from "@/audio/__tests__/mockAudio";
import { midiClock } from "@/audio/midi/clock";
let id=0; const cmd=(type:string,payload:any={})=>({id:++id,t:0,type,payload} as never);
describe("dbg",()=>{ it("x",async()=>{
 const ctx=new MockCtx(); (globalThis as any).window={AudioContext:function(){return ctx;}};
 const e=new AudioEngine(); await e.unlock();
 for(let i=0;i<4;i++) e.adoptBuffer(i as any, makeBuffer(1,SR*16,SR,0.25),{name:`s${i}`,provenance:"bundled-demo",contentHash:`h${i}`} as any);
 e.execute(cmd("transport.play")); ctx.currentTime+=1;
 console.log("elig",e.cueEligibility(),"phase",(e as any).transportPhase);
 const cal=midiClock.calibration();
 const ev=(kind:any,note:number)=>({kind,note,velocity:kind==="noteOn"?100:0,channel:0,timestampMs:cal.perfNowMs0+(ctx.currentTime-cal.ctxTime0)*1000,source:"test",deviceId:"d",deviceName:"d"} as any);
 console.log("on",e.handleMidiCue(ev("noteOn",60),{functionHeld:true,tracksHeld:[]}));
 ctx.currentTime+=1;
 console.log("off",e.handleMidiCue(ev("noteOff",60),{functionHeld:true,tracksHeld:[]}));
});});
