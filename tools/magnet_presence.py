"""No-magnet background calibration, separate from overlap regression labels."""
from datetime import datetime, timezone
import math
from statistics import median


def magnetic_signal(axes, background):
    """Largest sensor's vector change from the measured empty-field reference, uT."""
    return max(math.sqrt(sum((axes[i+j]-background[i+j])**2 for j in range(3))) for i in range(0,12,3))


def make_reference(frames, model_sha256):
    if len(frames)<100:
        raise ValueError('Need at least 100 healthy, distinct frames. Keep USB connected and try again.')
    if ((frames[-1]['ts']-frames[0]['ts'])&0xFFFFFFFF)<4500:
        raise ValueError('Background stream was interrupted. Keep USB connected for the full five seconds.')
    axes=[f['axes'] for f in frames]
    if any(len(row)!=12 or not all(math.isfinite(v) for v in row) for row in axes):
        raise ValueError('Invalid background measurements')
    center=[median(row[i] for row in axes) for i in range(12)]
    scores=sorted(magnetic_signal(row,center) for row in axes)
    p99=scores[min(len(scores)-1,math.ceil(.99*len(scores))-1)]
    first=[median(row[i] for row in axes[:40]) for i in range(12)]
    last=[median(row[i] for row in axes[-40:]) for i in range(12)]
    if p99>50 or magnetic_signal(first,last)>30:
        raise ValueError('Background was not stable. Remove magnets, keep the sensor mounting still, and try again.')
    off=max(20.,2*p99)
    return {'schema':1,'created_utc':datetime.now(timezone.utc).isoformat(),
            'model_sha256':model_sha256,'label':'no_magnet','frames':len(frames),
            'background_axes_uT':center,'off_threshold_uT':off,'on_threshold_uT':off*1.5,
            'background_p99_signal_uT':p99,'background_max_signal_uT':scores[-1],
            'note':'Measured with magnets removed. This setup-specific presence gate is not a 0 cm overlap example.'}
