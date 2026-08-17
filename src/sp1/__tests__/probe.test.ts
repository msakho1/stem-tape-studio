import { describe, it } from "vitest";
import { MockSp1 } from "/dev-server/src/sp1/__tests__/mockSerial";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "/dev-server/src/sp1/protocol";
import { parseCapabilities } from "/dev-server/src/sp1/compatibility";
import { StemTapeTransport } from "/dev-server/src/sp1/transport";
import { prepareCanonicalSong } from "/dev-server/src/sp1/song";
const NAMES=["vocal","drums","bass","instrument"] as const;
function tone(f:number,s:number){const l=new Float32Array(f),r=new Float32Array(f);for(let i=0;i<f;i++){l[i]=Math.sin(i*s/50)*.4;r[i]=Math.cos(i*s/70)*.4;}return {sampleRate:48000,numberOfChannels:2,length:f,duration:f/48000,getChannelData:(c:number)=>c===0?l:r} as unknown as AudioBuffer;}
const song=(t:string,seed:number)=>prepareCanonicalSong(NAMES.map((n,i)=>({name:n,filename:n+".wav",buffer:tone(2040,i+seed)})),{metadata:{title:t,artist:"T",bpm:120,downbeatSeconds:0}});
async function attach(m:MockSp1){const io=new Sp1Transport(m.port() as SerialLikePort);const s=new Sp1Session(io);await s.handshake();return new StemTapeTransport(s,parseCapabilities((await s.queryCapabilities())!),{kind:"mock"});}
describe("probe",()=>{it("x",async()=>{
 for (const n of [96,97,98,99]) {
  const mock=new MockSp1({stemTape:true,sectorsPerSong:16});const t=await attach(mock);await t.initialiseLibrary();
  const r1=await t.uploadSong({song:await song("ONE",3)});
  const base=mock.writes;
  mock.opts.onWrite=({n:w})=>(w>base+n?{disconnect:true}:undefined);
  const r2=await t.uploadSong({song:await song("TWO",11)});
  const lib=mock.activeLibrary();
  console.log(n, "ok",r2.ok,"outcome",r2.outcome,"writes",mock.writes-base,"gen",lib.generation,"title",lib.active?.title,"reqinit",lib.requiresInitialization, "g1",r1.generation);
 }
},120000)});
