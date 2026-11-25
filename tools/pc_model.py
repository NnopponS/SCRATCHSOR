"""Run a trained overlap model on the PC, reading the existing USB bridge stream."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from collections import deque
from pathlib import Path
import threading
import time
from urllib.parse import urlsplit
from urllib.request import urlopen
import webbrowser
from distance_model import DistanceModel, LivePredictor
from magnet_presence import make_reference
from measurement_hold import ManualMeasurement

ROOT=Path(__file__).resolve().parents[1]


class ModelReceiver:
    def __init__(self, model, source):
        self.model=model;self.source=source.rstrip('/')+'/usb/events'
        self.predictor=LivePredictor(model);self.lock=threading.RLock();self.stop=threading.Event()
        self.measurement=ManualMeasurement(model=model)
        self.result_path=model.path.with_name('last_result.json');self.result_error=''
        if self.result_path.exists():
            try:
                saved=json.loads(self.result_path.read_text(encoding='utf-8'))
                result=saved['result']
                if saved.get('schema')==1 and saved.get('model_sha256')==model.sha256 and all(isinstance(result.get(k),(int,float)) and math.isfinite(result[k]) for k in ('overlap_cm','half_cm','spread_cm','duration_seconds')):
                    self.measurement.result=result
            except (OSError,ValueError,KeyError,TypeError):self.result_error='Previous result could not be restored.'
        self.received=0;self.started=time.monotonic();self.source_error=''
        self.history=deque(maxlen=1024);self.history_seq=None;self.calibrating=None
        self.thread=threading.Thread(target=self.read,daemon=True)

    def read(self):
        while not self.stop.is_set():
            try:
                with urlopen(self.source,timeout=8) as response:
                    if response.headers.get_content_type()!='text/event-stream':
                        raise ValueError('Source is not the USB event stream')
                    with self.lock:self.source_error=''
                    while not self.stop.is_set():
                        line=response.readline(16385)
                        if not line:raise ConnectionError('USB bridge event connection closed')
                        if len(line)>16384:raise ValueError('Oversized sensor event')
                        if not line.startswith(b'data: '):continue
                        event=json.loads(line[6:])
                        with self.lock:
                            if event.get('t')=='sensors':
                                self.received+=1
                                samples={s.get('id'):s for s in event.get('s',[]) if isinstance(s,dict)}
                                if event.get('seq')!=self.history_seq and set(samples)==set(range(4)) and all(s.get('st')=='OK' and not s.get('sat') for s in samples.values()):
                                    axes=[samples[i].get(k) for i in range(4) for k in ('rx','ry','rz')]
                                    if all(isinstance(v,(float,int)) and math.isfinite(v) for v in axes):
                                        self.history.append((time.monotonic(),{'seq':event['seq'],'ts':event['ts'],'axes':axes}))
                                        self.history_seq=event['seq']
                            prediction=self.predictor.push(event)
                            current=self.predictor.snapshot()
                            previous=self.measurement.result
                            if prediction is not None:self.measurement.feed(prediction)
                            elif current['state']!='LIVE':self.measurement.fail('Measurement interrupted: '+current['state'].replace('_',' ').lower()+'. Measure again when readings are live.')
                            if self.measurement.result is not previous:self.save_result()
            except (OSError,ValueError) as error:
                with self.lock:
                    self.source_error=str(error)
                    self.predictor.clear('WAITING_FOR_USB_BRIDGE')
                    self.measurement.fail('USB bridge disconnected. Reconnect and measure again.')
                self.stop.wait(1)

    def save_result(self):
        try:
            temporary=self.result_path.with_suffix('.tmp')
            temporary.write_text(json.dumps({'schema':1,'model_sha256':self.model.sha256,'result':self.measurement.result},indent=2,allow_nan=False),encoding='utf-8')
            temporary.replace(self.result_path);self.result_error=''
        except OSError:self.result_error='Last result is retained in memory, but could not be saved to disk.'

    def snapshot(self):
        with self.lock:
            prediction=self.predictor.snapshot()
            previous=self.measurement.result
            measurement=self.measurement.snapshot(prediction)
            if self.measurement.result is not previous:self.save_result()
            if self.calibrating is not None:
                prediction={'state':'CAPTURING_BACKGROUND','overlap_cm':None,'half_cm':None,'remaining_seconds':max(0,5-time.monotonic()+self.calibrating)}
            return {'prediction':prediction,'measurement':measurement,'received_events':self.received,
                    'can_measure':prediction['state']=='LIVE' and measurement['state']!='MEASURING',
                    'source_error':self.source_error,'source':self.source,'result_error':self.result_error,
                    'background':self.model.background,
                    'model':{'sha256':self.model.sha256,'validation_description':self.model.data.get('validation_description',''),'measurement_validation':self.model.data.get('measurement_validation'),**{k:self.model.data[k] for k in ('name','target','range_cm','window_ms','holdout','unmeasured_half_cm','warning')}}}

    def start_measurement(self):
        with self.lock:
            if self.calibrating is not None:raise ValueError('Wait until background capture finishes.')
            self.measurement.start(self.predictor.snapshot())
            return self.snapshot()

    def capture_background(self):
        with self.lock:
            if self.calibrating is not None:raise ValueError('A background capture is already running')
            if self.measurement.state=='MEASURING':raise ValueError('Wait until the measurement finishes.')
            self.calibrating=time.monotonic()
            started=self.calibrating
        try:
            if self.stop.wait(5):raise ValueError('Model service stopped during capture')
            with self.lock:
                frames=[frame for received,frame in self.history if received>=started]
                if not self.history or time.monotonic()-self.history[-1][0]>.5:
                    raise ValueError('USB readings stopped during background capture. Reconnect and try again.')
                reference=make_reference(frames,self.model.sha256)
                path=self.model.path.with_name('no_magnet_reference.json')
                if path.exists():
                    previous=path.with_name('no_magnet_reference_previous_'+str(time.time_ns())+'.json')
                    previous.write_bytes(path.read_bytes())
                temporary=path.with_suffix('.tmp')
                temporary.write_text(json.dumps(reference,indent=2),encoding='utf-8')
                temporary.replace(path)
                self.model.set_background(reference)
                self.predictor.clear('WARMING_UP')
                return reference
        finally:
            with self.lock:self.calibrating=None

class Handler(BaseHTTPRequestHandler):
    def allowed(self):
        hosts={f'127.0.0.1:{self.server.server_port}',f'localhost:{self.server.server_port}'}
        if self.headers.get('Host') not in hosts:
            self.send_error(403);return False
        for name in ('Origin','Referer'):
            value=self.headers.get(name)
            if value:
                try:
                    url=urlsplit(value)
                    good=url.scheme=='http' and url.netloc in hosts and not url.username
                except ValueError:good=False
                if not good:self.send_error(403);return False
        return True

    def do_GET(self):
        if not self.allowed():return
        if self.path in ('/','/index.html'):
            data=(ROOT/'data/model.html').read_bytes();kind='text/html; charset=utf-8'
        elif self.path=='/prediction':
            data=json.dumps(self.server.receiver.snapshot(),allow_nan=False).encode();kind='application/json'
        else:return self.send_error(404)
        self.send_response(200);self.send_header('Content-Type',kind)
        self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store')
        self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass

    def do_POST(self):
        if not self.allowed():return
        if self.path not in ('/background','/measure'):return self.send_error(405)
        try:
            length=int(self.headers.get('Content-Length','0'))
            if self.headers.get('Transfer-Encoding') or not 0<=length<=1024:raise ValueError('Invalid request body')
            body=self.rfile.read(length)
            request=json.loads(body) if body else {}
            if not isinstance(request,dict):raise ValueError('JSON object required')
            if request:raise ValueError('Expected an empty JSON object')
            result=self.server.receiver.start_measurement() if self.path=='/measure' else {'background':self.server.receiver.capture_background()}
            data=json.dumps(result).encode();status=200
        except (ValueError,OSError) as error:
            data=json.dumps({'error':str(error)}).encode();status=409
        self.send_response(status);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
    def log_message(self,*args):pass


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model',default=str(ROOT/'models/v4/model.json'))
    parser.add_argument('--source',default='http://127.0.0.1:8766')
    parser.add_argument('--http-port',type=int,default=8780)
    parser.add_argument('--no-browser',action='store_true')
    args=parser.parse_args()
    model=DistanceModel(args.model)
    receiver=ModelReceiver(model,args.source)
    server=ThreadingHTTPServer(('127.0.0.1',args.http_port),Handler);server.receiver=receiver
    receiver.thread.start()
    url=f'http://127.0.0.1:{server.server_port}/'
    print(f'PC overlap model: {url}\nModel: {args.model}\nSensor source: {receiver.source}\nESP32 remains a streamer; all inference runs on the PC.',flush=True)
    if not args.no_browser:webbrowser.open(url)
    try:server.serve_forever(poll_interval=.25)
    except KeyboardInterrupt:pass
    finally:receiver.stop.set();server.server_close()


if __name__=='__main__':main()
