"""Portable distance inference. Training uses NumPy; live inference is stdlib-only."""
from collections import deque
import json
import hashlib
import math
from pathlib import Path
from statistics import median
import time
from magnet_presence import magnetic_signal


def features(axes, transform='raw'):
    magnitudes=[math.sqrt(sum(v*v for v in axes[i:i+3])) for i in range(0,12,3)]
    if transform in ('magnitudes','median_magnitudes'):return [math.log1p(value/100.) for value in magnitudes]
    if transform=='robust_direction':
        return [axes[i]/max(magnitudes[i//3],1.) for i in range(12)]+[math.log1p(value/100.) for value in magnitudes]
    if transform!='raw':raise ValueError('Unsupported feature transform')
    return list(axes)+magnitudes


def window_features(samples, transform='raw'):
    """Use magnitudes from real vectors before taking temporal medians in v4."""
    if transform not in ('median_magnitudes','robust_direction'):
        return features([median(row[i] for row in samples) for i in range(12)],transform)
    magnitudes=[[math.sqrt(sum(v*v for v in row[i:i+3])) for i in range(0,12,3)] for row in samples]
    strength=[math.log1p(median(row[i] for row in magnitudes)/100.) for i in range(4)]
    if transform=='median_magnitudes':return strength
    direction=[median(row[i]/max(m[i//3],1.) for row,m in zip(samples,magnitudes)) for i in range(12)]
    return direction+strength


def regression(model, x):
    kind=model['kind']
    if kind=='ensemble':
        return sum(member['weight']*regression(member['model'],[x[i] for i in member['input_indices']]) for member in model['members'])
    if kind=='mlp':
        hidden=[math.tanh(b+sum(a*w for a,w in zip(x,column))) for column,b in zip(model['hidden_weights'],model['hidden_bias'])]
        value=model['output_bias']+sum(a*w for a,w in zip(hidden,model['output_weights']))
        if model.get('output_activation')=='sigmoid':
            value=1/(1+math.exp(-value)) if value>=0 else math.exp(value)/(1+math.exp(value))
        return model['output_mean']+model['output_scale']*value
    if kind=='quadratic':
        row=x+[x[i]*x[j] for i in range(len(x)) for j in range(i,len(x))]
        return model['bias']+sum(a*b for a,b in zip(row,model['weights']))
    if kind=='linear':return model['bias']+sum(a*b for a,b in zip(x,model['weights']))
    if kind=='rbf':return model['bias']+sum(w*math.exp(-model['gamma']*sum((a-b)**2 for a,b in zip(x,c))) for c,w in zip(model['centers'],model['weights']))
    raise ValueError('Unsupported model kind')


class DistanceModel:
    def __init__(self, path, load_background=True):
        self.path = Path(path)
        self.sha256 = hashlib.sha256(self.path.read_bytes()).hexdigest()
        self.data = json.loads(self.path.read_text(encoding='utf-8'))
        if self.data.get('schema') != 1:
            raise ValueError('Unsupported model schema')
        self.background = None
        reference = self.path.with_name('no_magnet_reference.json')
        if load_background and reference.exists():
            self.set_background(json.loads(reference.read_text(encoding='utf-8')))

    def set_background(self, reference):
        axes=reference.get('background_axes_uT',[])
        if reference.get('schema')!=1 or reference.get('model_sha256')!=self.sha256:
            raise ValueError('Background reference does not match this model; capture it again')
        off,on=reference.get('off_threshold_uT'),reference.get('on_threshold_uT')
        if len(axes)!=12 or not all(isinstance(v,(float,int)) and math.isfinite(v) for v in axes):
            raise ValueError('Invalid background reference')
        if not isinstance(off,(float,int)) or not isinstance(on,(float,int)) or not 0<off<on or not math.isfinite(on):
            raise ValueError('Invalid background thresholds')
        self.background=reference

    def predict_features(self, values):
        d = self.data
        if len(values) != len(d['mean']) or not all(math.isfinite(v) for v in values):
            raise ValueError(f"{len(d['mean'])} finite magnetic features required")
        x = [(v-m)/s for v,m,s in zip(values,d['mean'],d['scale'])]
        y=regression(d['model'],x)
        supported=[x[i] for i in d['support'].get('feature_indices',range(len(x)))]
        nearest = min(math.sqrt(sum((a-b)**2 for a,b in zip(supported,c))) for c in d['support']['centers'])
        state = 'LIVE' if nearest <= d['support']['max_distance'] else 'UNFAMILIAR_POSE'
        lo,hi = d['range_cm']
        if y < lo-.25 or y > hi+.25:
            state = 'OUT_OF_RANGE'
        return {'state':state, 'overlap_cm':y, 'half_cm':math.floor(y*2+.5)/2,
                'support_distance':nearest, 'support_limit':d['support']['max_distance']}


class LivePredictor:
    def __init__(self, model, clock=time.monotonic):
        self.model, self.clock = model, clock
        self.frames = deque()
        self.predictions = deque()
        self.smoothing_ms = 800
        self.last_seq = self.last_ts = None
        self.last_received = None
        self.last_prediction = None
        self.magnet_present = False
        self.latest = {'state':'WAITING_FOR_USB','overlap_cm':None,'half_cm':None}

    def clear(self, state):
        self.frames.clear()
        self.predictions.clear()
        self.last_seq = self.last_ts = self.last_prediction = None
        self.last_received = None
        self.magnet_present = False
        self.latest = {'state':state,'overlap_cm':None,'half_cm':None}

    def push(self, event):
        if event.get('t') == 'usb':
            if event.get('connected') is False:
                self.clear('USB_DISCONNECTED')
            return None
        if event.get('t') != 'sensors':
            return None
        seq,ts = event.get('seq'),event.get('ts')
        if type(seq) is not int or type(ts) is not int:
            return None
        samples = {s.get('id'):s for s in event.get('s',[]) if isinstance(s,dict)}
        if set(samples) != set(range(4)) or any(s.get('st')!='OK' or s.get('sat') for s in samples.values()):
            self.clear('SENSOR_ERROR')
            self.last_received = self.clock()
            return dict(self.latest)
        axes = [samples[i].get(k) for i in range(4) for k in ('rx','ry','rz')]
        if not all(isinstance(v,(int,float)) and math.isfinite(v) for v in axes):
            self.clear('SENSOR_ERROR')
            self.last_received = self.clock()
            return dict(self.latest)
        # A repeated frame is not fresh acquisition. Still inspect error/status
        # transitions above, even when firmware republishes the same sequence.
        if seq == self.last_seq:
            return None
        if self.last_ts is not None and ((ts-self.last_ts)&0xFFFFFFFF) > 120:
            self.clear('WARMING_UP')
        self.last_received = self.clock()
        self.last_seq,self.last_ts = seq,ts
        self.frames.append((ts,axes))
        window = self.model.data['window_ms']
        while self.frames and ((ts-self.frames[0][0])&0xFFFFFFFF) > window:
            self.frames.popleft()
        span = (ts-self.frames[0][0])&0xFFFFFFFF
        if len(self.frames)<self.model.data['min_frames'] or span < window-40:
            self.latest = {'state':'WARMING_UP','overlap_cm':None,'half_cm':None}
            return None
        if self.last_prediction is not None and ((ts-self.last_prediction)&0xFFFFFFFF)<100:
            return None
        filtered = [median(frame[1][i] for frame in self.frames) for i in range(12)]
        context={'seq':seq,'ts':ts,'window_frames':len(self.frames),'acquisition_hz':event.get('rate')}
        background=self.model.background
        self.last_prediction = ts
        if background is None:
            self.predictions.clear()
            self.latest={'state':'BACKGROUND_REQUIRED','overlap_cm':None,'half_cm':None,**context}
            return dict(self.latest)
        signal=magnetic_signal(filtered,background['background_axes_uT'])
        context.update(magnet_signal_uT=signal,magnet_on_threshold_uT=background['on_threshold_uT'],magnet_off_threshold_uT=background['off_threshold_uT'])
        if signal<=background['off_threshold_uT']:self.magnet_present=False
        elif signal>=background['on_threshold_uT']:self.magnet_present=True
        if not self.magnet_present:
            self.predictions.clear()
            self.latest={'state':'NO_MAGNET','overlap_cm':None,'half_cm':None,**context}
            return dict(self.latest)
        transform=self.model.data.get('feature_transform','raw')
        values=window_features([frame[1] for frame in self.frames],transform) if transform in ('median_magnitudes','robust_direction') else features(filtered,transform)
        self.latest = self.model.predict_features(values) | context
        if self.model.data.get('measurement_aggregation')=='feature_median':self.latest['input_features']=values
        if self.latest['state']=='LIVE':
            instant=self.latest['overlap_cm']
            self.predictions.append((ts,instant))
            while self.predictions and ((ts-self.predictions[0][0])&0xFFFFFFFF)>self.smoothing_ms:
                self.predictions.popleft()
            smoothed=median(value for _,value in self.predictions)
            self.latest.update(instant_overlap_cm=instant,overlap_cm=smoothed,
                               half_cm=math.floor(smoothed*2+.5)/2,
                               smoothing_ms=self.smoothing_ms,smoothing_samples=len(self.predictions))
        else:self.predictions.clear()
        return dict(self.latest)

    def snapshot(self):
        if self.last_received is None:
            return dict(self.latest)
        if self.clock()-self.last_received>1.5:
            self.clear('USB_STALE')
        return dict(self.latest)
