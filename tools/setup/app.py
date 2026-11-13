"""Cross-platform local CitroWaker Setup app. No uploads, external website or GUI toolkit."""
from __future__ import annotations
import sys
import prepare
if len(sys.argv)>1 and sys.argv[1]=='--extract-worker':
    prepare.extract_worker(*sys.argv[2:]);raise SystemExit
import collections,hmac,json,secrets,shutil,subprocess,threading,webbrowser
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

class State:
    def __init__(self):
        self.token=secrets.token_urlsafe(32);self.lock=threading.Lock();self.cancel=threading.Event();self.running=False;self.status='ready';self.message='Choose your ISO and compatible release manifest.';self.logs=collections.deque(maxlen=120)
    def log(self,text):
        with self.lock:self.logs.extend(str(text).splitlines())
    def snapshot(self):
        with self.lock:return {'running':self.running,'status':self.status,'message':self.message,'logs':list(self.logs)}
    def begin(self,params):
        with self.lock:
            if self.running:raise ValueError('A preparation is already running.')
            paths=[Path(params[k]).expanduser() for k in ['iso','release','output']]
            if any(not str(params[k]).strip() for k in ['iso','release','output']):raise ValueError('Choose all three paths.')
            self.running=True;self.cancel.clear();self.status='running';self.logs.clear();self.message='Preparing locally…'
        def work():
            status='done';message='Export ready: '+str(paths[2])
            try:prepare.prepare(*paths,cancel=self.cancel,log=self.log)
            except Exception as e:status='cancelled' if isinstance(e,prepare.Cancelled) else 'error';message=str(e)
            with self.lock:self.running=False;self.status=status;self.message=message
        threading.Thread(target=work,daemon=False).start()

def pick(kind):
    if kind not in ['iso','release','output']:raise ValueError('Unknown file picker.')
    if sys.platform=='win32':
        script='Add-Type -AssemblyName System.Windows.Forms; [Console]::OutputEncoding=[System.Text.Encoding]::UTF8; '
        if kind=='output':script+='$d=New-Object System.Windows.Forms.FolderBrowserDialog; if($d.ShowDialog() -eq "OK"){[Console]::Write($d.SelectedPath)}'
        else:
            filt='GameCube ISO (*.iso)|*.iso' if kind=='iso' else 'CitroWaker release (*.json)|*.json'
            script+='$d=New-Object System.Windows.Forms.OpenFileDialog; $d.Filter="'+filt+'"; if($d.ShowDialog() -eq "OK"){[Console]::Write($d.FileName)}'
        args=['powershell.exe','-NoProfile','-STA','-Command',script]
    elif shutil.which('kdialog'):
        args=['kdialog','--getexistingdirectory',str(Path.home())] if kind=='output' else ['kdialog','--getopenfilename',str(Path.home()),'*.iso' if kind=='iso' else '*.json']
    elif shutil.which('zenity'):
        args=['zenity','--file-selection']+(['--directory'] if kind=='output' else [])
    else:raise ValueError('No native file picker available. Paste the full path into the field instead.')
    r=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=300)
    path=r.stdout.decode('utf-8','replace').strip()
    return str(Path(path)/'CitroWaker SD') if path and kind=='output' else path

def handler(state):
 class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass # Never log the session token or user paths.
    def reply(self,code,obj,html=False):
        b=obj if html else json.dumps(obj).encode();self.send_response(code);self.send_header('Content-Type','text/html; charset=utf-8' if html else 'application/json');self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.send_header('X-Frame-Options','DENY');self.end_headers();self.wfile.write(b)
    def do_GET(self):
        if self.path!='/':return self.reply(404,{'error':'Not found'})
        self.reply(200,(Path(__file__).parent/'index.html').read_bytes(),True)
    def do_POST(self):
        if not hmac.compare_digest(self.headers.get('X-CitroWaker-Token',''),state.token):return self.reply(403,{'error':'Invalid local session'})
        try:
            size=int(self.headers.get('Content-Length','0'))
            if not 0<size<=16384:raise ValueError('Invalid request size')
            p=json.loads(self.rfile.read(size))
            if not isinstance(p,dict):raise ValueError('Expected an object')
            route=self.path
            if route=='/status':return self.reply(200,state.snapshot())
            if route=='/pick':return self.reply(200,{'path':pick(p['kind'])})
            if route=='/start':state.begin(p);return self.reply(200,{'ok':True})
            if route=='/cancel':state.cancel.set();return self.reply(200,{'ok':True})
            return self.reply(404,{'error':'Not found'})
        except (ValueError,KeyError,OSError,subprocess.SubprocessError) as e:self.reply(400,{'error':str(e)})
 return Handler

def main():
    state=State();server=ThreadingHTTPServer(('127.0.0.1',0),handler(state));url=f'http://127.0.0.1:{server.server_port}/#{state.token}'
    print('CitroWaker Setup is running locally. Press Ctrl+C to stop.');print('Browser address: '+url,flush=True)
    if '--no-browser' not in sys.argv:webbrowser.open(url)
    try:server.serve_forever()
    except KeyboardInterrupt:state.cancel.set()
    finally:server.server_close()
if __name__=='__main__':main()
