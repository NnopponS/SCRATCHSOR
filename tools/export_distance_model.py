"""Export a selected PC model to fixed-size C++ arrays for later ESP32 use."""
import argparse
import json
import hashlib
from pathlib import Path


def floats(value):
    if isinstance(value,list):return '{'+','.join(floats(v) for v in value)+'}'
    text=format(value,'.9g')
    if '.' not in text and 'e' not in text:text+='.0'
    return text+'f'


def export(source,target):
    d=json.loads(Path(source).read_text(encoding='utf-8'));m=d['model']
    if d.get('feature_transform','raw')!='raw' or len(d['mean'])!=16:
        raise ValueError('This exporter supports raw 16-feature models. V3 needs its four log-magnitude features reproduced in a dedicated export before board deployment.')
    lines=['#pragma once','#include <cmath>','// Candidate export only: not installed in firmware. Median filtering stays on the caller.','namespace scratchsor_model_v1 {']
    count=0
    def array(name,value):
        nonlocal count
        shape=[];v=value
        while isinstance(v,list):shape.append(len(v));v=v[0]
        n=1
        for size in shape:n*=size
        count+=n
        lines.append('static constexpr float '+name+''.join('['+str(s)+']' for s in shape)+' = '+floats(value)+';')
    array('mean',d['mean']);array('scale',d['scale'])
    lines.append('static constexpr unsigned window_ms = '+str(d['window_ms'])+';')
    if m['kind']=='mlp':
        array('hidden_weights',m['hidden_weights']);array('hidden_bias',m['hidden_bias']);array('output_weights',m['output_weights'])
        count+=3
        lines += ['inline float predict(const float features[16]) {','  float x[16]; for (unsigned i=0;i<16;++i) x[i]=(features[i]-mean[i])/scale[i];','  float output='+floats(m['output_bias'])+';','  for (unsigned h=0;h<'+str(len(m['hidden_bias']))+';++h) {','    float v=hidden_bias[h]; for (unsigned i=0;i<16;++i) v+=hidden_weights[h][i]*x[i];','    output+=output_weights[h]*std::tanh(v);','  }','  return '+floats(m['output_mean'])+'+'+floats(m['output_scale'])+'*output;','}']
    else:
        array('weights',m['weights']);count+=1
        lines += ['inline float predict(const float features[16]) {','  float x[16]; for (unsigned i=0;i<16;++i) x[i]=(features[i]-mean[i])/scale[i];','  float output='+floats(m['bias'])+';']
        if m['kind']=='linear':lines.append('  for (unsigned i=0;i<16;++i) output+=weights[i]*x[i];')
        elif m['kind']=='quadratic':lines += ['  unsigned n=0;for(unsigned i=0;i<16;++i) output+=weights[n++]*x[i];','  for(unsigned i=0;i<16;++i)for(unsigned j=i;j<16;++j)output+=weights[n++]*x[i]*x[j];']
        else:raise ValueError('This exporter currently supports the linear, quadratic and MLP candidates')
        lines += ['  return output;','}']
    array('support_centers',d['support']['centers']);count+=1
    lines += ['inline bool familiar(const float features[16]) {','  float nearest=INFINITY;','  for (const auto& center:support_centers) {','    float distance=0; for(unsigned i=0;i<16;++i) {float delta=(features[i]-mean[i])/scale[i]-center[i];distance+=delta*delta;}','    if(distance<nearest)nearest=distance;','  }','  return nearest<='+floats(d['support']['max_distance']**2)+';','}']
    background=Path(source).with_name('no_magnet_reference.json')
    if background.exists():
        reference=json.loads(background.read_text(encoding='utf-8'))
        if reference.get('model_sha256')!=hashlib.sha256(Path(source).read_bytes()).hexdigest():
            raise ValueError('Background reference belongs to a different model')
        array('background_axes',reference['background_axes_uT']);count+=2
        lines += ['// Call before predict(); preserve was_present between healthy frames and reset it on errors.',
                  'inline float magnetSignal(const float features[16]) {',
                  '  float largest=0; for(unsigned s=0;s<4;++s) {float sum=0;for(unsigned a=0;a<3;++a){unsigned i=s*3+a;float delta=features[i]-background_axes[i];sum+=delta*delta;}if(sum>largest)largest=sum;}',
                  '  return std::sqrt(largest);','}',
                  'inline bool magnetPresent(const float features[16], bool was_present) {',
                  '  const float signal=magnetSignal(features);',
                  '  if(signal<='+floats(reference['off_threshold_uT'])+')return false;',
                  '  if(signal>='+floats(reference['on_threshold_uT'])+')return true;',
                  '  return was_present;','}']
    lines.append('} // namespace scratchsor_model_v1')
    Path(target).write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(json.dumps({'header':str(target),'float32_parameter_bytes':count*4,'filter_buffer_separate':True}))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--model',required=True);parser.add_argument('--output',required=True)
    args=parser.parse_args();export(args.model,args.output)
