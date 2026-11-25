"""Train compact continuous fingertip-overlap regressors; preserve every input CSV."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import time
import numpy as np
from distance_model import DistanceModel

SEED = 20261112


def load_label_corrections(directory, path=None):
    path=Path(path) if path else Path(directory)/'label_corrections.json'
    if not path.exists():
        raise ValueError(f'Label correction file not found: {path}')
    correction=json.loads(path.read_text(encoding='utf-8'))
    if correction.get('schema')!=1 or not isinstance(correction.get('files'),dict):
        raise ValueError('Unsupported label correction format')
    if set(correction['files'])!={p.name for p in Path(directory).glob('*.csv')}:
        raise ValueError('Label correction must cover exactly the CSV files in the dataset')
    for item in correction['files'].values():
        if not all(type(item.get(k)) in (int,float) and math.isfinite(item[k]) and item[k]>=0 for k in ('recorded_cm','corrected_cm')):
            raise ValueError('Label corrections must contain finite, nonnegative recorded_cm and corrected_cm')
    return {'file':path.name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),**correction}


def load_trials(directory, corrections=None):
    if corrections is None and (Path(directory)/'label_corrections.json').exists():
        corrections=load_label_corrections(directory)
    trials,manifest,audit = [],[],[]
    seen = set()
    for path in sorted(Path(directory).glob('*.csv')):
        manifest.append({'file':path.name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
        with path.open(encoding='utf-8-sig',newline='') as source:
            rows = list(csv.DictReader(source))
        groups = {}
        bad_rows = 0
        for row in rows:
            recorded_label = float(row['distance_cm'])
            if not math.isfinite(recorded_label) or not math.isclose(float(row['ref_mm']),recorded_label*10,abs_tol=1e-5):
                raise ValueError(f'{path}: inconsistent distance labels')
            label=recorded_label
            if corrections:
                correction=corrections['files'][path.name]
                if not math.isclose(recorded_label,correction['recorded_cm'],abs_tol=1e-5):
                    raise ValueError(f'{path}: recorded label does not match the correction manifest')
                label=float(correction['corrected_cm'])
            key = (row['session_id'],row['distance_cm'],row['trial_no'])
            group = groups.setdefault(key,{'label':label,'recorded_label':recorded_label,'frames':{},'saved_at':row['saved_at']})
            frame = group['frames'].setdefault(int(row['seq']),{})
            sensor = int(row['id'])
            if sensor in frame or sensor not in range(4):
                raise ValueError(f'{path}: duplicate/invalid sensor sample')
            frame[sensor] = row
            bad_rows += row['st']!='OK' or row['valid']!='1' or row['saturated']!='0'
        good = 0
        for key,group in groups.items():
            if key in seen:
                raise ValueError('A trial appears in multiple input files')
            seen.add(key)
            frames = []
            for frame in group['frames'].values():
                if set(frame)!=set(range(4)) or any(r['st']!='OK' or r['valid']!='1' or r['saturated']!='0' for r in frame.values()):
                    continue
                elapsed = {int(r['elapsed_ms']) for r in frame.values()}
                if len(elapsed)!=1:
                    raise ValueError('Mismatched sensor timestamps')
                values = [float(frame[i][k]) for i in range(4) for k in ('raw_x','raw_y','raw_z')]
                if not all(math.isfinite(v) for v in values):
                    continue
                frames.append((elapsed.pop(),values))
            frames.sort()
            if len(frames)<20:
                raise ValueError(f'Trial {key} has too few healthy frames')
            good += len(frames)
            trials.append({'id':'/'.join(key),'label':group['label'],'number':int(key[2]),'frames':frames})
        audit.append({'file':path.name,'recorded_distance_cm':sorted({g['recorded_label'] for g in groups.values()}),'distance_cm':sorted({g['label'] for g in groups.values()}),'trials':len(groups),'rows':len(rows),'healthy_complete_frames':good,'unhealthy_rows':bad_rows})
    if not trials:
        raise ValueError('No trial CSVs found')
    return trials,manifest,audit


def windows(trials, window_ms):
    X,y,ids = [],[],[]
    for trial in trials:
        buckets = {}
        for elapsed,axes in trial['frames']:
            buckets.setdefault(elapsed//window_ms,[]).append((elapsed,axes))
        for samples in buckets.values():
            if len(samples)<max(3,window_ms//40) or samples[-1][0]-samples[0][0]<window_ms-80:
                continue
            axes = np.median([s[1] for s in samples],axis=0)
            X.append(np.r_[axes,np.linalg.norm(axes.reshape(4,3),axis=1)])
            y.append(trial['label']);ids.append(trial['id'])
    return np.array(X),np.array(y),np.array(ids)


def centers(X, count):
    rng = np.random.default_rng(SEED)
    count = min(count,len(X))
    chosen = [int(rng.integers(len(X)))]
    d = np.sum((X-X[chosen[0]])**2,axis=1)
    for _ in range(1,count):
        index = int(rng.choice(len(X),p=d/d.sum())) if d.sum()>1e-12 else int(rng.integers(len(X)))
        chosen.append(index);d = np.minimum(d,np.sum((X-X[index])**2,axis=1))
    C = X[chosen].copy()
    for _ in range(12):
        assignment = np.argmin(np.sum((X[:,None,:]-C[None,:,:])**2,axis=2),axis=1)
        next_c = np.array([X[assignment==i].mean(axis=0) if np.any(assignment==i) else C[i] for i in range(count)])
        if np.allclose(next_c,C):break
        C = next_c
    return C


def design(X, spec, fitted=None):
    if spec['kind']=='linear':return X,None
    if spec['kind']=='quadratic':
        return np.column_stack([X]+[X[:,i]*X[:,j] for i in range(X.shape[1]) for j in range(i,X.shape[1])]),None
    C = centers(X,spec['centers']) if fitted is None else fitted
    return np.exp(-spec['gamma']*np.sum((X[:,None,:]-C[None,:,:])**2,axis=2)),C


def fit(X,y,spec):
    mean,scale = X.mean(axis=0),X.std(axis=0)
    scale = np.maximum(scale,1e-6)
    Z = (X-mean)/scale
    if spec['kind']=='mlp':
        rng=np.random.default_rng(SEED);width=spec['width']
        W=rng.normal(0,.2,(X.shape[1],width));b=np.zeros(width);v=rng.normal(0,.2,width);c=np.array(0.)
        ym=float(y.mean());ys=max(float(y.std()),1.);target=(y-ym)/ys
        params=[W,b,v,c];first=[np.zeros_like(p) for p in params];second=[np.zeros_like(p) for p in params]
        for epoch in range(1,1001):
            h=np.tanh(Z@W+b);error=(h@v+c-target)/len(y)
            dh=error[:,None]*v*(1-h*h)
            grads=[Z.T@dh+spec['alpha']*W,dh.sum(axis=0),h.T@error+spec['alpha']*v,error.sum()]
            for i,(p,g) in enumerate(zip(params,grads)):
                first[i]=.9*first[i]+.1*g;second[i]=.999*second[i]+.001*g*g
                p-=.008*(first[i]/(1-.9**epoch))/(np.sqrt(second[i]/(1-.999**epoch))+1e-8)
        model={'kind':'mlp','hidden_weights':W.T.tolist(),'hidden_bias':b.tolist(),'output_weights':v.tolist(),'output_bias':float(c),'output_mean':ym,'output_scale':ys}
    else:
        A,C=design(Z,spec)
        am=A.mean(axis=0);ym=float(y.mean());Ac=A-am
        weights=np.linalg.solve(Ac.T@Ac+spec['alpha']*np.eye(A.shape[1]),Ac.T@(y-ym))
        model={'kind':spec['kind'],'weights':weights.tolist(),'bias':float(ym-am@weights)}
        if C is not None:model.update(centers=C.tolist(),gamma=spec['gamma'])
    return {'mean':mean.tolist(),'scale':scale.tolist(),'model':model}


def predict(fitted,X):
    Z=(X-np.array(fitted['mean']))/np.array(fitted['scale']);m=fitted['model']
    if m['kind']=='mlp':
        h=np.tanh(Z@np.array(m['hidden_weights']).T+np.array(m['hidden_bias']))
        return m['output_mean']+m['output_scale']*(h@np.array(m['output_weights'])+m['output_bias'])
    spec=m|({'centers':len(m['centers'])} if m['kind']=='rbf' else {})
    A,_=design(Z,spec,np.array(m['centers']) if m['kind']=='rbf' else None)
    return A@np.array(m['weights'])+m['bias']


def metrics(y,p,ids):
    e=np.abs(y-p)
    medians=[(float(y[ids==g][0]),float(np.median(p[ids==g]))) for g in np.unique(ids)]
    return {'window_mae_cm':float(e.mean()),'window_rmse_cm':float(np.sqrt(np.mean((y-p)**2))),
            'window_p95_error_cm':float(np.quantile(e,.95)),'within_0_25_cm':float(np.mean(e<=.25)),
            'within_0_5_cm':float(np.mean(e<=.5)), 'trial_median_mae_cm':float(np.mean([abs(a-b) for a,b in medians])),
            'windows':len(y),'trials':len(medians)}


def split(trials):
    train,test,folds=[],[],[[] for _ in range(4)]
    rng=np.random.default_rng(SEED)
    for label in sorted({t['label'] for t in trials}):
        group=sorted([t for t in trials if t['label']==label],key=lambda t:t['number'])
        if len(group)<6:raise ValueError('At least six independent trials per distance are required for this split')
        rng.shuffle(group);test+=group[:2];train+=group[2:]
        for i,t in enumerate(group[2:]):folds[i%4].append(t['id'])
    return train,test,folds


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',default='raw-training-data/1');parser.add_argument('--output',default='models/v2')
    parser.add_argument('--labels',help='Correction manifest; otherwise use label_corrections.json in the data directory when present')
    parser.add_argument('--name',help='Display name for this model version')
    args=parser.parse_args();out=Path(args.output)
    if out.exists() and any(out.iterdir()):raise SystemExit('Choose a new output version; existing models will not be overwritten.')
    correction_path=Path(args.labels) if args.labels else Path(args.data)/'label_corrections.json'
    corrections=load_label_corrections(args.data,correction_path) if args.labels or correction_path.exists() else None
    out.mkdir(parents=True,exist_ok=True)
    trials,manifest,audit=load_trials(args.data,corrections);train,test,folds=split(trials)
    print(json.dumps({'training_labels_cm':sorted({t['label'] for t in trials}),'label_corrections':corrections}),flush=True)
    specs=[{'kind':'linear','alpha':a} for a in (.1,10,100)]
    specs += [{'kind':'quadratic','alpha':a} for a in (1,10,100)]
    specs += [{'kind':'rbf','centers':64,'gamma':g,'alpha':a} for g in (.03,.1,.3) for a in (.01,1)]
    specs += [{'kind':'mlp','width':w,'alpha':a} for w,a in [(24,.001),(32,.001),(24,.01)]]
    leaderboard=[]
    for window_ms in (200,400):
        X,y,ids=windows(train,window_ms)
        for spec in specs:
            p=np.zeros(len(y))
            for fold in folds:
                mask=np.isin(ids,fold)
                model=fit(X[~mask],y[~mask],spec);p[mask]=predict(model,X[mask])
            score=metrics(y,p,ids)
            row={'window_ms':window_ms,'spec':spec,'cv':score};leaderboard.append(row)
            print(json.dumps({'window_ms':window_ms,**spec,'cv_mae':round(score['window_mae_cm'],4),'cv_trial_mae':round(score['trial_median_mae_cm'],4)}),flush=True)
    leaderboard.sort(key=lambda r:r['cv']['window_mae_cm'])
    best=leaderboard[0]
    # Prefer the shorter filter if it is within 5% of the best validation error.
    shortlist=[r for r in leaderboard if r['cv']['window_mae_cm']<=best['cv']['window_mae_cm']*1.05]
    best=min(shortlist,key=lambda r:(r['window_ms'],r['cv']['window_mae_cm']))
    window_ms=best['window_ms'];spec=best['spec']
    X,y,ids=windows(train,window_ms);Xt,yt,it=windows(test,window_ms)
    evaluation=fit(X,y,spec);pt=predict(evaluation,Xt)
    holdout=metrics(yt,pt,it)
    interpolation=[]
    labels=sorted({t['label'] for t in trials})
    for label in labels[1:-1]:
        keep=y!=label;omitted=y==label
        fitted=fit(X[keep],y[keep],spec)
        interpolation.append({'withheld_distance_cm':label,**metrics(y[omitted],predict(fitted,X[omitted]),ids[omitted])})
    # Keep the reserved whole-trial holdout outside this run's training and tuning.
    trial_vectors=np.array([np.median(X[ids==g],axis=0) for g in np.unique(ids)])
    Z=(trial_vectors-np.array(evaluation['mean']))/np.array(evaluation['scale'])
    nearest=np.sqrt(np.sum((Z[:,None,:]-Z[None,:,:])**2,axis=2));np.fill_diagonal(nearest,np.inf)
    limit=max(float(np.quantile(nearest.min(axis=1),.95)*1.5),.5)
    artifact={'schema':1,'name':args.name or 'Fingertip overlap '+out.name,'created_utc':datetime.now(timezone.utc).isoformat(),
              'target':'middle-to-middle fingertip overlap','unit':'cm','range_cm':[min(labels),max(labels)],
              'window_ms':window_ms,'min_frames':max(3,window_ms//40),'feature_order':[f's{i+1}_{a}' for i in range(4) for a in ('x','y','z')]+[f's{i+1}_magnitude' for i in range(4)],
              **evaluation,'support':{'centers':Z.tolist(),'max_distance':limit},
              'training_distances_cm':labels,'unmeasured_half_cm':[i/2 for i in range(int(min(labels)*2),int(max(labels)*2)+1) if i/2 not in labels],
              'label_correction':corrections,'holdout':holdout,'warning':'Experimental, setup-specific. Half-centimetre display resolution is not validated accuracy.'}
    (out/'model.json').write_text(json.dumps(artifact,indent=2),encoding='utf-8')
    report={'dataset':audit,'inputs':manifest,'label_correction':corrections,'total_trials':len(trials),'training_trials':len(train),'holdout_trials':len(test),
            'training_trial_ids':[t['id'] for t in train],'holdout_trial_ids':[t['id'] for t in test],
            'selected':best,'holdout':holdout,'interpolation':interpolation,'leaderboard':leaderboard,
            'validation_notes':['All windows of a trial stay in one partition.','Scaler, centers and model fit on training folds only.','One recording session cannot establish between-session robustness.','Trial repositioning independence is not established by CSV metadata.','Unmeasured half-centimetre positions need new physical measurements.']}
    (out/'metrics.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    with (out/'holdout_predictions.csv').open('w',newline='',encoding='utf-8') as target:
        writer=csv.writer(target);writer.writerow(['trial_id','reference_cm','prediction_cm','error_cm'])
        writer.writerows(zip(it,yt,pt,pt-yt))
    portable=DistanceModel(out/'model.json')
    values=[portable.predict_features(row.tolist())['overlap_cm'] for row in Xt]
    discrepancy=float(np.max(np.abs(pt-values)))
    if discrepancy>1e-7:raise RuntimeError(f'Portable inference mismatch: {discrepancy}')
    start=time.perf_counter()
    for i in range(500):portable.predict_features(Xt[i%len(Xt)].tolist())
    timing=(time.perf_counter()-start)*1000/500
    print(json.dumps({'selected':best,'holdout':holdout,'interpolation':interpolation,'portable_max_difference':discrepancy,'pc_inference_ms':timing,'output':str(out)},indent=2),flush=True)
    (out/'runtime.json').write_text(json.dumps({'pc_inference_ms':timing,'portable_max_difference':discrepancy},indent=2))


if __name__=='__main__':main()
