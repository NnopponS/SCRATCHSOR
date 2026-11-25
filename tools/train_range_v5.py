"""Train censored-tail classification without changing any numeric distance labels."""
import os
for setting in ('OPENBLAS_NUM_THREADS','MKL_NUM_THREADS','OMP_NUM_THREADS'):os.environ.setdefault(setting,'1')
import argparse,hashlib,json,math,time
from collections import Counter,deque
from datetime import datetime,timezone
from pathlib import Path
import numpy as np
from distance_model import window_features
from magnet_presence import magnetic_signal
from train_distance_model import load_trials
from range_model import RangeModel,LABELS

SEED=20261113
CANDIDATES=[('linear',list(range(16)),0.),('rbf_wide',list(range(16)),.5),('rbf_local',list(range(16)),2.),('magnitude_rbf',list(range(12,16)),2.)]

def data():
    old,old_manifest,old_audit=load_trials('raw-training-data/2')
    new,new_manifest,new_audit=load_trials('raw-training-data/3')
    assert sorted({t['label'] for t in old})==list(range(9))
    assert {p['file'] for p in new_manifest}=={'distance_below_0.0cm.csv','distance_higher_8.0cm.csv'}
    assert {t['label'] for t in new}=={0.,9.}
    for t in old:t.update(id='dataset2/'+t['id'],region=1,source='dataset2')
    for t in new:t.update(id='dataset3/'+t['id'],region=0 if t['label']==0 else 2,source='dataset3')
    manifest=[{**p,'path':f'raw-training-data/{n}/'+p['file']} for n,items in [(2,old_manifest),(3,new_manifest)] for p in items]
    return old+new,manifest,{'dataset2':old_audit,'dataset3':new_audit,'region_trials':dict(Counter(LABELS[t['region']] for t in old+new)),
      'label_interpretation':{'distance_below_0.0cm.csv':'BELOW_ZERO; recorded numeric 0 is a placeholder, not an exact target','distance_higher_8.0cm.csv':'ABOVE_EIGHT; recorded numeric 9 is treated as a range label, not proof of exact 9 cm'}}

def prepare(trial,reference):
    rolling=deque();windows=[];last=-1000;present=False
    for ts,axes in trial['frames']:
        rolling.append((ts,axes))
        while ts-rolling[0][0]>200:rolling.popleft()
        if len(rolling)<5 or ts-rolling[0][0]<160 or ts-last<100:continue
        last=ts;raw=[a for _,a in rolling];f=window_features(raw,'robust_direction')
        center=np.median(raw,axis=0).tolist();signal=magnetic_signal(center,reference['background_axes_uT'])
        if signal<=reference['off_threshold_uT']:present=False
        elif signal>=reference['on_threshold_uT']:present=True
        windows.append((ts,f,present))
    attempts=[]
    for requested in (200,1300):
        start=next((ts for ts,_ in trial['frames'] if ts>=requested),requested)
        captured=[(ts,f) for ts,f,magnet in windows if start<ts<start+1000 and magnet]
        raw=[ts for ts,_ in trial['frames'] if start<ts<start+1000]
        valid=len(raw)>=35 and len(captured)>=6 and captured[-1][0]-captured[0][0]>=600 and all(b-a<=120 for a,b in zip(raw,raw[1:]))
        attempts.append({'trial_id':trial['id'],'region':trial['region'],'source':trial['source'],'numeric_label':trial['label'] if trial['region']==1 else None,'start_ms':start,'gate':valid,
          'features':np.median([f for _,f in captured],axis=0).tolist() if captured else [0.]*16,'windows':[f for _,f in captured], 'raw_frames':len(raw)})
    return {'X':np.array([f for _,f,_ in windows]),'attempts':attempts,'present_windows':sum(m for _,_,m in windows),'windows':len(windows)}

def folds(trials,count,seed):
    rng=np.random.default_rng(seed);out=[[] for _ in range(count)]
    for region in range(3):
        subset=sorted([t for t in trials if t['region']==region],key=lambda t:t['id']);rng.shuffle(subset)
        for i,t in enumerate(subset):out[i%count].append(t)
    return out

