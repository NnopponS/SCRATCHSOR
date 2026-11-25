"""Improve the one-second PC regressor with nested whole-trial evaluation."""
import argparse,hashlib,json,time,os
from collections import Counter,deque
from datetime import datetime,timezone
from pathlib import Path
for setting in ('OPENBLAS_NUM_THREADS','MKL_NUM_THREADS','OMP_NUM_THREADS'):os.environ.setdefault(setting,'1')
import numpy as np
from distance_model import DistanceModel,window_features
from model_evaluation import replay_measurements,summarize
from train_distance_model import load_trials,windows,fit,SEED,centers
from train_distance_v3 import artifact as v3_artifact,convert as v3_convert

CANDIDATES=('magnitude_ensemble','bounded_magnitude_ensemble','direction_mix_25','direction_mix_50')
SEEDS=(SEED+101,SEED+202,SEED+303)

def grouped_folds(trials,count,seed):
    rng=np.random.default_rng(seed);folds=[[] for _ in range(count)]
    for label in sorted({t['label'] for t in trials}):
        group=sorted([t for t in trials if t['label']==label],key=lambda t:t['id'])
        rng.shuffle(group)
        for i,trial in enumerate(group):folds[i%count].append(trial)
    return folds

def build_samples(trials):
    X=[];y=[];ids=[]
    for trial in trials:
        frames=deque();last=None
        for elapsed,axes in trial['frames']:
            frames.append((elapsed,axes))
            while elapsed-frames[0][0]>200:frames.popleft()
            if len(frames)<5 or elapsed-frames[0][0]<160:continue
            if last is not None and elapsed-last<100:continue
            last=elapsed;X.append(window_features([row[1] for row in frames],'robust_direction'))
            y.append(trial['label']);ids.append(trial['id'])
    return np.array(X),np.array(y),np.array(ids)

def balanced_weights(y,ids):
    count=Counter(ids);label_trials=Counter(float(y[ids==group][0]) for group in np.unique(ids))
    w=np.array([1/(count[group]*label_trials[float(label)]) for label,group in zip(y,ids)])
    return w/w.sum()

def train_head(Z,y,w,width,alpha,seed,bounded):
    rng=np.random.default_rng(seed);W=rng.normal(0,.2,(Z.shape[1],width));b=np.zeros(width)
    v=rng.normal(0,.2,width);c=np.array(0.)
    ym=0. if bounded else float(np.sum(w*y));ys=8. if bounded else max(float(np.sqrt(np.sum(w*(y-ym)**2))),1.)
    target=(y-ym)/ys;params=[W,b,v,c];first=[np.zeros_like(p) for p in params];second=[np.zeros_like(p) for p in params]
    for epoch in range(1,1201):
        h=np.tanh(Z@W+b);output=h@v+c
        if bounded:
            output=1/(1+np.exp(-np.clip(output,-60,60)))
            error=w*(output-target)*output*(1-output)
        else:error=w*(output-target)
        dh=error[:,None]*v*(1-h*h)
        gradients=[Z.T@dh+alpha*W,dh.sum(axis=0),h.T@error+alpha*v,error.sum()]
        for i,(p,g) in enumerate(zip(params,gradients)):
            first[i]=.9*first[i]+.1*g;second[i]=.999*second[i]+.001*g*g
            p-=.008*(first[i]/(1-.9**epoch))/(np.sqrt(second[i]/(1-.999**epoch))+1e-8)
    model={'kind':'mlp','hidden_weights':W.T.tolist(),'hidden_bias':b.tolist(),'output_weights':v.tolist(),
           'output_bias':float(c),'output_mean':ym,'output_scale':ys}
    if bounded:model['output_activation']='sigmoid'
    return model

def train_parts(trials,samples,options=CANDIDATES):
    X,y,ids=samples;w=balanced_weights(y,ids)
    mean=np.sum(w[:,None]*X,axis=0);scale=np.maximum(np.sqrt(np.sum(w[:,None]*(X-mean)**2,axis=0)),1e-6)
    Z=(X-mean)/scale;parts={}
    if any(c!='bounded_magnitude_ensemble' for c in options):
        parts['magnitude']=[train_head(Z[:,12:],y,w,24,.0003,seed,False) for seed in SEEDS]
    if 'bounded_magnitude_ensemble' in options:
        parts['bounded']=[train_head(Z[:,12:],y,w,24,.0003,seed,True) for seed in SEEDS]
    if any(c.startswith('direction_mix') for c in options):
        parts['direction']=[train_head(Z,y,w,32,.003,seed,False) for seed in SEEDS]
    # Cover the observed field-strength distribution. Calibrate the radius on
    # training-window variation and other-trial medians, never validation data.
    M=Z[:,12:];C=centers(M,min(48,len(M)))
    nearest=np.sqrt(np.maximum(0,np.min(np.sum((M[:,None,:]-C[None,:,:])**2,axis=2),axis=1)))
    groups=np.unique(ids);prototypes=np.array([np.median(M[ids==group],axis=0) for group in groups])
    other=np.sum((M[:,None,:]-prototypes[None,:,:])**2,axis=2)
    for i,group in enumerate(groups):other[ids==group,i]=np.inf
    cross=np.sqrt(other.min(axis=1))
    radius=max(.5,float(np.quantile(nearest,.99)*1.5),float(np.quantile(cross,.99)*1.25))
    return {'parts':parts,'mean':mean,'scale':scale,'support_centers':C,'support_radius':radius,'trials':len(trials)}

