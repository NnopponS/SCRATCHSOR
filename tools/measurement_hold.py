"""Capture one second after an explicit request, retaining the previous result."""
from datetime import datetime, timezone
import math
from statistics import median
import time


def quantile(values, fraction):
    values=sorted(values);position=(len(values)-1)*fraction
    lo=int(position);hi=min(lo+1,len(values)-1)
    return values[lo]+(values[hi]-values[lo])*(position-lo)


class ManualMeasurement:
    def __init__(self, clock=time.monotonic, model=None):
        self.clock=clock;self.target_seconds=1.;self.samples=[]
        self.model=model
        self.state='READY';self.result=None;self.last_seq=None;self.last_update=None
        self.started=None;self.detail='Press Measure when ready, then hold still for one second.'

    def start(self, prediction):
        if self.state=='MEASURING':raise ValueError('A measurement is already running.')
        if prediction.get('state')!='LIVE':raise ValueError('Wait for valid magnet and sensor readings before measuring.')
        self.started=self.clock();self.samples=[];self.last_seq=prediction.get('seq')
        self.last_update=self.started;self.state='MEASURING'
        self.detail='Hold still for one second. Your previous result stays visible.'

    def fail(self, detail):
        if self.state!='MEASURING':return
        self.samples=[];self.last_seq=None;self.last_update=None;self.started=None
        self.state='ERROR';self.detail=detail+' Previous result retained; press Measure to try again.'

    def finish(self, prediction):
        if prediction.get('state')!='LIVE' or len(self.samples)<6 or self.samples[-1][0]-self.samples[0][0]<.6 or self.clock()-self.last_update>.35:
            self.fail('Not enough fresh valid readings during the one-second measurement.');return
        values=[sample[1] for sample in self.samples];value=median(values)
        if self.model is not None and self.model.data.get('measurement_aggregation')=='feature_median':
            columns=len(self.model.data['mean'])
            captured=[median(sample[2][i] for sample in self.samples) for i in range(columns)]
            estimate=self.model.predict_features(captured)
            if estimate['state']!='LIVE':
                self.fail('The captured field pattern is outside the trained setup.');return
            value=estimate['overlap_cm']
        spread=quantile(values,.9)-quantile(values,.1);edge=max(2,len(values)//4)
        moving=spread>.6 or abs(median(values[:edge])-median(values[-edge:]))>.25
        self.result={'overlap_cm':value,'half_cm':math.floor(value*2+.5)/2,
                     'spread_cm':spread,'samples':len(values),'duration_seconds':self.target_seconds,
                     'method':'manual_one_second','motion_warning':moving,
                     'captured_utc':datetime.now(timezone.utc).isoformat()}
        self.state='HELD';self.started=None
        self.detail=('Signal changed during capture. Hold still and press Measure again.' if moving else 'Result held. Press Measure when ready for the next measurement.')

    def feed(self, prediction):
        if self.state!='MEASURING':return
        if prediction.get('state')!='LIVE':
            self.fail('Measurement interrupted: '+prediction.get('state','invalid readings').replace('_',' ').lower()+'.');return
        now=self.clock()
        if now-self.started>=self.target_seconds:
            self.finish(prediction);return
        if prediction.get('seq')==self.last_seq:return
        raw=prediction.get('instant_overlap_cm',prediction.get('overlap_cm'))
        if not isinstance(raw,(float,int)) or not math.isfinite(raw):
            self.fail('Invalid model reading.');return
        if now-self.last_update>.35:
            self.fail('Sensor readings paused during the measurement.');return
        captured=prediction.get('input_features')
        if self.model is not None and self.model.data.get('measurement_aggregation')=='feature_median':
            if not isinstance(captured,list) or len(captured)!=len(self.model.data['mean']) or not all(isinstance(v,(int,float)) and math.isfinite(v) for v in captured):
                self.fail('Invalid measurement features.');return
        self.last_seq=prediction.get('seq');self.last_update=now
        self.samples.append((now,raw,captured))

    def snapshot(self, prediction):
        now=self.clock()
        if self.state=='MEASURING':
            if prediction.get('state')!='LIVE':self.fail('Valid magnet and sensor readings are required throughout the measurement.')
            elif now-self.started>=self.target_seconds:self.finish(prediction)
            elif now-self.last_update>.35:self.fail('Sensor readings paused during the measurement.')
        elapsed=min(self.target_seconds,max(0.,now-self.started)) if self.started is not None else 0.
        return {'state':self.state,'detail':self.detail,'elapsed_seconds':elapsed,
                'remaining_seconds':self.target_seconds-elapsed if self.state=='MEASURING' else 0.,
                'target_seconds':self.target_seconds,'samples':len(self.samples),'result':self.result}