def fit(trials,cache,candidate):
    name,indices,gamma=candidate
    X=np.concatenate([cache[t['id']]['X'] for t in trials]);y=np.concatenate([np.full(len(cache[t['id']]['X']),t['region']) for t in trials])
    class_counts=Counter(t['region'] for t in trials)
    w=np.concatenate([np.full(len(cache[t['id']]['X']),1/(class_counts[t['region']]*len(cache[t['id']]['X']))) for t in trials]);w/=w.sum()
    X=X[:,indices];mean=np.sum(w[:,None]*X,axis=0);scale=np.maximum(np.sqrt(np.sum(w[:,None]*(X-mean)**2,axis=0)),1e-6);Z=(X-mean)/scale
    centers=np.array([np.median((cache[t['id']]['X'][:,indices]-mean)/scale,axis=0) for t in trials]);classes=np.array([t['region'] for t in trials])
    squared=np.maximum(0,((Z[:,None,:]-centers[None,:,:])**2).mean(axis=2))
    A=np.c_[Z,np.exp(-gamma*squared)] if gamma else Z
    target=np.eye(3)[y];W=np.zeros((A.shape[1],3));b=np.zeros(3);params=[W,b];first=[np.zeros_like(p) for p in params];second=[np.zeros_like(p) for p in params]
    for epoch in range(1,601):
        logits=A@W+b;logits-=logits.max(axis=1,keepdims=True);p=np.exp(logits);p/=p.sum(axis=1,keepdims=True)
        error=w[:,None]*(p-target);gradients=[A.T@error+.001*W,error.sum(axis=0)]
        for i,(param,g) in enumerate(zip(params,gradients)):
            first[i]=.9*first[i]+.1*g;second[i]=.999*second[i]+.001*g*g
            param-=.03*(first[i]/(1-.9**epoch))/(np.sqrt(second[i]/(1-.999**epoch))+1e-8)
    limits=[];offset=0;own=np.empty(len(y),dtype=int)
    for i,t in enumerate(trials):n=len(cache[t['id']]['X']);own[offset:offset+n]=i;offset+=n
    for region in range(3):
        mask=y==region;dist=squared[mask].copy();dist[:,classes!=region]=np.inf;dist[np.arange(mask.sum()),own[mask]]=np.inf
        cross=np.sqrt(dist.min(axis=1));limits.append(max(.2,float(np.quantile(cross,.99)*1.25)))
    return {'schema':1,'task':'overlap_region','candidate':name,'feature_transform':'robust_direction','window_ms':200,'measurement_aggregation':'feature_median','classes':list(LABELS),
      'feature_indices':indices,'mean':mean.tolist(),'scale':scale.tolist(),'centers':centers.tolist(),'center_classes':classes.tolist(),'gamma':gamma,'weights':W.T.tolist(),'bias':b.tolist(),
      'support_limits':limits,'confidence_min':.8,'margin_min':.2,'tail_vote_fraction':.8,'fit_trials':len(trials)}

def evaluate(model,trials,cache):
    model=RangeModel(model);rows=[]
    for t in trials:
        for a in cache[t['id']]['attempts']:
            p=model.capture(a['features'],a['windows']) if a['gate'] else {'accepted':False,'class':-1,'confidence':0.}
            rows.append({k:a[k] for k in ('trial_id','region','source','numeric_label','start_ms','gate','raw_frames')}|{'prediction':p['class'] if p['accepted'] else -1,'confidence':p['confidence'],'proposed':p['class']})
    return rows

def summary(rows):
    matrix=[[0]*4 for _ in range(3)]
    for r in rows:matrix[r['region']][r['prediction'] if r['prediction']>=0 else 3]+=1
    correct=sum(matrix[i][i] for i in range(3));accepted=sum(sum(m[:3]) for m in matrix);recalls=[matrix[i][i]/sum(matrix[i]) for i in range(3)]
    return {'attempts':len(rows),'accepted':accepted,'correct':correct,'accuracy_all_attempts':correct/len(rows),'accepted_accuracy':correct/accepted if accepted else 0.,'balanced_success':sum(recalls)/3,
      'classes':list(LABELS),'columns':[*LABELS,'UNSURE'],'confusion_matrix':matrix,'per_class_success':dict(zip(LABELS,recalls))}