def candidate_data(parts,name):
    mixed=name.startswith('direction_mix');mode='robust_direction' if mixed else 'median_magnitudes'
    indices=list(range(12,16)) if mixed else list(range(4))
    mean=parts['mean'] if mixed else parts['mean'][12:];scale=parts['scale'] if mixed else parts['scale'][12:]
    members=[];direction_weight=.25 if name=='direction_mix_25' else .5 if mixed else 0.
    base=parts['parts']['bounded'] if name=='bounded_magnitude_ensemble' else parts['parts']['magnitude']
    for head in base:members.append({'weight':(1-direction_weight)/len(base),'input_indices':indices,'model':head})
    if mixed:
        for head in parts['parts']['direction']:members.append({'weight':direction_weight/3,'input_indices':list(range(16)),'model':head})
    return {'schema':1,'name':'Fingertip overlap v4 · dataset 2','created_utc':datetime.now(timezone.utc).isoformat(),
            'target':'middle-to-middle fingertip overlap','unit':'cm','range_cm':[0,8],'window_ms':200,'min_frames':5,
            'feature_transform':mode,'measurement_aggregation':'feature_median','candidate':name,
            'mean':mean.tolist(),'scale':scale.tolist(),'model':{'kind':'ensemble','members':members},
            'support':{'centers':parts['support_centers'].tolist(),'max_distance':parts['support_radius'],'feature_indices':indices},
            'training_distances_cm':list(range(9)),'unmeasured_half_cm':[i+.5 for i in range(8)],
            'fit_trials':parts['trials'],'feature_order':([f'median_s{i+1}_unit_{axis}' for i in range(4) for axis in ('x','y','z')] if mixed else [])+[f'log1p_median_s{i+1}_magnitude_over_100uT' for i in range(4)],
            'warning':'Experimental, setup-specific. Reused single-session data cannot establish fresh-session or half-centimetre accuracy.'}

def runtime_model(data,path,reference):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(data),encoding='utf-8')
    model=DistanceModel(path,load_background=False);model.set_background({**reference,'model_sha256':model.sha256})
    return model

def baseline_data(trials):
    X,y,ids=windows(trials,200);X=v3_convert(X)
    fitted=fit(X,y,{'kind':'mlp','width':24,'alpha':.0003})
    return v3_artifact(trials,fitted,X,ids,'v3 comparison',{'trials':0},False)

def rank(results):
    scores={name:summarize(rows) for name,rows in results.items()}
    return sorted(CANDIDATES,key=lambda name:(-scores[name]['success_fraction_all_attempts'],scores[name]['mae_cm_completed'] if scores[name]['mae_cm_completed'] is not None else float('inf'),CANDIDATES.index(name)))

def inner_selection(trials,sample_cache,reference,work,seed):
    groups=grouped_folds(trials,3,seed);results={name:[] for name in CANDIDATES}
    for i,validation in enumerate(groups):
        validation_ids={t['id'] for t in validation};training=[t for t in trials if t['id'] not in validation_ids]
        combined=combine_samples(training,sample_cache);parts=train_parts(training,combined)
        for name in CANDIDATES:
            model=runtime_model(candidate_data(parts,name),work/f'inner_{i}_{name}.json',reference)
            results[name]+=replay_measurements(model,validation)
    selected=rank(results)[0]
    return selected,{name:summarize(rows) for name,rows in results.items()}

