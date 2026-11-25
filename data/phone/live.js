'use strict';
// Graph previews are independent of the ESP32's full-rate raw capture.
const LIVE_PAINT_INTERVAL=1000/15;
let livePageActive=true,livePaintHandle=0,lastLivePaint=-Infinity,signalTransport=null,signalRetryTimer=0,signalHooksInstalled=false;
let lastSignalSeq=null,lastDeviceTs=null,deviceTimeline=0,deviceOffset=null;
const sensorOnline=[false,false,false,false];

function sensorIndicator(id,online){if(sensorOnline[id]===online)return;sensorOnline[id]=online;$('s'+id).classList.toggle('online',online);}
function clearSensorIndicators(){for(let i=0;i<4;i++)sensorIndicator(i,false);}
function liveChartVisible(){return livePageActive&&!document.hidden&&$('live-chart').closest('details').open;}
function cancelLivePaint(){if(livePaintHandle)cancelAnimationFrame(livePaintHandle);livePaintHandle=0;}
function scheduleLivePaint(){if(!livePaintHandle&&liveChartVisible())livePaintHandle=requestAnimationFrame(paintLiveChart);}
function paintLiveChart(time){
  livePaintHandle=0;if(!liveChartVisible())return;
  if(time-lastLivePaint<LIVE_PAINT_INTERVAL){scheduleLivePaint();return;}
  lastLivePaint=time;draw($('live-chart'),signals,Number($('live-sensor').value),true);
}
function resetSignalClock(){lastSignalSeq=null;lastDeviceTs=null;deviceTimeline=0;deviceOffset=null;signals=[];}
function handleSensors(d){
  if(d.t!=='sensors'||!Array.isArray(d.s))return;
  const now=Date.now(),seq=Number.isFinite(d.seq)?d.seq>>>0:null;
  if(seq!==null&&lastSignalSeq===seq){
    for(const s of d.s)if(s.id>=0&&s.id<4)sensorIndicator(s.id,s.st==='OK'&&!s.sat&&now-rawLatest<1500);
    return; // status transitions still apply; repeated frames never extend history
  }
  let timestamp=now,gap=false;
  if(Number.isFinite(d.ts)){
    const ts=d.ts>>>0;
    if(lastDeviceTs===null)deviceTimeline=ts;
    else{const delta=(ts-lastDeviceTs)>>>0;if(delta>0x7fffffff){resetSignalClock();deviceTimeline=ts;}else{deviceTimeline+=delta;gap=delta>180;}}
    lastDeviceTs=ts;
    const candidate=now-deviceTimeline;
    if(deviceOffset===null)deviceOffset=candidate;
    else if(candidate<deviceOffset){const adjustment=candidate-deviceOffset;for(const r of signals)r.elapsedMs+=adjustment;deviceOffset=candidate;}
    timestamp=deviceTimeline+deviceOffset;
  }
  lastSignalSeq=seq;
  const fresh=now-timestamp<=400;
  for(const s of d.s)if(s.id>=0&&s.id<4)sensorIndicator(s.id,fresh&&s.st==='OK'&&!s.sat);
  if(!fresh)return; // discard delayed bursts instead of replaying them as current
  rawLatest=timestamp;
  const axes=Array(12).fill(null);
  for(const s of d.s)if(s.id>=0&&s.id<4&&s.st==='OK'&&!s.sat){for(const [a,key] of ['rx','ry','rz'].entries())axes[s.id*3+a]=Number.isFinite(s[key])?s[key]:null;}
  if(gap)signals.push({elapsedMs:timestamp-1,axes:Array(12).fill(null)});
  signals.push({elapsedMs:timestamp,axes});
  while(signals.length&&(signals[0].elapsedMs<now-3000||signals.length>180))signals.shift();
  scheduleLivePaint();
}
function queueSignalReconnect(){if(signalRetryTimer||document.hidden||!livePageActive)return;signalRetryTimer=setTimeout(()=>{signalRetryTimer=0;openSignals();},1500);}
function stopSignals(){
  if(signalRetryTimer)clearTimeout(signalRetryTimer);signalRetryTimer=0;cancelLivePaint();
  const previous=signalTransport;signalTransport=null;
  if(previous)previous.close();
  rawLatest=0;clearSensorIndicators();resetSignalClock();
}
function setLivePageVisible(visible){livePageActive=visible;if(visible)openSignals();else stopSignals();}
function installSignalHooks(){
  if(signalHooksInstalled)return;signalHooksInstalled=true;
  const details=$('live-chart').closest('details');
  details.addEventListener('toggle',()=>{if(details.open){lastLivePaint=-Infinity;scheduleLivePaint();}else cancelLivePaint();});
  $('live-sensor').onchange=()=>{lastLivePaint=-Infinity;scheduleLivePaint();};
  document.addEventListener('visibilitychange',()=>{if(document.hidden)stopSignals();else openSignals();});
  globalThis.addEventListener?.('pagehide',stopSignals);
  globalThis.addEventListener?.('pageshow',openSignals);
}
function openSignals(){
  installSignalHooks();if(signalTransport||document.hidden||!livePageActive)return;
  resetSignalClock();scheduleLivePaint();
  const loopback=location.hostname==='127.0.0.1'||location.hostname==='localhost';
  const transport=loopback?new EventSource('/usb/events'):new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.hostname+':81/ws');
  signalTransport=transport;
  transport.onmessage=e=>{if(signalTransport!==transport)return;try{handleSensors(JSON.parse(e.data));}catch{}};
  const disconnected=()=>{if(signalTransport!==transport)return;signalTransport=null;transport.close();rawLatest=0;clearSensorIndicators();signals=[];scheduleLivePaint();queueSignalReconnect();};
  transport.onerror=disconnected;transport.onclose=disconnected;
}
function draw(canvas,frames,sensor,live=false){
  const ratio=Math.min(devicePixelRatio||1,2),w=canvas.clientWidth||300,h=canvas.clientHeight||180;
  const pixelWidth=Math.round(w*ratio),pixelHeight=Math.round(h*ratio);
  if(canvas.width!==pixelWidth)canvas.width=pixelWidth;
  if(canvas.height!==pixelHeight)canvas.height=pixelHeight;
  const c=canvas.getContext('2d');c.setTransform(ratio,0,0,ratio,0,0);c.clearRect(0,0,w,h);
  if(!frames.length)return;
  let low=Infinity,high=-Infinity;
  for(const r of frames)for(let a=0;a<3;a++){const v=r.axes[sensor*3+a];if(Number.isFinite(v)){low=Math.min(low,v);high=Math.max(high,v);}}
  if(!Number.isFinite(low))return;
  if(high-low<1){high+=.5;low-=.5;}
  const begin=live?Date.now()-3000:0,end=live?Date.now():1000,colors=['#176b48','#2563a6','#b96524'];
  c.font='12px system-ui';c.fillStyle='#586b60';c.fillText(high.toFixed(0)+' µT',0,12);c.fillText(low.toFixed(0)+' µT',0,h-20);
  for(let a=0;a<3;a++){
    c.strokeStyle=colors[a];c.lineWidth=1.5;c.beginPath();let started=false;
    for(const r of frames){const v=r.axes[sensor*3+a];if(!Number.isFinite(v)){started=false;continue;}const x=42+(w-48)*(r.elapsedMs-begin)/(end-begin),y=15+(h-45)*(high-v)/(high-low);if(started)c.lineTo(x,y);else c.moveTo(x,y);started=true;}
    c.stroke();c.fillStyle=colors[a];c.fillText(['Bx','By','Bz'][a],48+a*55,h-3);
  }
}
