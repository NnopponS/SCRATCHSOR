"""Export v4's exact fixed-size ensemble and reference for the phone product."""
import json,hashlib
from pathlib import Path
from export_distance_model import floats

def main():
    path=Path('models/v4/model.json');d=json.loads(path.read_text());b=json.loads(path.with_name('no_magnet_reference.json').read_text())
    if b['model_sha256']!=hashlib.sha256(path.read_bytes()).hexdigest():raise ValueError('Reference mismatch')
    if d['candidate']!='direction_mix_50':raise ValueError('Unexpected model contract')
    lines=['#pragma once','#include <cmath>','namespace phone_model {',f'inline constexpr const char* version="v4-{b["model_sha256"][:12]}";']
    def array(name,v):
        shape=[];a=v
        while isinstance(a,list):shape.append(len(a));a=a[0]
        lines.append('static constexpr float '+name+''.join(f'[{n}]' for n in shape)+'='+floats(v)+';')
    array('mean',d['mean']);array('scale',d['scale']);array('background',b['background_axes_uT']);array('support',d['support']['centers'])
    lines += [f'constexpr float supportLimit={floats(d["support"]["max_distance"])};',f'constexpr float magnetOff={floats(b["off_threshold_uT"])};',f'constexpr float magnetOn={floats(b["on_threshold_uT"])};']
    for i,member in enumerate(d['model']['members']):
        m=member['model'];array(f'w{i}',m['hidden_weights']);array(f'b{i}',m['hidden_bias']);array(f'v{i}',m['output_weights'])
        indices=member['input_indices'];n=len(m['hidden_bias'])
        lines += [f'inline float head{i}(const float x[16]) {{ float h[{n}];']
        for h in range(n):lines.append(f'h[{h}]=std::tanh(b{i}[{h}]+'+'+'.join(f'w{i}[{h}][{j}]*x[{index}]' for j,index in enumerate(indices))+');')
        lines.append('return '+floats(m['output_mean'])+'+'+floats(m['output_scale'])+'*('+floats(m['output_bias'])+'+'+'+'.join(f'v{i}[{j}]*h[{j}]' for j in range(n))+');}')
    lines += ['inline float predict(const float f[16],float& distance,float& disagreement) {float x[16];for(unsigned i=0;i<16;i++)x[i]=(f[i]-mean[i])/scale[i];float p[6]={'+','.join(f'head{i}(x)' for i in range(6))+'};',
              'float y=0;for(float a:p)y+=a/6;disagreement=0;for(float a:p)disagreement+=(a-y)*(a-y)/6;disagreement=std::sqrt(disagreement);',
              'distance=INFINITY;for(const auto& c:support){float v=0;for(unsigned i=0;i<4;i++){float delta=x[i+12]-c[i];v+=delta*delta;}if(v<distance)distance=v;}distance=std::sqrt(distance);return y;}','}']
    Path('include/product').mkdir(exist_ok=True)
    Path('include/product/model_v4.h').write_text('\n'.join(lines)+'\n')
    print('Exported embedded v4 model with measured background reference')
if __name__=='__main__':main()