def choose(training,cache,seed):
    results={c[0]:[] for c in CANDIDATES}
    for validation in folds(training,3,seed):
        held={t['id'] for t in validation};fit_trials=[t for t in training if t['id'] not in held]
        for c in CANDIDATES:results[c[0]]+=evaluate(fit(fit_trials,cache,c),validation,cache)
    scores={name:summary(rows) for name,rows in results.items()}
    best=max(CANDIDATES,key=lambda c:(scores[c[0]]['balanced_success'],scores[c[0]]['accuracy_all_attempts'],-CANDIDATES.index(c)))
    return best,scores

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',default='models/v5');parser.add_argument('--work',default='.pio/v5-training');args=parser.parse_args()
    out=Path(args.output);work=Path(args.work);work.mkdir(parents=True,exist_ok=True)
    if out.exists() and any(out.iterdir()):raise SystemExit('Existing model preserved. Choose a new output directory.')
    started=time.monotonic();trials,manifest,audit=data();reference=json.loads(Path('models/v4/no_magnet_reference.json').read_text());cache={t['id']:prepare(t,reference) for t in trials}
    audit['presence_windows']={LABELS[c]:{'present':sum(cache[t['id']]['present_windows'] for t in trials if t['region']==c),'total':sum(cache[t['id']]['windows'] for t in trials if t['region']==c)} for c in range(3)}
    (work/'dataset_audit.json').write_text(json.dumps({'inputs':manifest,**audit},indent=2));print(json.dumps({'trials':len(trials),'audit':audit['region_trials'],'presence':audit['presence_windows']}),flush=True)
    all_rows=[];reports=[]
    for index,validation in enumerate(folds(trials,5,SEED)):
        held={t['id'] for t in validation};training=[t for t in trials if t['id'] not in held];candidate,inner=choose(training,cache,SEED+index+100)
        model=fit(training,cache,candidate);rows=evaluate(model,validation,cache);all_rows+=rows
        report={'fold':index,'candidate':candidate[0],'training_trials':[t['id'] for t in training],'validation_trials':[t['id'] for t in validation],'inner_selection':inner,'score':summary(rows)};reports.append(report)
        (work/'progress.json').write_text(json.dumps({'folds':reports,'total':summary(all_rows)},indent=2));print(json.dumps({'fold':index,'candidate':candidate[0],'score':summary(rows),'seconds':time.monotonic()-started}),flush=True)
    metrics={'method':'Nested five-fold whole-trial evaluation; inner three-fold selection. Two real one-second captures per trial. Scaling, kernels, classifier weights and support radii use training trials only; all rejections count as failures.',
      'inputs':manifest,'dataset':audit,'score':summary(all_rows),'outer_folds':reports,'attempts':all_rows,'limitation':'Dataset 2 contains all in-range trials and dataset 3 contains only the tails: session and region are confounded. No fresh-session in-range boundary controls or independent test set. No exact negative or above-range distances are inferred.'}
    (work/'metrics.json').write_text(json.dumps(metrics,indent=2))
    if min(metrics['score']['per_class_success'].values())<.8:
        print(json.dumps({'ready_to_embed':False,'reason':'At least one class below 80% held-out-trial success','score':metrics['score']}),flush=True);return
    candidate,selection=choose(trials,cache,SEED+500);model=fit(trials,cache,candidate)
    model.update(created_utc=datetime.now(timezone.utc).isoformat(),numeric_model='models/v4/model.json',numeric_model_sha256=hashlib.sha256(Path('models/v4/model.json').read_bytes()).hexdigest(),inputs=manifest,validation=metrics['score'],warning=metrics['limitation'])
    out.mkdir(parents=True,exist_ok=True);(out/'model.json').write_text(json.dumps(model,indent=2));metrics['final_candidate']=candidate[0];metrics['final_selection']=selection;metrics['elapsed_seconds']=time.monotonic()-started
    (out/'metrics.json').write_text(json.dumps(metrics,indent=2));(out/'dataset_audit.json').write_text(json.dumps(audit,indent=2))
    assert all(hashlib.sha256(Path(p['path']).read_bytes()).hexdigest()==p['sha256'] for p in manifest),'Inputs changed'
    (out/'verification.json').write_text(json.dumps({'input_files_unchanged':True,'fit_trials':len(trials),'training_windows':sum(len(x['X']) for x in cache.values()),'negative_numeric_values_invented':False,'firmware_installed':False},indent=2))
    np.savez(work/'verification_features.npz',X=np.concatenate([x['X'] for x in cache.values()]))
    print(json.dumps({'ready_to_embed':True,'output':str(out),'candidate':candidate[0],'score':metrics['score'],'seconds':time.monotonic()-started}),flush=True)

if __name__=='__main__':main()
