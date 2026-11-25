"""Portable three-region inference; numeric v4 estimation remains separate."""
import json,math
from pathlib import Path

LABELS=('BELOW_ZERO','VALUE','ABOVE_EIGHT')

class RangeModel:
    def __init__(self,source):
        self.data=json.loads(Path(source).read_text()) if not isinstance(source,dict) else source
        if self.data.get('schema')!=1 or self.data.get('task')!='overlap_region':raise ValueError('Not a range model')

    def predict(self,features):
        d=self.data
        if len(features)!=16 or not all(math.isfinite(v) for v in features):return {'class':-1,'accepted':False,'confidence':0.}
        z=[(features[i]-m)/s for i,m,s in zip(d['feature_indices'],d['mean'],d['scale'])]
        distances=[math.sqrt(sum((a-b)**2 for a,b in zip(z,c))/len(z)) for c in d['centers']]
        phi=z+([math.exp(-d['gamma']*v*v) for v in distances] if d['gamma'] else [])
        logits=[b+sum(x*w for x,w in zip(phi,weights)) for b,weights in zip(d['bias'],d['weights'])]
        maximum=max(logits);p=[math.exp(v-maximum) for v in logits];total=sum(p);p=[v/total for v in p]
        cls=max(range(3),key=p.__getitem__);second=max(p[i] for i in range(3) if i!=cls)
        nearest=min(v for v,label in zip(distances,d['center_classes']) if label==cls)
        accepted=p[cls]>=d['confidence_min'] and p[cls]-second>=d['margin_min'] and nearest<=d['support_limits'][cls]
        return {'class':cls,'range_result':LABELS[cls],'accepted':accepted,'confidence':p[cls],'distance':nearest,'probabilities':p}

    def capture(self,features,windows):
        result=self.predict(features)
        if result['accepted'] and result['class']!=1:
            votes=sum((p:=self.predict(f))['accepted'] and p['class']==result['class'] for f in windows)
            if votes*5<len(windows)*4:result={**result,'accepted':False,'reason':'Changing range pattern'}
        return result
