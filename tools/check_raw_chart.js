// Browser regression for a signed axis being mislabeled as |B| after navigation.
async () => {
  const assert=(ok,message)=>{if(!ok)throw Error(message);};
  const original=location.hash;
  const originalFrame=onFrame,originalSensors=S.sensors,originalAge=S.lastSensorsAt;
  const go=async name=>{location.hash='#/'+name;await new Promise(r=>setTimeout(r,60));};
  try {
    // Keep the real 50 Hz stream from replacing the controlled negative-axis sample.
    onFrame=m=>{if(m?.t!=='sensors')originalFrame(m);};
    await go('sensors');
    originalFrame({t:'sensors',seq:9999,s:[{id:0,st:'OK',sat:false,rx:3,ry:-12,rz:4,rm:13,x:30,y:-120,z:40,m:130}]});
    $('#ch-metric').value='y';$('#ch-metric').dispatchEvent(new Event('change'));
    $('#ch-only').value='2';$('#ch-only').dispatchEvent(new Event('change'));
    await go('measure');await go('sensors');
    assert($('#ch-metric').value===Views.sensors.metric,'Visible metric must match plotted data after navigation');
    assert($('#ch-only').value===String(Views.sensors.only),'Visible sensor must match chart visibility');
    assert(Views.sensors.chart.series[0].buf===hist[0].y,'By plot must retain signed raw data');
    assert(hist[0].y.last()===-12,'Signed raw axis must not be replaced by absolute values');
    $('#ch-metric').value='m';$('#ch-metric').dispatchEvent(new Event('change'));Views.sensors.upd(S.sensors);
    assert(Views.sensors.chart.series[0].buf===hist[0].m && hist[0].m.last()===13,'|B| must use non-negative raw vector magnitude');
    assert($('#lg-v-0').textContent===fmtUt(13),'Magnitude legend must match raw plot, without filtered-value lag');
    return 'Metric/sensor state survives navigation; signed axes and non-negative raw magnitude stay distinct';
  } finally {onFrame=originalFrame;S.sensors=originalSensors;S.lastSensorsAt=originalAge;location.hash=original;}
}
