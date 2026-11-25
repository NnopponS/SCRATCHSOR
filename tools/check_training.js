// Browser-only batch workflow check. Device/storage requests are replaced with fixtures.
async () => {
  const assert=(ok,message)=>{if(!ok)throw Error(message);};
  const view=Views.measure;
  const original={api,rawApi,record:S.record,hash:location.hash,data:view.data,sessionId:view.sessionId,distanceId:view.distanceId,sensor:view.sensor,trial:view._trial,lastSensorsAt:S.lastSensorsAt,remembered:localStorage.getItem('scratchsor-session')};
  const session={id:'browser-fixture',name:'Browser check',created:'2026-11-11T10:00:00Z',distances:[]};
  let capture=null,lastRequest=null,nextId=1;
  const snapshot=(extra={})=>JSON.parse(JSON.stringify({sessions:[{id:session.id,name:session.name,created:session.created}],session,capture,...extra}));
  const pause=()=>new Promise(r=>setTimeout(r,35));
  const complete=()=>{
    const group=session.distances.find(d=>d.id===capture.distance_id);
    const number=group.nextTrial++;
    group.trials.push({id:nextId++,number,name:'',frames:149,healthy:596,hz:50,included:1,saved:'2026-11-11T10:00:00Z'});
    capture=null;view.apply(snapshot());
  };
  try {
    rawApi=async(path,method='GET',body)=>{
      if(path.startsWith('/sessions')) {
        if(method==='GET' && /\/trials\/\d+$/.test(path)) {
          const id=Number(path.split('/').at(-1)),group=session.distances.find(d=>d.trials.some(t=>t.id===id)),trial=group.trials.find(t=>t.id===id);
          return {trial:{...trial,distance_id:group.id,distanceCm:group.distanceCm,distanceLabel:group.label},frames:Array.from({length:trial.frames},(_,i)=>({seq:i,elapsedMs:i*20,s:Array.from({length:4},(_,id)=>({id,rx:3,ry:-12,rz:4,rm:13,st:'OK',sat:false,valid:true}))}))};
        }
        if(method==='POST') {
          lastRequest={path,body};
          if(path.endsWith('/record'))return api(path,{body});
          if(path.endsWith('/distances')) {
            const cm=Number(body.distanceCm);let group=session.distances.find(d=>d.distanceCm===cm);
            if(!group){group={id:session.distances.length+1,distanceCm:cm,label:cm.toFixed(1),nextTrial:1,trials:[]};session.distances.push(group);}
            return snapshot({selectedDistanceId:group.id});
          }
          if(/\/trials\/\d+$/.test(path)) {
            const id=Number(path.split('/').at(-1));session.distances.flatMap(d=>d.trials).find(t=>t.id===id).included=body.included?1:0;
          }
          if(path.endsWith('/rename') || path.endsWith('/remove')) {
            const parts=path.split('/'),kind=parts.at(-3),id=Number(parts.at(-2)),remove=parts.at(-1)==='remove';
            if(kind==='trials') {
              const group=session.distances.find(d=>d.trials.some(t=>t.id===id));
              if(remove)group.trials=group.trials.filter(t=>t.id!==id);else group.trials.find(t=>t.id===id).name=body.name;
            }else if(kind==='distances') {
              if(remove)session.distances=session.distances.filter(d=>d.id!==id);
              else{const group=session.distances.find(d=>d.id===id);group.distanceCm=Number(body.distanceCm);group.label=group.distanceCm.toFixed(1);}
            }else if(remove){session.id='browser-fixture-new';session.name='New session';session.distances=[];}
            else session.name=body.name;
          }
          if(path.endsWith('/retry'))capture={...capture,state:'saving'};
        }
        return snapshot();
      }
      return {};
    };
    api=async(path,options)=>{
      lastRequest={path,body:options.body};
      assert(path.endsWith('/record'),'Only a recording should require the device API');
      capture={session_id:session.id,distance_id:options.body.distanceId,state:'recording',started_ms:100,remainingMs:3000,frames:0};
      return snapshot();
    };
    location.hash='#/measure';await pause();
    S.record={active:false};view.sessionId=session.id;view.distanceId=null;view.apply(snapshot());
    assert(view.chart?.cv.isConnected,'Realtime graph must stay mounted');
    assert($('#me-rec').disabled && !$('#me-export').hasAttribute('href'),'An empty session must not record/export');
    assert(!Views.calibration && !Views.export && !$('#me-save'),'Unused calibration, profile and separate export views must be removed');
    lastRequest=null;$('#me-ref').value='51';$('#me-add').click();await pause();
    assert(!lastRequest,'Invalid distance must not be submitted');
    for(const value of ['2.0','2.00']) {
      $('#me-ref').value=value;$('#me-add').click();await pause();
    }
    assert(session.distances.length===1 && !$('#me-rec').disabled,'Repeated labels must select one distance section');
    for(let n=1;n<=2;n++) {
      assert($('#me-rec').textContent.includes('Trial '+n),'Next trial number must be visible before recording');
      $('#me-rec').click();await pause();
      assert(lastRequest.body.distanceId===1 && $('#me-rec').disabled && $('#me-ref').disabled,'Capture must bind to the selected section and lock controls');
      assert([...document.querySelectorAll('[data-distance]')].every(e=>e.disabled),'Distance selection must stay locked during capture/save');
      assert(!$('#me-export').hasAttribute('href'),'Export must wait for the current save');
      capture.state='saving';capture.detail='Saving to PC';view.apply(snapshot());
      assert($('#me-rec').textContent.includes('Saving'),'Saving must be distinct from recording');
      complete();
    }
    assert(session.distances[0].trials.map(t=>t.number).join(',')==='1,2','Repeated captures must append trials');
    $('#me-ref').value='2.1';$('#me-add').click();await pause();
    assert(view.distanceId===2 && $('#me-rec').textContent.includes('Trial 1'),'A new manual distance starts at Trial 1');
    $('#me-rec').click();await pause();assert(lastRequest.body.distanceId===2,'New capture must use the new distance');complete();
    assert($('#me-export').getAttribute('href').endsWith('/export.zip'),'Export all must produce a ZIP');
    assert(document.querySelectorAll('#me-distances a[href$=".csv"]').length===2,'Each distance needs its own CSV');
    const include=$('#me-distances [data-trial="1"]');include.checked=false;include.dispatchEvent(new Event('change',{bubbles:true}));await pause();
    assert(session.distances[0].trials[0].included===0 && session.distances[0].trials[1].number===2,'Exclusion must keep saved data and trial numbering');
    const radio=$('#me-distances [data-distance][value="1"]');radio.checked=true;radio.dispatchEvent(new Event('change',{bubbles:true}));
    assert($('#me-rec').textContent.includes('Trial 3'),'Returning to a distance continues its numbering');
    capture={session_id:session.id,distance_id:1,state:'error',detail:'Download interrupted'};view.apply(snapshot());
    assert($('#me-rec').disabled && !$('#me-recovery').hidden,'A failed save must block overwriting and offer recovery');
    $('#me-retry').click();await pause();
    assert(lastRequest.path.endsWith('/retry'),'Retry save must not repeat the recording command');
    complete();await view.load();
    assert(view.data.session.distances[0].trials.length===3,'Reloading the session must keep every saved trial');
    $('#me-sensor').value='2';$('#me-sensor').dispatchEvent(new Event('change'));
    assert(view.chart.series[0].buf===hist[2].x,'Graph sensor control must keep working during a batch');
    const trial=session.distances[0].trials.at(-1);
    $('[data-view-trial="'+trial.id+'"]').click();await pause();
    assert(view._trial.trial.id===trial.id && view.chart.windowSec===3 && view.chart.times[1]===20,'Clicking a trial must use saved elapsed timestamps on a 0–3 second graph');
    assert(view.chart.series[1].buf.last()===-12 && view.chart.series[3].buf.last()===13,'Saved graphs must keep signed axes and non-negative magnitude');
    S.lastSensorsAt=Date.now()-5000;view.upd();
    assert(view.chart.series.every(s=>s.visible),'Saved graphs remain viewable when live data is stale');
    $('#me-trial-rename').click();await pause();
    $('#batch-edit-value').value='capture_'+'x'.repeat(70);$('.modal-actions [data-primary]').click();await pause();
    assert(view._trial.trial.name.startsWith('capture_') && $('#me-trial-name').textContent===view._trial.trial.name,'Trial rename must update the graph and saved label');
    $('#me-trial-remove').click();await pause();$('.modal-actions [data-primary]').click();await pause();
    assert(!view._trial && view.chart.times===null && $('#me-rec').textContent.includes('Trial 4'),'Removing the viewed trial returns live and does not reuse its number');
    $('[data-edit-distance="1"]').click();await pause();$('#batch-edit-value').value='2.2';$('.modal-actions [data-primary]').click();await pause();
    assert(session.distances[0].label==='2.2','Distance edit must update its measured label');
    $('#me-session-rename').click();await pause();$('#batch-edit-value').value='Session_'+'y'.repeat(70);$('.modal-actions [data-primary]').click();await pause();
    assert($('#me-session option').textContent.includes('Session_'),'Session rename must update the selector');
    assert(document.documentElement.scrollWidth<=innerWidth,'Long names must not overflow the page');
    $('[data-remove-distance="2"]').click();await pause();$('.modal-actions [data-primary]').click();await pause();
    assert(session.distances.length===1,'Distance removal must remove its section');
    $('#me-session-remove').click();await pause();$('.modal-actions [data-primary]').click();await pause();
    assert(!session.distances.length && $('#me-rec').disabled,'Session removal must leave an empty workspace');
    location.hash='#/calibration';await pause();
    assert(location.hash==='#/measure','Removed routes must return to the batch workspace');
    return 'Batch capture/export, rename/remove controls, stable numbering and saved raw trial graphs passed';
  } finally {
    api=original.api;rawApi=original.rawApi;S.record=original.record;
    view.sessionId=original.sessionId;view.distanceId=original.distanceId;view.sensor=original.sensor;view.data=original.data;
    view._trial=original.trial;S.lastSensorsAt=original.lastSensorsAt;
    if(original.remembered===null)localStorage.removeItem('scratchsor-session');else localStorage.setItem('scratchsor-session',original.remembered);
    location.hash=original.hash;
    if($('#me-session') && original.data){view.apply(original.data);$('#me-sensor').value=String(view.sensor);view.graph();}
  }
}
