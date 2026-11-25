"""Export the learned range classifier to fixed-size ESP32 float constants."""
import argparse,hashlib,json
from pathlib import Path
from export_distance_model import floats

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--model',default='models/v5/model.json');parser.add_argument('--output',default='include/product/range_v5.h');args=parser.parse_args()
    path=Path(args.model);d=json.loads(path.read_text());assert d['task']=='overlap_region'
    assert hashlib.sha256(Path(d['numeric_model']).read_bytes()).hexdigest()==d['numeric_model_sha256']
    n=len(d['feature_indices']);k=len(d['centers']);width=n+k if d['gamma'] else n
    assert len(d['weights'])==3 and all(len(w)==width for w in d['weights'])
    lines=['#pragma once','#include <cmath>','#include <cstdint>','namespace range_model {',f'inline constexpr const char* version="v5-{hashlib.sha256(path.read_bytes()).hexdigest()[:12]}";',f'constexpr unsigned dimensions={n},centerCount={k},designWidth={width};']
    def array(name,value,kind='float'):
        shape=[];v=value
        while isinstance(v,list):shape.append(len(v));v=v[0]
        body=floats(value) if kind=='float' else '{'+','.join(str(i) for i in value)+'}'
        lines.append('static constexpr '+kind+' '+name+''.join(f'[{x}]' for x in shape)+'='+body+';')
    array('indices',d['feature_indices'],'uint8_t');array('mean',d['mean']);array('scale',d['scale']);array('centers',d['centers']);array('classes',d['center_classes'],'uint8_t');array('weights',d['weights']);array('bias',d['bias']);array('limits',d['support_limits'])
    lines.extend([f'constexpr float gamma={floats(d["gamma"])},confidenceMin={floats(d["confidence_min"])},marginMin={floats(d["margin_min"])};',
      'struct Result {int cls=-1;float confidence=0,distance=INFINITY;bool accepted=false;float probabilities[3]{};};',
      'inline Result predict(const float f[16]){Result result;float phi[dimensions+centerCount],nearest[3]={INFINITY,INFINITY,INFINITY};',
      'for(unsigned i=0;i<16;i++)if(!std::isfinite(f[i]))return result;',
      'for(unsigned i=0;i<dimensions;i++)phi[i]=(f[indices[i]]-mean[i])/scale[i];',
      'for(unsigned c=0;c<centerCount;c++){float distance=0;for(unsigned j=0;j<dimensions;j++){float delta=phi[j]-centers[c][j];distance+=delta*delta/dimensions;}if(distance<nearest[classes[c]])nearest[classes[c]]=distance;if(gamma>0)phi[dimensions+c]=std::exp(-gamma*distance);}',
      'float logits[3],maximum=-INFINITY;for(unsigned c=0;c<3;c++){logits[c]=bias[c];for(unsigned j=0;j<designWidth;j++)logits[c]+=weights[c][j]*phi[j];if(!std::isfinite(logits[c]))return result;if(logits[c]>maximum)maximum=logits[c];}',
      'float total=0;for(unsigned c=0;c<3;c++){result.probabilities[c]=std::exp(logits[c]-maximum);total+=result.probabilities[c];}',
      'result.cls=0;for(unsigned c=0;c<3;c++){result.probabilities[c]/=total;if(result.probabilities[c]>result.probabilities[result.cls])result.cls=c;}',
      'float second=0;for(unsigned c=0;c<3;c++)if(int(c)!=result.cls&&result.probabilities[c]>second)second=result.probabilities[c];result.confidence=result.probabilities[result.cls];result.distance=std::sqrt(nearest[result.cls]);',
      'result.accepted=result.confidence>=confidenceMin&&result.confidence-second>=marginMin&&result.distance<=limits[result.cls];return result;}',
      'inline Result capture(const float f[16],const float windows[][16],unsigned count){Result result=predict(f);if(!count){result.accepted=false;return result;}if(result.accepted&&result.cls!=1){unsigned votes=0;for(unsigned i=0;i<count;i++){Result p=predict(windows[i]);votes+=p.accepted&&p.cls==result.cls;}if(votes*5<count*4)result.accepted=false;}return result;}','}'])
    Path(args.output).write_text('\n'.join(lines)+'\n');print(json.dumps({'export':args.output,'classifier':d['candidate'],'features':n,'centers':k,'floats':len(d['mean'])*2+k*n+3*width+6}))

if __name__=='__main__':main()