def combine_samples(trials,cache):
    return tuple(np.concatenate([cache[t['id']][column] for t in trials],axis=0) for column in range(3))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',default='raw-training-data/2');parser.add_argument('--output',default='models/v4')
    parser.add_argument('--work',default='.pio/v4-training');args=parser.parse_args()
    out=Path(args.output);work=Path(args.work)
    if out.exists() and any(out.iterdir()):raise SystemExit('Choose a new model version; existing artifacts are preserved.')
    trials,manifest,audit=load_trials(args.data)
    if sorted({t['label'] for t in trials})!=list(range(9)):raise ValueError('Expected the existing 0–8 cm dataset')
    previous=json.loads(Path('models/v3/metrics.json').read_text())
    if manifest!=previous['inputs']:raise ValueError('Dataset changed since v3; inspect the new data first')
    reference=json.loads(Path('models/v3/no_magnet_reference.json').read_text())
    cache={t['id']:build_samples([t]) for t in trials};work.mkdir(parents=True,exist_ok=True)
    baseline=[];improved=[];fold_reports=[];start=time.monotonic()
    for outer,validation in enumerate(grouped_folds(trials,5,SEED+404)):
        validation_ids={t['id'] for t in validation};training=[t for t in trials if t['id'] not in validation_ids]
        selected,inner=inner_selection(training,cache,reference,work/f'outer_{outer}',SEED+505+outer)
        parts=train_parts(training,combine_samples(training,cache),(selected,))
        new_model=runtime_model(candidate_data(parts,selected),work/f'outer_{outer}_selected.json',reference)
        old_model=runtime_model(baseline_data(training),work/f'outer_{outer}_v3.json',reference)
        old_rows=replay_measurements(old_model,validation);new_rows=replay_measurements(new_model,validation)
        baseline+=old_rows;improved+=new_rows
        report={'outer_fold':outer,'selected':selected,'inner':inner,'training_trials':[t['id'] for t in training],
                'validation_trials':[t['id'] for t in validation],'v3':summarize(old_rows),'v4':summarize(new_rows)}
        fold_reports.append(report);print(json.dumps({k:v for k,v in report.items() if k not in ('inner','training_trials','validation_trials')}),flush=True)
        (work/'progress.json').write_text(json.dumps({'folds':fold_reports,'v3':summarize(baseline),'v4':summarize(improved)},indent=2))
    old=summarize(baseline);new=summarize(improved)
    worthwhile=(new['success_fraction_all_attempts']>old['success_fraction_all_attempts'] and new['mae_cm_completed']<=old['mae_cm_completed']+.02) or (new['success_fraction_all_attempts']>=old['success_fraction_all_attempts'] and new['mae_cm_completed']<old['mae_cm_completed']*.95)
    comparison={'method':'Nested five-fold outer evaluation, three-fold inner selection; all frames of a trial stay together. Identical one-second replay attempts for v3 and v4; rejected attempts count as misses.',
                'inputs':manifest,'dataset':audit,'total_trials':len(trials),'v3':old,'v4':new,'worthwhile_improvement':worthwhile,
                'outer_folds':fold_reports,'v3_attempts':baseline,'v4_attempts':improved,'elapsed_seconds':time.monotonic()-start,
                'limitation':'These are reused development recordings, not a fresh session or unmeasured half-centimetre data.'}
    (work/'comparison.json').write_text(json.dumps(comparison,indent=2))
    if not worthwhile:
        print(json.dumps({'no_deployment':True,'v3':old,'v4':new,'comparison':str(work/'comparison.json')},indent=2),flush=True);return
    selected,final_cv=inner_selection(trials,cache,reference,work/'full_selection',SEED+606)
    final_parts=train_parts(trials,combine_samples(trials,cache),(selected,));data=candidate_data(final_parts,selected)
    data['refit_all_trials']=True;data['production_training_windows']=sum(len(cache[t['id']][1]) for t in trials)
    data['measurement_validation']=new;data['holdout']={'window_mae_cm':new['mae_cm_completed'],'within_0_5_cm':new['success_fraction_all_attempts'],'window_p95_error_cm':new['p95_error_cm_completed']}
    data['validation_description']=f"Nested whole-trial development replay: {new['within_half_cm']}/{new['attempts']} one-second attempts within ±0.5 cm; {new['completed']} completed. V3 scored {old['within_half_cm']}/{old['attempts']} on the same folds. Final model refitted on all {len(trials)} trials; fresh-session accuracy remains unverified."
    out.mkdir(parents=True,exist_ok=True)
    model=runtime_model(data,out/'model.json',reference)
    ref={**reference,'model_sha256':model.sha256,'reused_from':'models/v3/no_magnet_reference.json','reuse_reason':'The measured raw-axis presence gate is independent of the retrained regression; no new background capture was made.'}
    (out/'no_magnet_reference.json').write_text(json.dumps(ref,indent=2))
    (out/'background-no-magnet-capture.json').write_bytes(Path('models/v3/background-no-magnet-capture.json').read_bytes())
    comparison.update(selected=selected,final_inner_selection=final_cv,production_training_trials=len(trials),production_training_windows=data['production_training_windows'])
    (out/'metrics.json').write_text(json.dumps(comparison,indent=2))
    (out/'dataset_audit.json').write_bytes(Path('models/v3/dataset_audit.json').read_bytes())
    (out/'dataset_audit.md').write_bytes(Path('models/v3/dataset_audit.md').read_bytes())
    unchanged=all(hashlib.sha256((Path(args.data)/r['file']).read_bytes()).hexdigest()==r['sha256'] for r in manifest)
    if not unchanged:raise RuntimeError('Input fingerprints changed during training')
    (out/'verification.json').write_text(json.dumps({'input_files_unchanged':True,'model_sha256':model.sha256,'no_trials_dropped':True,'firmware_installed':False,'measurement_duration_seconds':1,'nested_outer_attempts':new['attempts']},indent=2))
    print(json.dumps({'selected':selected,'v3':old,'v4':new,'production_training_trials':len(trials),'output':str(out),'seconds':time.monotonic()-start},indent=2),flush=True)

if __name__=='__main__':main()
