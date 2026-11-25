"""Train the selected dataset-2 continuous model, evaluate whole trials, then refit all."""
import argparse,csv,hashlib,json,math
from datetime import datetime,timezone
from pathlib import Path
import time
import numpy as np
from train_distance_model import load_trials,split,windows,fit,predict,metrics
from distance_model import DistanceModel,features

def measurement_metrics(y,p,ids):
    reference=[];values=[];groups=[]
    for group in np.unique(ids):
        selected=np.flatnonzero(ids==group)
        for start in range(0,len(selected)-3,5):
            block=selected[start:start+5]
            if len(block)>=4:
                reference.append(y[block[0]]);values.append(np.median(p[block]));groups.append(group)
    reference=np.array(reference);values=np.array(values);e=np.abs(reference-values)
    return {'one_second_within_0_5_cm':float(np.mean(e<=.5)),
            'one_second_mae_cm':float(e.mean()),'one_second_p95_error_cm':float(np.quantile(e,.95)),
            'one_second_measurements':len(e),'one_second_successes':int(sum(e<=.5))},(reference,values,groups)

def convert(X):return np.array([features(row[:12],'magnitudes') for row in X])

def artifact(trials,fitted,X,ids,name,holdout,full_refit):
    centers=np.array([np.median(X[ids==group],axis=0) for group in np.unique(ids)])
    Z=(centers-np.array(fitted['mean']))/np.array(fitted['scale'])
    distances=np.sqrt(np.sum((Z[:,None,:]-Z[None,:,:])**2,axis=2));np.fill_diagonal(distances,np.inf)
    limit=max(float(np.quantile(distances.min(axis=1),.95)*1.5),.5)
    labels=sorted({t['label'] for t in trials})
    return {'schema':1,'name':name,'created_utc':datetime.now(timezone.utc).isoformat(),
            'target':'middle-to-middle fingertip overlap','unit':'cm','range_cm':[min(labels),max(labels)],
            'window_ms':200,'min_frames':5,'feature_transform':'magnitudes',
            'feature_order':[f'log1p_s{i+1}_magnitude_over_100uT' for i in range(4)],
            **fitted,'support':{'centers':Z.tolist(),'max_distance':limit},
            'training_distances_cm':labels,'unmeasured_half_cm':[i/2 for i in range(int(min(labels)*2),int(max(labels)*2)+1) if i/2 not in labels],
            'fit_trials':len(trials),'refit_all_trials':full_refit,'holdout':holdout,
            'validation_description':f"Evaluated on {holdout['trials']} reserved trials before the full-data refit. These single-session development scores do not establish accuracy on new testers or unmeasured half-centimetre positions.",
            'warning':'Experimental, setup-specific. Dataset has suspected range-wrap artifacts; half-centimetre accuracy on new data is not established.'}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',default='raw-training-data/2');parser.add_argument('--output',default='models/v3')
    parser.add_argument('--benchmark-report',required=True);parser.add_argument('--audit-report',required=True)
    args=parser.parse_args();out=Path(args.output)
    if out.exists() and any(out.iterdir()):raise SystemExit('Choose a new output directory; prior models are preserved.')
    trials,manifest,audit=load_trials(args.data);train,test,folds=split(trials)
    benchmark=json.loads(Path(args.benchmark_report).read_text());quality=json.loads(Path(args.audit_report).read_text())
    if manifest!=benchmark['inputs'] or {r['file']:r['sha256'] for r in manifest}!={r['file']:r['sha256'] for r in quality['files']}:raise ValueError('Audit/benchmark input fingerprints differ')
    selected=benchmark['selected'];spec=selected['spec']
    if selected['features']!='magnitudes' or spec['kind']!='mlp':raise ValueError('This trainer implements the selected continuous magnitude network')
    if benchmark['holdout_trial_ids']!=[t['id'] for t in test]:raise ValueError('Reserved partition changed')
    X,y,ids=windows(train,200);Xt,yt,it=windows(test,200);X=convert(X);Xt=convert(Xt)
    evaluation=fit(X,y,spec);p=predict(evaluation,Xt)
    holdout=metrics(yt,p,it);extra,measurements=measurement_metrics(yt,p,it);holdout.update(extra)
    if holdout['one_second_within_0_5_cm']<.8:raise ValueError('Selected model does not meet the requested development target; active model was not changed.')
    Xa,ya,ia=windows(trials,200);Xa=convert(Xa);production=fit(Xa,ya,spec)
    out.mkdir(parents=True,exist_ok=True)
    evaluation_artifact=artifact(train,evaluation,X,ids,'Fingertip overlap v3 evaluation',holdout,False)
    production_artifact=artifact(trials,production,Xa,ia,'Fingertip overlap v3 · dataset 2',holdout,True)
    (out/'evaluation_model.json').write_text(json.dumps(evaluation_artifact,indent=2),encoding='utf-8')
    (out/'model.json').write_text(json.dumps(production_artifact,indent=2),encoding='utf-8')
    (out/'dataset_audit.json').write_text(json.dumps(quality,indent=2),encoding='utf-8')
    (out/'feature_benchmark.json').write_text(json.dumps(benchmark,indent=2),encoding='utf-8')
    evaluation_portable=DistanceModel(out/'evaluation_model.json')
    portable=np.array([evaluation_portable.predict_features(row.tolist())['overlap_cm'] for row in Xt])
    difference=float(max(abs(portable-p)))
    if difference>1e-7:raise ValueError('Portable model predictions differ from training implementation')
    states=[evaluation_portable.predict_features(row.tolist())['state'] for row in Xt]
    production_portable=DistanceModel(out/'model.json');start=time.perf_counter()
    for i in range(500):production_portable.predict_features(Xa[i%len(Xa)].tolist())
    runtime={'pc_inference_ms':(time.perf_counter()-start)*1000/500,'portable_max_difference':difference}
    (out/'runtime.json').write_text(json.dumps(runtime,indent=2))
    report={'dataset':audit,'inputs':manifest,'total_trials':len(trials),'evaluation_training_trials':len(train),
            'holdout_trials':len(test),'production_training_trials':len(trials),'production_training_windows':len(ya),
            'selected':selected,'holdout':holdout,'evaluation_accepted_window_fraction':sum(s=='LIVE' for s in states)/len(states),
            'training_trial_ids':[t['id'] for t in train],'holdout_trial_ids':[t['id'] for t in test],
            'production_training_trial_ids':[t['id'] for t in trials],
            'validation_notes':['All windows of one trial stay together during evaluation.','The reserved trial partition was previously inspected during the baseline audit; it is not a fresh external test.',
            'Candidate selection used four-fold CV on the other 79 trials.','Reported one-second scores use medians of up to five disjoint 200ms windows, not an end-to-end live button replay.',
            'Production was subsequently refitted on all 97 trials including the reserved ones, as requested. Reported validation scores belong to the pre-refit evaluation model.',
            'All input rows carried OK flags; 11 trials contain suspected full-scale jumps. No trial was dropped or relabeled.',
            'Only integer positions are measured. Scores do not establish half-centimetre interpolation or between-session/tester accuracy.']}
    (out/'metrics.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    for filename,header,rows in [('holdout_predictions.csv',['trial_id','reference_cm','prediction_cm','error_cm'],zip(it,yt,p,p-yt)),
                                 ('holdout_one_second_predictions.csv',['trial_id','reference_cm','prediction_cm','error_cm'],zip(measurements[2],measurements[0],measurements[1],measurements[1]-measurements[0]))]:
        with (out/filename).open('w',newline='',encoding='utf-8') as target:
            writer=csv.writer(target);writer.writerow(header);writer.writerows(rows)
    unchanged=all(hashlib.sha256((Path(args.data)/item['file']).read_bytes()).hexdigest()==item['sha256'] for item in manifest)
    if not unchanged:raise RuntimeError('Input files changed during training')
    (out/'verification.json').write_text(json.dumps({'input_files_unchanged':True,'portable_max_difference_cm':difference,'production_trials':len(trials),'no_trials_dropped':True,'firmware_installed':False},indent=2))
    print(json.dumps({'selected':selected,'holdout':holdout,'all_trials_refit':len(trials),'evaluation_accepted_window_fraction':report['evaluation_accepted_window_fraction'],'output':str(out)},indent=2))

if __name__=='__main__':main()
