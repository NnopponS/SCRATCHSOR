"""Conservative experimental sign policy using existing nonnegative recordings."""
import json
from pathlib import Path
import numpy as np
from train_distance_model import load_trials
from train_distance_v4 import build_samples
from distance_model import regression
from export_distance_model import floats

def main():
    root=Path(__file__).resolve().parents[1]
    data=json.loads((root/'models/v4/model.json').read_text())
    trials,_,_=load_trials(root/'raw-training-data/2')
    X,labels,_=build_samples(trials)
    X=(X-np.array(data['mean']))/np.array(data['scale'])
    members=data['model']['members']
    heads=np.array([[regression(m['model'],[x[i] for i in m['input_indices']]) for m in members] for x in X])
    mean=heads.mean(axis=1);spread=heads.std(axis=1)
    # For six heads 2.5 sigma exceeds the largest possible head deviation
    # sqrt(5)*sigma. Also require a margin beyond every observed positive
    # window. This is extrapolation, not a validated negative classifier.
    bound=mean+2.5*spread
    cutoff=min(-.15,float(bound.min())-.10)
    (root/'include/product/negative_policy.h').write_text('#pragma once\nnamespace negative_policy {\nconstexpr float upperLimit='+floats(cutoff)+';\ninline bool candidate(float y,float spread){return y+2.5f*spread<upperLimit;}\n}\n')
    report={'experimental':True,'negative_labels':0,'existing_nonnegative_windows':len(X),'cutoff_cm':cutoff,'positive_windows_flagged':int(np.sum(bound<cutoff)),'zero_windows':int(np.sum(labels==0))}
    (root/'.pio/negative-policy-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':main()
