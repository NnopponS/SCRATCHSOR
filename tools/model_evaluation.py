"""Replay saved real trials through the PC prediction and one-second button path."""
from distance_model import LivePredictor
from measurement_hold import ManualMeasurement


class ReplayClock:
    def __init__(self):self.now=0.
    def __call__(self):return self.now


def replay_measurements(model,trials):
    attempts=[]
    for trial in trials:
        clock=ReplayClock();predictor=LivePredictor(model,clock);measurement=ManualMeasurement(clock,model)
        starts=iter((.2,1.3));next_start=next(starts,None);active=None
        for elapsed,axes in trial['frames']:
            clock.now=elapsed/1000
            event={'t':'sensors','seq':elapsed,'ts':elapsed,'rate':50,'s':[{'id':i,'st':'OK','sat':False,'rx':axes[3*i],'ry':axes[3*i+1],'rz':axes[3*i+2]} for i in range(4)]}
            prediction=predictor.push(event)
            if prediction is not None:measurement.feed(prediction)
            current=predictor.snapshot();state=measurement.snapshot(current)
            if active is not None and state['state']!='MEASURING':
                value=state['result']['overlap_cm'] if state['state']=='HELD' else None
                attempts.append({'trial_id':trial['id'],'reference_cm':trial['label'],'prediction_cm':value,
                                 'error_cm':value-trial['label'] if value is not None else None,
                                 'state':state['state'],'detail':state['detail'],'started_seconds':active})
                active=None
            if next_start is not None and clock.now>=next_start:
                try:measurement.start(current);active=clock.now
                except ValueError as error:attempts.append({'trial_id':trial['id'],'reference_cm':trial['label'],'prediction_cm':None,'error_cm':None,'state':'REJECTED','detail':str(error),'started_seconds':clock.now})
                next_start=next(starts,None)
        if active is not None:attempts.append({'trial_id':trial['id'],'reference_cm':trial['label'],'prediction_cm':None,'error_cm':None,'state':'INCOMPLETE','detail':'Capture ended before deadline','started_seconds':active})
    return attempts


def summarize(attempts):
    errors=sorted(abs(r['error_cm']) for r in attempts if r['error_cm'] is not None)
    within=sum(r['error_cm'] is not None and abs(r['error_cm'])<=.5 for r in attempts)
    p=(len(errors)-1)*.95 if errors else 0;i=int(p)
    return {'attempts':len(attempts),'completed':len(errors),'within_half_cm':within,
            'success_fraction_all_attempts':within/len(attempts) if attempts else 0.,
            'mae_cm_completed':sum(errors)/len(errors) if errors else None,
            'p95_error_cm_completed':errors[i]+(errors[min(i+1,len(errors)-1)]-errors[i])*(p-i) if errors else None}
